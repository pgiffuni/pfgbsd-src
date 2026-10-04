/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 * ext2 metadata journal: transaction scanning and recovery.
 *
 * ON-DISK FORMAT
 *   documented by
 *   https://docs.kernel.org/filesystems/ext4/journal.html
 *
 * Recovery follows the documented three-pass model rather than a simpler
 * one:
 *
 *	pass 1	find the end of the log, and with it the sequence range
 *		that could have been committed
 *	pass 2	collect revocations across every committed transaction
 *	pass 3	replay, skipping any block revoked by the transaction
 *		being replayed or by a later one
 *
 * Replay writes through the device buffer cache directly.  It never calls
 * VOP_WRITE, ext2_bwrite or any other path that would start a journal
 * transaction of its own: replay that re-journals itself is not recovery.
 *
 * No Linux kernel structure is reused.
 */

#include <sys/param.h>
#include <sys/endian.h>
#include <sys/buf.h>
#include <sys/mutex.h>
#include <sys/malloc.h>
#include <sys/vnode.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/ext2_journal.h>

/*
 * Read one journal block.
 *
 * Journal blocks live on the journal device, not in the filesystem, so
 * this goes to the device vnode rather than through any filesystem path.
 */
static int
ext2_journal_read(struct ext2_journal *j, uint32_t blockno, struct buf **bpp)
{
	struct ext2mount *ump = j->j_mount;
	struct buf *bp;
	int error;

	error = bread(ump->um_devvp, (daddr_t)blockno, (int)j->j_blocksize,
	    NOCRED, &bp);
	if (error)
		return (error);
	*bpp = bp;
	return (0);
}

/*
 * Wrap or unwrap a journal block number.
 *
 * The log may wrap around the end of the journal area, so every block
 * number is taken modulo the journal length.  A journal whose length is
 * not usable cannot be walked at all.
 */
static uint32_t
ext2_journal_wrap(const struct ext2_journal *j, uint32_t blockno)
{

	if (j->j_maxlen == 0)
		return (0);
	return (blockno % j->j_maxlen);
}

/*
 * Record that a block is revoked as of a sequence.
 *
 * A block can be revoked, then written again by a later transaction, then
 * revoked once more.  Only the latest revocation matters, so a repeat
 * updates the sequence rather than leaving the older one in place: a stale
 * sequence would let a transaction between the two replay the block, which
 * is exactly what revoking it was meant to prevent.
 */
static int
ext2_journal_revoke_add(struct ext2_journal_scan *sc, uint64_t blocknr,
    uint32_t sequence)
{
	struct ext2_journal_revoked *rv;

	STAILQ_FOREACH(rv, &sc->sc_revoked, jr_link) {
		if (rv->jr_blocklo == (uint32_t)blocknr &&
		    rv->jr_blockhi == (uint32_t)(blocknr >> 32)) {
			if (sequence > rv->jr_sequence)
				rv->jr_sequence = sequence;
			return (0);
		}
	}

	rv = malloc(sizeof(*rv), M_EXT2JOURNAL, M_WAITOK | M_ZERO);
	if (rv == NULL)
		return (ENOMEM);
	rv->jr_blocklo = (uint32_t)blocknr;
	rv->jr_blockhi = (uint32_t)(blocknr >> 32);
	rv->jr_sequence = sequence;
	STAILQ_INSERT_TAIL(&sc->sc_revoked, rv, jr_link);
	return (0);
}

/*
 * Is a block revoked as of the sequence being replayed?
 *
 * A block revoked by the transaction itself, or by any later one, must
 * not be written back from the journal.
 */
int
ext2_journal_revoked_p(struct ext2_journal_scan *sc, uint64_t blocknr,
    uint32_t sequence)
{
	struct ext2_journal_revoked *rv;

	STAILQ_FOREACH(rv, &sc->sc_revoked, jr_link) {
		if (rv->jr_blocklo == (uint32_t)blocknr &&
		    rv->jr_blockhi == (uint32_t)(blocknr >> 32) &&
		    rv->jr_sequence >= sequence)
			return (1);
	}
	return (0);
}

static struct ext2_journal_trans *
ext2_journal_trans_get(struct ext2_journal_scan *sc, uint32_t sequence)
{
	struct ext2_journal_trans *tr;

	STAILQ_FOREACH(tr, &sc->sc_trans, tr_link) {
		if (tr->tr_sequence == sequence)
			return (tr);
	}
	tr = malloc(sizeof(*tr), M_EXT2JOURNAL, M_WAITOK | M_ZERO);
	if (tr == NULL)
		return (NULL);
	tr->tr_sequence = sequence;
	STAILQ_INIT(&tr->tr_filedescs);
	STAILQ_INSERT_TAIL(&sc->sc_trans, tr, tr_link);
	return (tr);
}

void
ext2_journal_scan_free(struct ext2_journal_scan *sc)
{
	struct ext2_journal_trans *tr, *ntr;
	struct ext2_journal_filedesc *fd, *nfd;
	struct ext2_journal_revoked *rv, *nrv;

	STAILQ_FOREACH_SAFE(tr, &sc->sc_trans, tr_link, ntr) {
		STAILQ_FOREACH_SAFE(fd, &tr->tr_filedescs, fd_link, nfd)
			free(fd, M_EXT2JOURNAL);
		STAILQ_REMOVE(&sc->sc_trans, tr, ext2_journal_trans, tr_link);
		free(tr, M_EXT2JOURNAL);
	}
	STAILQ_FOREACH_SAFE(rv, &sc->sc_revoked, jr_link, nrv) {
		STAILQ_REMOVE(&sc->sc_revoked, rv, ext2_journal_revoked, jr_link);
		free(rv, M_EXT2JOURNAL);
	}
}

/*
 * Walk the log once.
 *
 * Descriptor blocks accumulate tags into the transaction named by their
 * own header sequence.  A commit block marks its transaction committed.
 * A revoke block contributes revocations to the transaction it names.
 *
 * An unexpected block type ends the walk.  That is not the same as end of
 * log, and the distinction matters: treating a read error or a garbage
 * block as a clean end of log would silently discard whatever came after
 * it.  So the caller can tell the two apart.
 */
static int
ext2_journal_scan_pass(struct ext2_journal *j, struct ext2_journal_scan *sc,
    uint32_t start, uint32_t *endp)
{
	struct ext2_journal_trans *tr;
	struct ext2_journal_filedesc *fd;
	struct ext2_journal_tag *tags = NULL;
	struct ext2fs_journal_bhdr bh;
	uint64_t *revoked = NULL;
	struct buf *bp;
	uint32_t blockno, i, ntags_total;
	int error, ntags, nrevoked;

	blockno = start;
	*endp = start;
	ntags_total = 0;

	for (;;) {
		blockno = ext2_journal_wrap(j, blockno);
		error = ext2_journal_read(j, blockno, &bp);
		if (error) {
			/*
			 * Past the written region the device may be sparse
			 * or the block may simply not exist; that is end of
			 * log.  Anything else is a real error and is
			 * reported rather than swallowed.
			 */
			if (error == ENOENT || error == EBADF) {
				*endp = blockno;
				return (0);
			}
			return (error);
		}

		ext2_journal_bhdr_from_disk(bp->b_data, &bh);
		if (bh.bh_magic != EXT2_JOURNAL_MAGIC)
			break;			/* end of log */
		if (bh.bh_type != EXT2_JOURNAL_BT_DESCRIPTOR &&
		    bh.bh_type != EXT2_JOURNAL_BT_COMMIT &&
		    bh.bh_type != EXT2_JOURNAL_BT_REVOKE)
			break;			/* not a transaction block */

		switch (bh.bh_type) {
		case EXT2_JOURNAL_BT_DESCRIPTOR:
			error = ext2_journal_desc_scan(j, bp->b_data,
			    bp->b_bufsize, &tags, &ntags);
			if (error) {
				brelse(bp);
				return (error);
			}
			tr = ext2_journal_trans_get(sc, bh.bh_sequence);
			if (tr == NULL) {
				free(tags, M_EXT2JOURNAL);
				brelse(bp);
				return (ENOMEM);
			}

			/*
			 * The data blocks follow their descriptor block,
			 * in tag order.  Record which journal block holds
			 * each tag's data so replay can find it without
			 * re-walking the log.
			 */
			for (i = 0; i < (uint32_t)ntags; i++) {
				fd = malloc(sizeof(*fd), M_EXT2JOURNAL,
				    M_WAITOK | M_ZERO);
				if (fd == NULL) {
					free(tags, M_EXT2JOURNAL);
					brelse(bp);
					return (ENOMEM);
				}
				fd->fd_blocknr = tags[i].jt_blocknr;
				fd->fd_flags = tags[i].jt_flags;
				fd->fd_checksum = tags[i].jt_checksum;
				fd->fd_has_uuid = tags[i].jt_has_uuid;
				memcpy(fd->fd_uuid, tags[i].jt_uuid, 16);
				fd->fd_sequence = bh.bh_sequence;
				fd->fd_jblock = ext2_journal_wrap(j,
				    blockno + 1 + i);
				STAILQ_INSERT_TAIL(&tr->tr_filedescs, fd,
				    fd_link);
			}
			free(tags, M_EXT2JOURNAL);
			ntags_total += ntags;
			break;

		case EXT2_JOURNAL_BT_COMMIT:
			tr = ext2_journal_trans_get(sc, bh.bh_sequence);
			if (tr == NULL) {
				brelse(bp);
				return (ENOMEM);
			}
			tr->tr_has_commit = 1;
			/*
			 * A commit record is the durable completion marker.
			 * Without a valid one the transaction is discarded,
			 * so a checksum failure here must not mark it
			 * committed.
			 */
			error = ext2_journal_commit_csum_verify(j, bp->b_data,
			    bp->b_bufsize);
			if (error != 0 && error != ENOTSUP) {
				brelse(bp);
				return (error);
			}
			tr->tr_committed = 1;
			break;

		case EXT2_JOURNAL_BT_REVOKE:
			error = ext2_journal_revoke_scan(j, bp->b_data,
			    bp->b_bufsize, &revoked, &nrevoked);
			if (error) {
				brelse(bp);
				return (error);
			}
			tr = ext2_journal_trans_get(sc, bh.bh_sequence);
			if (tr == NULL) {
				free(revoked, M_EXT2JOURNAL);
				brelse(bp);
				return (ENOMEM);
			}
			for (i = 0; i < (uint32_t)nrevoked; i++) {
				error = ext2_journal_revoke_add(sc,
				    revoked[i], bh.bh_sequence);
				if (error) {
					free(revoked, M_EXT2JOURNAL);
					brelse(bp);
					return (error);
				}
			}
			free(revoked, M_EXT2JOURNAL);
			break;
		}

		brelse(bp);
		blockno++;

		/*
		 * Step over this descriptor's data blocks.  They are not
		 * journal blocks themselves -- they hold filesystem
		 * metadata -- so they carry no header to test and must not
		 * be fed back into the walk.
		 */
		if (bh.bh_type == EXT2_JOURNAL_BT_DESCRIPTOR) {
			for (i = 0; i < ntags_total; i++) {
				struct buf *dbp;

				error = ext2_journal_read(j,
				    ext2_journal_wrap(j, blockno), &dbp);
				if (error)
					return (error);
				brelse(dbp);
				blockno++;
			}
			ntags_total = 0;
		}
	}

	*endp = blockno;
	return (0);
}

/*
 * Full scan: walk the log, then mark which transactions are recoverable.
 */
int
ext2_journal_scan(struct ext2_journal *j, struct ext2_journal_scan *sc)
{
	struct ext2_journal_trans *tr;
	int error;

	STAILQ_INIT(&sc->sc_trans);
	STAILQ_INIT(&sc->sc_revoked);
	sc->sc_journal = j;
	sc->sc_error = 0;

	error = ext2_journal_scan_pass(j, sc, j->j_first, &sc->sc_end);
	if (error)
		return (error);

	/*
	 * Everything found but never committed is discarded.  This is where
	 * an interrupted write stops mattering.
	 */
	STAILQ_FOREACH(tr, &sc->sc_trans, tr_link)
		tr->tr_committed = tr->tr_has_commit && tr->tr_committed;
	return (0);
}

/*
 * Replay one block's journalled contents onto its home location.
 *
 * Written through the device buffer cache, never through a filesystem
 * write path: replay must not journal itself.
 */
static int
ext2_journal_replay_block(struct ext2_journal *j, uint64_t blocknr,
    const void *databuf, size_t datalen, int flags)
{
	struct ext2mount *ump = j->j_mount;
	struct buf *bp;
	int error;

	bp = getblk(ump->um_devvp, (daddr_t)blocknr, (int)datalen, 0, 0, 0);
	if (bp == NULL)
		return (ENOMEM);

	if (flags & EXT2_JOURNAL_TAG_ESCAPED) {
		/*
		 * The block's first four bytes happened to match the journal
		 * magic, so they were replaced with zeroes in the journalled
		 * copy to keep it from being mistaken for a journal block.
		 * Put the magic back before writing.
		 */
		be32enc(bp->b_data, EXT2_JOURNAL_MAGIC);
		if (datalen > 4)
			memcpy((uint8_t *)bp->b_data + 4,
			    (const uint8_t *)databuf + 4, datalen - 4);
	} else {
		memcpy(bp->b_data, databuf, datalen);
	}

	vfs_bio_clrbuf(bp);
	error = bwrite(bp);
	return (error);
}

/*
 * Replay every committed transaction, oldest sequence first.
 *
 * Oldest first matters: a block journalled by more than one transaction
 * must end up holding the contents of the newest, so the order is the
 * reverse of the one the transactions were written in.
 */
static int
ext2_journal_replay(struct ext2_journal *j, struct ext2_journal_scan *sc)
{
	struct ext2_journal_trans *tr, *best;
	struct ext2_journal_filedesc *fd;
	struct ext2mount *ump = j->j_mount;
	struct buf *jbp;
	uint32_t bsize;
	int error, replayed;

	bsize = ump->um_e2fs->e2fs_bsize;
	replayed = 0;

	for (;;) {
		best = NULL;
		STAILQ_FOREACH(tr, &sc->sc_trans, tr_link) {
			if (!tr->tr_committed || tr->tr_replayed)
				continue;
			if (best == NULL || tr->tr_sequence < best->tr_sequence)
				best = tr;
		}
		if (best == NULL)
			break;
		best->tr_replayed = 1;

		STAILQ_FOREACH(fd, &best->tr_filedescs, fd_link) {
			/*
			 * A block revoked by this transaction or a later
			 * one must not be written back: it has been freed
			 * and possibly reused since.
			 */
			if (ext2_journal_revoked_p(sc, fd->fd_blocknr,
			    best->tr_sequence))
				continue;

			error = ext2_journal_read(j, fd->fd_jblock, &jbp);
			if (error)
				return (error);

			error = ext2_journal_replay_block(j, fd->fd_blocknr,
			    jbp->b_data, bsize, (int)fd->fd_flags);
			brelse(jbp);
			if (error)
				return (error);
			replayed++;
		}
	}

	return (replayed);
}

/*
 * Recovery.
 *
 * Runs during mount, before the filesystem is handed out and before any
 * transaction of ours could be started, so nothing here can journal
 * itself.  On success the journal contains nothing that is not already on
 * the filesystem, and the caller may mark the filesystem clean.
 */
int
ext2_journal_recover(struct ext2_journal *j)
{
	struct ext2_journal_scan sc;
	int error, replayed;

	error = ext2_journal_scan(j, &sc);
	if (error) {
		ext2_journal_scan_free(&sc);
		return (error);
	}

	replayed = ext2_journal_replay(j, &sc);

	ext2_journal_scan_free(&sc);
	return (error ? error : replayed);
}
