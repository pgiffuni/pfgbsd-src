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
 * ext2 metadata journal: on-disk format and FreeBSD-native interfaces.
 *
 * We adopt the JBD2 *format*, not the JBD2 *layer*.  Taken from JBD2 are
 * the block structures, the feature flags, the checksum types and the
 * big-endian encoding of all of it.  Not taken are handle_t, journal_t,
 * transaction_t and the long-lived journal object that filesystem
 * operations join: our unit of work is the filesystem operation, which
 * begins and ends with it.
 *
 * ON-DISK FORMAT
 *   documented by
 *   https://docs.kernel.org/filesystems/ext4/journal.html
 *
 *   Every JBD2 field is big-endian, which is the opposite of the ext4
 *   metadata around it.  The ext2fs_journal_* structures below exist so
 *   that the _Static_asserts can check every field offset against the
 *   documented layout.  A journal block is NEVER cast onto one of them.
 *   All access goes through the explicit conversions in
 *   ext2_inode_cnv.c, which use be32toh() and htobe32().
 *
 * No Linux kernel structure is reused.
 */

#ifndef _FS_EXT2FS_EXT2_JOURNAL_H_
#define	_FS_EXT2FS_EXT2_JOURNAL_H_

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/buf.h>
#include <sys/mutex.h>
#include <sys/mount.h>
#include <sys/vnode.h>

#include <sys/_offsetof.h>

#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_mount.h>

/* Journal block magic, documented at journal.rst 3.6.3. */
#define	EXT2_JOURNAL_MAGIC		0xc03b3998

/* Journal block types, journal.rst 3.6.3. */
#define	EXT2_JOURNAL_BT_DESCRIPTOR	1
#define	EXT2_JOURNAL_BT_COMMIT		2
#define	EXT2_JOURNAL_BT_SB_V1		3
#define	EXT2_JOURNAL_BT_SB_V2		4
#define	EXT2_JOURNAL_BT_REVOKE		5

/* Compatible feature flags, journal.rst 3.6.4. */
#define	EXT2_JOURNAL_COMPAT_CHECKSUM	0x00000001

/* Incompatible feature flags, journal.rst 3.6.4. */
#define	EXT2_JOURNAL_INCOMPAT_REVOKE	0x00000001
#define	EXT2_JOURNAL_INCOMPAT_64BIT	0x00000002
#define	EXT2_JOURNAL_INCOMPAT_ASYNC_COMMIT 0x00000004
#define	EXT2_JOURNAL_INCOMPAT_CSUM_V2	0x00000008
#define	EXT2_JOURNAL_INCOMPAT_CSUM_V3	0x00000010
#define	EXT2_JOURNAL_INCOMPAT_FAST_COMMIT 0x00000020

/* Checksum type codes, journal.rst 3.6.4. */
#define	EXT2_JOURNAL_CRC32		1
#define	EXT2_JOURNAL_MD5		2
#define	EXT2_JOURNAL_SHA1		3
#define	EXT2_JOURNAL_CRC32C		4

/* Descriptor tag flags, journal.rst 3.6.5. */
#define	EXT2_JOURNAL_TAG_ESCAPED	0x0001
#define	EXT2_JOURNAL_TAG_SAME_UUID	0x0002
#define	EXT2_JOURNAL_TAG_DELETED	0x0004
#define	EXT2_JOURNAL_TAG_LAST		0x0008

#define	EXT2_JOURNAL_TAGF_ALL		0x000f

/*
 * Documented commit-block checksum space, journal.rst 3.6.8.  The
 * documentation's prose calls the commit header "32 bytes" while its own
 * field table runs to 0x3c; the table is authoritative and is what the
 * assertions below check.
 */
#define	EXT2_JOURNAL_CHECKSUM_BYTES	32
#define	EXT2_JOURNAL_COMMIT_SIZE	0x3c	/* documented extent */

/*
 * On-disk structures.  These are assertion vehicles, not memory layouts:
 * nothing is ever cast onto them.  Every offset is checked against the
 * documented format so that a transcription error cannot survive, which
 * matters because this code cannot be tested against real journal data
 * during development.
 */

struct ext2fs_journal_bhdr {
	uint32_t	bh_magic;		/* 0x00 */
	uint32_t	bh_type;		/* 0x04 */
	uint32_t	bh_sequence;		/* 0x08 */
};

struct ext2fs_journal_sb {
	struct ext2fs_journal_bhdr sb_header;		/* 0x000 */
	uint32_t	sb_blocksize;		/* 0x00c */
	uint32_t	sb_maxlen;		/* 0x010 */
	uint32_t	sb_first;		/* 0x014 */
	uint32_t	sb_sequence;		/* 0x018 */
	uint32_t	sb_start;		/* 0x01c */
	uint32_t	sb_errno;		/* 0x020 */
	uint32_t	sb_feature_compat;	/* 0x024 */
	uint32_t	sb_feature_incompat;	/* 0x028 */
	uint32_t	sb_feature_ro_compat;	/* 0x02c */
	uint8_t		sb_uuid[16];		/* 0x030 */
	uint32_t	sb_nr_users;		/* 0x040 */
	uint32_t	sb_dynsuper;		/* 0x044 */
	uint32_t	sb_max_transaction;	/* 0x048 */
	uint32_t	sb_max_trans_data;	/* 0x04c */
	uint8_t		sb_checksum_type;	/* 0x050 */
	uint8_t		sb_padding2[3];		/* 0x051 */
	uint32_t	sb_num_fc_blocks;	/* 0x054 */
	uint32_t	sb_head;		/* 0x058 */
	uint8_t		sb_padding[160];	/* 0x05c */
	uint32_t	sb_checksum;		/* 0x0fc */
	uint8_t		sb_users[16 * 48];	/* 0x100 */
};

/*
 * The two documented descriptor tag encodings.  Which one applies is a
 * function of the journal's feature flags, and the size of a tag within
 * an encoding is a function of two more, so tag size is always computed
 * and never assumed.
 */
struct ext2fs_journal_tag_classic {
	uint32_t	t_blocknr;		/* 0x00 */
	uint16_t	t_checksum;		/* 0x04, low 16 bits only */
	uint16_t	t_flags;		/* 0x06 */
	uint32_t	t_blocknr_high;	/* 0x08, present only with 64BIT */
};

struct ext2fs_journal_tag_csum3 {
	uint32_t	t_blocknr;		/* 0x00 */
	uint32_t	t_flags;		/* 0x04 */
	uint32_t	t_blocknr_high;	/* 0x08, zero without 64BIT */
	uint32_t	t_checksum;		/* 0x0c */
};

struct ext2fs_journal_desc_tail {
	uint32_t	dt_checksum;		/* 0x00 */
};

struct ext2fs_journal_revoke_hdr {
	struct ext2fs_journal_bhdr rh_header;	/* 0x00 */
	uint32_t	rh_count;		/* 0x0c, a BYTE count */
};

struct ext2fs_journal_revoke_tail {
	uint32_t	rt_checksum;		/* 0x00 */
};

struct ext2fs_journal_commit {
	struct ext2fs_journal_bhdr jc_header;	/* 0x00 */
	uint8_t		jc_chksum_type;		/* 0x0c */
	uint8_t		jc_chksum_size;		/* 0x0d */
	uint8_t		jc_padding[2];		/* 0x0e */
	uint8_t		jc_chksum[EXT2_JOURNAL_CHECKSUM_BYTES]; /* 0x10 */
	uint64_t	jc_commit_sec;		/* 0x30 */
	uint32_t	jc_commit_nsec;		/* 0x38 */
};

struct ext2fs_fc_tl {
	uint16_t	fc_tag;
	uint16_t	fc_len;		/* the entire field, header included */
};

/*
 * Layout assertions.
 *
 * The conversions read `struct.field`, so the on-disk offsets now depend
 * on this ABI's idea of struct layout as well as on the format.  That is
 * a portability liability, not just a tidiness one: a field whose natural
 * alignment would introduce padding on some target -- the 64-bit commit
 * timestamp is the obvious candidate -- would shift every field after it,
 * and the parser would then read the wrong bytes without complaint.
 *
 * Asserting both the size and every field offset turns that into a build
 * failure on any platform where the layout disagrees, which is the only
 * place it can be caught: the code here has never read a journal.
 *
 * be32toh()/htobe32() are the host-aware conversions, so they are correct
 * on a big-endian target as well as a little-endian one.  Note that this
 * is a separate question from be32dec(), which assembles bytes explicitly
 * and so is host-independent regardless.
 *
 * Every offset below is checked against the documented format.  This is
 * the only verification available for this code: FreeBSD carries no
 * e2fsprogs, so no journal data can be produced here to test a parser
 * against.  An assertion is cheap; getting an offset wrong is not
 * recoverable by inspection later, because a misread field yields a
 * plausible parse rather than an obvious failure.
 */
#define	EXT2_J_ASSERT_OFF(type, field, off)				\
	_Static_assert(offsetof(type, field) == (off),			\
	    #type "." #field " must be at " #off " per the ext4 journal documentation")

EXT2_J_ASSERT_OFF(struct ext2fs_journal_bhdr, bh_magic, 0x00);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_bhdr, bh_type, 0x04);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_bhdr, bh_sequence, 0x08);
_Static_assert(sizeof(struct ext2fs_journal_bhdr) == 12,
    "the journal block header is 12 bytes");

EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_header, 0x000);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_blocksize, 0x00c);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_maxlen, 0x010);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_first, 0x014);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_sequence, 0x018);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_start, 0x01c);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_errno, 0x020);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_feature_compat, 0x024);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_feature_incompat, 0x028);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_feature_ro_compat, 0x02c);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_uuid, 0x030);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_nr_users, 0x040);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_dynsuper, 0x044);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_max_transaction, 0x048);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_max_trans_data, 0x04c);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_checksum_type, 0x050);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_padding2, 0x051);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_num_fc_blocks, 0x054);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_head, 0x058);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_padding, 0x05c);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_checksum, 0x0fc);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_sb, sb_users, 0x100);
_Static_assert(sizeof(struct ext2fs_journal_sb) == 1024,
    "the journal superblock is 1024 bytes; a shorter struct silently "
    "truncates it");

EXT2_J_ASSERT_OFF(struct ext2fs_journal_tag_classic, t_blocknr, 0x00);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_tag_classic, t_checksum, 0x04);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_tag_classic, t_flags, 0x06);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_tag_classic, t_blocknr_high, 0x08);

EXT2_J_ASSERT_OFF(struct ext2fs_journal_tag_csum3, t_blocknr, 0x00);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_tag_csum3, t_flags, 0x04);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_tag_csum3, t_blocknr_high, 0x08);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_tag_csum3, t_checksum, 0x0c);
_Static_assert(sizeof(struct ext2fs_journal_tag_csum3) == 16,
    "the checksum-v3 tag is a fixed 16 bytes before the optional uuid");

EXT2_J_ASSERT_OFF(struct ext2fs_journal_desc_tail, dt_checksum, 0x00);

EXT2_J_ASSERT_OFF(struct ext2fs_journal_revoke_hdr, rh_header, 0x00);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_revoke_hdr, rh_count, 0x0c);

EXT2_J_ASSERT_OFF(struct ext2fs_journal_revoke_tail, rt_checksum, 0x00);

/*
 * Fast commit tag/length header.  The widths are derived rather than
 * documented; see the comment above and ext2_journal_mining.md F.2.
 */
EXT2_J_ASSERT_OFF(struct ext2fs_fc_tl, fc_tag, 0x00);
EXT2_J_ASSERT_OFF(struct ext2fs_fc_tl, fc_len, 0x02);
_Static_assert(sizeof(struct ext2fs_fc_tl) == 4,
    "the fast commit tag header is two 16-bit fields");

EXT2_J_ASSERT_OFF(struct ext2fs_journal_commit, jc_header, 0x00);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_commit, jc_chksum_type, 0x0c);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_commit, jc_chksum_size, 0x0d);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_commit, jc_padding, 0x0e);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_commit, jc_chksum, 0x10);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_commit, jc_commit_sec, 0x30);
EXT2_J_ASSERT_OFF(struct ext2fs_journal_commit, jc_commit_nsec, 0x38);

_Static_assert(sizeof(struct ext2fs_journal_tag_classic) == 12,
    "the classic tag prefix is 8 bytes, plus blocknr_high when 64-bit");
_Static_assert(sizeof(struct ext2fs_journal_desc_tail) == 4,
    "the descriptor tail is a single 32-bit checksum");
_Static_assert(sizeof(struct ext2fs_journal_revoke_hdr) == 16,
    "the revoke header is the block header plus a byte count");
_Static_assert(sizeof(struct ext2fs_journal_revoke_tail) == 4,
    "the revoke tail is a single 32-bit checksum");
/*
 * The commit header occupies 0x3c bytes.  The structure may be larger
 * than that -- a trailing uint32_t pads out to the struct's eight-byte
 * alignment -- so the extent is asserted as a lower bound and the
 * serialisation copies exactly the documented bytes rather than
 * sizeof(), which would write four bytes past them.
 */
_Static_assert(sizeof(struct ext2fs_journal_commit) >= 0x3c,
    "the commit header must at least cover its documented 0x3c bytes");

/*
 * Fast commit framing.
 *
 * Documented by journal.rst 3.6.9, which specifies that the area is a log
 * of tag-length-values, that each record begins with a tag and the length
 * of the entire field, and that the tag set is HEAD, ADD_RANGE,
 * DEL_RANGE, CREAT, LINK, UNLINK, PAD and TAIL.
 *
 * The tag and length widths are NOT documented.  Two 16-bit fields is the
 * only encoding consistent with eight tags and a length bounded by a
 * journal block, and it is used here on that basis -- derived, not
 * documented, and recorded as such in ext2_journal_mining.md F.2.
 *
 * What follows the tag is not specified at all.  None of the five value
 * structures is implemented, and a record carrying one is refused rather
 * than skipped: a length is known, so skipping is safe, but interpreting
 * one is not, and this filesystem does not guess at a format it cannot
 * read.
 */
#define	EXT2_FC_TAG_HEAD		1
#define	EXT2_FC_TAG_ADD_RANGE	2
#define	EXT2_FC_TAG_DEL_RANGE	3
#define	EXT2_FC_TAG_CREAT	4
#define	EXT2_FC_TAG_LINK		5
#define	EXT2_FC_TAG_UNLINK	6
#define	EXT2_FC_TAG_PAD		7
#define	EXT2_FC_TAG_TAIL		8
#define	EXT2_FC_TAG_MIN		EXT2_FC_TAG_HEAD
#define	EXT2_FC_TAG_MAX		EXT2_FC_TAG_TAIL

/* What a walk of a fast commit area found. */
struct ext2_fc_scan {
	uint32_t	fcs_records;
	uint32_t	fcs_bytes;	/* consumed, including the last field */
	int		fcs_tail;	/* a TAIL terminated the area */
	int		fcs_pad;	/* trailing PAD seen */
};

int	ext2_fc_scan(const void *, size_t, struct ext2_fc_scan *);
int	ext2_fc_eligible(uint32_t num_fc_blocks, uint32_t live,
	    uint32_t need, int expressible);

/*
 * Runtime state.  FreeBSD-native, and deliberately not a transliteration
 * of the JBD2 layer: there is no long-lived handle that operations join,
 * because the unit of work is the operation.
 */
struct ext2_journal {
	struct ext2mount	*j_mount;
	struct m_ext2fs		*j_fs;
	uint8_t			*j_sb;		/* raw 1024-byte superblock */
	uint32_t		 j_blocksize;
	uint32_t		 j_maxlen;
	uint32_t		 j_first;
	/*
	 * First block of the log that is still needed: everything before it
	 * has been checkpointed to its home location and can be reclaimed.
	 * This is what s_start records on disk.
	 */
	uint32_t		 j_start;
	uint32_t		 j_sequence;	/* s_sequence, for superblock
						 * validity */
	uint32_t		 j_reclaim;	/* below here is reclaimable */
	uint32_t		 j_nr_users;
	/*
	 * Where the journal superblock actually lives.  For an internal
	 * journal that is the first block of the journal inode's data,
	 * which is nowhere near block 0 and moves with the filesystem's
	 * own allocation.
	 */
	e4fs_daddr_t		 j_sbblock;
	uint8_t			 j_uuid[16];
	uint32_t		 j_feature_compat;
	uint32_t		 j_feature_incompat;
	uint32_t		 j_feature_ro_compat;
	uint8_t			 j_checksum_type;
	struct mtx		 j_lock;
	int			 j_readonly;
	uint32_t		 j_cursor;	/* next log block to write */
	int			 j_writable;
	/*
	 * Blocks that are journalled and committed but not yet written to
	 * their home locations.  While this list is non-empty the journal
	 * holds the only copy of that metadata, which is the whole
	 * difference between a commit and a checkpoint.
	 */
	STAILQ_HEAD(, ext2_journal_ckpt) j_ckpt;
};

struct ext2_journal_ckpt {
	uint64_t	ck_blocknr;	/* where it belongs */
	uint32_t	ck_jblock;	/* journal block holding it now */
	uint32_t	ck_sequence;
	STAILQ_ENTRY(ext2_journal_ckpt) ck_link;
};

/* Deserialised, host-endian view of one descriptor tag. */
struct ext2_journal_tag {
	uint64_t	jt_blocknr;
	uint32_t	jt_flags;
	uint32_t	jt_checksum;
	uint32_t	jt_sequence;
	uint8_t		jt_seq_be[4];	/* sequence as it appears on disk */
	uint8_t		jt_uuid[16];
	int		jt_has_uuid;
};

/*
 * On-disk conversions.  Implemented in ext2_inode_cnv.c, with the rest
 * of this filesystem's on-disk-to-memory conversions.  These are the
 * only way to reach a journal field: a journal block is never cast onto
 * one of the structures above.
 */
void	ext2_journal_bhdr_from_disk(const void *, struct ext2fs_journal_bhdr *);
void	ext2_journal_bhdr_to_disk(void *, const struct ext2fs_journal_bhdr *);
void	ext2_journal_sb_from_disk(const void *, struct ext2_journal *);
void	ext2_journal_sb_to_disk(void *, const struct ext2_journal *);

/*
 * Recovery-time scan state.  Built by the scanner, consumed by replay.
 *
 * A revoke is global to the whole replay: the documentation is explicit
 * that adding a block to a transaction removes any existing revocation
 * for it, so the table is accumulated across every committed transaction
 * and consulted for all of them.
 */
struct ext2_journal_revoked {
	uint32_t	jr_blocklo;
	uint32_t	jr_blockhi;
	uint32_t	jr_sequence;	/* sequence that last revoked it */
	STAILQ_ENTRY(ext2_journal_revoked) jr_link;
};

/* One descriptor tag plus the block it names, as found by the scanner. */
struct ext2_journal_filedesc {
	uint64_t	fd_blocknr;	/* where the data goes */
	uint32_t	fd_jblock;	/* journal block holding the data */
	uint32_t	fd_flags;
	uint32_t	fd_sequence;
	uint32_t	fd_checksum;
	uint8_t		fd_uuid[16];
	int		fd_has_uuid;
	STAILQ_ENTRY(ext2_journal_filedesc) fd_link;
};

/*
 * One transaction found by the scanner: the tags of every descriptor
 * block belonging to it.  A transaction is recoverable only when a commit
 * block with a matching sequence was seen.  Distinct from
 * ext2_journal_trans below, which is one in progress being written.
 */
struct ext2_journal_scan_trans {
	uint32_t		tr_sequence;
	STAILQ_HEAD(, ext2_journal_filedesc) tr_filedescs;
	int			tr_has_commit;
	int			tr_committed;
	int			tr_replayed;	/* recovery bookkeeping */
	STAILQ_ENTRY(ext2_journal_scan_trans) tr_link;
};

struct ext2_journal_scan {
	struct ext2_journal	*sc_journal;
	uint32_t		 sc_start;	/* first block of the log */
	uint32_t		 sc_end;	/* one past the last used block */
	uint32_t		 sc_next_sequence;
	STAILQ_HEAD(, ext2_journal_scan_trans) sc_trans;
	STAILQ_HEAD(, ext2_journal_revoked) sc_revoked;
	int			 sc_error;	/* sticky; fail closed */
};

/* Format-level entry points.  Implemented in ext2_journal.c. */
int	ext2_journal_features_ok(uint32_t compat, uint32_t incompat,
	    uint32_t ro_compat, uint32_t *unsupported);
int	ext2_journal_block_is(const void *buf, size_t len, uint32_t type);
int	ext2_journal_tag_size(const struct ext2_journal *j,
	    uint32_t flags, int *size);
int	ext2_journal_desc_scan(struct ext2_journal *j, const void *buf,
	    size_t len, struct ext2_journal_tag **tagsp, int *ntags);
int	ext2_journal_revoke_scan(struct ext2_journal *j, const void *buf,
	    size_t len, uint64_t **blocks, int *nblocks);
#define	EXT2_JOURNAL_SB_SIZE	1024

int	ext2_journal_csum_usable(const struct ext2_journal *);
int	ext2_journal_csum_blocks(const struct ext2_journal *);
int	ext2_journal_csum(const struct ext2_journal *, const void *, size_t,
	    uint32_t *);
int	ext2_journal_sb_csum_verify(struct ext2_journal *, const void *, size_t);
int	ext2_journal_desc_csum_verify(struct ext2_journal *, const void *,
	    size_t);
int	ext2_journal_revoke_csum_verify(struct ext2_journal *, const void *,
	    size_t, size_t);
int	ext2_journal_tag_csum_verify(struct ext2_journal *,
	    const struct ext2_journal_tag *, const void *, size_t, int);
int	ext2_journal_commit_csum_verify(struct ext2_journal *, const void *,
	    size_t);

/*
 * A transaction in progress.  Created when a filesystem operation begins
 * and destroyed when it commits or aborts, so unlike a JBD2 handle it has
 * no lifetime beyond the operation and no slot for an operation to join.
 */
struct ext2_journal_trans {
	struct ext2_journal	*jt_journal;
	uint32_t		 jt_sequence;
	uint32_t		 jt_reserved;	/* journal blocks reserved */
	uint32_t		 jt_used;	/* journal blocks consumed */
	int			 jt_error;	/* sticky */
	int			 jt_dirty;	/* metadata seen */
	int			 jt_revoked;	/* revocations queued */
	uint32_t		 jt_nrevoked;	/* count, for the ceiling */
	STAILQ_HEAD(, ext2_journal_buf) jt_bufs;
	STAILQ_HEAD(, ext2_journal_revoke) jt_revokes;
};

/*
 * A block this transaction frees.  Replay of this or any earlier
 * transaction must not write it back, because by the time replay runs
 * the block may belong to something else entirely.
 */
struct ext2_journal_revoke {
	uint64_t	jr_blocknr;
	STAILQ_ENTRY(ext2_journal_revoke) jr_link;
};

struct ext2_journal_buf {
	struct buf		*jb_bp;
	STAILQ_ENTRY(ext2_journal_buf) jb_link;
};

/* Writer entry points.  All synchronous in this phase. */
int	ext2_journal_open_journal(struct ext2mount *, struct m_ext2fs *,
	    struct ext2_journal **);
void	ext2_journal_destroy(struct ext2_journal *);

int	ext2_journal_trans_start(struct ext2_journal *, uint32_t nblocks,
	    struct ext2_journal_trans **);
uint32_t ext2_journal_live(struct ext2_journal *);
uint32_t ext2_journal_footprint(struct ext2_journal_trans *);
int	ext2_journal_advance(struct ext2_journal *, uint32_t);
int	ext2_journal_dirty_metadata(struct ext2_journal_trans *,
	    struct buf *);
int	ext2_journal_revoke_block(struct ext2_journal_trans *, uint64_t);
int	ext2_journal_trans_commit(struct ext2_journal_trans *);
void	ext2_journal_trans_abort(struct ext2_journal_trans *);
int	ext2_journal_checkpoint(struct ext2_journal *);
int	ext2_journal_write_sb(struct ext2_journal *);
int	ext2_journal_checkpoint_if_needed(struct ext2mount *);

/*
 * Ceiling on revocations held by one transaction.
 *
 * Truncating a large file frees millions of blocks, and each needs a
 * revocation so that replay does not write it back over its new owner.
 * There is no bound on that set short of memory, so it is bounded here
 * and the transaction fails when it is reached.  Failing is the right
 * answer: carrying on would silently stop revoking, which is precisely
 * the failure revocation exists to prevent.
 */
#define	EXT2_JOURNAL_MAX_REVOKES	65536

/* Operation scope: the unit of journalling. */
int	ext2_mount_journal(struct ext2mount *, int ronly);
void	ext2_unmount_journal(struct ext2mount *);
int	ext2_op_start(struct inode *, int extent_operation, uint32_t nblocks);
int	ext2_op_end(struct inode *);
void	ext2_op_abort(struct inode *);

/*
 * Journal capacity.
 *
 * The log is the circular interval [j_first, j_maxlen).  Block 0 holds the
 * journal superblock and is never a transaction block, so the cursor wraps
 * to j_first and not to zero, and the usable capacity is the width of
 * that interval rather than the whole device:
 *
 *	capacity = j_maxlen - j_first
 *
 * Admission is one inequality, checked before anything is written:
 *
 *	live blocks + this transaction's footprint <= capacity
 *
 * where "live" is the committed-but-uncheckpointed run from j_start to
 * the cursor.  A transaction that does not fit is refused whole; there is
 * no partial write, because a descriptor without its data is worse than a
 * failed operation.
 *
 * This is deliberately not JBD2's credit and semaphore machinery.  Soft
 * Updates already removes most operations from the journal -- only
 * extent-mapped inodes reach a transaction -- so the journal has to handle
 * the largest journal-required operation rather than everything.  One
 * explicit inequality is enough for that, and it fails loudly.
 */
#define	EXT2_JOURNAL_CACITY(j)	((j)->j_maxlen - (j)->j_first)

/* Implemented in ext2_journal_recovery.c. */
int	ext2_journal_scan(struct ext2_journal *, struct ext2_journal_scan *);
void	ext2_journal_scan_free(struct ext2_journal_scan *);
int	ext2_journal_recover(struct ext2_journal *);
int	ext2_journal_revoked_p(struct ext2_journal_scan *, uint64_t,
	    uint32_t sequence);

#endif /* !_FS_EXT2FS_EXT2_JOURNAL_H_ */

/*
 * Operation classification.
 *
 * One function decides which consistency mechanism owns an operation.
 * Inferring it independently in several places is how two mechanisms end
 * up disagreeing about the same operation, and a disagreement here is
 * worse than either consistent answer: a partially journaled operation
 * writes some of its metadata to the log and some of it nowhere.
 *
 * The classification follows sys/fs/ext2fs/ext2_journal_mining.md K.
 */
enum ext2_op_consistency {
	EXT2_OP_SU = 0,		/* Soft Updates orders it; no journal needed */
	EXT2_OP_JOURNAL		/* needs the journal; commit mode undecided */
};

/*
 * Classify an operation on an inode.
 *
 * Soft Updates covers everything whose metadata is the inode's block
 * pointers plus at most one parent buffer per level, because that is what
 * the dependency graph can express.  It has no mechanism for a tree whose
 * mutation spans several buffers with no parent-child relationship the
 * graph knows about, which is every extent operation.
 */
enum ext2_op_consistency
ext2_op_classify(struct inode *ip, int extent_operation);

static inline int
ext2_op_needs_journal(struct inode *ip, int extent_operation)
{

	return (ext2_op_classify(ip, extent_operation) == EXT2_OP_JOURNAL);
}
