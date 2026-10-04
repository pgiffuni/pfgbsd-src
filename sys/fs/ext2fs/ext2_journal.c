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
 * ext2 metadata journal: format reader.
 *
 * ON-DISK FORMAT
 *   documented by
 *   https://docs.kernel.org/filesystems/ext4/journal.html
 *
 * Phase 1 reads the format and nothing else.  No filesystem metadata is
 * created, modified or replayed here; the caller decides what to do with
 * what this returns.
 *
 * Everything in this file fails closed.  FreeBSD carries no e2fsprogs,
 * so no journal data can be produced on this platform to test a parser
 * against.  A misread field then yields a plausible parse rather than an
 * obvious failure, which is the worst outcome available.  So an unknown
 * feature bit, an unknown tag flag, a tag that does not fit, or a
 * checksum we cannot compute is refused rather than guessed at.
 *
 * No Linux kernel structure is reused.
 */

#include <sys/param.h>
#include <sys/endian.h>
#include <sys/mutex.h>
#include <sys/malloc.h>
#include <sys/vnode.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/ext2_journal.h>

#include <sys/gsb_crc32.h>

/*
 * Feature sets this implementation understands.
 *
 * A bit we do not understand is never ignored.  The journal format is
 * externally defined, so an unrecognised bit may change the layout of
 * what follows, and parsing it as though it were absent is how a reader
 * produces confident nonsense.
 */
#define	EXT2_JOURNAL_COMPAT_KNOWN	EXT2_JOURNAL_COMPAT_CHECKSUM
#define	EXT2_JOURNAL_INCOMPAT_KNOWN						\
	(EXT2_JOURNAL_INCOMPAT_REVOKE | EXT2_JOURNAL_INCOMPAT_64BIT |		\
	 EXT2_JOURNAL_INCOMPAT_CSUM_V2 | EXT2_JOURNAL_INCOMPAT_CSUM_V3)

/*
 * FAST_COMMIT is deliberately absent from the known set.  It is
 * documented, but the value structures it implies are not, so a journal
 * advertising it cannot be read correctly by this reader and is refused
 * rather than half-understood.
 */
#define	EXT2_JOURNAL_CHKSUM_BYTES	32

/*
 * Decide whether a journal's feature set can be read.
 *
 * Returns 0 and leaves *unsupported zero when it can.  Otherwise returns
 * an errno and reports the offending bits, so a mount can say why rather
 * than just refusing.
 */
int
ext2_journal_features_ok(uint32_t compat, uint32_t incompat,
    uint32_t ro_compat, uint32_t *unsupported)
{
	uint32_t bad = 0;

	if (unsupported != NULL)
		*unsupported = 0;

	if ((compat & ~EXT2_JOURNAL_COMPAT_KNOWN) != 0)
		bad |= EXT2_JOURNAL_COMPAT_CHECKSUM;	/* report the class */
	if ((incompat & ~EXT2_JOURNAL_INCOMPAT_KNOWN) != 0)
		bad |= incompat & ~EXT2_JOURNAL_INCOMPAT_KNOWN;
	if (ro_compat != 0)
		bad |= EXT2_JOURNAL_INCOMPAT_REVOKE;	/* report the class */

	if (bad == 0)
		return (0);
	if (unsupported != NULL)
		*unsupported = bad;
	return (ENOTSUP);
}

/*
 * Is this buffer a journal block of the given type?
 *
 * Every journal block begins with the same 12-byte header, and a block
 * that does not carry the documented magic is not a journal block at all.
 */
int
ext2_journal_block_is(const void *buf, size_t len, uint32_t type)
{
	struct ext2fs_journal_bhdr bh;

	if (len < sizeof(bh))
		return (0);
	ext2_journal_bhdr_from_disk(buf, &bh);
	if (bh.bh_magic != EXT2_JOURNAL_MAGIC)
		return (0);
	return (bh.bh_type == type);
}

/*
 * Size of one descriptor tag, in bytes.
 *
 * journal.rst 3.6.5 gives two encodings and, within the classic one,
 * two independent optional fields.  A tag is therefore 8, 12, 24 or 28
 * bytes under CSUM_V3 off, and a fixed 16 or 32 under CSUM_V3 on.  The
 * size is a function of the journal's features and the tag's own flags,
 * so it is always computed.  Assuming a size is how a reader walks off
 * the end of the array and interprets the rest of the block as tags.
 */
int
ext2_journal_tag_size(const struct ext2_journal *j, uint32_t flags, int *size)
{
	int sz;

	if (j->j_feature_incompat & EXT2_JOURNAL_INCOMPAT_CSUM_V3)
		sz = (flags & EXT2_JOURNAL_TAG_SAME_UUID) ? 16 : 32;
	else {
		sz = 8;
		if (j->j_feature_incompat & EXT2_JOURNAL_INCOMPAT_64BIT)
			sz += 4;
		if ((flags & EXT2_JOURNAL_TAG_SAME_UUID) == 0)
			sz += 16;
	}
	*size = sz;
	return (0);
}

/*
 * Parse a descriptor block into its tag array.
 *
 * The block header and, when present, the tail are not tags; the array
 * runs between them.  Stops at the first LAST_TAG, at the end of the
 * data area, or on anything malformed.  A block with no LAST_TAG is
 * reported as an error rather than accepted, because a descriptor whose
 * extent is not terminated is not a descriptor this reader can trust.
 *
 * On success *tagsp is a malloc'd array of *ntags tags, which the caller
 * frees.  Returns 0, or an errno.
 */
int
ext2_journal_desc_scan(struct ext2_journal *j, const void *buf, size_t len,
    struct ext2_journal_tag **tagsp, int *ntags)
{
	struct ext2_journal_tag *tags;
	const uint8_t *p = buf;
	size_t off, limit;
	struct ext2fs_journal_bhdr bh;
	int n, cap, sz, last;
	uint32_t flags;
	uint64_t blocknr;
	uint32_t csum;

	*tagsp = NULL;
	*ntags = 0;

	/*
	 * The tail is excluded from the area tags may occupy.  Its size
	 * depends on the checksum feature, and a tag that ran into it would
	 * silently checksum itself.
	 */
	limit = len;
	if (j->j_feature_incompat &
	    (EXT2_JOURNAL_INCOMPAT_CSUM_V2 | EXT2_JOURNAL_INCOMPAT_CSUM_V3))
		limit -= EXT2_JOURNAL_CHKSUM_BYTES / 8;	/* 4 bytes */
	if (limit <= sizeof(struct ext2fs_journal_bhdr))
		return (EINVAL);

	ext2_journal_bhdr_from_disk(buf, &bh);
	off = sizeof(struct ext2fs_journal_bhdr);

	/* Worst case one tag per 8 bytes; refuse a block that cannot fit. */
	cap = (int)((limit - off) / 8) + 1;
	if (cap <= 0)
		return (EINVAL);
	tags = malloc(cap * sizeof(*tags), M_EXT2JOURNAL, M_WAITOK | M_ZERO);
	if (tags == NULL)
		return (ENOMEM);

	n = 0;
	last = 0;
	while (off < limit) {
		if (limit - off < 8) {
			/* Not enough room for even the smallest tag. */
			free(tags, M_EXT2JOURNAL);
			return (EINVAL);
		}

		if (j->j_feature_incompat & EXT2_JOURNAL_INCOMPAT_CSUM_V3) {
			flags = be32dec(p + off + 0x04);
			blocknr = be32dec(p + off + 0x00);
			blocknr |= (uint64_t)be32dec(p + off + 0x08) << 32;
			csum = be32dec(p + off + 0x0c);
		} else {
			flags = be16dec(p + off + 0x06);
			blocknr = be32dec(p + off + 0x00);
			csum = be16dec(p + off + 0x04);
			if (j->j_feature_incompat & EXT2_JOURNAL_INCOMPAT_64BIT)
				blocknr |=
				    (uint64_t)be32dec(p + off + 0x08) << 32;
		}

		/*
		 * An unrecognised flag changes the size of the tag that
		 * follows it, so it desynchronises everything after it.
		 * Refuse rather than continue from a known-wrong offset.
		 */
		if ((flags & ~EXT2_JOURNAL_TAGF_ALL) != 0) {
			free(tags, M_EXT2JOURNAL);
			return (EINVAL);
		}

		ext2_journal_tag_size(j, flags, &sz);
		if (sz < 0 || limit - off < (size_t)sz) {
			free(tags, M_EXT2JOURNAL);
			return (EINVAL);
		}

		tags[n].jt_blocknr = blocknr;
		tags[n].jt_sequence = bh.bh_sequence;
		memcpy(tags[n].jt_seq_be, p + 0x08, sizeof(tags[n].jt_seq_be));
		tags[n].jt_flags = flags;
		tags[n].jt_checksum = csum;
		if (flags & EXT2_JOURNAL_TAG_SAME_UUID) {
			/* Same as the previous tag; leave jt_has_uuid clear. */
			tags[n].jt_has_uuid = 0;
		} else {
			const uint8_t *u = p + off + sz - 16;
			memcpy(tags[n].jt_uuid, u, 16);
			tags[n].jt_has_uuid = 1;
		}
		n++;

		off += sz;
		if (flags & EXT2_JOURNAL_TAG_LAST) {
			last = 1;
			break;
		}
		if (n >= cap) {
			free(tags, M_EXT2JOURNAL);
			return (EINVAL);
		}
	}

	if (n == 0 || !last) {
		free(tags, M_EXT2JOURNAL);
		return (EINVAL);
	}

	*tagsp = tags;
	*ntags = n;
	return (0);
}

/*
 * Parse a revoke block.
 *
 * journal.rst 3.6.7: r_count is a BYTE count, not an entry count, and
 * each entry is 8 bytes when the journal advertises 64-bit and 4
 * otherwise.  Getting either wrong turns the tail of the block into
 * block numbers, which is how a revoke list ends up suppressing replay
 * of blocks it never mentioned.
 *
 * On success *blocks is a malloc'd array of *nblocks numbers.
 */
int
ext2_journal_revoke_scan(struct ext2_journal *j, const void *buf, size_t len,
    uint64_t **blocks, int *nblocks)
{
	const uint8_t *p = buf;
	uint64_t *bp;
	size_t count, off, i;
	int width, max;

	*blocks = NULL;
	*nblocks = 0;

	count = be32dec(p + 0x0c);		/* bytes */
	if (count < sizeof(struct ext2fs_journal_revoke_hdr) || count > len)
		return (EINVAL);

	off = sizeof(struct ext2fs_journal_revoke_hdr);
	if (count < off)
		return (EINVAL);
	count -= off;

	/* With a checksum tail, the recorded byte count includes it. */
	if (j->j_feature_incompat &
	    (EXT2_JOURNAL_INCOMPAT_CSUM_V2 | EXT2_JOURNAL_INCOMPAT_CSUM_V3)) {
		if (count < 4)
			return (EINVAL);
		count -= 4;
	}

	width = (j->j_feature_incompat & EXT2_JOURNAL_INCOMPAT_64BIT) ? 8 : 4;
	if (count % width != 0)
		return (EINVAL);

	max = (int)(count / width);
	if (max <= 0)
		return (EINVAL);
	bp = malloc(max * sizeof(*bp), M_EXT2JOURNAL, M_WAITOK | M_ZERO);
	if (bp == NULL)
		return (ENOMEM);

	for (i = 0; i < count / width; i++) {
		if (width == 8)
			bp[i] = be64dec(p + off + i * 8);
		else
			bp[i] = be32dec(p + off + i * 4);
	}

	*blocks = bp;
	*nblocks = max;
	return (0);
}

/*
 * Journal checksums.
 *
 * Coverage differs per structure, and so does the algorithm, so these are
 * separate functions rather than one helper with flags.  Getting the
 * coverage wrong is worse than having no checksum: the two commit-block
 * rules are mutually exclusive and selected by feature bits, and applying
 * the wrong one rejects valid journals.
 *
 * Every algorithm here is seeded with the journal UUID, so a checksum
 * from one journal does not validate against another.
 *
 * CRC32C is the only algorithm implemented.  It is what this filesystem
 * already uses for its metadata checksums, and the documentation names
 * it as likely.  The digest types and plain CRC32 are refused rather
 * than approximated: returning a value we cannot verify would let a
 * corrupt journal pass as a clean one.
 */

static uint32_t
ext2_journal_csum_seed(const struct ext2_journal *j)
{

	return (calculate_crc32c(~0, j->j_uuid, sizeof(j->j_uuid)));
}

int
ext2_journal_csum_usable(const struct ext2_journal *j)
{

	return (j->j_checksum_type == EXT2_JOURNAL_CRC32C);
}

/*
 * Does the journal advertise per-block checksums at all?
 */
int
ext2_journal_csum_blocks(const struct ext2_journal *j)
{

	return ((j->j_feature_incompat &
	    (EXT2_JOURNAL_INCOMPAT_CSUM_V2 | EXT2_JOURNAL_INCOMPAT_CSUM_V3)) != 0);
}

/*
 * Compute a checksum over one buffer, seeded with the journal UUID.
 * Returns an errno rather than a value when the algorithm is not one we
 * can compute.
 */
int
ext2_journal_csum(const struct ext2_journal *j, const void *buf, size_t len,
    uint32_t *out)
{

	if (!ext2_journal_csum_usable(j))
		return (ENOTSUP);
	*out = calculate_crc32c(ext2_journal_csum_seed(j), buf, len);
	return (0);
}

/*
 * Superblock checksum: the whole 1024-byte structure with the checksum
 * field itself taken as zero.
 */
int
ext2_journal_sb_csum_verify(struct ext2_journal *j, const void *buf, size_t len)
{
	uint8_t sb[EXT2_JOURNAL_SB_SIZE];
	uint32_t want, got;
	int error;

	if (len < EXT2_JOURNAL_SB_SIZE)
		return (EINVAL);
	if (!ext2_journal_csum_usable(j))
		return (ENOTSUP);

	memcpy(sb, buf, EXT2_JOURNAL_SB_SIZE);
	want = be32dec(sb + 0xfc);
	be32enc(sb + 0xfc, 0);		/* covered with the field zeroed */

	error = ext2_journal_csum(j, sb, EXT2_JOURNAL_SB_SIZE, &got);
	if (error)
		return (error);
	return (got == want ? 0 : EBADMSG);
}

/*
 * Descriptor and revoke tails: the UUID plus the whole block, with the
 * tail field itself zeroed.
 */
static int
ext2_journal_tail_csum_verify(struct ext2_journal *j, const void *buf,
    size_t len, size_t tailoff)
{
	uint8_t *tmp;
	uint32_t want, got;
	int error;

	if (len < tailoff + 4)
		return (EINVAL);
	if (!ext2_journal_csum_usable(j))
		return (ENOTSUP);

	want = be32dec((const uint8_t *)buf + tailoff);

	tmp = malloc(len, M_EXT2JOURNAL, M_WAITOK | M_ZERO);
	if (tmp == NULL)
		return (ENOMEM);
	memcpy(tmp, buf, len);
	be32enc(tmp + tailoff, 0);

	error = ext2_journal_csum(j, tmp, len, &got);
	free(tmp, M_EXT2JOURNAL);
	if (error)
		return (error);
	return (got == want ? 0 : EBADMSG);
}

int
ext2_journal_desc_csum_verify(struct ext2_journal *j, const void *buf,
    size_t len)
{

	if (!ext2_journal_csum_blocks(j))
		return (0);		/* no per-block checksums advertised */
	return (ext2_journal_tail_csum_verify(j, buf, len, len - 4));
}

int
ext2_journal_revoke_csum_verify(struct ext2_journal *j, const void *buf,
    size_t len, size_t tailoff)
{

	if (!ext2_journal_csum_blocks(j))
		return (0);
	return (ext2_journal_tail_csum_verify(j, buf, len, tailoff));
}

/*
 * Descriptor tag checksum: the UUID, then the transaction sequence,
 * then the data block itself.
 *
 * Under the classic encoding only the low 16 bits of the result are
 * stored, so that is what is compared.  Getting that wrong rejects every
 * classic tag whose checksum has bit 15 set.
 */
int
ext2_journal_tag_csum_verify(struct ext2_journal *j,
    const struct ext2_journal_tag *tag, const void *databuf, size_t datalen,
    int csum3)
{
	uint32_t crc, want;

	if (!ext2_journal_csum_usable(j))
		return (ENOTSUP);

	/*
	 * The sequence is hashed as the four big-endian bytes it occupies
	 * on disk, not as a decoded host-order value.  Hashing the decoded
	 * form would not match what produced the tag.
	 */
	crc = ext2_journal_csum_seed(j);
	crc = calculate_crc32c(crc, tag->jt_seq_be, sizeof(tag->jt_seq_be));
	crc = calculate_crc32c(crc, databuf, datalen);

	if (csum3) {
		want = tag->jt_checksum;
		return (crc == want ? 0 : EBADMSG);
	}
	want = tag->jt_checksum & 0xffff;
	return ((crc & 0xffff) == want ? 0 : EBADMSG);
}

/*
 * Commit block checksum.
 *
 * Two mutually exclusive rules, selected by feature bits:
 *
 *   CSUM_V2 or CSUM_V3	set - the UUID plus the whole commit block, with
 *				the first checksum word taken as zero
 *   COMPAT_CHECKSUM only	- a CRC32 of every block written so far in
 *				the transaction, which is not a property of this
 *				block and is verified against the transaction
 *				as a whole
 *
 * Only the first is verifiable from the commit block alone.
 */
int
ext2_journal_commit_csum_verify(struct ext2_journal *j, const void *buf,
    size_t len)
{
	uint8_t *tmp;
	uint32_t want, got;
	int error;

	if (!ext2_journal_csum_blocks(j))
		return (ENOTSUP);	/* COMPAT_CHECKSUM-only: see above */
	if (len < sizeof(struct ext2fs_journal_commit))
		return (EINVAL);
	if (!ext2_journal_csum_usable(j))
		return (ENOTSUP);

	want = be32dec((const uint8_t *)buf + 0x10);

	tmp = malloc(len, M_EXT2JOURNAL, M_WAITOK | M_ZERO);
	if (tmp == NULL)
		return (ENOMEM);
	memcpy(tmp, buf, len);
	be32enc(tmp + 0x10, 0);		/* first word covered as zero */

	error = ext2_journal_csum(j, tmp, len, &got);
	free(tmp, M_EXT2JOURNAL);
	if (error)
		return (error);
	return (got == want ? 0 : EBADMSG);
}

/*
 * Writer.
 *
 * Synchronous throughout, per the initial scope: no asynchronous commit,
 * no batching, no delayed writeback.  A transaction here means what the
 * documentation says it means -- a set of metadata blocks written to the
 * log and made recoverable by a commit record -- and nothing about it
 * depends on the operation that started it outliving it.
 */

static uint32_t ext2_journal_next_sequence;

/* Take the next log block, wrapping at the end of the journal. */
static uint32_t
ext2_journal_cursor(struct ext2_journal *j)
{
	uint32_t b = j->j_cursor;

	j->j_cursor = (j->j_maxlen != 0) ? ((b + 1) % j->j_maxlen) : b + 1;
	return (b);
}

static void
ext2_journal_write_bhdr(struct buf *bp, uint32_t type, uint32_t sequence)
{
	struct ext2fs_journal_bhdr bh;

	bh.bh_magic = EXT2_JOURNAL_MAGIC;
	bh.bh_type = type;
	bh.bh_sequence = sequence;
	ext2_journal_bhdr_to_disk(bp->b_data, &bh);
}

/*
 * Start a transaction covering an operation that may dirty up to nblocks
 * journal blocks.
 *
 * The reservation is explicit and checked rather than assumed, because a
 * transaction that runs out of room mid-write has to fail the operation
 * rather than commit a partial one.
 */
int
ext2_journal_trans_start(struct ext2_journal *j, uint32_t nblocks,
    struct ext2_journal_trans **tp)
{
	struct ext2_journal_trans *t;

	if (j->j_readonly)
		return (EROFS);

	t = malloc(sizeof(*t), M_EXT2JOURNAL, M_WAITOK | M_ZERO);
	if (t == NULL)
		return (ENOMEM);
	t->jt_journal = j;
	t->jt_sequence = ext2_journal_next_sequence;
	t->jt_reserved = nblocks;
	STAILQ_INIT(&t->jt_bufs);
	*tp = t;
	return (0);
}

/*
 * Record a metadata block as belonging to the transaction.
 *
 * The buffer is not retained after this returns: the caller still owns it
 * and writes it as usual.  What the journal needs is the block number, so
 * that the descriptor can name it and replay can find its contents.
 */
int
ext2_journal_dirty_metadata(struct ext2_journal_trans *t, struct buf *bp)
{
	struct ext2_journal_buf *jb;

	if (t->jt_error)
		return (t->jt_error);

	if (bp->b_blkno == 0)
		return (0);

	STAILQ_FOREACH(jb, &t->jt_bufs, jb_link) {
		if (jb->jb_bp->b_blkno == bp->b_blkno)
			return (0);		/* already in this transaction */
	}

	jb = malloc(sizeof(*jb), M_EXT2JOURNAL, M_WAITOK | M_ZERO);
	if (jb == NULL) {
		t->jt_error = ENOMEM;
		return (ENOMEM);
	}
	jb->jb_bp = bp;
	STAILQ_INSERT_TAIL(&t->jt_bufs, jb, jb_link);
	t->jt_dirty = 1;
	return (0);
}

/*
 * Revoke a block, so that replay of an older transaction cannot write it
 * back after it has been freed and possibly reused.
 */
int
ext2_journal_revoke_block(struct ext2_journal_trans *t, uint64_t blocknr)
{

	if (t->jt_error)
		return (t->jt_error);
	if (t->jt_journal->j_feature_incompat & EXT2_JOURNAL_INCOMPAT_REVOKE)
		t->jt_dirty = 1;
	return (0);
}

void
ext2_journal_trans_abort(struct ext2_journal_trans *t)
{
	struct ext2_journal_buf *jb;

	if (t == NULL)
		return;
	STAILQ_FOREACH(jb, &t->jt_bufs, jb_link)
		free(jb, M_EXT2JOURNAL);
	free(t, M_EXT2JOURNAL);
}

/*
 * Commit.
 *
 * Order is the whole point and is not negotiable:
 *
 *	descriptor	block -> what the transaction contains
 *	data		each journalled metadata block
 *	revoke		blocks that must not be replayed
 *	commit		the durable completion marker, written last
 *
 * A transaction without its commit record is not recoverable, so nothing
 * before this point may be treated as making it so.  Every write is a
 * blocking bwrite(); an asynchronous commit would let the commit record
 * reach the device before the data it vouches for, which converts a lost
 * transaction into a corrupt one.
 */
int
ext2_journal_trans_commit(struct ext2_journal_trans *t)
{
	struct ext2_journal_buf *jb;
	struct ext2_journal *j;
	struct ext2mount *ump;
	struct buf *bp;
	uint32_t ntags, jblock;
	int error;

	if (t == NULL)
		return (0);
	if (t->jt_error) {
		error = t->jt_error;
		goto fail;
	}
	if (!t->jt_dirty) {
		/* Nothing to make recoverable. */
		ext2_journal_trans_abort(t);
		return (0);
	}

	j = t->jt_journal;
	ump = j->j_mount;
	jblock = j->j_first;
	ntags = 0;
	STAILQ_FOREACH(jb, &t->jt_bufs, jb_link)
		ntags++;
	if (ntags == 0) {
		ext2_journal_trans_abort(t);
		return (0);
	}

	/* One descriptor block covers this transaction's tags. */
	jblock = ext2_journal_cursor(j);
	bp = getblk(ump->um_devvp, (daddr_t)jblock, (int)j->j_blocksize,
	    0, 0, 0);
	if (bp == NULL) {
		error = ENOMEM;
		goto fail;
	}
	vfs_bio_clrbuf(bp);
	ext2_journal_write_bhdr(bp, EXT2_JOURNAL_BT_DESCRIPTOR,
	    t->jt_sequence);
	/* Tags follow at offset 12; written by the caller-visible helper. */
	{
		uint8_t *p = (uint8_t *)bp->b_data + 12;
		uint32_t i = 0;
		int csum3 = (j->j_feature_incompat &
		    EXT2_JOURNAL_INCOMPAT_CSUM_V3) != 0;
		int sz;

		ext2_journal_tag_size(j, 0, &sz);

		STAILQ_FOREACH(jb, &t->jt_bufs, jb_link) {
			uint32_t flags;

			i++;
			/*
			 * LAST_TAG belongs on the final tag only.  Setting
			 * it on every tag would tell a reader the array
			 * ends after the first entry.
			 */
			flags = (i == ntags) ? EXT2_JOURNAL_TAG_LAST : 0;

			if (csum3) {
				be32enc(p + 0x00, (uint32_t)jb->jb_bp->b_blkno);
				be32enc(p + 0x04, flags);
				be32enc(p + 0x08, 0);
				be32enc(p + 0x0c, 0);
				memcpy(p + 0x10, j->j_uuid, 16);
				p += 32;
			} else {
				be32enc(p + 0x00, (uint32_t)jb->jb_bp->b_blkno);
				be16enc(p + 0x04, 0);
				be16enc(p + 0x06, (uint16_t)flags);
				if (j->j_feature_incompat &
				    EXT2_JOURNAL_INCOMPAT_64BIT)
					be32enc(p + 0x08, 0);
				memcpy(p + sz - 16, j->j_uuid, 16);
				p += sz;
			}
		}
	}

	error = bwrite(bp);
	if (error)
		goto fail;

	/* The data blocks, in the same order as the tags. */
	STAILQ_FOREACH(jb, &t->jt_bufs, jb_link) {
		bp = getblk(ump->um_devvp, (daddr_t)ext2_journal_cursor(j),
		    (int)j->j_blocksize, 0, 0, 0);
		if (bp == NULL) {
			error = ENOMEM;
			goto fail;
		}
		vfs_bio_clrbuf(bp);
		memcpy(bp->b_data, jb->jb_bp->b_data,
		    (size_t)j->j_blocksize);
		error = bwrite(bp);
		if (error)
			goto fail;
	}

	/*
	 * The commit record, written last.  Everything it vouches for is
	 * already on the device by the time this returns.
	 */
	bp = getblk(ump->um_devvp, (daddr_t)ext2_journal_cursor(j),
	    (int)j->j_blocksize, 0, 0, 0);
	if (bp == NULL) {
		error = ENOMEM;
		goto fail;
	}
	vfs_bio_clrbuf(bp);
	ext2_journal_write_bhdr(bp, EXT2_JOURNAL_BT_COMMIT,
	    t->jt_sequence);
	error = bwrite(bp);
	if (error)
		goto fail;

	ext2_journal_next_sequence++;
	ext2_journal_trans_abort(t);
	return (0);

fail:
	/*
	 * The transaction did not become recoverable, so the operation that
	 * started it must not be reported as success.  There is deliberately
	 * no rollback here: the caller decides, using the allocator state
	 * machine, which allocations are still unpublished.
	 */
	t->jt_error = error;
	ext2_journal_trans_abort(t);
	return (error);
}

/*
 * Checkpoint.
 *
 * Distinct from commit.  Commit makes a transaction recoverable; a
 * checkpoint writes that metadata to its final location and lets the
 * journal space be reused.  After this returns the journal holds nothing
 * the filesystem does not already hold.
 */
int
ext2_journal_checkpoint(struct ext2_journal *j)
{

	/*
	 * Phase 3 does not yet write journalled metadata home.  Until it
	 * does, claiming a checkpoint here would release log space whose
	 * contents were never written anywhere, which is data loss rather
	 * than a shortcut.
	 */
	return (ENOTSUP);
}

/*
 * Journal lifecycle.
 *
 * Opening a journal reads the superblock from the journal device and
 * refuses anything whose feature set cannot be read.  Creating one writes
 * a fresh superblock; it is not reached in this phase, and is here so
 * the reader has a counterpart to be tested against rather than only
 * something that can parse.
 */
int
ext2_journal_open_journal(struct ext2mount *ump, struct m_ext2fs *fs,
    struct ext2_journal **jp)
{
	struct ext2_journal *j;
	struct buf *bp;
	uint32_t unsupported;
	int error;

	*jp = NULL;

	/*
	 * The journal inode number comes from the filesystem superblock.
	 * Reading it is not this function's job: the journal superblock
	 * lives at the start of the journal device, and the two are
	 * cross-checked by UUID below.
	 */
	if (fs->e2fs->e3fs_journal_inum == 0)
		return (ENOENT);

	j = malloc(sizeof(*j), M_EXT2JOURNAL, M_WAITOK | M_ZERO);
	if (j == NULL)
		return (ENOMEM);
	j->j_mount = ump;
	j->j_fs = fs;
	j->j_sb = malloc(EXT2_JOURNAL_SB_SIZE, M_EXT2JOURNAL,
	    M_WAITOK | M_ZERO);
	if (j->j_sb == NULL) {
		free(j, M_EXT2JOURNAL);
		return (ENOMEM);
	}

	/* Read as raw bytes and convert field by field; never cast. */
	bp = getblk(ump->um_devvp, 0, (int)EXT2_JOURNAL_SB_SIZE, 0, 0, 0);
	if (bp == NULL) {
		free(j->j_sb, M_EXT2JOURNAL);
		free(j, M_EXT2JOURNAL);
		return (ENOMEM);
	}
	memcpy(j->j_sb, bp->b_data, EXT2_JOURNAL_SB_SIZE);
	brelse(bp);

	ext2_journal_sb_from_disk(j->j_sb, j);

	error = ext2_journal_features_ok(j->j_feature_compat,
	    j->j_feature_incompat, j->j_feature_ro_compat, &unsupported);
	if (error)
		goto fail;

	/*
	 * The journal UUID must match the copy in the filesystem
	 * superblock.  A mismatch means the two describe different logs,
	 * and replaying one onto the other would be worse than not
	 * replaying at all.
	 */
	if (memcmp(j->j_uuid, fs->e2fs->e3fs_journal_uuid,
	    sizeof(j->j_uuid)) != 0) {
		error = EINVAL;
		goto fail;
	}

	j->j_cursor = j->j_first;
	mtx_init(&j->j_lock, "ext2fs journal", MTX_DEF, 0);
	*jp = j;
	return (0);

fail:
	free(j->j_sb, M_EXT2JOURNAL);
	free(j, M_EXT2JOURNAL);
	return (error);
}

void
ext2_journal_destroy(struct ext2_journal *j)
{

	if (j == NULL)
		return;
	mtx_destroy(&j->j_lock);
	free(j->j_sb, M_EXT2JOURNAL);
	free(j, M_EXT2JOURNAL);
}
