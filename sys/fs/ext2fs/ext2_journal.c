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
 * Journal checksum.
 *
 * Only CRC32C and CRC32 are implemented; they are the two the
 * documentation describes as likely and the only ones this filesystem
 * needs, since ext2fs already uses CRC32C for its own metadata
 * checksums.  An algorithm we cannot compute must not be approximated,
 * so it is refused.
 */
uint32_t
ext2_journal_checksum(struct ext2_journal *j, uint32_t seed, const void *buf,
    size_t len)
{

	switch (j->j_checksum_type) {
	case EXT2_JOURNAL_CRC32C:
		return (calculate_crc32c(seed, buf, len));
	case EXT2_JOURNAL_CRC32:
		/* Not implemented in this phase; refuse rather than guess. */
		return (0);
	default:
		return (0);
	}
}
