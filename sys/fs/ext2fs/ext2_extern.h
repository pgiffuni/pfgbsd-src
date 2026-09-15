/*-
 *  modified for EXT2FS support in Lites 1.1
 *
 *  Aug 1995, Godmar Back (gback@cs.utah.edu)
 *  University of Utah, Department of Computer Science
 */
/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1991, 1993, 1994
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

#ifndef _FS_EXT2FS_EXT2_EXTERN_H_
#define	_FS_EXT2FS_EXT2_EXTERN_H_

struct ext2fs_dinode;
struct ext2fs_direct_2;
struct ext2fs_direct_tail;
struct ext2fs_searchslot;
struct indir;
struct inode;
struct mount;
struct vfsconf;
struct vnode;

int	ext2_add_entry(struct vnode *, struct ext2fs_direct_2 *);
int	ext2_alloc(struct inode *, daddr_t, e4fs_daddr_t, int,
	    struct ucred *, e4fs_daddr_t *);
e4fs_daddr_t ext2_alloc_meta(struct inode *ip);
int	ext2_alloc_run(struct inode *, e4fs_lbn_t, uint32_t, e4fs_daddr_t,
	    enum ext2_alloc_class, struct ext2_alloc_context *);
void	ext2_abort_allocated_runs(struct inode *, struct ext2_alloc_run *);

/*
 * ext2_free_published_runs_placeholder — free previously published
 *   blocks without dependency ordering.
 *
 * IMPORTANT — PLACEHOLDER ONLY:
 *   This function is a staging boundary for future Soft Update
 *   dependency ordering or journaling.  It currently calls
 *   ext2_blkfree() directly and provides NO consistency guarantees
 *   for published blocks.  See the in-source documentation for the
 *   full rationale and the list of invariants it does NOT provide.
 *
 *   Under INVARIANTS, this function panics if called — published-
 *   block freeing must not be used until dependency objects exist.
 */
void	ext2_free_published_runs_placeholder(struct inode *,
	    struct ext2_alloc_run *);

/*
 * ext2_rollback_inode_accounting — revert i_blocks and i_flag
 *   for an unpublished allocation run.
 *
 * This is the accounting-only half of ext2_rollback_unpublished(),
 * separated so it can be unit-tested independently from physical
 * block freeing.
 *
 * Validation is active in ALL builds (debug and production).  This is
 * a cheap check (comparisons) on the error path, and omitting it in
 * production would leave corrupted runs silently corrupting i_blocks.
 *
 * Contract:
 *   - Called WITHOUT EXT2_LOCK held (returns EDEADLK if held)
 *   - phys_start != 0 (returns EINVAL)
 *   - length > 0 (returns EINVAL)
 *   - length <= EXT4_MAX_LEN (returns EINVAL)
 *   - phys_start within filesystem block range
 *     [e2fs_first_dblock, e2fs_bcount) (returns EINVAL)
 *   - phys_start + length - 1 does not overflow uint64_t
 *     and does not exceed e2fs_bcount - 1 (returns EFBIG or EINVAL)
 *   - i_blocks does not underflow (returns EIO)
 *
 * NOTE: range validation proves only that the run lies inside the
 * filesystem's legal block range.  It does NOT prove ownership —
 * that the blocks were allocated by this operation, are still
 * allocated, or match the recorded allocation.  The scalar
 * (phys_start, length) interface cannot provide these guarantees.
 *
 * The context-based ext2_rollback_allocation() provides stronger
 * validation by tracking:
 *   - whether blocks were allocated by this context (not a stale
 *     or unrelated range);
 *   - whether accounting was applied (accounting_applied flag);
 *   - whether blocks have already been freed (physically_freed
 *     flag, preventing double-free);
 *   - whether the mapping is published (metadata_published flag,
 *     preventing rollback after durability).
 */
void	ext2_rollback_inode_accounting(struct inode *, e4fs_daddr_t,
	    uint32_t);

/*
 * ext2_rollback_inode_accounting_checked — same as
 *   ext2_rollback_inode_accounting() but returns errno instead of
 *   panicking, for use with externally derived or potentially
 *   corrupted allocation-run arguments.
 *
 * Performs ALL validation in EVERY build (not compiled out in
 * production).  Returns 0 on success, or one of:
 *   EDEADLK — EXT2_LOCK held on entry
 *   EINVAL  — invalid phys_start, length, overflow, or range
 *   EFBIG   — run extends past end of filesystem
 *   EIO     — i_blocks underflow.  This is NOT a normal caller error;
 *             it indicates filesystem corruption, double rollback, or
 *             an accounting bug.  The checked variant returns EIO so
 *             the wrapper can panic with context.  Under INVARIANTS,
 *             the checked variant panics directly before returning.
 */
int	ext2_rollback_inode_accounting_checked(struct inode *, e4fs_daddr_t,
	    uint32_t);

/*
 * ext2_rollback_unpublished — revert a block run that was allocated
 *   but never published into on-disk inode metadata.
 *
 * This is a convenience wrapper that delegates to:
 *   1. ext2_rollback_inode_accounting() — validates all preconditions
 *      and adjusts i_blocks/i_flag under EXT2_LOCK.
 *   2. ext2_abort_allocated_runs() — frees the physical blocks via
 *      ext2_blkfree() (manages its own locking).
 *
 * TRANSITIONAL INTERFACE:
 *   This scalar interface accepts only (phys_start, length) and cannot
 *   verify allocation identity or lifecycle state.  Once the allocation
 *   context is wired through ext2_alloc_run() and the mapping layer,
 *   the preferred interface will be ext2_rollback_allocation(ctx) which
 *   can enforce exact-run matching and state transitions deterministically.
 *
 * Contract (all enforced by ext2_rollback_inode_accounting):
 *   - Called WITHOUT EXT2_LOCK held — callers that hold the lock
 *     (e.g. ext2_balloc.c multi-block EFBIF path) must release it
 *     before calling.
 *   - phys_start != 0
 *   - length > 0
 *   - length <= EXT4_MAX_LEN
 *   - phys_start within filesystem block range
 *   - phys_start + length - 1 does not overflow and does not exceed
 *     e2fs_bcount
 *   - i_blocks does not underflow
 *
 * Publication state:
 *   The blocks MUST be unpublished — i.e., no on-disk inode metadata
 *   (direct/indirect block pointers, extent records) references them.
 *   If a pointer has been committed to storage via bwrite(), the
 *   operation is a consistency repair, not an allocation rollback.
 *   Use ext2_free_published_runs_placeholder() for published-block
 *   freeing (Soft Update dependency ordering or journaling).
 *
 * Current exactly-once guarantee:
 *   The i_blocks underflow check detects some double-rollback scenarios
 *   in debug kernels.  This is a diagnostic, NOT a recovery mechanism.
 *   Future guarantee (with allocation context tracking an
 *   accounting_applied bit) will reject duplicate state transitions
 *   deterministically.
 *
 * Buffer ownership:
 *   The caller is responsible for releasing any held buffers (e.g.
 *   bp from bread() or getblk()) before or after calling this
 *   helper.  The helper operates only on the inode and the
 *   physical extent; it does not touch caller-owned buffers.
 *
 * Exactly-once guarantee:
 *   A run must not be passed to this helper more than once.  The
 *   i_blocks underflow check in INVARIANTS builds will catch
 *   double-rollback, but this is a diagnostic, not a recovery
 *   mechanism.  For stronger guarantees, the future allocation
 *   context (see below) should track an accounting-applied bit.
 */
void	ext2_rollback_unpublished(struct inode *, e4fs_daddr_t, uint32_t);

/*
 * ext2_rollback_allocation — revert an allocation tracked by an
 *   ext2_alloc_context.
 *
 * This is the preferred rollback interface.  Unlike the scalar
 * ext2_rollback_unpublished(), it can verify allocation state and
 * guard against double-rollback.
 *
 * Contract:
 *   - ctx->state must be ALLOCATED, ACCOUNTED, INITIALIZED,
 *     MAPPED, or ROLLBACK_ELIGIBLE.  Panics if PUBLISHED, PERSISTENT,
 *     or DEFERRED_FREE.
 *   - Panics if ctx->state is already ROLLED_BACK (double rollback).
 *   - On success: undoes i_blocks (if accounting_applied), frees
 *     physical blocks (if not already freed), sets state to
 *     ROLLED_BACK, clears stale allocation hints.
 *   - Does NOT require EXT2_LOCK held on entry.
 *   - Calls ext2_rollback_inode_accounting() (acquires lock
 *     internally) and ext2_abort_allocated_runs() (calls
 *     ext2_blkfree which also manages its own lock).
 */
void	ext2_rollback_allocation(struct ext2_alloc_context *);

/*
 * ext2_commit_allocated_block — finalize inode accounting and
 *   allocation hints for a successfully published block run.
 *
 * Called by the mapping layer after a physical allocation has been
 * successfully installed into inode metadata (i_db[], i_ib[], or an
 * extent record) and before the buffer is committed via bwrite().
 *
 * Performs:
 *   - i_blocks update (if not already applied)
 *   - i_next_alloc_block / i_next_alloc_goal update for sequential hints
 *   - i_flag |= IN_CHANGE | IN_UPDATE
 *
 * Contract:
 *   - Called WITH EXT2_LOCK held (caller is responsible for locking).
 *   - ctx->state should be ALLOCATED on entry.
 *     Sets ctx->accounting_applied = true and transitions
 *     ctx->state to ACCOUNTED.  This represents "inode accounting
 *     committed" — it does NOT mean block contents are initialized
 *     or the mapping is published.
 *   - Does NOT free blocks or unlock.
 */
void	ext2_commit_allocated_block(struct inode *, struct ext2_alloc_context *);

/*
 * ext2_alloc_transition — enforce a state-machine transition on an
 * allocation context.
 *
 * Transitions are the only sanctioned way to advance the state.
 * Direct assignment of ctx->state by callers is forbidden except for
 * the initial ALLOCATED state set by ext2_alloc_run().
 *
 * Permitted transitions:
 *   ALLOCATED     → INITIALIZED   (after accounting committed)
 *   INITIALIZED   → MAPPED        (after block ptr installed in metadata)
 *   MAPPED        → PUBLISHED     (after metadata durably written)
 *   PUBLISHED     → PERSISTENT    (after metadata visible on disk)
 *   PUBLISHED     → DEFERRED_FREE (Soft Update deferred free)
 *   any unpublished → ROLLBACK_ELIGIBLE → ROLLED_BACK
 *
 * Panics if the transition is not in the permitted set.
 */
void	ext2_alloc_transition(struct ext2_alloc_context *,
	    enum ext2_alloc_state);
int	ext2_balloc(struct inode *,
	    e2fs_lbn_t, int, struct ucred *, struct buf **, int);
int	ext2_blkatoff(struct vnode *, off_t, char **, struct buf **);
void	ext2_blkfree(struct inode *,  e4fs_daddr_t, long);
e4fs_daddr_t	ext2_blkpref(struct inode *, e2fs_lbn_t, int, e2fs_daddr_t *,
	    e2fs_daddr_t);
int	ext2_bmap(struct vop_bmap_args *);
int	ext2_bmaparray(struct vnode *, daddr_t, daddr_t *, int *, int *);
int	ext4_bmapext(struct vnode *, int32_t, int64_t *, int *, int *);
int	ext2_bmap_seekdata(struct vnode *, off_t *);
void	ext2_clusteracct(struct m_ext2fs *, char *, int, e4fs_daddr_t, int);
void	ext2_dirbad(struct inode *ip, doff_t offset, char *how);
int	ext2_ei2i(struct ext2fs_dinode *, struct inode *);
int	ext2_getlbns(struct vnode *, daddr_t, struct indir *, int *);
int	ext2_i2ei(struct inode *, struct ext2fs_dinode *);
void	ext2_itimes(struct vnode *vp);
int	ext2_reallocblks(struct vop_reallocblks_args *);
int	ext2_reclaim(struct vop_reclaim_args *);
int	ext2_truncate(struct vnode *, off_t, int, struct ucred *, struct thread *);
int	ext2_update(struct vnode *, int);
int	ext2_valloc(struct vnode *, int, struct ucred *, struct vnode **);
int	ext2_vfree(struct vnode *, ino_t, int);
int	ext2_vinit(struct mount *, struct vop_vector *, struct vnode **vpp);
int	ext2_lookup(struct vop_cachedlookup_args *);
int	ext2_readdir(struct vop_readdir_args *);
#ifdef EXT2FS_PRINT_EXTENTS
void	ext2_print_inode(struct inode *);
#endif
int	ext2_direnter(struct inode *, 
		struct vnode *, struct componentname *);
int	ext2_dirremove(struct vnode *, struct componentname *);
int	ext2_dirrewrite(struct inode *,
		struct inode *, struct componentname *);
int	ext2_dirempty(struct inode *, ino_t, struct ucred *);
int	ext2_checkpath(struct inode *, struct inode *, struct ucred *);
int	ext2_cg_has_sb(struct m_ext2fs *fs, int cg);
uint64_t	ext2_cg_number_gdb(struct m_ext2fs *fs, int cg);
int	ext2_inactive(struct vop_inactive_args *);
int	ext2_htree_add_entry(struct vnode *, struct ext2fs_direct_2 *,
	    struct componentname *);
int	ext2_htree_create_index(struct vnode *, struct componentname *,
	    struct ext2fs_direct_2 *);
int	ext2_htree_has_idx(struct inode *);
int	ext2_htree_hash(const char *, int, uint32_t *, int, uint32_t *,
	    uint32_t *);
int	ext2_htree_lookup(struct inode *, const char *, int, struct buf **,
	    int *, doff_t *, doff_t *, doff_t *, struct ext2fs_searchslot *);
int	ext2_search_dirblock(struct inode *, void *, int *, const char *, int,
	    int *, doff_t *, doff_t *, doff_t *, struct ext2fs_searchslot *);
uint32_t	e2fs_gd_get_ndirs(struct ext2_gd *gd);
uint64_t	e2fs_gd_get_b_bitmap(struct ext2_gd *);
uint64_t	e2fs_gd_get_i_bitmap(struct ext2_gd *);
uint64_t	e2fs_gd_get_i_tables(struct ext2_gd *);
void	ext2_sb_csum_set_seed(struct m_ext2fs *);
int	ext2_sb_csum_verify(struct m_ext2fs *);
void	ext2_sb_csum_set(struct m_ext2fs *);
int	ext2_extattr_blk_csum_verify(struct inode *, struct buf *);
void	ext2_extattr_blk_csum_set(struct inode *, struct buf *);
int	ext2_dir_blk_csum_verify(struct inode *, struct buf *);
struct ext2fs_direct_tail	*ext2_dirent_get_tail(struct inode *ip,
    struct ext2fs_direct_2 *ep);
void	ext2_dirent_csum_set(struct inode *, struct ext2fs_direct_2 *);
int	ext2_dirent_csum_verify(struct inode *ip, struct ext2fs_direct_2 *ep);
void	ext2_dx_csum_set(struct inode *, struct ext2fs_direct_2 *);
int	ext2_dx_csum_verify(struct inode *ip, struct ext2fs_direct_2 *ep);
int	ext2_extent_blk_csum_verify(struct inode *, void *);
void	ext2_extent_blk_csum_set(struct inode *, void *);
void	ext2_init_dirent_tail(struct ext2fs_direct_tail *);
int	ext2_is_dirent_tail(struct inode *, struct ext2fs_direct_2 *);
int	ext2_gd_i_bitmap_csum_verify(struct m_ext2fs *, int, struct buf *);
void	ext2_gd_i_bitmap_csum_set(struct m_ext2fs *, int, struct buf *);
int	ext2_gd_b_bitmap_csum_verify(struct m_ext2fs *, int, struct buf *);
void	ext2_gd_b_bitmap_csum_set(struct m_ext2fs *, int, struct buf *);
int	ext2_ei_csum_verify(struct inode *, struct ext2fs_dinode *);
void	ext2_ei_csum_set(struct inode *, struct ext2fs_dinode *);
int	ext2_gd_csum_verify(struct m_ext2fs *, struct cdev *);
void	ext2_gd_csum_set(struct m_ext2fs *);


/*
 * Physical allocation run descriptor.
 *
 * Represents a contiguous range of physical filesystem blocks
 * allocated by the physical allocator.  This abstraction keeps the
 * physical allocator independent of filesystem mapping structures
 * (indirect blocks, extent trees, etc.).
 */
struct ext2_alloc_run {
	e4fs_daddr_t	par_physical_start;	/* first physical block */
	uint32_t	par_length;			/* count of blocks */
};

/*
 * Allocation lifecycle state for an allocated run.
 *
 * Tracks where a run sits in its publication lifecycle.  This is a
 * runtime-only (in-memory) state carried by the allocation context —
 * it is NOT written to disk and requires no new on-disk ext2 fields.
 *
 * Lifecycle states (see enum ext2_alloc_state).
 *
 * The lifecycle is divided into independent operations, each tracked
 * by a boolean flag.  The state enum provides a coarse lifecycle
 * summary; the flags provide fine-grained, independently-settable
 * tracking of each operation.
 *
 *   Phase 1 — Allocation:
 *     physical blocks are marked in-use in the bitmap;
 *     free-space counts are decremented.
 *     State: ALLOCATED           Flags: (none)
 *
 *   Phase 2 — Accounting:
 *     i_blocks and i_flag are updated for the inode.
 *     State: ACCOUNTED          Flags: accounting_applied = true
 *
 *   Phase 3 — Initialization:
 *     block contents are zeroed (vfs_bio_clrbuf / getblk + clear).
 *     State: INITIALIZED        Flags: accounting_applied = true
 *     (init is tracked by state only; not all paths zero buffers)
 *
 *   Phase 4 — Mapping:
 *     block pointer is installed in in-memory inode metadata
 *     (bap[], i_db[], or an extent record).
 *     State: MAPPED              Flags: accounting_applied = true,
 *                                  mapping_installed = true
 *
 *   Phase 5 — Publication:
 *     metadata containing the block pointer is durably written
 *     via bwrite().
 *     State: PUBLISHED           Flags: accounting_applied,
 *                                  mapping_installed,
 *                                  metadata_published = true
 *
 *   Phase 6 — Persistent:
 *     the run is reachable from on-disk inode metadata.
 *     State: PERSISTENT          Flags: all of the above
 *
 * Failure transitions (unpublished states only):
 *   ALLOCATED / ACCOUNTED / INITIALIZED / MAPPED
 *     → ROLLBACK_ELIGIBLE → ROLLED_BACK
 *
 * Published run free (Soft Update / journal ordering):
 *   PUBLISHED / PERSISTENT
 *     → DEFERRED_FREE
  *
  * States:
  *   EXT2_ALLOC_ALLOCATED     Run allocated in the physical allocator
  *                            (bitmap updated, free-space count
  *                            decremented).  i_blocks not yet
  *                            incremented — see accounting_applied.
  *                            Run is unpublished.
  *
  *   EXT2_ALLOC_ACCOUNTED   Inode accounting (i_blocks, i_flag) has
  *                            been updated via
  *                            ext2_commit_allocated_block().  Physical
  *                            blocks are reserved but NOT yet
  *                            initialized or mapped.  Run is
  *                            unpublished.
  *
  *   EXT2_ALLOC_INITIALIZED   Block contents have been initialized
  *                            in memory or buffer cache.  May be
  *                            same as ACCOUNTED if no init was
  *                            needed (e.g. extent leaf blocks that
  *                            receive immediate extent records).
  *                            Run is unpublished.
  *
  *   EXT2_ALLOC_MAPPED        Block pointer has been installed in
  *                            in-memory inode metadata (bap[], i_db[],
  *                            or an extent record).  Run is unpublished
  *                            but reachable from in-memory metadata.
  *
  *   EXT2_ALLOC_PUBLISHED     Metadata containing the block pointer
  *                            has been durably written via bwrite().
  *                            Must NOT use ext2_rollback_unpublished();
  *                            use ext2_free_published_runs_placeholder()
  *                            instead.
  *
  *   EXT2_ALLOC_PERSISTENT    Run is on-disk and reachable from inode
  *                            metadata.  Final stable state.
  *
  *   EXT2_ALLOC_ROLLBACK_ELIGIBLE
  *                            Run is unpublished and eligible for
  *                            rollback via ext2_rollback_allocation().
  *                            Only the extent-insertion-failure path
  *                            and EFBIG/bwrite-error paths should set
  *                            this state.
  *
  *   EXT2_ALLOC_ROLLED_BACK   Run has been rolled back and freed.
  *                            Must not be touched again.
  *
  *   EXT2_ALLOC_DEFERRED_FREE   Run is published but free has been
  *                            deferred for Soft Update / journaling
  *                            ordering.
  */
enum ext2_alloc_state {
	EXT2_ALLOC_ALLOCATED = 0,
	EXT2_ALLOC_ACCOUNTED,
	EXT2_ALLOC_INITIALIZED,
	EXT2_ALLOC_MAPPED,
	EXT2_ALLOC_PUBLISHED,
	EXT2_ALLOC_PERSISTENT,
	EXT2_ALLOC_ROLLBACK_ELIGIBLE,
	EXT2_ALLOC_ROLLED_BACK,
	EXT2_ALLOC_DEFERRED_FREE,
};

/*
 * Allocation context — runtime-only tracking of an allocation run's
 * lifecycle through the publication pipeline.
 *
 * This struct is never written to disk.  It exists solely to let
 * rollback and deferred-free distinguish unpublished from published
 * blocks at runtime.
 *
 * Fields:
 *   run                The physical run (start + length).
 *   logical_start      Logical block number (for diagnostics).
 *   state              Current lifecycle state (see enum above).
 *   accounting_applied Flag: i_blocks (and i_flag) has been incremented
 *                      for this run by ext2_commit_allocated_block().
 *                      Prevents double-accounting on rollback or retry.
 *                      When true, the state is at least INITIALIZED.
 *   mapping_installed  Flag: block pointer has been installed in
 *                      in-memory inode metadata (bap[], i_db[], or
 *                      an extent record).  Does NOT imply durability.
 *                      Prevents rollback after the mapping is
 *                      reachable from the inode.
 *   metadata_published Flag: metadata containing the block pointer
 *                      has been durably written via bwrite().
 *                      Rollback is forbidden; use
 *                      ext2_free_published_runs_placeholder().
 *   physically_freed   Flag: blocks have been freed to the bitmap.
 *                      Prevents double-free on rollback or retry.
 *
 * This is the future boundary for Soft Update dependency objects.
 * Allocation paths should populate `state` at each stage; rollback
 * and deferred-free should verify `state` before proceeding.
 */
struct ext2_alloc_context {
	struct inode		*ip;		/* owning inode */
	struct ext2_alloc_run	run;		/* physical run (start + length) */
	e4fs_lbn_t		logical_start;	/* logical block number */
	uint32_t		logical_length;	/* requested logical blocks */
	enum ext2_alloc_state	state;		/* lifecycle state */
	bool			accounting_applied; /* i_blocks incremented */
	bool			mapping_installed;  /* ptr in in-memory inode metadata */
	bool			metadata_published; /* metadata written via bwrite() */
	bool			physically_freed;	/* blocks freed to bitmap */
};

/*
 * Allocation class for the physical allocator.
 *
 * These classes let the allocation policy tune itself for the kind
 * of data being allocated.
 */
enum ext2_alloc_class {
	EXT2_ALLOC_DATA_SEQ,		/* sequential data write */
	EXT2_ALLOC_DATA_RAND,		/* random data write */
	EXT2_ALLOC_DIRECTORY,		/* directory */
	EXT2_ALLOC_INDIR_METADATA,	/* indirect block metadata */
	EXT2_ALLOC_EXTENT_METADATA,	/* extent tree metadata */
};

/* Flags to low-level allocation routines.
 * The low 16-bits are reserved for IO_ flags from vnode.h.
 *
 * The BA_CLRBUF flag specifies that the existing content of the block
 * will not be completely overwritten by the caller, so buffers for new
 * blocks must be cleared and buffers for existing blocks must be read.
 * When BA_CLRBUF is not set the buffer will be completely overwritten
 * and there is no reason to clear them or to spend I/O fetching existing
 * data. The BA_CLRBUF flag is handled in the ext2_balloc() functions.
 */
#define	BA_CLRBUF	0x00010000	/* Clear invalid areas of buffer. */
#define	BA_SEQMASK	0x7F000000	/* Bits holding seq heuristic. */
#define	BA_SEQSHIFT	24
#define	BA_SEQMAX	0x7F

extern struct vop_vector ext2_vnodeops;
extern struct vop_vector ext2_fifoops;

#endif	/* !_FS_EXT2FS_EXT2_EXTERN_H_ */
