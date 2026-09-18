/*-
 *  modified for Lites 1.1
 *
 *  Aug 1995, Godmar Back (gback@cs.utah.edu)
 *  University of Utah, Department of Computer Science
 */
/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/vnode.h>
#include <sys/sdt.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/buf.h>
#include <sys/endian.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/ext2_softdep.h>

SDT_PROVIDER_DEFINE(ext2fs);
/*
 * ext2fs trace probe:
 * arg0: verbosity. Higher numbers give more verbose messages
 * arg1: Textual message
 */
SDT_PROBE_DEFINE2(ext2fs, , alloc, trace, "int", "char*");
SDT_PROBE_DEFINE3(ext2fs, , alloc, ext2_reallocblks_realloc,
    "ino_t", "e2fs_lbn_t", "e2fs_lbn_t");
SDT_PROBE_DEFINE1(ext2fs, , alloc, ext2_reallocblks_bap, "uint32_t");
SDT_PROBE_DEFINE1(ext2fs, , alloc, ext2_reallocblks_blkno, "e2fs_daddr_t");
SDT_PROBE_DEFINE2(ext2fs, , alloc, ext2_b_bitmap_validate_error, "char*", "int");
SDT_PROBE_DEFINE3(ext2fs, , alloc, ext2_nodealloccg_bmap_corrupted,
    "int", "daddr_t", "char*");
SDT_PROBE_DEFINE2(ext2fs, , alloc, ext2_blkfree_bad_block, "ino_t", "e4fs_daddr_t");
SDT_PROBE_DEFINE2(ext2fs, , alloc, ext2_vfree_doublefree, "char*", "ino_t");

static daddr_t	ext2_alloccg(struct inode *, int, daddr_t, int);
static daddr_t	ext2_clusteralloc(struct inode *, int, daddr_t, int);
static u_long	ext2_dirpref(struct inode *);
static e4fs_daddr_t ext2_hashalloc(struct inode *, int, long, int,
    daddr_t (*)(struct inode *, int, daddr_t, 
						int));
static daddr_t	ext2_nodealloccg(struct inode *, int, daddr_t, int);
static daddr_t  ext2_mapsearch(struct m_ext2fs *, char *, daddr_t);
static uint32_t	ext2_run_desired_length(struct inode *, uint32_t,
    enum ext2_alloc_class);

/*
 * Maximum physical allocation run length (tunable via sysctl).
 * Bounds the target length returned by ext2_run_desired_length()
 * so the physical allocator never requests an unreasonably long
 * contiguous run.  This is a cap, not a preallocation amount.
 */
static int	ext2_alloc_max_run = EXT2_MAXCONTIG;

/*
 * Allocate a block in the filesystem.
 *
 * A preference may be optionally specified. If a preference is given
 * the following hierarchy is used to allocate a block:
 *   1) allocate the requested block.
 *   2) allocate a rotationally optimal block in the same cylinder.
 *   3) allocate a block in the same cylinder group.
 *   4) quadratically rehash into other cylinder groups, until an
 *        available block is located.
 * If no block preference is given the following hierarchy is used
 * to allocate a block:
 *   1) allocate a block in the cylinder group that contains the
 *        inode for the file.
 *   2) quadratically rehash into other cylinder groups, until an
 *        available block is located.
 */
int
ext2_alloc(struct inode *ip, daddr_t lbn, e4fs_daddr_t bpref, int size,
    struct ucred *cred, e4fs_daddr_t *bnp)
{
	struct m_ext2fs *fs;
	struct ext2mount *ump;
	struct ext2_alloc_context ctx;
	int error;

	*bnp = 0;
	fs = ip->i_e2fs;
	ump = ip->i_ump;

	mtx_assert(EXT2_MTX(ump), MA_OWNED);
#ifdef INVARIANTS
	if ((u_int)size > fs->e2fs_bsize || blkoff(fs, size) != 0) {
		vn_printf(ip->i_devvp, "bsize = %lu, size = %d, fs = %s\n",
		    (long unsigned int)fs->e2fs_bsize, size, fs->e2fs_fsmnt);
		panic("ext2_alloc: bad size");
	}
	if (cred == NOCRED)
		panic("ext2_alloc: missing credential");
#endif		/* INVARIANTS */
	if (size == fs->e2fs_bsize && fs->e2fs_fbcount == 0)
		goto nospace;

	/*
	 * ext2_alloc_run() now owns context initialization and the
	 * reserved-block policy check.  Callers must not pre-initialize
	 * the context.
	 */
	error = ext2_alloc_run(ip, lbn, 1, bpref, EXT2_ALLOC_DATA_RAND,
	    cred, &ctx);
	/*
	 * Lock contract: ext2_alloc_run() returns with EXT2_LOCK held
	 * on both success and failure.  Caller is responsible for
	 * unlocking.
	 */
	if (error) {
		/*
		 * Preserve the original error from ext2_alloc_run()
		 * (e.g. EINVAL, EDEADLK, ENOSPC) rather than collapsing
		 * every failure path to ENOSPC.  The nospace label below
		 * handles only the pre-check fbcount/rbcount failures.
		 */
		mtx_assert(EXT2_MTX(ump), MA_OWNED);
		EXT2_UNLOCK(ump);
		return (error);
	}

	/*
	 * Commit inode accounting (i_blocks, hints, flags) while the
	 * lock is still held from ext2_alloc_run().  The block is
	 * unpublished at this point — callers in ext2_balloc.c install
	 * it into i_db[]/i_ib[] and publish via bwrite() separately.
	 */
	ext2_commit_allocated_block(ip, &ctx);
	*bnp = ctx.run.par_physical_start;
	EXT2_UNLOCK(ump);
	return (0);
nospace:
	mtx_assert(EXT2_MTX(ump), MA_OWNED);
	EXT2_UNLOCK(ump);
	SDT_PROBE2(ext2fs, , alloc, trace, 1, "cannot allocate data block");
	return (ENOSPC);
}

/*
 * Allocate EA's block for inode.
 */
e4fs_daddr_t
ext2_alloc_meta(struct inode *ip)
{
	struct m_ext2fs *fs;
	daddr_t blk;

	fs = ip->i_e2fs;

	EXT2_LOCK(ip->i_ump);
	blk = ext2_hashalloc(ip, ino_to_cg(fs, ip->i_number), 0, fs->e2fs_bsize,
	    ext2_alloccg);
	if (0 == blk) {
		EXT2_UNLOCK(ip->i_ump);
		SDT_PROBE2(ext2fs, , alloc, trace, 1, "cannot allocate meta block");
	}

	return (blk);
}
/*
 * Calculate the desired physical run length from the request and
 * filesystem policy information.
 *
 * This function returns the target run length that ext2_alloc_run()
 * should attempt to allocate.  It does NOT add e2fs_prealloc on top
 * of the request — the physical allocator allocates only the blocks
 * the caller can consume.  e2fs_prealloc is used solely as a cap
 * (via ext2_alloc_max_run), never as an inflation factor.
 *
 * The function distinguishes two quantities:
 *   - logical_length: minimum blocks required (also the target when
 *     no preallocation policy applies).
 *   - maximum_requestable_length: the largest run that satisfies all
 *     hard caps (cluster summary capacity, group size, free block
 *     count, sysctl maximum).  This is an upper bound, NOT a guarantee
 *     that a contiguous run of this length exists — the actual
 *     feasibility decision is made by ext2_clusteralloc() or other
 *     run-search routines at allocation time.
 *
 * If maximum_requestable_length < logical_length, returns 0 to signal
 * failure.
 */
static uint32_t
ext2_run_desired_length(struct inode *ip, uint32_t logical_length,
    enum ext2_alloc_class alloc_class)
{
	struct m_ext2fs *fs;
	uint32_t maximum_requestable_length;

	fs = ip->i_e2fs;

	/*
	 * Compute the maximum requestable physical run length from all
	 * hard caps.  This is the largest run we may request; whether
	 * a contiguous run of this length actually exists in any
	 * cylinder group is a feasibility question for the run-search
	 * routine, not for this cap computation.
	 */
	maximum_requestable_length = (uint32_t)fs->e2fs_fbcount;
	if (fs->e2fs_contigsumsize > 0 &&
	    (uint32_t)fs->e2fs_contigsumsize < maximum_requestable_length)
		maximum_requestable_length = fs->e2fs_contigsumsize;
	if ((uint32_t)fs->e2fs_bpg < maximum_requestable_length)
		maximum_requestable_length = fs->e2fs_bpg;
	if ((uint32_t)ext2_alloc_max_run < maximum_requestable_length)
		maximum_requestable_length = ext2_alloc_max_run;

	/*
	 * If the minimum requested length exceeds all hard caps,
	 * signal failure.  Do NOT coerce maximum_requestable_length
	 * to 1 when it is zero — that would convert an impossible
	 * request into an apparently valid one.  A zero
	 * maximum_requestable_length means no cap could be derived
	 * from filesystem geometry or free-space summary, and the
	 * caller should treat the request as unserviceable.
	 */
	if (maximum_requestable_length < logical_length)
		return (0);

	switch (alloc_class) {
	case EXT2_ALLOC_DATA_SEQ:
	case EXT2_ALLOC_DATA_RAND:
	case EXT2_ALLOC_DIRECTORY:
		/*
		 * Physical allocator does not inflate the request with
		 * e2fs_prealloc.  Return exactly logical_length; the
		 * mapping layer owns preallocation policy.
		 */
		return (logical_length);

	case EXT2_ALLOC_INDIR_METADATA:
	case EXT2_ALLOC_EXTENT_METADATA:
		/*
		 * Metadata: remain conservative, one block at a time.
		 */
		if (logical_length > 1)
			return (0);
		return (1);

	default:
		return (logical_length);
	}
}

/*
 * Physical run allocator.
 *
 * Allocates one physically contiguous run of blocks for the given inode.
 * The physical allocator only updates block bitmaps and free-space
 * accounting — NEVER inode accounting (i_blocks, i_next_alloc_block,
 * i_next_alloc_goal, i_flag).  The mapping layer caller owns all
 * inode state updates.
 *
 * Allocation policy (lexicographic):
 *   1. continue the previous physical run when practical;
 *   2. prefer the same block group;
 *   3. prefer the requested run length;
 *   4. avoid destroying large free runs unnecessarily;
 *   5. fall back to other groups when necessary.
 *
 * Minimum satisfaction rule:
 *   If logical_length == 1, any single allocated block is acceptable.
 *   If logical_length > 1, the allocator returns a run with length >=
 *   logical_length or returns ENOSPC.  Partial runs shorter than
 *   logical_length are never returned for logical_length > 1.
 *
 * Callback contract — ext2_clusteralloc() and ext2_alloccg():
 *   Both callbacks are invoked by ext2_hashalloc() with EXT2_LOCK held.
 *   On success, each returns the physical block number and allocates
 *   EXACTLY the requested number of blocks (len for clusteralloc, 1 for
 *   alloccg).  On failure, each returns 0 and allocates nothing.
 *   Callbacks release EXT2_LOCK on success; they leave it held on
 *   failure so ext2_hashalloc() can try the next group.
 *
 *   ext2_clusteralloc() is guaranteed to allocate precisely `len`
 *   contiguous blocks — the bitmap search loop requires run == len
 *   before proceeding, and the allocation loop sets exactly `len`
 *   bits.  ext2_alloccg() allocates exactly one block.
 *
 * Lock ownership:
 *   Called with EXT2_LOCK held (mtx_asserted at entry).
 *   Returns with EXT2_LOCK held on every path (success and failure).
 *   Caller must release the lock.
 *
 * Parameters:
 *   ip              inode to allocate for
 *   logical_start   logical block number (continuation hint)
 *   logical_length  minimum blocks required (and target when no
 *                   preallocation policy is active)
 *   bpref           preferred physical block (0 for none)
 *   alloc_class     allocation class
 *   cred            credential for reserved-block policy check
 *                  (NOCRED is treated as privileged — see notes below)
 *   ctxp            output: allocation context with physical run and
 *                   lifecycle state
 *
 * Returns:
 *   0 and ctxp->run populated, ctxp->state == EXT2_ALLOC_ALLOCATED
 *   on success; errno on failure (ctxp zeroed, state unchanged).
 *
 * Reserved-block policy:
 *   The check is centralized here rather than at each call site.
 *   Non-privileged callers (cr_uid != 0) are denied when free blocks
 *   fall below the reserved margin (e2fs_fbcount < e2fs_rbcount).
 *   Root (cr_uid == 0) may consume the reserved margin.
 *
 *   NOCRED handling:
 *   Callers should always provide a real credential.  Under
 *   INVARIANTS, NOCRED triggers a panic (coding error).  In
 *   production, NOCRED is treated as privileged (root) to avoid
 *   blocking legitimate kernel-internal allocation paths that may
 *   not have a credential.  This matches the convention used by
 *   ext2_alloc_meta() which operates under the mount lock and
 *   assumes kernel authority.
 */
int
ext2_alloc_run(struct inode *ip, e2fs_lbn_t logical_start,
    uint32_t logical_length, e4fs_daddr_t bpref,
    enum ext2_alloc_class alloc_class, struct ucred *cred,
    struct ext2_alloc_context *ctxp)
{
	struct m_ext2fs *fs;
	struct ext2mount *ump;
	e4fs_daddr_t bno;
	uint32_t desired_len;
	int cg;
	int error;

	fs = ip->i_e2fs;
	ump = ip->i_ump;

	mtx_assert(EXT2_MTX(ump), MA_OWNED);

	if (ctxp == NULL)
		return (EINVAL);

#ifdef INVARIANTS
	if (cred == NOCRED)
		panic("ext2_alloc_run: missing credential");
#endif		/* INVARIANTS */

	/*
	 * Centralized reserved-block policy (see docstring above):
	 * non-privileged callers cannot consume the reserved margin.
	 * NOCRED is treated as privileged.
	 */
	if (cred != NOCRED && cred->cr_uid != 0 &&
	    fs->e2fs_fbcount < fs->e2fs_rbcount) {
		error = ENOSPC;
		goto out;
	}

	bzero(ctxp, sizeof(*ctxp));
	ctxp->ip = ip;
	ctxp->logical_start = logical_start;
	ctxp->logical_length = logical_length;
	ctxp->state = EXT2_ALLOC_ALLOCATED;

	if (logical_length == 0) {
		error = EINVAL;
		goto out;
	}
	if (fs->e2fs_fbcount == 0) {
		error = ENOSPC;
		goto out;
	}

	desired_len = ext2_run_desired_length(ip, logical_length, alloc_class);
	if (desired_len == 0) {
		error = ENOSPC;
		goto out;
	}

	/*
	 * Determine preferred cylinder group:
	 *   1. supplied physical preference
	 *   2. continuation of previous allocation (i_next_alloc_goal)
	 *   3. inode's block group
	 *
	 * Hint validation: i_next_alloc_goal is a hint, not ownership.
	 * The lower-level allocator (ext2_alloccg/ext2_clusteralloc)
	 * validates that the preference is in the correct cylinder group
	 * and will treat an out-of-group hint as bpref=0.  Hints are
	 * cleared on rollback by ext2_rollback_allocation().
	 */
	if (bpref >= fs->e2fs_bcount)
		bpref = 0;
	if (bpref == 0) {
		if (ip->i_next_alloc_goal != 0 &&
		    ip->i_next_alloc_block == logical_start)
			bpref = ip->i_next_alloc_goal;
		cg = ino_to_cg(fs, ip->i_number);
	} else {
		cg = dtog(fs, bpref);
	}

	/*
	 * Step 1: for multi-block requests, try contiguous cluster
	 * allocation.  ext2_hashalloc() tries the preferred group
	 * first, then quadratically rehashes into other groups,
	 * calling ext2_clusteralloc() at each group.
	 *
	 * Callback contract: on success, ext2_clusteralloc allocates
	 * exactly desired_len contiguous blocks and returns the
	 * physical start.  On failure, it allocates nothing and
	 * returns 0 with the lock held.
	 */
	if (desired_len > 1 && fs->e2fs_contigsumsize > 0) {
#ifdef INVARIANTS
		mtx_assert(EXT2_MTX(ump), MA_OWNED);
#endif
		bno = ext2_hashalloc(ip, cg, bpref, desired_len,
		    ext2_clusteralloc);
		if (bno > 0) {
#ifdef INVARIANTS
			mtx_assert(EXT2_MTX(ump), MA_NOTOWNED);
#endif
			EXT2_LOCK(ump);
			ctxp->run.par_physical_start = bno;
			ctxp->run.par_length = desired_len;
			ctxp->state = EXT2_ALLOC_ALLOCATED;
			error = 0;

			/* Register ALLOC_MULTI dependency for the multi-block run */
			if (ip->i_e2fs->e2fs_softdep != NULL) {
				struct ext2_dep *dep = ext2_dep_alloc(ip->i_e2fs->e2fs_softdep, EXT2_DEP_ALLOC_MULTI);
				if (dep != NULL) {
					dep->dep_parent = NULL;
				}
			}
			goto out;
		}
		/*
		 * cluster allocation failed.  ext2_hashalloc and the
		 * underlying allocator left the lock held on failure.
		 */
#ifdef INVARIANTS
		mtx_assert(EXT2_MTX(ump), MA_OWNED);
#endif
	}

	/*
	 * If the minimum requirement is more than one block and
	 * cluster allocation failed, we cannot satisfy the request.
	 * Return ENOSPC rather than a partial (too-short) run.
	 */
	if (logical_length > 1) {
		error = ENOSPC;
		goto out;
	}

	/*
	 * Single-block fallback for logical_length == 1.
	 */
#ifdef INVARIANTS
	mtx_assert(EXT2_MTX(ump), MA_OWNED);
#endif
	bno = ext2_hashalloc(ip, cg, bpref, fs->e2fs_bsize, ext2_alloccg);
	if (bno > 0) {
#ifdef INVARIANTS
		mtx_assert(EXT2_MTX(ump), MA_NOTOWNED);
#endif
		EXT2_LOCK(ump);
		ctxp->run.par_physical_start = bno;
		ctxp->run.par_length = 1;
		ctxp->state = EXT2_ALLOC_ALLOCATED;
		error = 0;

		/* Register ALLOCDIRECT for data, NEWBLK for metadata */
		if (ip->i_e2fs->e2fs_softdep != NULL) {
			ext2_dep_type_t dep_type = (alloc_class == EXT2_ALLOC_DATA_SEQ ||
						    alloc_class == EXT2_ALLOC_DATA_RAND) ?
			    EXT2_DEP_ALLOCDIRECT : EXT2_DEP_NEWBLK;
			struct ext2_dep *dep = ext2_dep_alloc(ip->i_e2fs->e2fs_softdep, dep_type);
			if (dep != NULL) {
				dep->dep_parent = NULL;
			}
		}
		goto out;
	}
#ifdef INVARIANTS
	mtx_assert(EXT2_MTX(ump), MA_OWNED);
#endif
	error = ENOSPC;

out:
	mtx_assert(EXT2_MTX(ump), MA_OWNED);
	return (error);
}

/*
 * Rollback an unpublished allocation — adjust inode accounting
 * (i_blocks, i_flag) under EXT2_LOCK, then free the physical blocks.
 *
 * This helper unifies the "re-lock / fix i_blocks / unlock / free"
 * sequence that every allocation-failure cleanup path must perform.
 * It replaces hand-written boilerplate at:
 *   - ext2_balloc.c: direct-block, first-indirect, middle-indirect
 *     EFBIG paths and bwrite-error paths
 *   - ext2_extents.c: extent-insertion-failure path
 *
 * Published vs. unpublished:
 *   The blocks passed here MUST NOT be referenced by any on-disk
 *   inode metadata (direct/indirect block pointers, extent records)
 *   at the time of the call.  They are unpublished — allocated and
 *   accounted-for, but not yet wired into the file's block map.
 *   If a pointer to these blocks has already been written to a
 *   buffer that has been committed via bwrite(), the block is no
 *   longer unpublished and this helper must NOT be used.  In that
 *   case the operation is a consistency repair, not an allocation
 *   rollback, and must use ext2_free_published_runs() (a future
 *   helper that uses Soft Update dependency ordering or journaling
 *   to ensure the old pointer is overwritten before the block is
 *   freed).
 *
 * Lock ownership:
 *   Called WITHOUT EXT2_LOCK held (ext2_alloc() and ext4_new_blocks()
 *   both release the lock before returning to the cleanup path).
 *   The helper acquires the lock internally to adjust inode
 *   accounting, then releases it before calling
 *   ext2_abort_allocated_runs() (which itself acquires the lock
 *   inside ext2_blkfree()).
 *
 * Validation is performed by ext2_rollback_inode_accounting_checked()
 * which runs in ALL builds.  The wrapper panics on any validation
 * failure — callers that need recoverable error handling should
 * call the checked variant directly.
 *
 * i_blocks underflow handling:
 *   An i_blocks underflow detected by the checked variant indicates
 *   filesystem corruption, double rollback, or an accounting bug —
 *   NOT a normal caller error.  Under INVARIANTS, the checked
 *   variant panics directly.  In production, it returns EIO and the
 *   wrapper panics.  Both paths result in a panic.
 */
void
ext2_rollback_inode_accounting(struct inode *ip, e4fs_daddr_t phys_start,
    uint32_t length)
{
	int error;

	error = ext2_rollback_inode_accounting_checked(ip, phys_start, length);
	if (error)
		panic("ext2_rollback_inode_accounting: validation failed "
		    "(error %d)", error);
}

/*
 * Checked variant of ext2_rollback_inode_accounting().
 *
 * Performs all validation in EVERY build (debug and production) and
 * returns errno on failure.  The wrapper panics on error; this
 * variant lets callers that deal with externally derived or
 * potentially corrupted values recover gracefully.
 *
 * Returns 0 on success, or one of:
 *   EDEADLK — EXT2_LOCK held on entry (lock-contract violation)
 *   EINVAL  — invalid phys_start, length, overflow, or range
 *   EFBIG   — run extends past end of filesystem
 *   EIO     — i_blocks underflow (internal accounting inconsistency)
 */
int
ext2_rollback_inode_accounting_checked(struct inode *ip,
    e4fs_daddr_t phys_start, uint32_t length)
{
	struct m_ext2fs *fs;
	struct ext2mount *ump;
	uint64_t sectors, last_block;

	if (mtx_owned(EXT2_MTX(ip->i_ump)))
		return (EDEADLK);

	if (length == 0)
		return (EINVAL);
	if (phys_start == 0)
		return (EINVAL);
	if (length > EXT4_MAX_LEN)
		return (EINVAL);

	fs = ip->i_e2fs;
	ump = ip->i_ump;

	/*
	 * Validate physical block range.  phys_start must be within
	 * the filesystem's allocated block range.
	 */
	if (phys_start < (e4fs_daddr_t)le32toh(fs->e2fs->e2fs_first_dblock) ||
	    (uint64_t)phys_start >= fs->e2fs_bcount)
		return (EINVAL);

	/*
	 * Overflow-safe run-end computation.  Verify that
	 * phys_start + (length - 1) does not wrap uint64_t.
	 * For length == 1, length - 1 == 0 so no overflow is possible.
	 * For length > 1, check the sum does not exceed UINT64_MAX.
	 */
	if (length > 1) {
		uint64_t ulen = (uint64_t)length - 1;
		if ((uint64_t)phys_start > (uint64_t)-1 - ulen)
			return (EINVAL);
	}
	last_block = (uint64_t)phys_start + (length > 0 ? length - 1 : 0);
	if (last_block >= fs->e2fs_bcount)
		return (EFBIG);

	/*
	 * Compute the sector count in uint64_t.  btodb() returns daddr_t
	 * (int64_t); since e2fs_bsize is always positive and fits in
	 * uint16_t, the result of btodb() is always a small non-negative
	 * value.  Multiplying by 'length' (bounded at EXT4_MAX_LEN = 32767
	 * above) yields a maximum of ~262K sectors for 4 KiB blocks — well
	 * within uint64_t range.  The cast to uint64_t before multiplication
	 * prevents any signed-overflow concerns in the intermediate result.
	 */
	sectors = (uint64_t)btodb(fs->e2fs_bsize) * length;

#ifdef INVARIANTS
	if (ip->i_blocks < sectors)
		panic("ext2_rollback_inode_accounting_checked: i_blocks "
		    "underflow (have %llu, need %llu)",
		    (unsigned long long)ip->i_blocks,
		    (unsigned long long)sectors);
#endif
	if (ip->i_blocks < sectors)
		return (EIO);

	/*
	 * Adjust inode accounting under the mount lock so the change
	 * is atomic with respect to other threads that may read or
	 * modify i_blocks.
	 */
	EXT2_LOCK(ump);
	ip->i_blocks -= sectors;
	ip->i_flag |= IN_CHANGE | IN_UPDATE;
	EXT2_UNLOCK(ump);

	return (0);
}

/*
 * ext2_rollback_unpublished — TRANSITIONAL scalar wrapper for
 * rolling back an unpublished allocation.
 *
 * New code should call ext2_rollback_allocation() with an
 * ext2_alloc_context instead of this scalar variant.  This wrapper
 * synthesizes a minimal context for callers (e.g. ext2_balloc.c
 * EFBIG paths, ext4_ext_get_blocks extent-insertion-failure path)
 * that do not have the allocation context available.
 *
 * Delegates to:
 *   1. ext2_rollback_inode_accounting() — validates all preconditions
 *      (no lock held, non-zero start/length, length <= EXT4_MAX_LEN,
 *      block range valid, i_blocks non-underflowing) and adjusts
 *      inode accounting under EXT2_LOCK.
 *   2. ext2_abort_allocated_runs() — frees the physical blocks via
 *      ext2_blkfree() (manages its own locking).
 *
 * See ext2_extern.h for the full contract.  Replaces hand-written
 * boilerplate at:
 *   - ext2_balloc.c: direct-block, first-indirect, middle-indirect
 *     EFBIG paths and bwrite-error paths
 *   - ext2_extents.c: extent-insertion-failure path
 */
void
ext2_rollback_unpublished(struct inode *ip, e4fs_daddr_t phys_start,
    uint32_t length)
{
	struct ext2_alloc_context ctx;

	/*
	 * TRANSITIONAL INTERFACE — new code should use
	 * ext2_rollback_allocation() with a properly tracked
	 * ext2_alloc_context instead of this scalar variant.
	 * This wrapper synthesizes a minimal context for callers
	 * (e.g. ext2_balloc.c EFBIG paths and bwrite-error paths
	 * in the non-extent allocation path) that do not have an
	 * ext2_alloc_context available.
	 */
 	bzero(&ctx, sizeof(ctx));
	ctx.ip = ip;
	ctx.logical_start = 0;
	ctx.logical_length = length;
	ctx.run.par_physical_start = phys_start;
	ctx.run.par_length = length;
	ctx.accounting_applied = true;
	ext2_rollback_allocation(&ctx);
}

/*
 * ext2_rollback_allocation — context-based rollback of an unpublished
 * allocation run.
 *
 * Validates the allocation lifecycle state, then:
 *   1. If accounting_applied: revert i_blocks and i_flag via
 *      ext2_rollback_inode_accounting() (acquires EXT2_LOCK internally).
 *   2. Free physical blocks via ext2_abort_allocated_runs() (calls
 *      ext2_blkfree which manages its own locking).
 *   3. Clear stale allocation hints (i_next_alloc_block/goal).
 *   4. Set state to ROLLED_BACK.
 *
 * State machine (see enum ext2_alloc_state):
 *   Permitted entry states: ALLOCATED, MAPPED.
 *   Forbidden entry states: PUBLISHED, ROLLED_BACK
 *      (blocks may be referenced by committed metadata or already
 *      rolled back).
 *
 * Programmer-contract checks use KASSERT.  Operational failures
 * (invalid ranges, i_blocks underflow) are handled internally
 * by ext2_rollback_inode_accounting_checked() which may panic with
 * context under INVARIANTS.
 */
void
ext2_rollback_allocation(struct ext2_alloc_context *ctxp)
{
	struct ext2mount *ump;

	KASSERT(ctxp != NULL, ("ext2_rollback_allocation: NULL context"));

	/*
	 * State validation: never roll back published blocks.
	 * PUBLISHED and ROLLED_BACK are terminal — rollback must not
	 * touch them.  This is a programmer-contract check.
	 */
	KASSERT(ctxp->state == EXT2_ALLOC_ALLOCATED ||
	    ctxp->state == EXT2_ALLOC_MAPPED,
	    ("ext2_rollback_allocation: rollback from state %d",
	    ctxp->state));

	ump = ctxp->ip->i_ump;

	/*
	 * Undo inode accounting if it was applied by
	 * ext2_commit_allocated_block().
	 */
	if (ctxp->accounting_applied)
		ext2_rollback_inode_accounting(ctxp->ip,
		    ctxp->run.par_physical_start, ctxp->run.par_length);

	/*
	 * Free the physical blocks.  ext2_abort_allocated_runs() calls
	 * ext2_blkfree() which acquires EXT2_LOCK internally.
	 */
	if (ctxp->run.par_physical_start != 0 &&
	    ctxp->run.par_length > 0)
		ext2_abort_allocated_runs(ctxp->ip, &ctxp->run);

	/*
	 * Clear stale allocation hints so they are not reused after
	 * rollback.
	 */
	EXT2_LOCK(ump);
	ctxp->ip->i_next_alloc_block = 0;
	ctxp->ip->i_next_alloc_goal = 0;
	ctxp->ip->i_flag |= IN_CHANGE | IN_UPDATE;
	EXT2_UNLOCK(ump);

	ctxp->state = EXT2_ALLOC_ROLLED_BACK;
}

/*
 * ext2_commit_allocated_block — finalize inode accounting and
 * allocation hints for a successfully allocated (but not yet
 * published) block run.
 *
 * Called by the mapping layer after ext2_alloc_run() succeeds and
 * before the physical block is installed into on-disk inode metadata.
 * Updates:
 *   - i_blocks (sector count for the allocated run)
 *   - i_next_alloc_block / i_next_alloc_goal (sequential allocation hints)
 *   - i_flag (IN_CHANGE | IN_UPDATE)
 *
 * Must be called WITH EXT2_LOCK held.
 */
void
ext2_commit_allocated_block(struct inode *ip,
    struct ext2_alloc_context *ctxp)
{
	struct m_ext2fs *fs;

	fs = ip->i_e2fs;

	mtx_assert(EXT2_MTX(ip->i_ump), MA_OWNED);

	if (ctxp->accounting_applied)
		return;

	/*
	 * Record the physical block number for the next allocation hint.
	 * i_next_alloc_block tracks the logical block that was just
	 * allocated; i_next_alloc_goal is the physical goal for the
	 * next sequential allocation.  For multi-block runs the hint
	 * points to the block after the allocated run to encourage
	 * contiguity.
	 *
	 * Invariant: i_next_alloc_goal is the preferred physical
	 * continuation after the last successful allocation for this
	 * inode.  It is consumed by ext2_blkpref() when
	 * i_next_alloc_block == logical_start, indicating sequential
	 * allocation.  It is cleared on rollback to prevent reuse of
	 * stale preferences.
	 */
	ip->i_blocks += (uint64_t)btodb(fs->e2fs_bsize) * ctxp->run.par_length;
	ip->i_next_alloc_block = ctxp->logical_start;
	ip->i_next_alloc_goal = ctxp->run.par_physical_start + ctxp->run.par_length;
	ip->i_flag |= IN_CHANGE | IN_UPDATE;

	ctxp->accounting_applied = true;

	/* Register INODEDEP for inode accounting update */
	if (ip->i_e2fs->e2fs_softdep != NULL && ip->i_inodedep == NULL) {
		struct ext2_dep *dep = ext2_dep_alloc(ip->i_e2fs->e2fs_softdep, EXT2_DEP_INODEDEP);
		if (dep != NULL) {
			ip->i_inodedep = (struct ext2_inodedep *)dep;
		}
	}
}

/*
 * ext2_alloc_transition — enforce a state-machine transition on an
 * allocation context.
 *
 * This is the only sanctioned way to advance the allocation state.
 * Callers must NOT assign ctxp->state directly after initialization
 * (the initial ALLOCATED state is an exception, set by ext2_alloc_run).
 *
 * Broken internal contracts are assertions (KASSERT), not production
 * error paths.  The control flow of callers is responsible for
 * reaching each transition in order.
 */
void
ext2_alloc_transition(struct ext2_alloc_context *ctxp,
    enum ext2_alloc_state new_state)
{
	switch (ctxp->state) {
	case EXT2_ALLOC_ALLOCATED:
		KASSERT(new_state == EXT2_ALLOC_MAPPED ||
		    new_state == EXT2_ALLOC_ROLLED_BACK,
		    ("ext2_alloc_transition: ALLOCATED -> %d", new_state));
		break;
	case EXT2_ALLOC_MAPPED:
		KASSERT(new_state == EXT2_ALLOC_PUBLISHED ||
		    new_state == EXT2_ALLOC_ROLLED_BACK,
		    ("ext2_alloc_transition: MAPPED -> %d", new_state));
		break;
	case EXT2_ALLOC_PUBLISHED:
	case EXT2_ALLOC_ROLLED_BACK:
		KASSERT(false,
		    ("ext2_alloc_transition: terminal state %d -> %d",
		    ctxp->state, new_state));
		break;
	}

	ctxp->state = new_state;
}

/*
 * Abort an unpublished allocation — free blocks that were allocated
 * by ext2_alloc_run() but never became reachable from persistent
 * inode metadata.
 *
 * Published vs. unpublished distinction:
 *   This function is ONLY for blocks allocated but not yet referenced
 *   by on-disk inode metadata (direct block pointers, indirect block
 *   entries, or extent records).  The block was allocated, i_blocks
 *   was incremented, but the block number was never stored into any
 *   metadata buffer that was committed via bwrite().
 *
 *   If a pointer to the block has been written to a buffer and that
 *   buffer has been bwrite()'d, the block is PUBLISHED.  Do NOT use
 *   this function for published blocks — use the future
 *   ext2_free_published_runs() instead, which respects
 *   dependency ordering via Soft Updates or journaling.
 *
 *   | Situation                              | Action             |
 *   |----------------------------------------|--------------------|
 *   | Allocation failed before publication   | Rollback immediately
 *   | Metadata init failed before pub        | Rollback after release
 *   | Extent insertion failed before pub     | Rollback immediately
 *   | Block was published then replaced      | ext2_free_published_runs (future)
 *   | Block reachable from committed metadata| Never use abort helper
 *
 * Contract:
 *   - Lock ownership: does NOT require EXT2_LOCK.  ext2_blkfree()
 *     acquires it internally.
 *   - Publication state: run MUST be unpublished (see table above).
 *   - i_blocks: does NOT change i_blocks or i_flag.  The caller
 *     (ext2_rollback_unpublished()) handles i_blocks BEFORE calling
 *     this function.
 *   - Sleep: may sleep (ext2_blkfree calls brelse, which can block).
 *   - Buffer ownership: may be called with caller-held buffers, but
 *     this function does NOT release them.  The caller retains
 *     ownership of all buffers.
 */
void
ext2_abort_allocated_runs(struct inode *ip, struct ext2_alloc_run *runp)
{
	struct m_ext2fs *fs;
	int j;

	fs = ip->i_e2fs;
	for (j = 0; j < runp->par_length; j++)
	ext2_blkfree(ip, runp->par_physical_start + j,
		    fs->e2fs_bsize);
	}

/*
 * Reallocate a sequence of blocks into a contiguous sequence of blocks.
 *
 * The vnode and an array of buffer pointers for a range of sequential
 * logical blocks to be made contiguous is given. The allocator attempts
 * to find a range of sequential blocks starting as close as possible to
 * an fs_rotdelay offset from the end of the allocation for the logical
 * block immediately preceding the current range. If successful, the
 * physical block numbers in the buffer pointers and in the inode are
 * changed to reflect the new allocation. If unsuccessful, the allocation
 * is left unchanged. The success in doing the reallocation is returned.
 * Note that the error return is not reflected back to the user. Rather
 * the previous block allocation will be used.
 */

static SYSCTL_NODE(_vfs, OID_AUTO, ext2fs, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "EXT2FS filesystem");

static int doasyncfree = 1;

SYSCTL_INT(_vfs_ext2fs, OID_AUTO, doasyncfree, CTLFLAG_RW, &doasyncfree, 0,
    "Use asynchronous writes to update block pointers when freeing blocks");

static int doreallocblks = 0;

SYSCTL_INT(_vfs_ext2fs, OID_AUTO, doreallocblks, CTLFLAG_RW, &doreallocblks, 0, "");

/*
 * sysctl handler for ext2_alloc_max_run:
 * Validates that the value is a positive integer within the valid
 * range [1, EXT4_MAX_LEN].  Rejects negative values that would
 * become huge unsigned values when cast to uint32_t.
 */
static int
sysctl_ext2_alloc_max_run(SYSCTL_HANDLER_ARGS)
{
	int error, new_val;

	new_val = ext2_alloc_max_run;
	error = sysctl_handle_int(oidp, &new_val, 0, req);
	if (error || !req->newptr)
		return (error);

	if (new_val < 1)
		return (EINVAL);
	if (new_val > EXT4_MAX_LEN)
		new_val = EXT4_MAX_LEN;

	ext2_alloc_max_run = new_val;
	return (0);
}

SYSCTL_PROC(_vfs_ext2fs, OID_AUTO, alloc_max_run, CTLTYPE_INT | CTLFLAG_RW,
    0, 0, sysctl_ext2_alloc_max_run, "I",
    "Maximum physical allocation run length");

int
ext2_reallocblks(struct vop_reallocblks_args *ap)
{
	struct m_ext2fs *fs;
	struct inode *ip;
	struct vnode *vp;
	struct buf *sbp, *ebp;
	uint32_t *bap, *sbap, *ebap;
	struct ext2mount *ump;
	struct cluster_save *buflist;
	struct indir start_ap[EXT2_NIADDR + 1], end_ap[EXT2_NIADDR + 1], *idp;
	e2fs_lbn_t start_lbn, end_lbn;
	int soff;
	e2fs_daddr_t newblk, blkno;
	int i, len, start_lvl, end_lvl, pref, ssize;

	if (doreallocblks == 0)
		return (ENOSPC);

	vp = ap->a_vp;
	ip = VTOI(vp);
	fs = ip->i_e2fs;
	ump = ip->i_ump;

	if (fs->e2fs_contigsumsize <= 0 || ip->i_flag & IN_E4EXTENTS)
		return (ENOSPC);

	buflist = ap->a_buflist;
	len = buflist->bs_nchildren;
	start_lbn = buflist->bs_children[0]->b_lblkno;
	end_lbn = start_lbn + len - 1;
#ifdef INVARIANTS
	for (i = 1; i < len; i++)
		if (buflist->bs_children[i]->b_lblkno != start_lbn + i)
			panic("ext2_reallocblks: non-cluster");
#endif
	/*
	 * If the cluster crosses the boundary for the first indirect
	 * block, leave space for the indirect block. Indirect blocks
	 * are initially laid out in a position after the last direct
	 * block. Block reallocation would usually destroy locality by
	 * moving the indirect block out of the way to make room for
	 * data blocks if we didn't compensate here. We should also do
	 * this for other indirect block boundaries, but it is only
	 * important for the first one.
	 */
	if (start_lbn < EXT2_NDADDR && end_lbn >= EXT2_NDADDR)
		return (ENOSPC);
	/*
	 * If the latest allocation is in a new cylinder group, assume that
	 * the filesystem has decided to move and do not force it back to
	 * the previous cylinder group.
	 */
	if (dtog(fs, dbtofsb(fs, buflist->bs_children[0]->b_blkno)) !=
	    dtog(fs, dbtofsb(fs, buflist->bs_children[len - 1]->b_blkno)))
		return (ENOSPC);
	if (ext2_getlbns(vp, start_lbn, start_ap, &start_lvl) ||
	    ext2_getlbns(vp, end_lbn, end_ap, &end_lvl))
		return (ENOSPC);
	/*
	 * Get the starting offset and block map for the first block.
	 */
	if (start_lvl == 0) {
		sbap = &ip->i_db[0];
		soff = start_lbn;
	} else {
		idp = &start_ap[start_lvl - 1];
		if (bread(vp, idp->in_lbn, (int)fs->e2fs_bsize, NOCRED, &sbp)) {
			brelse(sbp);
			return (ENOSPC);
		}
		sbap = (u_int *)sbp->b_data;
		soff = idp->in_off;
	}
	/*
	 * If the block range spans two block maps, get the second map.
	 */
	ebap = NULL;
	if (end_lvl == 0 || (idp = &end_ap[end_lvl - 1])->in_off + 1 >= len) {
		ssize = len;
	} else {
#ifdef INVARIANTS
		if (start_ap[start_lvl - 1].in_lbn == idp->in_lbn)
			panic("ext2_reallocblks: start == end");
#endif
		ssize = len - (idp->in_off + 1);
		if (bread(vp, idp->in_lbn, (int)fs->e2fs_bsize, NOCRED, &ebp))
			goto fail;
		ebap = (u_int *)ebp->b_data;
	}
	/*
	 * Find the preferred location for the cluster.
	 */
	EXT2_LOCK(ump);
	pref = ext2_blkpref(ip, start_lbn, soff, sbap, 0);
	/*
	 * Search the block map looking for an allocation of the desired size.
	 */
	if ((newblk = (e2fs_daddr_t)ext2_hashalloc(ip, dtog(fs, pref), pref,
	    len, ext2_clusteralloc)) == 0) {
		EXT2_UNLOCK(ump);
		goto fail;
	}
	/*
	 * We have found a new contiguous block.
	 *
	 * First we have to replace the old block pointers with the new
	 * block pointers in the inode and indirect blocks associated
	 * with the file.
	 */
	SDT_PROBE3(ext2fs, , alloc, ext2_reallocblks_realloc,
	    ip->i_number, start_lbn, end_lbn);

	/* Register REALLOCBLKS dependency for the reallocation */
	if (ip->i_e2fs->e2fs_softdep != NULL) {
		struct ext2_dep *dep = ext2_dep_alloc(ip->i_e2fs->e2fs_softdep, EXT2_DEP_ALLOC_MULTI);
		if (dep != NULL) {
			dep->dep_parent = NULL;
		}
	}

	blkno = newblk;
	for (bap = &sbap[soff], i = 0; i < len; i++, blkno += fs->e2fs_fpb) {
		if (i == ssize) {
			bap = ebap;
			soff = -i;
		}
#ifdef INVARIANTS
		if (buflist->bs_children[i]->b_blkno != fsbtodb(fs, *bap))
			panic("ext2_reallocblks: alloc mismatch");
#endif
		SDT_PROBE1(ext2fs, , alloc, ext2_reallocblks_bap, *bap);
		*bap++ = blkno;
	}
	/*
	 * Next we must write out the modified inode and indirect blocks.
	 * For strict correctness, the writes should be synchronous since
	 * the old block values may have been written to disk. In practise
	 * they are almost never written, but if we are concerned about
	 * strict correctness, the `doasyncfree' flag should be set to zero.
	 *
	 * The test on `doasyncfree' should be changed to test a flag
	 * that shows whether the associated buffers and inodes have
	 * been written. The flag should be set when the cluster is
	 * started and cleared whenever the buffer or inode is flushed.
	 * We can then check below to see if it is set, and do the
	 * synchronous write only when it has been cleared.
	 */
	if (sbap != &ip->i_db[0]) {
		/* Indirect block write with dependency control */
		if (ip->i_e2fs->e2fs_softdep != NULL && !ext2_can_write_buffer(sbp)) {
			if (sbp->b_dep == NULL && ip->i_inodedep != NULL) {
				sbp->b_dep = &ip->i_inodedep->id_dep;
			}
			ext2_defer_buffer_write(sbp, sbp->b_dep);
		} else {
			if (doasyncfree)
				bdwrite(sbp);
			else
				bwrite(sbp);
		}
	} else {
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
		if (!doasyncfree)
			ext2_update(vp, 1);
	}
	if (ssize < len) {
		/* End indirect block write with dependency control */
		if (ip->i_e2fs->e2fs_softdep != NULL && !ext2_can_write_buffer(ebp)) {
			if (ebp->b_dep == NULL && ip->i_inodedep != NULL) {
				ebp->b_dep = &ip->i_inodedep->id_dep;
			}
			ext2_defer_buffer_write(ebp, ebp->b_dep);
		} else {
			if (doasyncfree)
				bdwrite(ebp);
			else
				bwrite(ebp);
		}
	}
	/*
	 * Last, free the old blocks and assign the new blocks to the buffers.
	 */
	for (blkno = newblk, i = 0; i < len; i++, blkno += fs->e2fs_fpb) {
		/* Register FREEBLKS for the old blocks being freed */
		if (ip->i_e2fs->e2fs_softdep != NULL) {
			struct ext2_dep *dep = ext2_dep_alloc(ip->i_e2fs->e2fs_softdep, EXT2_DEP_FREEBLKS);
			if (dep != NULL) {
				dep->dep_parent = NULL;
			}
		}
		ext2_blkfree(ip, dbtofsb(fs, buflist->bs_children[i]->b_blkno),
		    fs->e2fs_bsize);
		buflist->bs_children[i]->b_blkno = fsbtodb(fs, blkno);
		SDT_PROBE1(ext2fs, , alloc, ext2_reallocblks_blkno, blkno);
	}

	return (0);

fail:
	if (ssize < len)
		brelse(ebp);
	if (sbap != &ip->i_db[0])
		brelse(sbp);
	return (ENOSPC);
}

/*
 * Allocate an inode in the filesystem.
 *
 */
int
ext2_valloc(struct vnode *pvp, int mode, struct ucred *cred, struct vnode **vpp)
{
	struct timespec ts;
	struct m_ext2fs *fs;
	struct ext2mount *ump;
	struct inode *pip;
	struct inode *ip;
	struct vnode *vp;
	struct thread *td;
	ino_t ino, ipref;
	int error, cg;

	*vpp = NULL;
	pip = VTOI(pvp);
	fs = pip->i_e2fs;
	ump = pip->i_ump;

	EXT2_LOCK(ump);
	if (fs->e2fs_ficount == 0)
		goto noinodes;
	/*
	 * If it is a directory then obtain a cylinder group based on
	 * ext2_dirpref else obtain it using ino_to_cg. The preferred inode is
	 * always the next inode.
	 */
	if ((mode & IFMT) == IFDIR) {
		cg = ext2_dirpref(pip);
		if (fs->e2fs_contigdirs[cg] < 255)
			fs->e2fs_contigdirs[cg]++;
	} else {
		cg = ino_to_cg(fs, pip->i_number);
		if (fs->e2fs_contigdirs[cg] > 0)
			fs->e2fs_contigdirs[cg]--;
	}
	ipref = cg * fs->e2fs_ipg + 1;
	ino = (ino_t)ext2_hashalloc(pip, cg, (long)ipref, mode, ext2_nodealloccg);
	if (ino == 0)
		goto noinodes;

	td = curthread;
	error = vfs_hash_get(ump->um_mountp, ino, LK_EXCLUSIVE, td, vpp, NULL, NULL);
	if (error || *vpp != NULL) {
		return (error);
	}

	ip = malloc(sizeof(struct inode), M_EXT2NODE, M_WAITOK | M_ZERO);

	/* Allocate a new vnode/inode. */
	if ((error = getnewvnode("ext2fs", ump->um_mountp, &ext2_vnodeops, &vp)) != 0) {
		free(ip, M_EXT2NODE);
		return (error);
	}

	lockmgr(vp->v_vnlock, LK_EXCLUSIVE, NULL);
	vp->v_data = ip;
	ip->i_vnode = vp;
	ip->i_e2fs = fs = ump->um_e2fs;
	ip->i_ump = ump;
	ip->i_number = ino;
	ip->i_block_group = ino_to_cg(fs, ino);
	ip->i_next_alloc_block = 0;
	ip->i_next_alloc_goal = 0;

	error = insmntque(vp, ump->um_mountp);
	if (error) {
		free(ip, M_EXT2NODE);
		return (error);
	}

	error = vfs_hash_insert(vp, ino, LK_EXCLUSIVE, td, vpp, NULL, NULL);
	if (error || *vpp != NULL) {
		*vpp = NULL;
		free(ip, M_EXT2NODE);
		return (error);
	}

	if ((error = ext2_vinit(ump->um_mountp, &ext2_fifoops, &vp)) != 0) {
		vput(vp);
		*vpp = NULL;
		free(ip, M_EXT2NODE);
		return (error);
	}

	if (EXT2_HAS_INCOMPAT_FEATURE(fs, EXT2F_INCOMPAT_EXTENTS)
	    && (S_ISREG(mode) || S_ISDIR(mode)))
		ext4_ext_tree_init(ip);
	else
		memset(ip->i_data, 0, sizeof(ip->i_data));

	/*
	 * Set up a new generation number for this inode.
	 * Avoid zero values.
	 */
	do {
		ip->i_gen = arc4random();
	} while (ip->i_gen == 0);

	vfs_timestamp(&ts);
	ip->i_birthtime = ts.tv_sec;
	ip->i_birthnsec = ts.tv_nsec;

	vn_set_state(vp, VSTATE_CONSTRUCTED);
	*vpp = vp;

	return (0);

noinodes:
	EXT2_UNLOCK(ump);
	SDT_PROBE2(ext2fs, , alloc, trace, 1, "out of inodes");
	return (ENOSPC);
}

/*
 * 64-bit compatible getters and setters for struct ext2_gd from ext2fs.h
 */
uint64_t
e2fs_gd_get_b_bitmap(struct ext2_gd *gd)
{

	return (((uint64_t)(le32toh(gd->ext4bgd_b_bitmap_hi)) << 32) |
	    le32toh(gd->ext2bgd_b_bitmap));
}

uint64_t
e2fs_gd_get_i_bitmap(struct ext2_gd *gd)
{

	return (((uint64_t)(le32toh(gd->ext4bgd_i_bitmap_hi)) << 32) |
	    le32toh(gd->ext2bgd_i_bitmap));
}

uint64_t
e2fs_gd_get_i_tables(struct ext2_gd *gd)
{

	return (((uint64_t)(le32toh(gd->ext4bgd_i_tables_hi)) << 32) |
	    le32toh(gd->ext2bgd_i_tables));
}

static uint32_t
e2fs_gd_get_nbfree(struct ext2_gd *gd)
{

	return (((uint32_t)(le16toh(gd->ext4bgd_nbfree_hi)) << 16) |
	    le16toh(gd->ext2bgd_nbfree));
}

static void
e2fs_gd_set_nbfree(struct ext2_gd *gd, uint32_t val)
{

	gd->ext2bgd_nbfree = htole16(val & 0xffff);
	gd->ext4bgd_nbfree_hi = htole16(val >> 16);
}

static uint32_t
e2fs_gd_get_nifree(struct ext2_gd *gd)
{

	return (((uint32_t)(le16toh(gd->ext4bgd_nifree_hi)) << 16) |
	    le16toh(gd->ext2bgd_nifree));
}

static void
e2fs_gd_set_nifree(struct ext2_gd *gd, uint32_t val)
{

	gd->ext2bgd_nifree = htole16(val & 0xffff);
	gd->ext4bgd_nifree_hi = htole16(val >> 16);
}

uint32_t
e2fs_gd_get_ndirs(struct ext2_gd *gd)
{

	return (((uint32_t)(le16toh(gd->ext4bgd_ndirs_hi)) << 16) |
	    le16toh(gd->ext2bgd_ndirs));
}

static void
e2fs_gd_set_ndirs(struct ext2_gd *gd, uint32_t val)
{

	gd->ext2bgd_ndirs = htole16(val & 0xffff);
	gd->ext4bgd_ndirs_hi = htole16(val >> 16);
}

static uint32_t
e2fs_gd_get_i_unused(struct ext2_gd *gd)
{
	return ((uint32_t)(le16toh(gd->ext4bgd_i_unused_hi) << 16) |
	    le16toh(gd->ext4bgd_i_unused));
}

static void
e2fs_gd_set_i_unused(struct ext2_gd *gd, uint32_t val)
{

	gd->ext4bgd_i_unused = htole16(val & 0xffff);
	gd->ext4bgd_i_unused_hi = htole16(val >> 16);
}

/*
 * Find a cylinder to place a directory.
 *
 * The policy implemented by this algorithm is to allocate a
 * directory inode in the same cylinder group as its parent
 * directory, but also to reserve space for its files inodes
 * and data. Restrict the number of directories which may be
 * allocated one after another in the same cylinder group
 * without intervening allocation of files.
 *
 * If we allocate a first level directory then force allocation
 * in another cylinder group.
 *
 */
static u_long
ext2_dirpref(struct inode *pip)
{
	struct m_ext2fs *fs;
	int cg, prefcg, cgsize;
	uint64_t avgbfree, minbfree;
	u_int avgifree, avgndir, curdirsize;
	u_int minifree, maxndir;
	u_int mincg, minndir;
	u_int dirsize, maxcontigdirs;

	mtx_assert(EXT2_MTX(pip->i_ump), MA_OWNED);
	fs = pip->i_e2fs;

	avgifree = fs->e2fs_ficount / fs->e2fs_gcount;
	avgbfree = fs->e2fs_fbcount / fs->e2fs_gcount;
	avgndir = fs->e2fs_total_dir / fs->e2fs_gcount;

	/*
	 * Force allocation in another cg if creating a first level dir.
	 */
	ASSERT_VOP_LOCKED(ITOV(pip), "ext2fs_dirpref");
	if (ITOV(pip)->v_vflag & VV_ROOT) {
		prefcg = arc4random() % fs->e2fs_gcount;
		mincg = prefcg;
		minndir = fs->e2fs_ipg;
		for (cg = prefcg; cg < fs->e2fs_gcount; cg++)
			if (e2fs_gd_get_ndirs(&fs->e2fs_gd[cg]) < minndir &&
			    e2fs_gd_get_nifree(&fs->e2fs_gd[cg]) >= avgifree &&
			    e2fs_gd_get_nbfree(&fs->e2fs_gd[cg]) >= avgbfree) {
				mincg = cg;
				minndir = e2fs_gd_get_ndirs(&fs->e2fs_gd[cg]);
			}
		for (cg = 0; cg < prefcg; cg++)
			if (e2fs_gd_get_ndirs(&fs->e2fs_gd[cg]) < minndir &&
			    e2fs_gd_get_nifree(&fs->e2fs_gd[cg]) >= avgifree &&
			    e2fs_gd_get_nbfree(&fs->e2fs_gd[cg]) >= avgbfree) {
				mincg = cg;
				minndir = e2fs_gd_get_ndirs(&fs->e2fs_gd[cg]);
			}
		return (mincg);
	}
	/*
	 * Count various limits which used for
	 * optimal allocation of a directory inode.
	 */
	maxndir = min(avgndir + fs->e2fs_ipg / 16, fs->e2fs_ipg);
	minifree = avgifree - avgifree / 4;
	if (minifree < 1)
		minifree = 1;
	minbfree = avgbfree - avgbfree / 4;
	if (minbfree < 1)
		minbfree = 1;
	cgsize = fs->e2fs_fsize * fs->e2fs_fpg;
	dirsize = AVGDIRSIZE;
	curdirsize = avgndir ?
	    (cgsize - avgbfree * fs->e2fs_bsize) / avgndir : 0;
	if (dirsize < curdirsize)
		dirsize = curdirsize;
	maxcontigdirs = min((avgbfree * fs->e2fs_bsize) / dirsize, 255);
	maxcontigdirs = min(maxcontigdirs, fs->e2fs_ipg / AFPDIR);
	if (maxcontigdirs == 0)
		maxcontigdirs = 1;

	/*
	 * Limit number of dirs in one cg and reserve space for
	 * regular files, but only if we have no deficit in
	 * inodes or space.
	 */
	prefcg = ino_to_cg(fs, pip->i_number);
	for (cg = prefcg; cg < fs->e2fs_gcount; cg++)
		if (e2fs_gd_get_ndirs(&fs->e2fs_gd[cg]) < maxndir &&
		    e2fs_gd_get_nifree(&fs->e2fs_gd[cg]) >= minifree &&
		    e2fs_gd_get_nbfree(&fs->e2fs_gd[cg]) >= minbfree) {
			if (fs->e2fs_contigdirs[cg] < maxcontigdirs)
				return (cg);
		}
	for (cg = 0; cg < prefcg; cg++)
		if (e2fs_gd_get_ndirs(&fs->e2fs_gd[cg]) < maxndir &&
		    e2fs_gd_get_nifree(&fs->e2fs_gd[cg]) >= minifree &&
		    e2fs_gd_get_nbfree(&fs->e2fs_gd[cg]) >= minbfree) {
			if (fs->e2fs_contigdirs[cg] < maxcontigdirs)
				return (cg);
		}
	/*
	 * This is a backstop when we have deficit in space.
	 */
	for (cg = prefcg; cg < fs->e2fs_gcount; cg++)
		if (e2fs_gd_get_nifree(&fs->e2fs_gd[cg]) >= avgifree)
			return (cg);
	for (cg = 0; cg < prefcg; cg++)
		if (e2fs_gd_get_nifree(&fs->e2fs_gd[cg]) >= avgifree)
			break;
	return (cg);
}

/*
 * Select the desired position for the next block in a file.
 *
 * we try to mimic what Remy does in inode_getblk/block_getblk
 *
 * we note: blocknr == 0 means that we're about to allocate either
 * a direct block or a pointer block at the first level of indirection
 * (In other words, stuff that will go in i_db[] or i_ib[])
 *
 * blocknr != 0 means that we're allocating a block that is none
 * of the above. Then, blocknr tells us the number of the block
 * that will hold the pointer
 */
e4fs_daddr_t
ext2_blkpref(struct inode *ip, e2fs_lbn_t lbn, int indx, e2fs_daddr_t *bap,
    e2fs_daddr_t blocknr)
{
	struct m_ext2fs *fs;
	int tmp;

	fs = ip->i_e2fs;

	mtx_assert(EXT2_MTX(ip->i_ump), MA_OWNED);

	/*
	 * If the next block is actually what we thought it is, then set the
	 * goal to what we thought it should be.
	 */
	if (ip->i_next_alloc_block == lbn && ip->i_next_alloc_goal != 0)
		return ip->i_next_alloc_goal;

	/*
	 * Now check whether we were provided with an array that basically
	 * tells us previous blocks to which we want to stay close.
	 */
	if (bap)
		for (tmp = indx - 1; tmp >= 0; tmp--)
			if (bap[tmp])
				return (le32toh(bap[tmp]));

	/*
	 * Else lets fall back to the blocknr or, if there is none, follow
	 * the rule that a block should be allocated near its inode.
	 */
	return (blocknr ? blocknr :
	    (e2fs_daddr_t)(ip->i_block_group *
	    EXT2_BLOCKS_PER_GROUP(fs)) + le32toh(fs->e2fs->e2fs_first_dblock));
}

/*
 * Implement the cylinder overflow algorithm.
 *
 * The policy implemented by this algorithm is:
 *   1) allocate the block in its requested cylinder group.
 *   2) quadratically rehash on the cylinder group number.
 *   3) brute force search for a free block.
 *
 * Lock contract:
 *   Entry: EXT2_LOCK held (asserted via mtx_assert at entry).
 *   On success: releases EXT2_LOCK (allocator callback frees it).
 *   On failure: returns 0 with EXT2_LOCK still held.
 *   Callers in ext2_alloc_run() re-lock after success.
 */
static e4fs_daddr_t
ext2_hashalloc(struct inode *ip, int cg, long pref, int size,
    daddr_t (*allocator) (struct inode *, int, daddr_t, int))
{
	struct m_ext2fs *fs;
	e4fs_daddr_t result;
	int i, icg = cg;

	mtx_assert(EXT2_MTX(ip->i_ump), MA_OWNED);
	fs = ip->i_e2fs;
	/*
	 * 1: preferred cylinder group
	 */
	result = (*allocator)(ip, cg, pref, size);
	if (result)
		return (result);
	/*
	 * 2: quadratic rehash
	 */
	for (i = 1; i < fs->e2fs_gcount; i *= 2) {
		cg += i;
		if (cg >= fs->e2fs_gcount)
			cg -= fs->e2fs_gcount;
		result = (*allocator)(ip, cg, 0, size);
		if (result)
			return (result);
	}
	/*
	 * 3: brute force search
	 * Note that we start at i == 2, since 0 was checked initially,
	 * and 1 is always checked in the quadratic rehash.
	 */
	cg = (icg + 2) % fs->e2fs_gcount;
	for (i = 2; i < fs->e2fs_gcount; i++) {
		result = (*allocator)(ip, cg, 0, size);
		if (result)
			return (result);
		cg++;
		if (cg == fs->e2fs_gcount)
			cg = 0;
	}
	return (0);
}

static uint64_t
ext2_cg_number_gdb_nometa(struct m_ext2fs *fs, int cg)
{

	if (!ext2_cg_has_sb(fs, cg))
		return (0);

	if (EXT2_HAS_INCOMPAT_FEATURE(fs, EXT2F_INCOMPAT_META_BG))
		return (le32toh(fs->e2fs->e3fs_first_meta_bg));

	return ((fs->e2fs_gcount + EXT2_DESCS_PER_BLOCK(fs) - 1) /
	    EXT2_DESCS_PER_BLOCK(fs));
}

static uint64_t
ext2_cg_number_gdb_meta(struct m_ext2fs *fs, int cg)
{
	unsigned long metagroup;
	int first, last;

	metagroup = cg / EXT2_DESCS_PER_BLOCK(fs);
	first = metagroup * EXT2_DESCS_PER_BLOCK(fs);
	last = first + EXT2_DESCS_PER_BLOCK(fs) - 1;

	if (cg == first || cg == first + 1 || cg == last)
		return (1);

	return (0);
}

uint64_t
ext2_cg_number_gdb(struct m_ext2fs *fs, int cg)
{
	unsigned long first_meta_bg, metagroup;

	first_meta_bg = le32toh(fs->e2fs->e3fs_first_meta_bg);
	metagroup = cg / EXT2_DESCS_PER_BLOCK(fs);

	if (!EXT2_HAS_INCOMPAT_FEATURE(fs, EXT2F_INCOMPAT_META_BG) ||
	    metagroup < first_meta_bg)
		return (ext2_cg_number_gdb_nometa(fs, cg));

	return ext2_cg_number_gdb_meta(fs, cg);
}

static int
ext2_number_base_meta_blocks(struct m_ext2fs *fs, int cg)
{
	int number;

	number = ext2_cg_has_sb(fs, cg);

	if (!EXT2_HAS_INCOMPAT_FEATURE(fs, EXT2F_INCOMPAT_META_BG) ||
	    cg < le32toh(fs->e2fs->e3fs_first_meta_bg) *
	    EXT2_DESCS_PER_BLOCK(fs)) {
		if (number) {
			number += ext2_cg_number_gdb(fs, cg);
			number += le16toh(fs->e2fs->e2fs_reserved_ngdb);
		}
	} else {
		number += ext2_cg_number_gdb(fs, cg);
	}

	return (number);
}

static void
ext2_mark_bitmap_end(int start_bit, int end_bit, char *bitmap)
{
	int i;

	if (start_bit >= end_bit)
		return;

	for (i = start_bit; i < ((start_bit + 7) & ~7UL); i++)
		setbit(bitmap, i);
	if (i < end_bit)
		memset(bitmap + (i >> 3), 0xff, (end_bit - i) >> 3);
}

static int
ext2_get_group_number(struct m_ext2fs *fs, e4fs_daddr_t block)
{

	return ((block - le32toh(fs->e2fs->e2fs_first_dblock)) /
	    fs->e2fs_bsize);
}

static int
ext2_block_in_group(struct m_ext2fs *fs, e4fs_daddr_t block, int cg)
{

	return ((ext2_get_group_number(fs, block) == cg) ? 1 : 0);
}

static int
ext2_cg_block_bitmap_init(struct m_ext2fs *fs, int cg, struct buf *bp)
{
	int bit, bit_max, inodes_per_block;
	uint64_t start, tmp;

	if (!(le16toh(fs->e2fs_gd[cg].ext4bgd_flags) & EXT2_BG_BLOCK_UNINIT))
		return (0);

	memset(bp->b_data, 0, fs->e2fs_bsize);

	bit_max = ext2_number_base_meta_blocks(fs, cg);
	if ((bit_max >> 3) >= fs->e2fs_bsize)
		return (EINVAL);

	for (bit = 0; bit < bit_max; bit++)
		setbit(bp->b_data, bit);

	start = (uint64_t)cg * fs->e2fs_bpg +
	    le32toh(fs->e2fs->e2fs_first_dblock);

	/* Set bits for block and inode bitmaps, and inode table. */
	tmp = e2fs_gd_get_b_bitmap(&fs->e2fs_gd[cg]);
	if (!EXT2_HAS_INCOMPAT_FEATURE(fs, EXT2F_INCOMPAT_FLEX_BG) ||
	    ext2_block_in_group(fs, tmp, cg))
		setbit(bp->b_data, tmp - start);

	tmp = e2fs_gd_get_i_bitmap(&fs->e2fs_gd[cg]);
	if (!EXT2_HAS_INCOMPAT_FEATURE(fs, EXT2F_INCOMPAT_FLEX_BG) ||
	    ext2_block_in_group(fs, tmp, cg))
		setbit(bp->b_data, tmp - start);

	tmp = e2fs_gd_get_i_tables(&fs->e2fs_gd[cg]);
	inodes_per_block = fs->e2fs_bsize/EXT2_INODE_SIZE(fs);
	while( tmp < e2fs_gd_get_i_tables(&fs->e2fs_gd[cg]) +
	    fs->e2fs_ipg / inodes_per_block ) {
		if (!EXT2_HAS_INCOMPAT_FEATURE(fs, EXT2F_INCOMPAT_FLEX_BG) ||
		    ext2_block_in_group(fs, tmp, cg))
			setbit(bp->b_data, tmp - start);
		tmp++;
	}

	/*
	 * Also if the number of blocks within the group is less than
	 * the blocksize * 8 ( which is the size of bitmap ), set rest
	 * of the block bitmap to 1
	 */
	ext2_mark_bitmap_end(fs->e2fs_bpg, fs->e2fs_bsize * 8,
	    bp->b_data);

	/* Clean the flag */
	fs->e2fs_gd[cg].ext4bgd_flags = htole16(le16toh(
	    fs->e2fs_gd[cg].ext4bgd_flags) & ~EXT2_BG_BLOCK_UNINIT);

	return (0);
}

static int
ext2_b_bitmap_validate(struct m_ext2fs *fs, struct buf *bp, int cg)
{
	struct ext2_gd *gd;
	uint64_t group_first_block;
	unsigned int offset, max_bit;

	if (EXT2_HAS_INCOMPAT_FEATURE(fs, EXT2F_INCOMPAT_FLEX_BG)) {
		/*
		 * It is not possible to check block bitmap in case of this
		 * feature, because the inode and block bitmaps and inode table
		 * blocks may not be in the group at all.
		 * So, skip check in this case.
		 */
		return (0);
	}

	gd = &fs->e2fs_gd[cg];
	max_bit = fs->e2fs_bpg;
	group_first_block = ((uint64_t)cg) * fs->e2fs_bpg +
	    le32toh(fs->e2fs->e2fs_first_dblock);

	/* Check block bitmap block number */
	offset = e2fs_gd_get_b_bitmap(gd) - group_first_block;
	if (offset >= max_bit || !isset(bp->b_data, offset)) {
		SDT_PROBE2(ext2fs, , alloc, ext2_b_bitmap_validate_error,
		    "bad block bitmap, group", cg);
		return (EINVAL);
	}

	/* Check inode bitmap block number */
	offset = e2fs_gd_get_i_bitmap(gd) - group_first_block;
	if (offset >= max_bit || !isset(bp->b_data, offset)) {
		SDT_PROBE2(ext2fs, , alloc, ext2_b_bitmap_validate_error,
		    "bad inode bitmap", cg);
		return (EINVAL);
	}

	/* Check inode table */
	offset = e2fs_gd_get_i_tables(gd) - group_first_block;
	if (offset >= max_bit || offset + fs->e2fs_itpg >= max_bit) {
		SDT_PROBE2(ext2fs, , alloc, ext2_b_bitmap_validate_error,
		    "bad inode table, group", cg);
		return (EINVAL);
	}

	return (0);
}

/*
 * Determine whether a block can be allocated.
 *
 * Callback contract (called by ext2_hashalloc):
 *   - Entry: EXT2_LOCK held.
 *   - Allocates EXACTLY one block on success.
 *   - Returns physical block number on success; releases EXT2_LOCK
 *     before returning.
 *   - Returns 0 on failure; allocates nothing; leaves EXT2_LOCK held.
 *   - `size` parameter is the block size in bytes (ignored for
 *     bitmap search — always allocates one block).
 *   - Bitmap updates and free-space accounting (e2fs_fbcount,
 *     e2fs_gd[cg] nbfree) are performed atomically under the lock.
 */
static daddr_t
ext2_alloccg(struct inode *ip, int cg, daddr_t bpref, int size)
{
	struct m_ext2fs *fs;
	struct buf *bp;
	struct ext2mount *ump;
	daddr_t bno, runstart, runlen;
	int bit, loc, end, error, start;
	char *bbp;
	/* XXX ondisk32 */
	fs = ip->i_e2fs;
	ump = ip->i_ump;
	if (e2fs_gd_get_nbfree(&fs->e2fs_gd[cg]) == 0)
		return (0);

#ifdef INVARIANTS
	mtx_assert(EXT2_MTX(ump), MA_OWNED);
#endif
	EXT2_UNLOCK(ump);
	error = bread(ip->i_devvp, fsbtodb(fs,
	    e2fs_gd_get_b_bitmap(&fs->e2fs_gd[cg])),
	    (int)fs->e2fs_bsize, NOCRED, &bp);
	if (error)
		goto fail;

	if (EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_GDT_CSUM) ||
	    EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM)) {
		error = ext2_cg_block_bitmap_init(fs, cg, bp);
		if (error)
			goto fail;

		ext2_gd_b_bitmap_csum_set(fs, cg, bp);
	}
	error = ext2_gd_b_bitmap_csum_verify(fs, cg, bp);
	if (error)
		goto fail;

	error = ext2_b_bitmap_validate(fs,bp, cg);
	if (error)
		goto fail;

	/*
	 * Check, that another thread did not not allocate the last block in
	 * this group while we were waiting for the buffer.
	 */
	if (e2fs_gd_get_nbfree(&fs->e2fs_gd[cg]) == 0)
		goto fail;

	bbp = (char *)bp->b_data;

	if (dtog(fs, bpref) != cg)
		bpref = 0;
	if (bpref != 0) {
		bpref = dtogd(fs, bpref);
		/*
		 * if the requested block is available, use it
		 */
		if (isclr(bbp, bpref)) {
			bno = bpref;
			goto gotit;
		}
	}
	/*
	 * no blocks in the requested cylinder, so take next
	 * available one in this cylinder group.
	 * first try to get 8 contigous blocks, then fall back to a single
	 * block.
	 */
	if (bpref)
		start = dtogd(fs, bpref) / NBBY;
	else
		start = 0;
	end = howmany(fs->e2fs_bpg, NBBY);
retry:
	runlen = 0;
	runstart = 0;
	for (loc = start; loc < end; loc++) {
		if (bbp[loc] == (char)0xff) {
			runlen = 0;
			continue;
		}

		/* Start of a run, find the number of high clear bits. */
		if (runlen == 0) {
			bit = fls(bbp[loc]);
			runlen = NBBY - bit;
			runstart = loc * NBBY + bit;
		} else if (bbp[loc] == 0) {
			/* Continue a run. */
			runlen += NBBY;
		} else {
			/*
			 * Finish the current run.  If it isn't long
			 * enough, start a new one.
			 */
			bit = ffs(bbp[loc]) - 1;
			runlen += bit;
			if (runlen >= 8) {
				bno = runstart;
				goto gotit;
			}

			/* Run was too short, start a new one. */
			bit = fls(bbp[loc]);
			runlen = NBBY - bit;
			runstart = loc * NBBY + bit;
		}

		/* If the current run is long enough, use it. */
		if (runlen >= 8) {
			bno = runstart;
			goto gotit;
		}
	}
	if (start != 0) {
		end = start;
		start = 0;
		goto retry;
	}
	bno = ext2_mapsearch(fs, bbp, bpref);
	if (bno < 0)
		goto fail;

gotit:
#ifdef INVARIANTS
	if (isset(bbp, bno)) {
		printf("ext2fs_alloccgblk: cg=%d bno=%jd fs=%s\n",
		    cg, (intmax_t)bno, fs->e2fs_fsmnt);
		panic("ext2fs_alloccg: dup alloc");
	}
#endif
	setbit(bbp, bno);
#ifdef INVARIANTS
	mtx_assert(EXT2_MTX(ump), MA_NOTOWNED);
#endif
	EXT2_LOCK(ump);
	ext2_clusteracct(fs, bbp, cg, bno, -1);
	fs->e2fs_fbcount--;
	e2fs_gd_set_nbfree(&fs->e2fs_gd[cg],
	    e2fs_gd_get_nbfree(&fs->e2fs_gd[cg]) - 1);
	fs->e2fs_fmod = 1;
	EXT2_UNLOCK(ump);
	ext2_gd_b_bitmap_csum_set(fs, cg, bp);
	bdwrite(bp);
	return (((uint64_t)cg) * fs->e2fs_bpg +
	    le32toh(fs->e2fs->e2fs_first_dblock) + bno);

fail:
	brelse(bp);
#ifdef INVARIANTS
	mtx_assert(EXT2_MTX(ump), MA_NOTOWNED);
#endif
	EXT2_LOCK(ump);
	return (0);
}

/*
 * Determine whether a cluster can be allocated.
 *
 * Callback contract (called by ext2_hashalloc):
 *   - Entry: EXT2_LOCK held.
 *   - Allocates EXACTLY `len` contiguous blocks on success.
 *   - Returns physical block number (relative to filesystem start)
 *     on success; releases EXT2_LOCK before returning.
 *   - Returns 0 on failure; allocates nothing; leaves EXT2_LOCK held.
 *   - `len` is a block count, NOT a byte count.
 *   - Bitmap updates and free-space accounting (e2fs_fbcount,
 *     e2fs_gd[cg] nbfree) are performed atomically under the lock.
 *   - If the bitmap does not contain a run of exactly `len` free
 *     blocks, returns 0 with the lock held (no partial allocation).
 */
static daddr_t
ext2_clusteralloc(struct inode *ip, int cg, daddr_t bpref, int len)
{
	struct m_ext2fs *fs;
	struct ext2mount *ump;
	struct buf *bp;
	char *bbp;
	int bit, error, got, i, loc, run;
	int32_t *lp;
	daddr_t bno;

	fs = ip->i_e2fs;
	ump = ip->i_ump;

	if (fs->e2fs_maxcluster[cg] < len)
		return (0);

#ifdef INVARIANTS
	mtx_assert(EXT2_MTX(ump), MA_OWNED);
#endif
	EXT2_UNLOCK(ump);
	error = bread(ip->i_devvp,
	    fsbtodb(fs, e2fs_gd_get_b_bitmap(&fs->e2fs_gd[cg])),
	    (int)fs->e2fs_bsize, NOCRED, &bp);
	if (error)
		goto fail_lock;

	bbp = (char *)bp->b_data;
#ifdef INVARIANTS
	mtx_assert(EXT2_MTX(ump), MA_NOTOWNED);
#endif
	EXT2_LOCK(ump);
	/*
	 * Check to see if a cluster of the needed size (or bigger) is
	 * available in this cylinder group.
	 */
	lp = &fs->e2fs_clustersum[cg].cs_sum[len];
	for (i = len; i <= fs->e2fs_contigsumsize; i++)
		if (*lp++ > 0)
			break;
	if (i > fs->e2fs_contigsumsize) {
		/*
		 * Update the cluster summary information to reflect
		 * the true maximum-sized cluster so that future cluster
		 * allocation requests can avoid reading the bitmap only
		 * to find no cluster.
		 */
		lp = &fs->e2fs_clustersum[cg].cs_sum[len - 1];
		for (i = len - 1; i > 0; i--)
			if (*lp-- > 0)
				break;
		fs->e2fs_maxcluster[cg] = i;
		goto fail;
	}
#ifdef INVARIANTS
	mtx_assert(EXT2_MTX(ump), MA_NOTOWNED);
#endif
	EXT2_UNLOCK(ump);

	/* Search the bitmap to find a big enough cluster like in FFS. */
	if (dtog(fs, bpref) != cg)
		bpref = 0;
	if (bpref != 0)
		bpref = dtogd(fs, bpref);
	loc = bpref / NBBY;
	bit = 1 << (bpref % NBBY);
	for (run = 0, got = bpref; got < fs->e2fs_bpg; got++) {
		if ((bbp[loc] & bit) != 0)
			run = 0;
		else {
			run++;
			if (run == len)
				break;
		}
		if ((got & (NBBY - 1)) != (NBBY - 1))
			bit <<= 1;
		else {
			loc++;
			bit = 1;
		}
	}

	if (got >= fs->e2fs_bpg)
		goto fail_lock;

	/* Allocate the cluster that we found. */
	for (i = 1; i < len; i++)
		if (!isclr(bbp, got - run + i))
			panic("ext2_clusteralloc: map mismatch");

	bno = got - run + 1;
	if (bno >= fs->e2fs_bpg)
		panic("ext2_clusteralloc: allocated out of group");

	EXT2_LOCK(ump);
	for (i = 0; i < len; i += fs->e2fs_fpb) {
		setbit(bbp, bno + i);
		ext2_clusteracct(fs, bbp, cg, bno + i, -1);
		fs->e2fs_fbcount--;
		e2fs_gd_set_nbfree(&fs->e2fs_gd[cg],
		    e2fs_gd_get_nbfree(&fs->e2fs_gd[cg]) - 1);
	}
	fs->e2fs_fmod = 1;
	EXT2_UNLOCK(ump);

	bdwrite(bp);
	return (cg * fs->e2fs_bpg + le32toh(fs->e2fs->e2fs_first_dblock)
	    + bno);

fail_lock:
#ifdef INVARIANTS
	mtx_assert(EXT2_MTX(ump), MA_NOTOWNED);
#endif
	EXT2_LOCK(ump);
fail:
	brelse(bp);
	return (0);
}

static int
ext2_zero_inode_table(struct inode *ip, int cg)
{
	struct m_ext2fs *fs;
	struct buf *bp;
	int i, all_blks, used_blks;

	fs = ip->i_e2fs;

	if (le16toh(fs->e2fs_gd[cg].ext4bgd_flags) & EXT2_BG_INODE_ZEROED)
		return (0);

	all_blks = le16toh(fs->e2fs->e2fs_inode_size) * fs->e2fs_ipg /
	    fs->e2fs_bsize;

	used_blks = howmany(fs->e2fs_ipg -
	    e2fs_gd_get_i_unused(&fs->e2fs_gd[cg]),
	    fs->e2fs_bsize / EXT2_INODE_SIZE(fs));

	for (i = 0; i < all_blks - used_blks; i++) {
		bp = getblk(ip->i_devvp, fsbtodb(fs,
		    e2fs_gd_get_i_tables(&fs->e2fs_gd[cg]) + used_blks + i),
		    fs->e2fs_bsize, 0, 0, 0);
		if (!bp)
			return (EIO);

		vfs_bio_bzero_buf(bp, 0, fs->e2fs_bsize);
		bawrite(bp);
	}

	fs->e2fs_gd[cg].ext4bgd_flags = htole16(le16toh(
	    fs->e2fs_gd[cg].ext4bgd_flags) | EXT2_BG_INODE_ZEROED);

	return (0);
}

static void
ext2_fix_bitmap_tail(unsigned char *bitmap, int first, int last)
{
	int i;

	for (i = first; i <= last; i++)
		bitmap[i] = 0xff;
}

/*
 * Determine whether an inode can be allocated.
 *
 * Check to see if an inode is available, and if it is,
 * allocate it using tode in the specified cylinder group.
 */
static daddr_t
ext2_nodealloccg(struct inode *ip, int cg, daddr_t ipref, int mode)
{
	struct m_ext2fs *fs;
	struct buf *bp;
	struct ext2mount *ump;
	int error, start, len, ifree, ibytes;
	char *ibp, *loc;

	ipref--;	/* to avoid a lot of (ipref -1) */
	if (ipref == -1)
		ipref = 0;
	fs = ip->i_e2fs;
	ump = ip->i_ump;
	if (e2fs_gd_get_nifree(&fs->e2fs_gd[cg]) == 0)
		return (0);
	EXT2_UNLOCK(ump);
	error = bread(ip->i_devvp, fsbtodb(fs,
	    e2fs_gd_get_i_bitmap(&fs->e2fs_gd[cg])),
	    (int)fs->e2fs_bsize, NOCRED, &bp);
	if (error) {
		EXT2_LOCK(ump);
		return (0);
	}
	if (EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_GDT_CSUM) ||
	    EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM)) {
		if (le16toh(fs->e2fs_gd[cg].ext4bgd_flags) &
		    EXT2_BG_INODE_UNINIT) {
			ibytes = fs->e2fs_ipg / 8;
			memset(bp->b_data, 0, ibytes - 1);
			ext2_fix_bitmap_tail(bp->b_data, ibytes,
			    fs->e2fs_bsize - 1);
			fs->e2fs_gd[cg].ext4bgd_flags = htole16(le16toh(
			    fs->e2fs_gd[cg].ext4bgd_flags) &
			    ~EXT2_BG_INODE_UNINIT);
		}
		ext2_gd_i_bitmap_csum_set(fs, cg, bp);
		error = ext2_zero_inode_table(ip, cg);
		if (error) {
			brelse(bp);
			EXT2_LOCK(ump);
			return (0);
		}
	}
	error = ext2_gd_i_bitmap_csum_verify(fs, cg, bp);
	if (error) {
		brelse(bp);
		EXT2_LOCK(ump);
		return (0);
	}
	if (e2fs_gd_get_nifree(&fs->e2fs_gd[cg]) == 0) {
		/*
		 * Another thread allocated the last i-node in this
		 * group while we were waiting for the buffer.
		 */
		brelse(bp);
		EXT2_LOCK(ump);
		return (0);
	}
	ibp = (char *)bp->b_data;
	if (ipref) {
		ipref %= fs->e2fs_ipg;
		if (isclr(ibp, ipref))
			goto gotit;
	}
	start = ipref / NBBY;
	len = howmany(fs->e2fs_ipg - ipref, NBBY);
	loc = memcchr(&ibp[start], 0xff, len);
	if (loc == NULL) {
		len = start + 1;
		start = 0;
		loc = memcchr(&ibp[start], 0xff, len);
		if (loc == NULL) {
			SDT_PROBE3(ext2fs, , alloc,
			    ext2_nodealloccg_bmap_corrupted, cg, ipref,
			    fs->e2fs_fsmnt);
			brelse(bp);
			EXT2_LOCK(ump);
			return (0);
		}
	}
	ipref = (loc - ibp) * NBBY + ffs(~*loc) - 1;
gotit:
	setbit(ibp, ipref);
	EXT2_LOCK(ump);
	e2fs_gd_set_nifree(&fs->e2fs_gd[cg],
	    e2fs_gd_get_nifree(&fs->e2fs_gd[cg]) - 1);
	if (EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_GDT_CSUM) ||
	    EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM)) {
		ifree = fs->e2fs_ipg - e2fs_gd_get_i_unused(&fs->e2fs_gd[cg]);
		if (ipref + 1 > ifree)
			e2fs_gd_set_i_unused(&fs->e2fs_gd[cg],
			    fs->e2fs_ipg - (ipref + 1));
	}
	fs->e2fs_ficount--;
	fs->e2fs_fmod = 1;
	if ((mode & IFMT) == IFDIR) {
		e2fs_gd_set_ndirs(&fs->e2fs_gd[cg],
		    e2fs_gd_get_ndirs(&fs->e2fs_gd[cg]) + 1);
		fs->e2fs_total_dir++;
	}
	EXT2_UNLOCK(ump);
	ext2_gd_i_bitmap_csum_set(fs, cg, bp);
	bdwrite(bp);
	return ((uint64_t)cg * fs->e2fs_ipg + ipref + 1);
}

/*
 * Free a block or fragment.
 *
 */
void
ext2_blkfree(struct inode *ip, e4fs_daddr_t bno, long size)
{
	struct m_ext2fs *fs;
	struct buf *bp;
	struct ext2mount *ump;
	int cg, error;
	char *bbp;

	fs = ip->i_e2fs;
	ump = ip->i_ump;
	cg = dtog(fs, bno);
	if (bno >= fs->e2fs_bcount) {
		SDT_PROBE2(ext2fs, , alloc, ext2_blkfree_bad_block,
		    ip->i_number, bno);
		return;
	}
	error = bread(ip->i_devvp,
	    fsbtodb(fs, e2fs_gd_get_b_bitmap(&fs->e2fs_gd[cg])),
	    (int)fs->e2fs_bsize, NOCRED, &bp);
	if (error) {
		return;
	}
	bbp = (char *)bp->b_data;
	bno = dtogd(fs, bno);
	if (isclr(bbp, bno)) {
		panic("ext2_blkfree: freeing free block %lld, fs=%s",
		    (long long)bno, fs->e2fs_fsmnt);
	}
	clrbit(bbp, bno);
	EXT2_LOCK(ump);
	ext2_clusteracct(fs, bbp, cg, bno, 1);
	fs->e2fs_fbcount++;
	e2fs_gd_set_nbfree(&fs->e2fs_gd[cg],
	    e2fs_gd_get_nbfree(&fs->e2fs_gd[cg]) + 1);
	fs->e2fs_fmod = 1;
	EXT2_UNLOCK(ump);
	ext2_gd_b_bitmap_csum_set(fs, cg, bp);

	/* Register BMSAFEMAP dependency for block bitmap update */
	if (fs->e2fs_softdep != NULL) {
		struct ext2_dep *dep = ext2_dep_alloc(fs->e2fs_softdep, EXT2_DEP_BMSAFEMAP);
		if (dep != NULL) {
			dep->dep_parent = NULL;
		}
	}

	/*
	 * Block bitmap update with dependency control.
	 */
	if (fs->e2fs_softdep != NULL && !ext2_can_write_buffer(bp)) {
		ext2_defer_buffer_write(bp, bp->b_dep);
		return;
	}

	bdwrite(bp);
}

/*
 * Free an inode.
 *
 */
int
ext2_vfree(struct vnode *pvp, ino_t ino, int mode)
{
	struct m_ext2fs *fs;
	struct inode *pip;
	struct buf *bp;
	struct ext2mount *ump;
	int error, cg;
	char *ibp;

	pip = VTOI(pvp);
	fs = pip->i_e2fs;
	ump = pip->i_ump;
	if ((u_int)ino > fs->e2fs_ipg * fs->e2fs_gcount)
		panic("ext2_vfree: range: devvp = %p, ino = %ju, fs = %s",
		    pip->i_devvp, (uintmax_t)ino, fs->e2fs_fsmnt);

	cg = ino_to_cg(fs, ino);
	error = bread(pip->i_devvp,
	    fsbtodb(fs, e2fs_gd_get_i_bitmap(&fs->e2fs_gd[cg])),
	    (int)fs->e2fs_bsize, NOCRED, &bp);
	if (error) {
		return (0);
	}
	ibp = (char *)bp->b_data;
	ino = (ino - 1) % fs->e2fs_ipg;
	if (isclr(ibp, ino)) {
		SDT_PROBE2(ext2fs, , alloc, ext2_vfree_doublefree,
		    fs->e2fs_fsmnt, ino);
		if (fs->e2fs_ronly == 0)
			panic("ext2_vfree: freeing free inode");
	}
	clrbit(ibp, ino);
	EXT2_LOCK(ump);
	fs->e2fs_ficount++;
	e2fs_gd_set_nifree(&fs->e2fs_gd[cg],
	    e2fs_gd_get_nifree(&fs->e2fs_gd[cg]) + 1);
	if ((mode & IFMT) == IFDIR) {
		e2fs_gd_set_ndirs(&fs->e2fs_gd[cg],
		    e2fs_gd_get_ndirs(&fs->e2fs_gd[cg]) - 1);
		fs->e2fs_total_dir--;
	}
	fs->e2fs_fmod = 1;
	EXT2_UNLOCK(ump);
	ext2_gd_i_bitmap_csum_set(fs, cg, bp);

	/* Register BMSAFEMAP dependency for inode bitmap update */
	if (fs->e2fs_softdep != NULL) {
		struct ext2_dep *dep = ext2_dep_alloc(fs->e2fs_softdep, EXT2_DEP_BMSAFEMAP);
		if (dep != NULL) {
			dep->dep_parent = NULL;
		}
	}

	/*
	 * Inode bitmap update with dependency control.
	 */
	if (fs->e2fs_softdep != NULL && !ext2_can_write_buffer(bp)) {
		ext2_defer_buffer_write(bp, bp->b_dep);
		return (0);
	}

	bdwrite(bp);
	return (0);
}

/*
 * Find a block in the specified cylinder group.
 *
 * It is a panic if a request is made to find a block if none are
 * available.
 */
static daddr_t
ext2_mapsearch(struct m_ext2fs *fs, char *bbp, daddr_t bpref)
{
	char *loc;
	int start, len;

	/*
	 * find the fragment by searching through the free block
	 * map for an appropriate bit pattern
	 */
	if (bpref)
		start = dtogd(fs, bpref) / NBBY;
	else
		start = 0;
	len = howmany(fs->e2fs_bpg, NBBY) - start;
	loc = memcchr(&bbp[start], 0xff, len);
	if (loc == NULL) {
		len = start + 1;
		start = 0;
		loc = memcchr(&bbp[start], 0xff, len);
		if (loc == NULL) {
			panic("ext2_mapsearch: map corrupted: start=%d, len=%d,"
			    "fs=%s", start, len, fs->e2fs_fsmnt);
			/* NOTREACHED */
		}
	}
	return ((loc - bbp) * NBBY + ffs(~*loc) - 1);
}

int
ext2_cg_has_sb(struct m_ext2fs *fs, int cg)
{
	int a3, a5, a7;

	if (cg == 0)
		return (1);

	if (EXT2_HAS_COMPAT_FEATURE(fs, EXT2F_COMPAT_SPARSESUPER2)) {
		if (cg == le32toh(fs->e2fs->e4fs_backup_bgs[0]) ||
		    cg == le32toh(fs->e2fs->e4fs_backup_bgs[1]))
			return (1);
		return (0);
	}

	if ((cg <= 1) ||
	    !EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_SPARSESUPER))
		return (1);

	if (!(cg & 1))
		return (0);

	for (a3 = 3, a5 = 5, a7 = 7;
	    a3 <= cg || a5 <= cg || a7 <= cg;
	    a3 *= 3, a5 *= 5, a7 *= 7)
		if (cg == a3 || cg == a5 || cg == a7)
			return (1);
	return (0);
}
