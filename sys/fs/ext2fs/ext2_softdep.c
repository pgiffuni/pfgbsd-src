/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * ext2fs Soft-Updates-style dependency system implementation.
 *
 * This module implements a simplified dependency engine that enforces
 * correct ordering of metadata updates to maintain filesystem
 * consistency without full journaling.  The engine tracks four
 * fundamental dependency classes (NEWBLK, METAD_PUB, FREEBLK, INODE)
 * and uses prerequisite counting to order operations.
 *
 * Locking model:
 *   - One mutex (sd_lock) per mount protects all dependency graph
 *     state (all_deps, worklist, dep_succs chains, dep_state,
 *     dep_prereq_count, dep_on_worklist, dep_on_all_deps).
 *   - sd_lock must be held when calling ext2_can_write_buffer().
 *   - The orphaned_lock protects frozen orphan chain state.
 *
 * Synchronous write model:
 *   - ext2_dep_bwrite() is the single entry point for writing a
 *     buffer whose dep has been resolved.  It calls
 *     ext2_drive_prerequisites() to recursively write any
 *     predecessor buffers, then bwrite(), then ext2_dep_satisfy().
 *   - ext2_dep_bdwrite() and ext2_dep_bawrite() delegate to
 *     ext2_dep_bwrite() for consistent ordering.
 *   - ext2_dep_satisfy() is the only function that transitions a
 *     dep to COMPLETE.
 *
 * Buffer association:
 *   - The buffer's b_fsprivate1 field stores a pointer to the
 *     primary ext2_dep associated with the buffer.
 *   - One buffer has at most one dep (enforced by KASSERT).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/lock.h>
#include <sys/buf.h>
#include <sys/vnode.h>
#include <sys/proc.h>
#include <sys/kthread.h>
#include <sys/endian.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/ext2_softdep.h>
#include <fs/ext2fs/ext2_extents.h>

MALLOC_DEFINE(M_EXT2SOFTSEC, "ext2_softdep", "EXT2 softdep state");

struct ext2_softdep_mount;
extern int ext2_vget(struct mount *, ino_t, int, struct vnode **);

static struct mtx ext2_softdep_global_lock;
static int ext2_softdep_initialized = 0;

/* Dependency type names for debugging */
static const char *ext2_dep_typenames[] = {
	[EXT2_DEP_UNUSED]		= "UNUSED",
	[EXT2_DEP_ALLOCDIRECT]		= "ALLOCDIRECT",
	[EXT2_DEP_INDIRDEP]		= "INDIRDEP",
	[EXT2_DEP_METAD_PUB]		= "METAD_PUB",
	[EXT2_DEP_INODEDEP]		= "INODEDEP",
	[EXT2_DEP_FREEBLKS]		= "FREEBLKS",
	[EXT2_DEP_ORPHAN_ADD]		= "ORPHAN_ADD",
	[EXT2_DEP_ORPHAN_REMOVE]		= "ORPHAN_REMOVE",
};

/* ------------------------------------------------------------------ */
/* Subsystem initialization                                            */
/* ------------------------------------------------------------------ */

void
ext2_softdep_initialize(void)
{
	if (ext2_softdep_initialized)
		return;

	mtx_init(&ext2_softdep_global_lock, "ext2sdglobal", NULL, MTX_DEF);
	ext2_softdep_initialized = 1;
}

void
ext2_softdep_uninitialize(void)
{
	if (!ext2_softdep_initialized)
		return;

	mtx_destroy(&ext2_softdep_global_lock);
	ext2_softdep_initialized = 0;
}

static void
ext2_softdep_modload(void *arg)
{
	ext2_softdep_initialize();
}

static void
ext2_softdep_modunload(void *arg)
{
	ext2_softdep_uninitialize();
}

SYSINIT(ext2_softdep_subsys, SI_SUB_DRIVERS, SI_ORDER_FIRST,
    ext2_softdep_modload, NULL);
SYSUNINIT(ext2_softdep_subsys, SI_SUB_DRIVERS, SI_ORDER_FIRST,
    ext2_softdep_modunload, NULL);

/* ------------------------------------------------------------------ */
/* Mount-level initialization / teardown                               */
/* ------------------------------------------------------------------ */

/*
 * Called from ext2_mountfs() after the in-memory superblock is ready
 * and fs->e2fs_softdep is NULL.
 */
int
ext2_softdep_mount(struct mount *mp, struct m_ext2fs *fs)
{
	struct ext2_softdep_mount *sd;

	if (!ext2_softdep_initialized)
		return (ENODEV);

	if (fs->e2fs_softdep != NULL)
		return (0); /* already mounted with softdep */

	sd = malloc(sizeof(*sd), M_EXT2SOFTSEC, M_WAITOK | M_ZERO);
	if (sd == NULL)
		return (ENOMEM);

	sd->sd_mp = mp;
	mtx_init(&sd->sd_lock, "ext2sdlock", NULL, MTX_DEF);
	mtx_init(&sd->sd_orphan_lock, "ext2orphlock", NULL, MTX_DEF);

	TAILQ_INIT(&sd->sd_all_deps);
	TAILQ_INIT(&sd->sd_worklist);
	sd->sd_shutting_down = false;
	sd->sd_orphan_head = le32toh(fs->e2fs->e3fs_last_orphan);

	fs->e2fs_softdep = sd;

	return (0);
}

/*
 * Called from ext2_unmount() after vflush() has released all vnodes.
 */
int
ext2_softdep_unmount(struct ext2mount *ump)
{
	struct ext2_softdep_mount *sd;
	struct ext2_dep *dep, *next;

	sd = ump->um_e2fs->e2fs_softdep;
	if (sd == NULL)
		return (0);

	EXT2_SOFTDEP_LOCK(sd);
	sd->sd_shutting_down = true;

	/*
	 * Cancel all remaining dependencies on the all_deps list.
	 * Since the synchronous model means no deferred writes are
	 * outstanding, every dep should be in a terminal state except
	 * orphans from an interrupted synchronous write.
	 */
	TAILQ_FOREACH_SAFE(dep, &sd->sd_all_deps, dep_all_link, next) {
		if (dep->dep_state != EXT2_DEP_COMPLETE &&
		    dep->dep_state != EXT2_DEP_CANCELLED)
			dep->dep_state = EXT2_DEP_CANCELLED;
	}
	EXT2_SOFTDEP_UNLOCK(sd);

	/*
	 * Free all remaining deps.  Since all deps are now COMPLETE or
	 * CANCELLED, there are no outstanding I/O operations.  We must
	 * release sd_lock around ext2_dep_rele() because ext2_dep_free()
	 * itself acquires sd_lock.
	 */
	EXT2_SOFTDEP_LOCK(sd);
	while ((dep = TAILQ_FIRST(&sd->sd_all_deps)) != NULL) {
		TAILQ_REMOVE(&sd->sd_all_deps, dep, dep_all_link);
		dep->dep_on_all_deps = false;
		EXT2_SOFTDEP_UNLOCK(sd);
		sd->sd_deps_freed++;
		ext2_dep_rele(dep);
		EXT2_SOFTDEP_LOCK(sd);
	}
	EXT2_SOFTDEP_UNLOCK(sd);

	mtx_destroy(&sd->sd_lock);
	mtx_destroy(&sd->sd_orphan_lock);

	ump->um_e2fs->e2fs_softdep = NULL;
	free(sd, M_EXT2SOFTSEC);

	return (0);
}

/* ------------------------------------------------------------------ */
/* Dependency lifecycle                                                */
/* ------------------------------------------------------------------ */

/*
 * Allocate a dependency object.
 *
 * Reference ownership model:
 *   - sd_all_deps holds 1 reference for the mount lifetime
 *   - ext2_dep_ref()/ext2_dep_rele() for temporary callers
 *   - Graph edges (dep_preds/dep_succs) do NOT hold references
 *   - Buffer association (dep_bp) does NOT hold a reference
 *
 * This means a dep cannot be freed while it's on the graph or
 * attached to a buffer.  It is only freed when ext2_dep_rele() drops
 * the refcount to zero, which only happens after ext2_dep_free()
 * removes it from sd_all_deps (unmount) or after all temporary
 * refs are dropped AND it's removed from sd_all_deps.
 *
 * The new dep starts in EXT2_DEP_PENDING state with prereq_count == 0.
 * It is linked into the mount's sd_all_deps list (which owns one
 * reference via dep_refcnt).
 */
struct ext2_dep *
ext2_dep_alloc(struct ext2_softdep_mount *sd, ext2_dep_type_t type)
{
	struct ext2_dep *dep;
	size_t size;

	if (sd == NULL || type <= EXT2_DEP_UNUSED || type >= EXT2_DEP_MAX)
		return (NULL);

	EXT2_SOFTDEP_LOCK(sd);
	if (sd->sd_shutting_down) {
		EXT2_SOFTDEP_UNLOCK(sd);
		return (NULL);
	}
	EXT2_SOFTDEP_UNLOCK(sd);

	/* INODEDEP deps are allocated as ext2_inonedep */
	size = (type == EXT2_DEP_INODEDEP)
	    ? sizeof(struct ext2_inonedep)
	    : sizeof(struct ext2_dep);

	dep = malloc(size, M_EXT2SOFTSEC, M_WAITOK | M_ZERO);
	if (dep == NULL)
		return (NULL);

	dep->dep_mp = sd;
	dep->dep_type = type;
	dep->dep_class = ext2_dep_type_to_class(type);
	dep->dep_state = EXT2_DEP_PENDING;
	dep->dep_refcnt = 1;
	dep->dep_prereq_count = 0;
	dep->dep_bp = NULL;
	dep->dep_op = NULL;
	dep->dep_arg = NULL;
	dep->dep_on_worklist = false;
	TAILQ_INIT(&dep->dep_preds);
	TAILQ_INIT(&dep->dep_succs);

#ifdef INVARIANTS
	dep->dep_func = __func__;
	dep->dep_line = __LINE__;
#endif

	EXT2_SOFTDEP_LOCK(sd);
	if (sd->sd_shutting_down) {
		EXT2_SOFTDEP_UNLOCK(sd);
		free(dep, M_EXT2SOFTSEC);
		return (NULL);
	}
	TAILQ_INSERT_TAIL(&sd->sd_all_deps, dep, dep_all_link);
	dep->dep_on_all_deps = true;
	sd->sd_deps_allocated++;
	EXT2_SOFTDEP_UNLOCK(sd);

	return (dep);
}

/*
 * Free a dependency object.
 * Must be called with refcount == 0.
 */
void
ext2_dep_free(struct ext2_dep *dep)
{
	struct ext2_softdep_mount *sd;

	if (dep == NULL)
		return;

	sd = dep->dep_mp;

	if (sd != NULL) {
		EXT2_SOFTDEP_LOCK(sd);
		/* Remove from all_deps if still there */
		if (dep->dep_on_all_deps) {
			TAILQ_REMOVE(&sd->sd_all_deps, dep, dep_all_link);
			dep->dep_on_all_deps = false;
			sd->sd_deps_freed++;
		}
		EXT2_SOFTDEP_UNLOCK(sd);
	}

	free(dep, M_EXT2SOFTSEC);
}

/*
 * Reference counting.
 */
void
ext2_dep_ref(struct ext2_dep *dep)
{
	if (dep == NULL)
		return;
	atomic_add_int(&dep->dep_refcnt, 1);
}

void
ext2_dep_rele(struct ext2_dep *dep)
{
	if (dep == NULL)
		return;
	if (atomic_fetchadd_int(&dep->dep_refcnt, -1) == 1)
		ext2_dep_free(dep);
}

/* ------------------------------------------------------------------ */
/* Dependency graph management                                         */
/* ------------------------------------------------------------------ */

/*
	 * Add a predecessor-successor edge: pred must complete before succ.
	 *
	 * This increments succ's prereq_count.  When the count reaches zero
	 * the successor is enqueued on the worklist.
	 *
	 * Both edges are established: succ is added to pred->dep_succs and
	 * pred is added to succ->dep_preds, so multiple predecessors can be
	 * represented correctly.
	 *
	 * Reference model: graph edges do NOT hold references to their
	 * endpoints.  The `sd_all_deps` list holds the only references for
	 * the lifetime of the mount, plus transient references during
	 * active I/O.  Callers must not rely on edge traversal for
	 * lifetime management; use ext2_dep_rele() for explicit reference
	 * counting on individual deps.
	 */
void
ext2_dep_add_dependency(struct ext2_dep *pred, struct ext2_dep *succ)
{
	if (pred == NULL || succ == NULL || pred == succ)
		return;

	if (pred->dep_mp != succ->dep_mp)
		return;

	/*
	 * Enforce graph construction invariants:
	 * - pred must not be CANCELLED (no edges from dead deps)
	 * - succ must be PENDING (only add edges to waiting deps)
	 */
#ifdef INVARIANTS
	KASSERT(pred->dep_state != EXT2_DEP_CANCELLED,
	    ("ext2_dep_add_dependency: predecessor %p (type %s) is CANCELLED",
	    (void *)pred,
	    ext2_dep_typenames[pred->dep_type] ?
	    ext2_dep_typenames[pred->dep_type] : "???"));
	KASSERT(succ->dep_state == EXT2_DEP_PENDING,
	    ("ext2_dep_add_dependency: successor %p (type %s) is %s, "
	    "expected PENDING", (void *)succ,
	    ext2_dep_typenames[succ->dep_type] ?
	    ext2_dep_typenames[succ->dep_type] : "???",
	    succ->dep_state == EXT2_DEP_COMPLETE ? "COMPLETE" :
	    succ->dep_state == EXT2_DEP_CANCELLED ? "CANCELLED" :
	    succ->dep_state == EXT2_DEP_READY ? "READY" :
	    succ->dep_state == EXT2_DEP_WRITING ? "WRITING" : "UNKNOWN"));
#endif

	EXT2_SOFTDEP_LOCK(pred->dep_mp);

	/* Already linked from this predecessor? */
	if (TAILQ_FIRST(&succ->dep_preds) != NULL) {
		struct ext2_dep *p;
		TAILQ_FOREACH(p, &succ->dep_preds, dep_pred_link) {
			if (p == pred) {
				EXT2_SOFTDEP_UNLOCK(pred->dep_mp);
				return;
			}
		}
	}

	succ->dep_prereq_count++;
	TAILQ_INSERT_TAIL(&pred->dep_succs, succ, dep_succ_link);
	TAILQ_INSERT_TAIL(&succ->dep_preds, pred, dep_pred_link);

	EXT2_SOFTDEP_UNLOCK(pred->dep_mp);
}

/*
 * Remove a predecessor-successor edge.
 *
 * Removes pred from succ->dep_preds and succ from pred->dep_succs,
 * decrementing succ's prereq_count.  Safe to call from satisfy paths.
 */
void
ext2_dep_remove_dependency(struct ext2_dep *pred, struct ext2_dep *succ)
{
	struct ext2_dep *p;

	if (pred == NULL || succ == NULL)
		return;

	if (pred->dep_mp != succ->dep_mp)
		return;

	EXT2_SOFTDEP_LOCK(pred->dep_mp);

	TAILQ_FOREACH(p, &succ->dep_preds, dep_pred_link) {
		if (p == pred) {
			TAILQ_REMOVE(&succ->dep_preds, pred, dep_pred_link);
			TAILQ_REMOVE(&pred->dep_succs, succ, dep_succ_link);
			if (succ->dep_prereq_count > 0)
				succ->dep_prereq_count--;
			break;
		}
	}

	EXT2_SOFTDEP_UNLOCK(pred->dep_mp);
}

/*
 * Mark a dependency as complete and notify successors.
 *
 * Called when the underlying operation (typically a buffer write)
 * finishes successfully.
 */
void
ext2_dep_satisfy(struct ext2_dep *dep)
{
	struct ext2_softdep_mount *sd;
	struct ext2_dep *succ, *tmp;

	if (dep == NULL)
		return;

	sd = dep->dep_mp;
	if (sd == NULL)
		return;

	EXT2_SOFTDEP_LOCK(sd);

	if (dep->dep_state == EXT2_DEP_COMPLETE ||
	    dep->dep_state == EXT2_DEP_CANCELLED) {
		EXT2_SOFTDEP_UNLOCK(sd);
		return;
	}

	dep->dep_state = EXT2_DEP_COMPLETE;

	/* Notify successors: remove edge, decrement prereq_count */
	TAILQ_FOREACH_SAFE(succ, &dep->dep_succs, dep_succ_link, tmp) {
		struct ext2_dep *p;

		/* Remove dep from succ->dep_preds */
		TAILQ_FOREACH(p, &succ->dep_preds, dep_pred_link) {
			if (p == dep) {
				TAILQ_REMOVE(&succ->dep_preds, dep,
				    dep_pred_link);
				break;
			}
		}
		/* Remove succ from dep->dep_succs */
		TAILQ_REMOVE(&dep->dep_succs, succ, dep_succ_link);

		if (succ->dep_prereq_count > 0)
			succ->dep_prereq_count--;

		if (succ->dep_prereq_count == 0 &&
		    succ->dep_state == EXT2_DEP_PENDING) {
			succ->dep_state = EXT2_DEP_READY;
			if (!succ->dep_on_worklist) {
				TAILQ_INSERT_TAIL(&sd->sd_worklist, succ,
				    dep_work_link);
				succ->dep_on_worklist = true;
			}
		}
	}

	EXT2_SOFTDEP_UNLOCK(sd);
}

/*
 * Cancel a dependency: remove all graph edges, detach from buffer,
 * mark CANCELLED.  Safe to call on any state.
 *
 * A cancelled dependency does NOT satisfy its successors.  When a
 * predecessor is cancelled, its successors' prereq_count is decremented
 * but they remain blocked — the cancelled predecessor is not a
 * "satisfied prerequisite".  Successors can only become READY when
 * their remaining predecessors complete normally via ext2_dep_satisfy().
 *
 * This ensures that a failure in one part of the allocation chain
 * (e.g. ALLOCATED -> MAPPED transition failing) never causes the
 * PUBLISHED transition (which writes metadata pointing to the block)
 * to proceed.
 */
void
ext2_dep_cancel(struct ext2_dep *dep)
{
	struct ext2_softdep_mount *sd;
	struct ext2_dep *p, *s, *tmp;

	if (dep == NULL)
		return;

	sd = dep->dep_mp;
	if (sd == NULL)
		return;

	EXT2_SOFTDEP_LOCK(sd);

	dep->dep_state = EXT2_DEP_CANCELLED;

	/* Remove dep from all successor's dep_preds lists */
	TAILQ_FOREACH_SAFE(s, &dep->dep_succs, dep_succ_link, tmp) {
		TAILQ_REMOVE(&dep->dep_succs, s, dep_succ_link);
		TAILQ_FOREACH(p, &s->dep_preds, dep_pred_link) {
			if (p == dep) {
				TAILQ_REMOVE(&s->dep_preds, dep,
				    dep_pred_link);
				break;
			}
		}
		if (s->dep_prereq_count > 0)
			s->dep_prereq_count--;
	}

	/* Remove dep from all predecessor's dep_succs lists */
	TAILQ_FOREACH_SAFE(p, &dep->dep_preds, dep_pred_link, tmp) {
		TAILQ_REMOVE(&dep->dep_preds, p, dep_pred_link);
		TAILQ_FOREACH(s, &p->dep_succs, dep_succ_link) {
			if (s == dep) {
				TAILQ_REMOVE(&p->dep_succs, dep,
				    dep_succ_link);
				break;
			}
		}
	}

	/* Remove from worklist if present */
	if (dep->dep_on_worklist) {
		TAILQ_REMOVE(&sd->sd_worklist, dep, dep_work_link);
		dep->dep_on_worklist = false;
	}

	/* Detach from buffer */
	if (dep->dep_bp != NULL) {
		EXT2_BP_DEP_CLEAR(dep->dep_bp);
		dep->dep_bp = NULL;
	}

	EXT2_SOFTDEP_UNLOCK(sd);
}

/* ------------------------------------------------------------------ */
/* Buffer association                                                  */

/*
 * Attach a dependency to a buffer via b_fsprivate1.
 * A buffer has at most one primary dependency for write gating.
 */
void
ext2_dep_attach_buffer(struct ext2_dep *dep, struct buf *bp)
{
	struct ext2_dep *olddep;

	if (dep == NULL || bp == NULL)
		return;

	olddep = EXT2_BP_DEP(bp);
	KASSERT(olddep == NULL || olddep == dep,
	    ("ext2_dep_attach_buffer: buffer %p already has dep %p (type %s), "
	    "cannot attach %p (type %s)", (void *)bp, (void *)olddep,
	    olddep ? (ext2_dep_typenames[olddep->dep_type] ?
	    ext2_dep_typenames[olddep->dep_type] : "???") : "none",
	    (void *)dep,
	    ext2_dep_typenames[dep->dep_type] ?
	    ext2_dep_typenames[dep->dep_type] : "???"));

	EXT2_SET_BP_DEP(bp, dep);
	dep->dep_bp = bp;
}

void
ext2_dep_detach_buffer(struct ext2_dep *dep)
{
	if (dep == NULL || dep->dep_bp == NULL)
		return;

	EXT2_BP_DEP_CLEAR(dep->dep_bp);
	dep->dep_bp = NULL;
}

/* ------------------------------------------------------------------ */
/* Inodedep helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * Allocate an ext2_inonedep for the given inode.
 *
 * The dep is allocated as a full ext2_inonedep (which starts with
 * an ext2_dep) and stored on the inode's i_inodedep field.
 */
struct ext2_inonedep *
ext2_inonedep_alloc(struct ext2_softdep_mount *sd, struct inode *ip)
{
	struct ext2_inonedep *id;
	struct ext2_dep *dep;

	dep = ext2_dep_alloc(sd, EXT2_DEP_INODEDEP);
	if (dep == NULL)
		return (NULL);

	id = (struct ext2_inonedep *)dep;
	id->id_ino = ip->i_number;
	id->id_ip = ip;

	return (id);
}

/*
 * Attach an inonedep to an inode if one does not already exist.
 * Creates the dependency and stores it on ip->i_inodedep.
 */
void
ext2_inonedep_attach(struct ext2_softdep_mount *sd, struct inode *ip)
{
	struct ext2_inonedep *id;

	if (ip->i_inonedep != NULL)
		return;

	id = ext2_inonedep_alloc(sd, ip);
 	if (id != NULL)
 		ip->i_inonedep = id;
 }

/*
 * Get the inode's INODEDEP dependency, creating it if necessary.
 * Returns the dep, or NULL on failure.
 */
struct ext2_dep *
ext2_inode_dep_get(struct inode *ip)
{
	struct ext2_softdep_mount *sd;

	if (ip == NULL)
		return (NULL);

	sd = ip->i_e2fs->e2fs_softdep;
	if (sd == NULL)
		return (NULL);

	if (ip->i_inonedep == NULL)
		ext2_inonedep_attach(sd, ip);

	if (ip->i_inonedep != NULL)
		return (&ip->i_inonedep->id_dep);

	return (NULL);
}

/*
 * Create a NEWBLK dependency for a newly allocated block.
 *
 * Allocates a NEWBLK dep, attaches it to the block's buffer (bp),
 * and returns it.  The caller is responsible for linking this dep
 * as a prerequisite of the appropriate metadata dep.
 */
struct ext2_dep *
ext2_newblk_dep_create(struct inode *ip, struct buf *bp)
{
	struct ext2_softdep_mount *sd;
	struct ext2_dep *newblk_dep;

	if (ip == NULL || bp == NULL)
		return (NULL);

	sd = ip->i_e2fs->e2fs_softdep;
	if (sd == NULL)
		return (NULL);

	newblk_dep = ext2_dep_alloc(sd, EXT2_DEP_ALLOCDIRECT);
	if (newblk_dep == NULL)
		return (NULL);

	/* Attach the NEWBLK dep to the new block's buffer */
	ext2_dep_attach_buffer(newblk_dep, bp);

	return (newblk_dep);
}

/*
 * Get or create a METAD_PUB dep for a metadata buffer that will
 * publish a pointer to a newly allocated block.
 *
 * If the buffer already has a dep (from a prior allocation in the
 * same call), that dep is returned.  Otherwise a new METAD_PUB
 * dep is allocated, attached to the buffer, and linked as a
 * prerequisite of the inode's INODEDEP.
 *
 * This establishes the chain:
 *   NEWBLK(child) -> METAD_PUB(parent) -> INODEDEP(inode)
 *
 * Returns the metadata dep, or NULL on failure.
 */
struct ext2_dep *
ext2_metadep_get_or_create(struct inode *ip, struct buf *bp)
{
	struct ext2_softdep_mount *sd;
	struct ext2_dep *metadep;

	if (ip == NULL || bp == NULL)
		return (NULL);

	sd = ip->i_e2fs->e2fs_softdep;
	if (sd == NULL)
		return (NULL);

 	/* Check if buffer already has a dep */
	metadep = EXT2_BP_DEP(bp);
	if (metadep != NULL) {
		/*
		 * Enforce invariant: one buffer -> one primary dep.
		 * If the existing dep is already METAD_PUB, reuse it.
		 * If it's a different type, this is a programming error.
		 */
		KASSERT(metadep->dep_type == EXT2_DEP_METAD_PUB,
		    ("ext2_metadep_get_or_create: buffer %p has dep type %s, "
		    "expected METAD_PUB", (void *)bp,
		    ext2_dep_typenames[metadep->dep_type] ?
		    ext2_dep_typenames[metadep->dep_type] : "???"));
		return (metadep);
	}

	/* Ensure the inode has an INODEDEP dep */
	if (ip->i_inonedep == NULL)
		ext2_inonedep_attach(sd, ip);

	metadep = ext2_dep_alloc(sd, EXT2_DEP_METAD_PUB);
	if (metadep == NULL)
		return (NULL);

	ext2_dep_attach_buffer(metadep, bp);

	if (ip->i_inonedep != NULL) {
		struct ext2_dep *inode_dep = &ip->i_inonedep->id_dep;
		/* METAD_PUB must complete before inode dep can proceed */
		ext2_dep_add_dependency(metadep, inode_dep);
	}

	return (metadep);
}

/* ------------------------------------------------------------------ */
/* Write scheduling                                                    */
/* ------------------------------------------------------------------ */

/*
 * Check whether a buffer can currently be written.
 *
 * Caller MUST hold sd_lock (sd = dep->dep_mp).  Reads dep fields that
 * are protected by sd_lock — no internal locking.
 *
 * Returns true if the buffer has no blocking dependency, false if
 * it should be deferred.
 */
bool
ext2_can_write_buffer(struct buf *bp)
{
	struct ext2_dep *dep;
	struct ext2_softdep_mount *sd;

	if (bp == NULL)
		return (true);

	dep = EXT2_BP_DEP(bp);
	if (dep == NULL)
		return (true);

	sd = dep->dep_mp;
	KASSERT(sd != NULL, ("ext2_can_write_buffer: dep has no mount"));
	mtx_assert(&sd->sd_lock, MA_OWNED);

	/*
	 * Allow write if:
	 * - the dependency is already complete or cancelled
	 * - the dependency is ready (prereq count at zero)
	 * - the dependency has no prerequisites (prereq_count == 0)
	 *   and is still pending — nothing to wait for
	 */
	if (dep->dep_state == EXT2_DEP_COMPLETE ||
	    dep->dep_state == EXT2_DEP_CANCELLED ||
	    dep->dep_state == EXT2_DEP_READY ||
	    dep->dep_prereq_count == 0)
		return (true);

	return (false);
}

/*
 * Drive prerequisite writes synchronously before a buffer can be written.
 *
 * When ext2_dep_bwrite() is called on a buffer whose dep has unsatisfied
 * prerequisites, this function recursively writes the prerequisites'
 * buffers until the target dep's prereq_count reaches 0.
 *
 * Lock handling:
 *   - sd_lock is held briefly to inspect dep state, then released
 *     around bwrite() (which may sleep).
 *   - The recursive ext2_dep_bwrite() call handles its own locking.
 *
 * Returns 0 on success or an error code if a prerequisite write fails.
 */
int
ext2_drive_prerequisites(struct ext2_dep *dep)
{
	struct ext2_softdep_mount *sd;
	struct ext2_dep *prereq;
	struct buf *prereq_bp;
	int error = 0;

	if (dep == NULL || dep->dep_mp == NULL)
		return (0);

	sd = dep->dep_mp;

	for (;;) {
		EXT2_SOFTDEP_LOCK(sd);

		/* Already complete or cancelled - nothing to drive */
		if (dep->dep_state == EXT2_DEP_COMPLETE ||
		    dep->dep_state == EXT2_DEP_CANCELLED) {
			EXT2_SOFTDEP_UNLOCK(sd);
			return (0);
		}

		/* All prerequisites satisfied - dep is ready */
		if (dep->dep_prereq_count == 0) {
			if (dep->dep_state == EXT2_DEP_PENDING)
				dep->dep_state = EXT2_DEP_READY;
			EXT2_SOFTDEP_UNLOCK(sd);
			return (0);
		}

		/*
		 * Find a predecessor that hasn't completed yet.
		 */
		prereq = NULL;
		TAILQ_FOREACH(prereq, &dep->dep_preds, dep_pred_link) {
			if (prereq->dep_state != EXT2_DEP_COMPLETE &&
			    prereq->dep_state != EXT2_DEP_CANCELLED)
				break;
		}

		if (prereq == NULL) {
			/*
			 * All visible prereqs appear done but count > 0.
			 * This is a graph invariant violation.
			 */
#ifdef INVARIANTS
			KASSERT(false, ("ext2_drive_prerequisites: prereq_count "
			    "=%u but no unsatisfied predecessor found on dep %p "
			    "(type %s)", dep->dep_prereq_count, (void *)dep,
			    ext2_dep_typenames[dep->dep_type] ?
			    ext2_dep_typenames[dep->dep_type] : "???"));
#else
			/*
			 * In production, fail closed: cancel the dep and
			 * return an error rather than silently proceeding
			 * with a potentially inconsistent state.
			 */
			EXT2_SOFTDEP_UNLOCK(sd);
			ext2_dep_cancel(dep);
			return (EIO);
#endif
		}

		/*
		 * If the prereq has no buffer, satisfy it directly —
		 * the cascade in ext2_dep_satisfy() will notify our dep.
		 */
		if (prereq->dep_bp == NULL) {
			EXT2_SOFTDEP_UNLOCK(sd);
			ext2_dep_satisfy(prereq);
			continue;
		}

		/*
		 * Hold a ref on the prereq so it can't be freed while
		 * we release sd_lock and call bwrite.
		 */
		ext2_dep_ref(prereq);
		prereq_bp = prereq->dep_bp;
		EXT2_SOFTDEP_UNLOCK(sd);

		/*
		 * Recursively write the prereq's buffer.  This will:
		 *   - drive ITS prerequisites
		 *   - bwrite the prereq's buffer
		 *   - ext2_dep_satisfy(prereq) on success
		 *     (which decrements dep->dep_prereq_count)
		 */
		error = ext2_dep_bwrite(prereq_bp);

		/* Drop our reference */
		ext2_dep_rele(prereq);

		if (error) {
			ext2_dep_cancel(dep);
			return (error);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Worklist management */
/* ------------------------------------------------------------------ */

/*
 * Enqueue a dependency on the worklist if it is ready (prereq_count == 0)
 * and currently pending.
 */
void
ext2_worklist_insert(struct ext2_softdep_mount *sd, struct ext2_dep *dep)
{
	if (dep == NULL || dep->dep_mp != sd)
		return;

	EXT2_SOFTDEP_LOCK(sd);

	if (dep->dep_prereq_count == 0 &&
	    dep->dep_state == EXT2_DEP_PENDING &&
	    !dep->dep_on_worklist) {
		dep->dep_state = EXT2_DEP_READY;
		TAILQ_INSERT_TAIL(&sd->sd_worklist, dep, dep_work_link);
		dep->dep_on_worklist = true;
	}

	EXT2_SOFTDEP_UNLOCK(sd);
}

/*
 * Remove a dependency from the worklist.
 */
void
ext2_worklist_remove(struct ext2_softdep_mount *sd, struct ext2_dep *dep)
{
	if (dep == NULL || dep->dep_mp != sd)
		return;

	EXT2_SOFTDEP_LOCK(sd);

	if (dep->dep_on_worklist) {
		TAILQ_REMOVE(&sd->sd_worklist, dep, dep_work_link);
		dep->dep_on_worklist = false;
		if (dep->dep_state == EXT2_DEP_READY)
			dep->dep_state = EXT2_DEP_PENDING;
	}

	EXT2_SOFTDEP_UNLOCK(sd);
}

/*
 * Process all dependencies on the worklist.
 *
 * For each READY dep that has a buffer, write the buffer via
 * ext2_dep_bwrite().  This is the ONLY path that transitions a
 * dep to COMPLETE — ext2_dep_bwrite calls ext2_drive_prerequisites()
 * to write prerequisites, then bwrite(), then ext2_dep_satisfy()
 * on success (or ext2_dep_cancel() on error).
 *
 * READY deps without a buffer are left for ext2_dep_satisfy() to
 * handle via its successor-notification cascade; they are simply
 * removed from the worklist.
 */
void
ext2_worklist_process(struct ext2_softdep_mount *sd)
{
	struct ext2_dep *dep;
	struct buf *bp;
	uint32_t processed = 0;

	if (sd == NULL)
		return;

	EXT2_SOFTDEP_LOCK(sd);

	while ((dep = TAILQ_FIRST(&sd->sd_worklist)) != NULL) {
		if (dep->dep_state != EXT2_DEP_READY) {
			TAILQ_REMOVE(&sd->sd_worklist, dep, dep_work_link);
			dep->dep_on_worklist = false;
			continue;
		}

		/*
		 * Only process deps that have a buffer to write.
		 * READY deps without a buffer have no operation to
		 * perform; they remain for ext2_dep_satisfy() cascade.
		 */
		if (dep->dep_bp == NULL) {
			TAILQ_REMOVE(&sd->sd_worklist, dep, dep_work_link);
			dep->dep_on_worklist = false;
			continue;
		}

		/*
		 * Take a reference so dep survives while we
		 * release sd_lock and perform bwrite().
		 */
		ext2_dep_ref(dep);
		bp = dep->dep_bp;
		TAILQ_REMOVE(&sd->sd_worklist, dep, dep_work_link);
		dep->dep_on_worklist = false;
		dep->dep_state = EXT2_DEP_WRITING;

		EXT2_SOFTDEP_UNLOCK(sd);

		/*
		 * ext2_dep_bwrite is the authoritative path to COMPLETE:
		 * it drives prerequisites, writes the buffer, and calls
		 * ext2_dep_satisfy on success or ext2_dep_cancel on error.
		 */
		(void)ext2_dep_bwrite(bp);

		ext2_dep_rele(dep);
		processed++;

		EXT2_SOFTDEP_LOCK(sd);
	}

	EXT2_SOFTDEP_UNLOCK(sd);
}

/* ------------------------------------------------------------------ */
/* Orphan list operations (FROZEN - see ext2_orphan_recovery below)   */
/* ------------------------------------------------------------------ */

/*
 * Add an inode to the orphan list.
 *
 * FROZEN: Orphan management currently uses raw bwrite() bypassing the
 * dependency engine, creating a second metadata-ordering mechanism.
 * This is deferred until the core allocation/free dependency chains
 * are fully validated.  Returns 0 as a safe no-op.
 */
int
ext2_orphan_add(struct inode *ip)
{
	return (0);
}

/*
 * Remove an inode from the orphan list.
 *
 * FROZEN: See ext2_orphan_add() comment.  Returns 0 as a safe no-op.
 */
int
ext2_orphan_remove(struct inode *ip)
{
	return (0);
}

/*
 * Orphan recovery at mount time.
 *
 * FROZEN: Orphan recovery traverses the on-disk orphan list and cleans
 * up orphans left by unclean shutdowns.  This path uses raw bwrite()
 * and ext2_truncate() outside the dependency engine, creating
 * consistency risks with the softdep model.
 *
 * Recovery should be integrated with the same dependency machinery
 * (newblk/metadep chains) once the core allocation/free paths are
 * proven correct.  For now, this is a no-op; orphaned inodes from a
 * crash will simply consume space until manually reclaimed.
 */
void
ext2_orphan_recovery(struct mount *mp)
{
	return;
}

/* ------------------------------------------------------------------ */
/* Debugging                                                           */
/* ------------------------------------------------------------------ */

#ifdef INVARIANTS
void
ext2_dep_assert_valid(struct ext2_dep *dep)
{
	KASSERT(dep != NULL, ("ext2_dep_assert_valid: NULL dep"));
	KASSERT(dep->dep_type > EXT2_DEP_UNUSED && dep->dep_type < EXT2_DEP_MAX,
	    ("ext2_dep_assert_valid: invalid type %d", dep->dep_type));
	KASSERT(dep->dep_refcnt > 0,
	    ("ext2_dep_assert_valid: refcnt %d", dep->dep_refcnt));
	KASSERT(dep->dep_mp != NULL,
	    ("ext2_dep_assert_valid: NULL mount"));
}
#endif
