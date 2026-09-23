/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * This file defines the ext2fs Soft-Updates-style dependency system.
 * It is an ext2-native implementation inspired by UFS Soft Updates concepts
 * but adapted for ext2/ext3/ext4 on-disk format and FreeBSD VFS.
 *
 * Design principles:
 * - Separate dependency graph from write scheduler
 * - Four fundamental dependency classes: NEWBLK, METAD_PUB, FREEBLK, INODE
 * - Explicit dependency graph with prerequisite counting
 * - Worklist processes actual operations, not just marks complete
 */

#ifndef _FS_EXT2FS_EXT2_SOFTDEP_H_
#define _FS_EXT2FS_EXT2_SOFTDEP_H_

#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mount.h>
#include <sys/buf.h>
#include <sys/vnode.h>
#include <sys/malloc.h>

MALLOC_DECLARE(M_EXT2SOFTSEC);

/* Forward declarations */
struct ext2_softdep_mount;
struct ext2_dep;
struct ext2_inonedep;

/*
 * Dependency types - specific operations tracked by the engine.
 * Every type maps to one of the four fundamental classes
 * (see ext2_dep_class_t below).
 */
typedef enum {
	EXT2_DEP_UNUSED = 0,
	/* Block allocation: new block must be initialized before reference */
	EXT2_DEP_ALLOCDIRECT,		/* New direct block allocation */
	EXT2_DEP_INDIRDEP,		/* Indirect block allocation */
	/* Inode accounting */
	EXT2_DEP_INODEDEP,		/* Inode i_blocks / i_flag accounting */
	/* Metadata publication (indirect block / extent tree update) */
	EXT2_DEP_METAD_PUB,		/* Metadata that publishes a block pointer */
	/* Block/file freeing */
	EXT2_DEP_FREEBLKS,		/* Block freeing */
	/* Orphan list */
	EXT2_DEP_ORPHAN_ADD,
	EXT2_DEP_ORPHAN_REMOVE,
	EXT2_DEP_MAX
} ext2_dep_type_t;

/*
 * Four fundamental dependency classes.
 * All ordering rules reduce to combinations of these four:
 *
 *   NEWBLK:      A new block must be zero-initialized and its buffer
 *                written before any metadata references it.
 *   METAD_PUB:   A metadata update containing a pointer must be written
 *                before the pointed-to block can be freed or reused.
 *   FREEBLK:     A block being freed must not be reallocated until all
 *                old references to it have been removed from old metadata.
 *   INODE:       Inode accounting (i_blocks), link count, and orphan-
 *                state transitions must be ordered correctly relative
 *                to block allocation and freeing.
 */
typedef enum {
	EXT2_DEPCLASS_NEWBLK = 0,
	EXT2_DEPCLASS_METAD_PUB,
	EXT2_DEPCLASS_FREEBLK,
	EXT2_DEPCLASS_INODE,
	EXT2_DEPCLASS_MAX
} ext2_dep_class_t;

/*
 * Dependency lifecycle state (single-valued, not bitmask).
 *
 *   PENDING:     Created, waiting for prerequisites to be satisfied.
 *   READY:       Ready to be enqueued for processing on the worklist.
 *   WRITING:     Write I/O in progress for this dependency.
 *   COMPLETE:    Operation fully complete, successors notified.
 *   CANCELLED:   Cancelled due to error or shutdown.
 */
typedef enum {
	EXT2_DEP_PENDING = 0,
	EXT2_DEP_READY,
	EXT2_DEP_WRITING,
	EXT2_DEP_COMPLETE,
	EXT2_DEP_CANCELLED,
	EXT2_DEP_STATE_MAX
} ext2_dep_state_t;

/*
 * Map a dependency type to its fundamental class.
 */
static inline ext2_dep_class_t
ext2_dep_type_to_class(ext2_dep_type_t type)
{
	switch (type) {
	/* NEWBLK: new block must be initialized before reference */
	case EXT2_DEP_ALLOCDIRECT:
	case EXT2_DEP_INDIRDEP:
		return EXT2_DEPCLASS_NEWBLK;

	/* METAD_PUB: metadata that publishes a block pointer */
	case EXT2_DEP_METAD_PUB:
		return EXT2_DEPCLASS_METAD_PUB;

	/* INODE: inode/link/orphan transitions */
	case EXT2_DEP_INODEDEP:
	case EXT2_DEP_ORPHAN_ADD:
	case EXT2_DEP_ORPHAN_REMOVE:
		return EXT2_DEPCLASS_INODE;

	/* FREEBLK: no reuse until old refs gone */
	case EXT2_DEP_FREEBLKS:
		return EXT2_DEPCLASS_FREEBLK;

	default:
		return EXT2_DEPCLASS_NEWBLK;
	}
}

/*
 * Dependency object - core of the dependency graph.
 *
 * Dual linkage:
 *   dep_all_link   - TAILQ on the mount's sd_all_deps list (for cleanup)
 *   dep_work_link  - TAILQ on the per-mount worklist (for scheduling)
 *   dep_succ_link  - TAILQ on the predecessor's dep_succs list
 *   dep_pred_link  - TAILQ on the successor's dep_preds list
 */
struct ext2_dep {
	/* Mount-wide tracking linkage (all live deps) */
	TAILQ_ENTRY(ext2_dep) dep_all_link;

	/* Worklist (ready-to-process) linkage */
	TAILQ_ENTRY(ext2_dep) dep_work_link;

	struct ext2_softdep_mount *dep_mp;
	ext2_dep_type_t		dep_type;
	ext2_dep_class_t	dep_class;
	ext2_dep_state_t	dep_state;

	/* Dependency graph edges */
	uint32_t		dep_prereq_count; /* unsatisfied predecessors */
	TAILQ_HEAD(, ext2_dep) dep_preds;  /* incoming edges */
	TAILQ_ENTRY(ext2_dep) dep_pred_link; /* link in successor's dep_preds */
	TAILQ_HEAD(, ext2_dep) dep_succs;   /* outgoing edges */
	TAILQ_ENTRY(ext2_dep) dep_succ_link; /* link in predecessor's dep_succs */

	/* Operation to perform when ready */
	void			(*dep_op)(struct ext2_dep *);
	void			*dep_arg;		/* argument for operation */

	/* Associated buffer (if any) */
	struct buf		*dep_bp;

	/* Reference counting */
	uint32_t		dep_refcnt;

	/* On-worklist flag */
	bool			dep_on_worklist;

	/* Explicitly tracked all_deps list membership */
	bool			dep_on_all_deps;

	/* Debugging */
#ifdef INVARIANTS
	const char		*dep_func;
	int			dep_line;
#endif
};

TAILQ_HEAD(ext2_all_deps_list, ext2_dep);
TAILQ_HEAD(ext2_worklist_head, ext2_dep);
TAILQ_HEAD(ext2_pred_list, ext2_dep);

/*
 * Inode dependency - extends ext2_dep for inode-specific operations.
 * struct ext2_dep must be the first member so that a `struct ext2_dep *`
 * can be safely cast to `struct ext2_inonedep *` and vice versa.
 */
struct ext2_inonedep {
	struct ext2_dep	id_dep;		/* base dependency (must be first) */
	ino_t		id_ino;		/* inode number */
	struct inode	*id_ip;		/* owning inode (or NULL) */
};

/*
 * Mount-private dependency state.
 */
struct ext2_softdep_mount {
	struct mount		*sd_mp;		/* associated mount point */
	struct mtx		sd_lock;	/* single lock for all dep state */

	/* All dependencies on this mount (for cleanup) */
	struct ext2_all_deps_list sd_all_deps;

	/* Worklist of deps ready to process (prereq_count == 0) */
	struct ext2_worklist_head sd_worklist;

	/* Orphan list state (frozen - no-ops, see ext2_orphan_*) */
	uint32_t		sd_orphan_head;
	struct mtx		sd_orphan_lock;

	/* Statistics */
	uint64_t		sd_deps_allocated;
	uint64_t		sd_deps_freed;
	uint64_t		sd_workitems_processed;

	/* Shutdown state */
	bool			sd_shutting_down;
};

/*
 * Buffer dependency accessors.
 *
 * The generic `struct buf` has `b_fsprivate1` (void *) reserved for
 * filesystem-private use.  We store a single primary dependency there.
 */
#define	EXT2_BP_DEP(bp)		((struct ext2_dep *)((bp)->b_fsprivate1))
#define	EXT2_SET_BP_DEP(bp, d)	((bp)->b_fsprivate1 = (void *)(d))
#define	EXT2_BP_DEP_CLEAR(bp)	((bp)->b_fsprivate1 = NULL)

/* Locking macros */
#define	EXT2_SOFTDEP_LOCK(sd)	mtx_lock(&(sd)->sd_lock)
#define	EXT2_SOFTDEP_UNLOCK(sd)	mtx_unlock(&(sd)->sd_lock)

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Subsystem initialization / teardown */
void	ext2_softdep_initialize(void);
void	ext2_softdep_uninitialize(void);

/* Mount-level initialization / teardown */
int	ext2_softdep_mount(struct mount *mp, struct m_ext2fs *fs);
int	ext2_softdep_unmount(struct ext2mount *ump);

/* Dependency object lifecycle */
struct ext2_dep *ext2_dep_alloc(struct ext2_softdep_mount *sd,
	    ext2_dep_type_t type);
void	ext2_dep_free(struct ext2_dep *dep);
void	ext2_dep_ref(struct ext2_dep *dep);
void	ext2_dep_rele(struct ext2_dep *dep);

/* Dependency graph construction */
void	ext2_dep_add_dependency(struct ext2_dep *pred, struct ext2_dep *succ);
void	ext2_dep_remove_dependency(struct ext2_dep *pred, struct ext2_dep *succ);
void	ext2_dep_satisfy(struct ext2_dep *dep);
void	ext2_dep_cancel(struct ext2_dep *dep);

/* Buffer association */
void	ext2_dep_attach_buffer(struct ext2_dep *dep, struct buf *bp);
void	ext2_dep_detach_buffer(struct ext2_dep *dep);

/* Inodedep helpers */
struct ext2_inonedep *ext2_inodedep_alloc(struct ext2_softdep_mount *sd,
	    struct inode *ip);
void	ext2_inonedep_attach(struct ext2_softdep_mount *sd,
	    struct inode *ip);
struct ext2_dep *ext2_inode_dep_get(struct inode *ip);

/*
 * Create a NEWBLK dependency for a newly allocated block.
 *
 * Allocates a dep, attaches it to the block's buffer (bp), and
 * returns it.  The caller is responsible for establishing the
 * correct dependency chain by linking this NEWBLK dep as a
 * predecessor of the appropriate metadata dep (the inode's
 * INODEDEP for direct/indirect blocks whose pointer is stored
 * in the inode, or a METAD_PUB dep for parent indirect blocks
 * whose pointer is stored in parent metadata).
 *
 * The newblk dep is attached to bp via EXT2_BP_DEP so that
 * ext2_dep_bwrite(bp) will satisfy it after the buffer I/O.
 */
struct ext2_dep *ext2_newblk_dep_create(struct inode *ip, struct buf *bp);

/*
 * Get or create a METAD_PUB dep for a metadata buffer that will
 * publish a pointer to a newly allocated block.
 *
 * If the buffer already has a dep (from a prior allocation in the
 * same call), that dep is returned.  Otherwise a new METAD_PUB
 * dep is allocated, attached to the buffer, and linked as a
 * prerequisite of the inode's INODEDEP.
 *
 * Returns the metadata dep, or NULL on failure.
 */
struct ext2_dep *ext2_metadep_get_or_create(struct inode *ip,
	    struct buf *bp);
/* Write scheduling */
bool	ext2_can_write_buffer(struct buf *bp);
int	ext2_drive_prerequisites(struct ext2_dep *dep);

/* Helper wrappers for buffer I/O with dep handling */
int	ext2_dep_bwrite(struct buf *bp);
void	ext2_dep_bdwrite(struct buf *bp);
int	ext2_dep_bawrite(struct buf *bp);

/* Worklist management */
void	ext2_worklist_insert(struct ext2_softdep_mount *sd,
	    struct ext2_dep *dep);
void	ext2_worklist_remove(struct ext2_softdep_mount *sd,
	    struct ext2_dep *dep);
void	ext2_worklist_process(struct ext2_softdep_mount *sd);

/* Orphan list operations */
int	ext2_orphan_add(struct inode *ip);
int	ext2_orphan_remove(struct inode *ip);
void	ext2_orphan_recovery(struct mount *mp);

/* Debugging */
#ifdef INVARIANTS
void	ext2_dep_assert_valid(struct ext2_dep *dep);
#define	EXT2_DEP_ASSERT_VALID(dep)	ext2_dep_assert_valid(dep)
#else
#define	EXT2_DEP_ASSERT_VALID(dep)	do {} while (0)
#endif

#endif /* !_FS_EXT2FS_EXT2_SOFTDEP_H_ */
