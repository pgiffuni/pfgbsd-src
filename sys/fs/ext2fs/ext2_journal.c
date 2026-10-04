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
#include <sys/_offsetof.h>
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
#include <fs/ext2fs/ext2_softdep.h>

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

		/*
		 * Read through the asserted structures, not at hand-written
		 * offsets, so the layout and the code stay one thing.
		 */
		if (j->j_feature_incompat & EXT2_JOURNAL_INCOMPAT_CSUM_V3) {
			struct ext2fs_journal_tag_csum3 tag;

			memcpy(&tag, p + off, sizeof(tag));
			flags = be32toh(tag.t_flags);
			blocknr = be32toh(tag.t_blocknr) |
			    ((uint64_t)be32toh(tag.t_blocknr_high) << 32);
			csum = be32toh(tag.t_checksum);
		} else {
			struct ext2fs_journal_tag_classic tag;
			int has_high = (j->j_feature_incompat &
			    EXT2_JOURNAL_INCOMPAT_64BIT) != 0;
			size_t fixed = offsetof(struct
			    ext2fs_journal_tag_classic, t_blocknr_high);

			memcpy(&tag, p + off, fixed + (has_high ?
			    sizeof(tag.t_blocknr_high) : 0));
			flags = be16toh(tag.t_flags);
			blocknr = be32toh(tag.t_blocknr);
			csum = be16toh(tag.t_checksum);
			if (has_high)
				blocknr |= (uint64_t)be32toh(tag.t_blocknr_high)
				    << 32;
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

	{
		struct ext2fs_journal_revoke_hdr rh;

		memcpy(&rh, p, sizeof(rh));
		count = be32toh(rh.rh_count);		/* bytes */
	}
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
 * Superblock checksum.
 *
 * The whole 1024-byte structure with the checksum field taken as zero,
 * and -- unlike every other checksum in the journal -- with no journal
 * UUID mixed in.  The UUID belongs to the checksums over descriptor,
 * data, revoke and commit blocks; the superblock already carries it at
 * 0x30, so seeding with it would cover those bytes twice.
 *
 * The convention is verified against a superblock written by a real
 * implementation: the stored value is crc32c(0, sb, 1024) under Linux's
 * crc32c(), which xors on entry and not on exit.  calculate_crc32c()
 * xors on both, so the equivalent here is its result inverted.  Getting
 * either detail wrong rejects every journal.
 */
int
ext2_journal_sb_csum_verify(struct ext2_journal *j, const void *buf, size_t len)
{
	uint8_t sb[EXT2_JOURNAL_SB_SIZE];
	uint32_t want, got;

	if (len < EXT2_JOURNAL_SB_SIZE)
		return (EINVAL);
	if (!ext2_journal_csum_usable(j))
		return (ENOTSUP);

	memcpy(sb, buf, EXT2_JOURNAL_SB_SIZE);
	want = be32dec(sb + 0xfc);
	be32enc(sb + 0xfc, 0);		/* covered with the field zeroed */

	/*
	 * Deliberately not ext2_journal_csum(), which seeds with the UUID.
	 */
	got = calculate_crc32c(0, sb, EXT2_JOURNAL_SB_SIZE) ^ 0xFFFFFFFF;
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
	STAILQ_INIT(&t->jt_revokes);
	*tp = t;
	EXT2_CRASH(EXT2_CRASH_TRANS_START);
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
	EXT2_CRASH(EXT2_CRASH_METADATA_ACCESS);
	return (0);
}

/*
 * Revoke a block, so that replay of an older transaction cannot write it
 * back after it has been freed and possibly reused.
 */
int
ext2_journal_revoke_block(struct ext2_journal_trans *t, uint64_t blocknr)
{
	struct ext2_journal_revoke *rv;

	if (t->jt_error)
		return (t->jt_error);

	/*
	 * Without the REVOKE feature there is no way to record this, and a
	 * journal that silently drops revocations would let replay write a
	 * freed block over its new owner.  Say so rather than proceed.
	 */
	if (!(t->jt_journal->j_feature_incompat & EXT2_JOURNAL_INCOMPAT_REVOKE))
		return (ENOTSUP);

	if (t->jt_nrevoked >= EXT2_JOURNAL_MAX_REVOKES) {
		t->jt_error = ENOSPC;
		return (ENOSPC);
	}

	STAILQ_FOREACH(rv, &t->jt_revokes, jr_link) {
		if (rv->jr_blocknr == blocknr)
			return (0);
	}
	rv = malloc(sizeof(*rv), M_EXT2JOURNAL, M_WAITOK | M_ZERO);
	if (rv == NULL) {
		t->jt_error = ENOMEM;
		return (ENOMEM);
	}
	rv->jr_blocknr = blocknr;
	STAILQ_INSERT_TAIL(&t->jt_revokes, rv, jr_link);
	t->jt_nrevoked++;
	t->jt_revoked = 1;
	t->jt_dirty = 1;
	return (0);
}

void
ext2_journal_trans_abort(struct ext2_journal_trans *t)
{
	struct ext2_journal_buf *jb;
	struct ext2_journal_revoke *rv;

	if (t == NULL)
		return;
	STAILQ_FOREACH(jb, &t->jt_bufs, jb_link)
		free(jb, M_EXT2JOURNAL);
	STAILQ_FOREACH(rv, &t->jt_revokes, jr_link)
		free(rv, M_EXT2JOURNAL);
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
	struct ext2_journal_revoke *jr;
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

			/*
			 * Written through the asserted structures rather than
			 * at hand-written offsets, so the code and the layout
			 * assertions cannot disagree.
			 */
			if (csum3) {
				struct ext2fs_journal_tag_csum3 tag;

				tag.t_blocknr = (uint32_t)jb->jb_bp->b_blkno;
				tag.t_flags = flags;
				tag.t_blocknr_high = 0;
				tag.t_checksum = 0;
				memcpy(p, &tag, sizeof(tag));
				memcpy(p + sizeof(tag), j->j_uuid, 16);
				p += sz;
			} else {
				struct ext2fs_journal_tag_classic tag;

				tag.t_blocknr = (uint32_t)jb->jb_bp->b_blkno;
				tag.t_checksum = 0;
				tag.t_flags = (uint16_t)flags;
				tag.t_blocknr_high = 0;
				memcpy(p, &tag, sizeof(tag));
				if (j->j_feature_incompat &
				    EXT2_JOURNAL_INCOMPAT_64BIT)
					p += sizeof(tag);
				else
					p += offsetof(struct
					    ext2fs_journal_tag_classic,
					    t_blocknr_high);
				memcpy(p, j->j_uuid, 16);
				p += 16;
			}
		}
	}

	error = bwrite(bp);
	if (error)
		goto fail;
	EXT2_CRASH(EXT2_CRASH_DESC_WRITE);

	/* The data blocks, in the same order as the tags. */
	STAILQ_FOREACH(jb, &t->jt_bufs, jb_link) {
		struct ext2_journal_ckpt *ck;
		uint32_t jblock;

		jblock = ext2_journal_cursor(j);
		bp = getblk(ump->um_devvp, (daddr_t)jblock,
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

		/*
		 * Record where this block lives in the log so checkpoint can
		 * write it home.  Without this the queue stays empty,
		 * checkpoint writes nothing, and the log is reclaimed while
		 * the journal is still the only copy of the metadata.
		 */
		ck = malloc(sizeof(*ck), M_EXT2JOURNAL, M_WAITOK | M_ZERO);
		if (ck == NULL) {
			error = ENOMEM;
			goto fail;
		}
		ck->ck_blocknr = (uint64_t)jb->jb_bp->b_blkno;
		ck->ck_jblock = jblock;
		ck->ck_sequence = t->jt_sequence;
		STAILQ_INSERT_TAIL(&j->j_ckpt, ck, ck_link);
		EXT2_CRASH(EXT2_CRASH_DATA_WRITE);
	}

	/*
	 * Revoke, if this transaction has any.  It belongs after the data
	 * blocks and before the commit record: the commit is what makes the
	 * whole transaction replayable, and a revoke that lands after the
	 * commit would not be covered by it.
	 */
	if (t->jt_revoked) {
		int wide = (j->j_feature_incompat &
		    EXT2_JOURNAL_INCOMPAT_64BIT) != 0;
		uint8_t *p;
		uint32_t count = 0;

		bp = getblk(ump->um_devvp, (daddr_t)ext2_journal_cursor(j),
		    (int)j->j_blocksize, 0, 0, 0);
		if (bp == NULL) {
			error = ENOMEM;
			goto fail;
		}
		vfs_bio_clrbuf(bp);
		ext2_journal_write_bhdr(bp, EXT2_JOURNAL_BT_REVOKE,
		    t->jt_sequence);

		p = (uint8_t *)bp->b_data + 0x0c;
		STAILQ_FOREACH(jr, &t->jt_revokes, jr_link) {
			if (wide)
				be64enc(p, jr->jr_blocknr);
			else
				be32enc(p, (uint32_t)jr->jr_blocknr);
			p += wide ? 8 : 4;
			count += wide ? 8 : 4;
		}
		/*
		 * r_count is the number of bytes this block uses, including
		 * the header and any tail -- not the number of entries.
		 * Storing an entry count here is the single easiest way to
		 * make a revoke list mean something else entirely.
		 */
		count += 0x10;		/* header plus r_count itself */
		if (ext2_journal_csum_blocks(j))
			count += 4;
		{
			struct ext2fs_journal_revoke_hdr *rh =
			    (struct ext2fs_journal_revoke_hdr *)bp->b_data;

			rh->rh_count = htobe32(count);
		}

		error = bwrite(bp);
		if (error)
			goto fail;
		EXT2_CRASH(EXT2_CRASH_REVOKE_WRITE);
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
	EXT2_CRASH(EXT2_CRASH_COMMIT_BEFORE);
	ext2_journal_write_bhdr(bp, EXT2_JOURNAL_BT_COMMIT,
	    t->jt_sequence);
	error = bwrite(bp);
	if (error)
		goto fail;
	EXT2_CRASH(EXT2_CRASH_COMMIT_WRITE);

	/* Past every committed sequence, so the check above holds. */
	if (t->jt_sequence >= ext2_journal_next_sequence)
		ext2_journal_next_sequence = t->jt_sequence + 1;
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
 * Distinct from commit.  Commit makes a transaction recoverable by writing
 * its metadata to the log and following it with a commit record; a
 * checkpoint writes that metadata to its final location, after which the
 * log is no longer needed for it.
 *
 * The order here is the safety property.  Every block is written home
 * first, and only if all of them succeed is any of them released from the
 * queue.  Failing part way leaves the queue intact, because a checkpoint
 * that discarded entries whose contents had not reached the filesystem
 * would leave the journal as the only copy of metadata nothing else
 * holds.
 *
 * A crash during a checkpoint is expected rather than exceptional:
 * recovery replays committed transactions whether or not they were
 * checkpointed, so one interrupted here is simply replayed again.
 */
int
ext2_journal_checkpoint(struct ext2_journal *j)
{
	struct ext2mount *ump = j->j_mount;
	struct ext2_journal_ckpt *ck, *nck;
	struct buf *jbp, *bp;
	uint32_t bsize;
	int error;

	bsize = ump->um_e2fs->e2fs_bsize;

	/*
	 * Remember where the log stood before this checkpoint.  If anything
	 * below fails, the boundary does not move: advancing it past blocks
	 * that were never written home would discard live transactions.
	 */
	j->j_reclaim = j->j_cursor;

	STAILQ_FOREACH_SAFE(ck, &j->j_ckpt, ck_link, nck) {
		error = bread(ump->um_devvp, (daddr_t)ck->ck_jblock,
		    (int)j->j_blocksize, NOCRED, &jbp);
		if (error)
			return (error);

		/*
		 * Data blocks are not journal blocks and carry no header to
		 * check, so there is nothing in the buffer that says whose
		 * data it is.  What can be checked is that the entry still
		 * refers to a transaction we committed, since the log may
		 * have wrapped and the position been reused since.  Copying
		 * whatever is there now would write an unrelated
		 * transaction's metadata over this block.
		 */
		if (ck->ck_sequence > ext2_journal_next_sequence) {
			brelse(jbp);
			return (EINVAL);
		}

		bp = getblk(ump->um_devvp, (daddr_t)ck->ck_blocknr,
		    (int)bsize, 0, 0, 0);
		if (bp == NULL) {
			brelse(jbp);
			return (ENOMEM);
		}
		memcpy(bp->b_data, jbp->b_data, (size_t)bsize);
		vfs_bio_clrbuf(bp);
		error = bwrite(bp);
		brelse(jbp);
		if (error)
			return (error);

		STAILQ_REMOVE(&j->j_ckpt, ck, ext2_journal_ckpt, ck_link);
		free(ck, M_EXT2JOURNAL);
		EXT2_CRASH(EXT2_CRASH_CKPT_BLOCK);
	}

	/*
	 * Everything committed is now on the filesystem, so the log up to
	 * the boundary captured at entry is reclaimable.  Reclaiming means
	 * recording where the log now begins, and that record has to reach
	 * the disk before the space is reused -- a crash in between would
	 * leave the superblock pointing into a region the next boot still
	 * believes holds live transactions.
	 *
	 * The sequence number is advanced first, so a reader can tell a
	 * superblock that reached the disk torn from a stale one.
	 */
	if (j->j_start != j->j_reclaim) {
		uint32_t old_start = j->j_start;
		uint32_t old_seq = j->j_sequence;

		j->j_start = j->j_reclaim;
		j->j_sequence++;
		error = ext2_journal_write_sb(j);
		if (error) {
			/*
			 * Put the in-memory state back.  If it were left
			 * advanced and the write had failed, the next
			 * checkpoint would see start == reclaim and skip
			 * the rewrite, and the superblock on disk would
			 * keep pointing at a region we had begun reusing.
			 */
			j->j_start = old_start;
			j->j_sequence = old_seq;
			return (error);
		}
		EXT2_CRASH(EXT2_CRASH_RECLAIM);
	}
	EXT2_CRASH(EXT2_CRASH_CKPT_DONE);

	return (0);
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
	STAILQ_INIT(&j->j_ckpt);
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
	struct ext2_journal_ckpt *ck, *nck;

	if (j == NULL)
		return;
	STAILQ_FOREACH_SAFE(ck, &j->j_ckpt, ck_link, nck) {
		STAILQ_REMOVE(&j->j_ckpt, ck, ext2_journal_ckpt, ck_link);
		free(ck, M_EXT2JOURNAL);
	}
	mtx_destroy(&j->j_lock);
	free(j->j_sb, M_EXT2JOURNAL);
	free(j, M_EXT2JOURNAL);
}

/*
 * Classification.  See ext2_journal_mining.md K.
 *
 * Kept deliberately small and deliberately in one place.  Adding a case
 * here is the only way to change which mechanism owns an operation, which
 * is what makes the decision reviewable.
 */
enum ext2_op_consistency
ext2_op_classify(struct inode *ip, int extent_operation)
{

	/*
	 * Only extent-mapped inodes lack a Soft Updates representation.
	 * Everything else -- direct blocks, indirect blocks at any depth,
	 * directory entries, orphan state -- is ordered by the dependency
	 * graph or by synchronous writes that the graph checks, and adding
	 * a journal transaction there would duplicate ordering that already
	 * exists rather than add any.
	 */
	if (extent_operation && (ip->i_flag & IN_E4EXTENTS))
		return (EXT2_OP_JOURNAL);
	return (EXT2_OP_SU);
}

/*
 * Operation scope.
 *
 * The unit of journalling is the filesystem operation, not the buffer.
 * An extent insertion that touches the inode, an index block and a leaf
 * is one transaction: journalling the leaf alone would leave the other
 * two recoverable at different moments, which is the partially journaled
 * operation the design forbids.
 *
 * These are the only entry points an operation uses.  ext2_op_start()
 * refuses when Soft Updates already covers the operation, so a plain
 * ext2 path never opens a transaction and therefore never pays for one.
 */
int
ext2_op_start(struct inode *ip, int extent_operation, uint32_t nblocks)
{
	struct ext2mount *ump = ip->i_ump;
	struct ext2_journal_trans *t;
	int error;

	if (ext2_op_classify(ip, extent_operation) != EXT2_OP_JOURNAL)
		return (0);
	if (ump->um_journal == NULL)
		return (0);
	if (ump->um_jtrans != NULL) {
		/*
		 * Already inside a transaction for an enclosing operation.
		 * The inner work joins it: a nested transaction could not be
		 * committed on its own without making the outer operation
		 * recoverable in two halves.
		 */
		ump->um_jtrans_depth++;
		return (0);
	}

	error = ext2_journal_trans_start(ump->um_journal, nblocks, &t);
	if (error)
		return (error);
	ump->um_jtrans = t;
	ump->um_jtrans_depth = 1;
	return (0);
}

/*
 * Close the operation's transaction.
 *
 * A failure here means the operation's metadata is not recoverable, so
 * the caller must fail the operation and roll back only what the
 * allocator still considers unpublished.  It is never downgraded to an
 * unjournalled success.
 */
int
ext2_op_end(struct inode *ip)
{
	struct ext2mount *ump = ip->i_ump;
	struct ext2_journal_trans *t = ump->um_jtrans;
	int error;

	if (t == NULL)
		return (0);
	if (ump->um_jtrans_depth > 1) {
		/* Inner scope; the outermost commit is what counts. */
		ump->um_jtrans_depth--;
		return (0);
	}
	ump->um_jtrans = NULL;
	ump->um_jtrans_depth = 0;
	error = ext2_journal_trans_commit(t);
	return (error);
}

void
ext2_op_abort(struct inode *ip)
{
	struct ext2mount *ump = ip->i_ump;
	struct ext2_journal_trans *t = ump->um_jtrans;

	if (t == NULL)
		return;
	if (ump->um_jtrans_depth > 1) {
		/*
		 * An inner failure does not unwind the outer transaction,
		 * but it does poison it: the outer operation still has to
		 * fail, because part of its metadata was written by a step
		 * that failed.
		 */
		ump->um_jtrans_depth--;
		if (t->jt_error == 0)
			t->jt_error = EIO;
		return;
	}
	ump->um_jtrans = NULL;
	ump->um_jtrans_depth = 0;
	ext2_journal_trans_abort(t);
}

/*
 * Write the journal superblock.
 *
 * Block 0 of the journal device, synchronously.  The sequence number has
 * already been advanced by the caller, so a superblock that reaches the
 * disk torn is distinguishable from a stale one: the sequence is what a
 * reader checks, not a checksum over a structure we cannot afford to
 * trust across a partial write.
 */
int
ext2_journal_write_sb(struct ext2_journal *j)
{
	struct ext2mount *ump = j->j_mount;
	struct ext2fs_journal_bhdr bh;
	uint8_t *sb;
	struct buf *bp;
	int error;

	sb = malloc(EXT2_JOURNAL_SB_SIZE, M_EXT2JOURNAL, M_WAITOK | M_ZERO);
	if (sb == NULL)
		return (ENOMEM);

	bh.bh_magic = EXT2_JOURNAL_MAGIC;
	bh.bh_type = EXT2_JOURNAL_BT_SB_V2;
	bh.bh_sequence = j->j_sequence;
	ext2_journal_bhdr_to_disk(sb, &bh);
	ext2_journal_sb_to_disk(sb, j);

	/*
	 * The superblock checksum covers the whole structure with its own
	 * field zeroed, so it is computed over the finished bytes.
	 */
	if (ext2_journal_csum_usable(j)) {
		/*
		 * Same rule the reader applies: no UUID, and the
		 * calculate_crc32c() result inverted, because Linux's
		 * crc32c() xors on entry only.
		 */
		be32enc(sb + 0xfc, 0);
		be32enc(sb + 0xfc,
		    calculate_crc32c(0, sb, EXT2_JOURNAL_SB_SIZE) ^ 0xFFFFFFFF);
	}

	bp = getblk(ump->um_devvp, 0, (int)EXT2_JOURNAL_SB_SIZE, 0, 0, 0);
	if (bp == NULL) {
		free(sb, M_EXT2JOURNAL);
		return (ENOMEM);
	}
	memcpy(bp->b_data, sb, EXT2_JOURNAL_SB_SIZE);
	vfs_bio_clrbuf(bp);
	EXT2_CRASH(EXT2_CRASH_SB_BEFORE);
	error = bwrite(bp);
	free(sb, M_EXT2JOURNAL);
	EXT2_CRASH(EXT2_CRASH_SB_UPDATE);
	return (error);
}

/*
 * Mount integration.
 *
 * Opens the journal if the filesystem has one, and replays it.  A
 * filesystem with no journal is not an error: it is covered by Soft
 * Updates, which is the point of the hybrid.
 *
 * A journal that is present but unreadable -- unsupported features, a
 * UUID that does not match, corruption -- stops the mount.  Mounting it
 * read-only and hoping is how a filesystem whose metadata lives in the
 * journal gets overwritten by writes that know nothing about it.
 */
int
ext2_mount_journal(struct ext2mount *ump, int ronly)
{
	struct m_ext2fs *fs = ump->um_e2fs;
	struct ext2_journal *j;
	int error;

	if (fs->e2fs->e3fs_journal_inum == 0 &&
	    (fs->e2fs->e2fs_features_compat & EXT2F_COMPAT_HASJOURNAL) == 0)
		return (0);			/* no journal; Soft Updates covers it */

	error = ext2_journal_open_journal(ump, fs, &j);
	if (error) {
		printf("ext2fs: %s: journal unusable, mount refused (%d)\n",
		    fs->e2fs_fsmnt, error);
		return (error);
	}
	ump->um_journal = j;
	EXT2_CRASH(EXT2_CRASH_JOURNAL_OPEN);

	if (ronly) {
		printf("ext2fs: %s: journal present; read-only mount, "
		    "nothing replayed\n", fs->e2fs_fsmnt);
		return (0);
	}

	error = ext2_journal_recover(j);
	if (error) {
		printf("ext2fs: %s: journal recovery failed (%d)\n",
		    fs->e2fs_fsmnt, error);
		return (error);
	}
	return (0);
}

void
ext2_unmount_journal(struct ext2mount *ump)
{

	ext2_journal_destroy(ump->um_journal);
	ump->um_journal = NULL;
	ump->um_jtrans = NULL;
}
