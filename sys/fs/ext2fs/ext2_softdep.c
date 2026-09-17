/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * ext2fs Soft-Updates-style dependency system implementation.
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
#include <sys/sdt.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_softdep.h>
#include <fs/ext2fs/ext2_extents.h>

/* MALLOC_DECLARE for ext2_softdep */
MALLOC_DECLARE(M_EXT2SOFTDEP);

static struct ext2_softdep_mount *ext2_softdep_mount_head = NULL;
static struct mtx ext2_softdep_global_lock;
static int ext2_softdep_initialized = 0;

/*
 * Dependency type names for debugging
 */
static const char *ext2_dep_type_names[] = {
	[EXT2_DEP_UNUSED]	= "UNUSED",
	[EXT2_DEP_NEWBLK]	= "NEWBLK",
	[EXT2_DEP_ALLOCDIRECT]	= "ALLOCDIRECT",
	[EXT2_DEP_INDIRDEP]	= "INDIRECTDEP",
	[EXT2_DEP_ALLOCINDIR]	= "ALLOCINDIRECT",
	[EXT2_DEP_EXTENTDEP]	= "EXTENTDEP",
	[EXT2_DEP_ALLOCEXTENT]	= "ALLOCEXTENT",
	[EXT2_DEP_INODEDEP]	= "INODEDEP",
	[EXT2_DEP_BMSAFEMAP]	= "BMSAFEMAP",
	[EXT2_DEP_FREEBLKS]	= "FREEBLKS",
	[EXT2_DEP_FREEFILE]	= "FREEFILE",
	[EXT2_DEP_DIRADD]	= "DIRADD",
	[EXT2_DEP_DIRREM]	= "DIRREM",
	[EXT2_DEP_MKDIR]	= "MKDIR",
	[EXT2_DEP_SBDEP]	= "SBDEP",
	[EXT2_DEP_ORPHAN_ADD]	= "ORPHAN_ADD",
	[EXT2_DEP_ORPHAN_REMOVE] = "ORPHAN_REMOVE",
};

/*
 * Initialize the softdep subsystem
 */
void
ext2_softdep_initialize(void)
{
	if (ext2_softdep_initialized)
		return;

	mtx_init(&ext2_softdep_global_lock, "ext2softdep", NULL, MTX_DEF);
	ext2_softdep_mount_head = NULL;
	ext2_softdep_initialized = 1;

	printf("ext2_softdep: initialized\n");
}

/*
 * Uninitialize the softdep subsystem
 */
void
ext2_softdep_uninitialize(void)
{
	if (!ext2_softdep_initialized)
		return;

	mtx_destroy(&ext2_softdep_global_lock);
	ext2_softdep_initialized = 0;

	printf("ext2_softdep: uninitialized\n");
}

/*
 * Mount initialization - allocate per-mount softdep state
 */
int
ext2_softdep_mount(struct mount *mp, struct m_ext2fs *fs)
{
	struct ext2_softdep_mount *sd;

	if (!ext2_softdep_initialized)
		return (ENODEV);

	sd = malloc(sizeof(*sd), M_EXT2SOFTDEP, M_WAITOK | M_ZERO);
	if (sd == NULL)
		return (ENOMEM);

	sd->sd_mp = mp;
	mtx_init(&sd->sd_lock, "ext2sdlock", NULL, MTX_DEF);
	mtx_init(&sd->sd_orphan_lock, "ext2orphlock", NULL, MTX_DEF);
	mtx_init(&sd->sd_deferred_lock, "ext2deflock", MTX_DEF);

	for (int i = 0; i < EXT2_DEP_MAX; i++)
		LIST_INIT(&sd->sd_worklist[i]);
	TAILQ_INIT(&sd->sd_all_deps);
	TAILQ_INIT(&sd->sd_deferred_writes);

	sd->sd_shutting_down = false;

	/* Store in mount private data */
	VFSTOEXT2(mp)->e2fs_softdep = sd;

	/* Add to global list */
	mtx_lock(&ext2_softdep_global_lock);
	/* Could maintain a global list if needed */
	mtx_unlock(&ext2_softdep_global_lock);

	printf("ext2_softdep: mounted on %s\n", mp->mnt_stat.f_mntonname);

	return (0);
}

/*
 * Unmount - drain and free all dependencies
 */
void
ext2_softdep_unmount(struct mount *mp)
{
	struct ext2_softdep_mount *sd;

	sd = VFSTOEXT2(mp)->e2fs_softdep;
	if (sd == NULL)
		return;

	EXT2_SOFTDEP_LOCK(sd);
	sd->sd_shutting_down = true;

	/* Flush all deferred writes */
	ext2_flush_deferred_writes(sd);

	/* Process all pending work items */
	ext2_worklist_process(sd);

	/* Cancel remaining dependencies */
	struct ext2_dep *dep;
	while ((dep = TAILQ_FIRST(&sd->sd_all_deps)) != NULL) {
		TAILQ_REMOVE(&sd->sd_all_deps, dep, dep_list);
		ext2_dep_cancel(dep);
		ext2_dep_free(dep);
	}

	EXT2_SOFTDEP_UNLOCK(sd);

	mtx_destroy(&sd->sd_lock);
	mtx_destroy(&sd->sd_orphan_lock);
	mtx_destroy(&sd->sd_deferred_lock);

	VFSTOEXT2(mp)->e2fs_softdep = NULL;
	free(sd, M_EXT2SOFTDEP);

	printf("ext2_softdep: unmounted from %s\n", mp->mnt_stat.f_mntonname);
}

/*
 * Allocate a dependency object
 */
struct ext2_dep *
ext2_dep_alloc(struct ext2_softdep_mount *sd, ext2_dep_type_t type)
{
	struct ext2_dep *dep;

	if (type <= EXT2_DEP_UNUSED || type >= EXT2_DEP_MAX) {
		panic("ext2_dep_alloc: invalid type %d", type);
	}

	dep = malloc(sizeof(*dep), M_EXT2SOFTDEP, M_WAITOK | M_ZERO);
	if (dep == NULL)
		return (NULL);

	dep->dep_mp = sd;
	dep->dep_type = type;
	dep->dep_state = EXT2_DEP_ATTACHED;
	dep->dep_refcnt = 1;

	LIST_INIT(&dep->dep_deps);

	/* Add to mount's all_deps list */
	EXT2_SOFTDEP_LOCK(sd);
	if (!sd->sd_shutting_down) {
		TAILQ_INSERT_TAIL(&sd->sd_all_deps, dep, dep_list);
		sd->sd_deps_allocated++;
	} else {
		/* Mount shutting down, don't register */
		free(dep, M_EXT2SOFTDEP);
		EXT2_SOFTDEP_UNLOCK(sd);
		return (NULL);
	}
	EXT2_SOFTDEP_UNLOCK(sd);

	return (dep);
}

/*
 * Reference counting
 */
void
ext2_dep_ref(struct ext2_dep *dep)
{
	KASSERT(dep != NULL, ("ext2_dep_ref: NULL dep"));
	atomic_add_int(&dep->dep_refcnt, 1);
}

void
ext2_dep_rele(struct ext2_dep *dep)
{
	KASSERT(dep != NULL, ("ext2_dep_rele: NULL dep"));

	if (atomic_fetchadd_int(&dep->dep_refcnt, -1) == 1) {
		ext2_dep_free(dep);
	}
}

/*
 * Free a dependency object
 */
void
ext2_dep_free(struct ext2_dep *dep)
{
	struct ext2_softdep_mount *sd;

	KASSERT(dep != NULL, ("ext2_dep_free: NULL dep"));
	KASSERT(dep->dep_refcnt == 0, ("ext2_dep_free: refcnt %d", dep->dep_refcnt));

	sd = dep->dep_mp;

	EXT2_SOFTDEP_LOCK(sd);

	/* Remove from all_deps if still there */
	if (dep->dep_list.le_next != NULL || dep->dep_list.le_prev != NULL) {
		TAILQ_REMOVE(&sd->sd_all_deps, dep, dep_list);
		sd->sd_deps_freed++;
	}

	EXT2_SOFTDEP_UNLOCK(sd);

	free(dep, M_EXT2SOFTDEP);
}

/*
 * Cancel a dependency (mark as cancelled, remove from worklists)
 */
void
ext2_dep_cancel(struct ext2_dep *dep)
{
	struct ext2_softdep_mount *sd;

	KASSERT(dep != NULL, ("ext2_dep_cancel: NULL dep"));

	sd = dep->dep_mp;

	EXT2_SOFTDEP_LOCK(sd);
	dep->dep_state |= EXT2_DEP_CANCELLED | EXT2_DEP_GOINGAWAY;
	dep->dep_state &= ~EXT2_DEP_ONWORKLIST;
	ext2_worklist_remove(sd, dep);
	EXT2_SOFTDEP_UNLOCK(sd);
}

/*
 * State management
 */
void
ext2_dep_set_state(struct ext2_dep *dep, uint32_t flags)
{
	KASSERT(dep != NULL, ("ext2_dep_set_state: NULL dep"));
	dep->dep_state |= flags;
}

void
ext2_dep_clear_state(struct ext2_dep *dep, uint32_t flags)
{
	KASSERT(dep != NULL, ("ext2_dep_clear_state: NULL dep"));
	dep->dep_state &= ~flags;
}

bool
ext2_dep_has_state(struct ext2_dep *dep, uint32_t flags)
{
	KASSERT(dep != NULL, ("ext2_dep_has_state: NULL dep"));
	return ((dep->dep_state & flags) == flags);
}

bool
ext2_dep_is_complete(struct ext2_dep *dep)
{
	KASSERT(dep != NULL, ("ext2_dep_is_complete: NULL dep"));
	return ((dep->dep_state & EXT2_DEP_ALLCOMPLETE) == EXT2_DEP_ALLCOMPLETE);
}

/*
 * Dependency graph management
 */
void
ext2_dep_add_dependency(struct ext2_dep *pred, struct ext2_dep *succ)
{
	KASSERT(pred != NULL && succ != NULL, ("ext2_dep_add_dependency: NULL dep"));
	KASSERT(pred->dep_mp == succ->dep_mp, ("ext2_dep_add_dependency: mount mismatch"));

	/* Add succ to pred's dependent list */
	EXT2_SOFTDEP_LOCK(pred->dep_mp);
	LIST_INSERT_HEAD(&pred->dep_deps, succ, dep_deps);
	EXT2_SOFTDEP_UNLOCK(pred->dep_mp);
}

void
ext2_dep_remove_dependency(struct ext2_dep *pred, struct ext2_dep *succ)
{
	KASSERT(pred != NULL && succ != NULL, ("ext2_dep_remove_dependency: NULL dep"));
	KASSERT(pred->dep_mp == succ->dep_mp, ("ext2_dep_remove_dependency: mount mismatch"));

	EXT2_SOFTDEP_LOCK(pred->dep_mp);
	LIST_REMOVE(succ, dep_deps);
	EXT2_SOFTDEP_UNLOCK(pred->dep_mp);
}

void
ext2_dep_satisfy(struct ext2_dep *dep)
{
	KASSERT(dep != NULL, ("ext2_dep_satisfy: NULL dep"));

	ext2_dep_set_state(dep, EXT2_DEP_COMPLETE | EXT2_DEP_DEPCOMPLETE);

	/* Process any deferred writes waiting for this dependency */
	if (dep->dep_mp != NULL)
		ext2_process_deferred_writes(dep->dep_mp);

	/* If all complete and no references, free */
	if (ext2_dep_is_complete(dep) && dep->dep_refcnt == 1) {
		ext2_dep_rele(dep);
	}
}

/*
 * Worklist management
 */
void
ext2_worklist_insert(struct ext2_softdep_mount *sd, struct ext2_dep *dep)
{
	KASSERT(dep != NULL, ("ext2_worklist_insert: NULL dep"));
	KASSERT(dep->dep_mp == sd, ("ext2_worklist_insert: mount mismatch"));

	EXT2_SOFTDEP_LOCK(sd);
	if (!sd->sd_shutting_down) {
		LIST_INSERT_HEAD(&sd->sd_worklist[dep->dep_type], dep, dep_list);
		dep->dep_state |= EXT2_DEP_ONWORKLIST;
	}
	EXT2_SOFTDEP_UNLOCK(sd);
}

void
ext2_worklist_remove(struct ext2_softdep_mount *sd, struct ext2_dep *dep)
{
	KASSERT(dep != NULL, ("ext2_worklist_remove: NULL dep"));
	KASSERT(dep->dep_mp == sd, ("ext2_worklist_remove: mount mismatch"));

	EXT2_SOFTDEP_LOCK(sd);
	if (dep->dep_state & EXT2_DEP_ONWORKLIST) {
		LIST_REMOVE(dep, dep_list);
		dep->dep_state &= ~EXT2_DEP_ONWORKLIST;
	}
	EXT2_SOFTDEP_UNLOCK(sd);
}

void
ext2_worklist_process(struct ext2_softdep_mount *sd)
{
	KASSERT(sd != NULL, ("ext2_worklist_process: NULL sd"));

	EXT2_SOFTDEP_LOCK(sd);

	for (int type = 0; type < EXT2_DEP_MAX; type++) {
		struct ext2_dep *dep, *next;

		LIST_FOREACH_SAFE(dep, &sd->sd_worklist[type], dep_list, next) {
			if (dep->dep_state & EXT2_DEP_CANCELLED)
				continue;

			if (dep->dep_state & EXT2_DEP_INPROGRESS)
				continue;

			dep->dep_state |= EXT2_DEP_INPROGRESS;

			/* Process the work item - for now just mark complete */
			ext2_dep_satisfy(dep);

			sd->sd_workitems_processed++;
		}
	}

	EXT2_SOFTDEP_UNLOCK(sd);
}

/*
 * Buffer association
 */
void
ext2_dep_attach_buffer(struct ext2_dep *dep, struct buf *bp)
{
	KASSERT(dep != NULL && bp != NULL, ("ext2_dep_attach_buffer: NULL args"));

	/* Store dependency in buffer's b_dep field */
	bp->b_dep = dep;
}

void
ext2_dep_detach_buffer(struct ext2_dep *dep)
{
	KASSERT(dep != NULL, ("ext2_dep_detach_buffer: NULL dep"));
	/* Buffer will clear b_dep on write complete */
}

void
ext2_buf_write_start(struct buf *bp)
{
	KASSERT(bp != NULL, ("ext2_buf_write_start: NULL bp"));

	if (bp->b_dep != NULL) {
		struct ext2_dep *dep = bp->b_dep;
		ext2_dep_set_state(dep, EXT2_DEP_UNDONE);
	}
}

void
ext2_buf_write_complete(struct buf *bp, int error)
{
	KASSERT(bp != NULL, ("ext2_buf_write_complete: NULL bp"));

	if (bp->b_dep != NULL) {
		struct ext2_dep *dep = bp->b_dep;

		if (error == 0) {
			ext2_dep_clear_state(dep, EXT2_DEP_UNDONE);
			ext2_dep_set_state(dep, EXT2_DEP_COMPLETE);

			/* Check if all dependencies satisfied */
			if (ext2_dep_is_complete(dep)) {
				ext2_dep_satisfy(dep);
			}
		} else {
			/* Write error - rollback will be handled by caller */
			ext2_dep_clear_state(dep, EXT2_DEP_UNDONE);
			ext2_dep_set_state(dep, EXT2_DEP_CANCELLED);
		}

		bp->b_dep = NULL;
	}
}

/*
 * Orphan list operations
 */
int
ext2_orphan_add(struct inode *ip)
{
	struct m_ext2fs *fs = ip->i_e2fs;
	struct ext2_softdep_mount *sd = fs->e2fs_softdep;
	struct buf *ibp = NULL, *sbp = NULL;
	int error = 0;

	if (sd == NULL)
		return (ENODEV);

	mtx_lock(&sd->sd_orphan_lock);

	/* Read inode buffer */
	error = bread(ip->i_devvp,
	    fsbtodb(fs, ino_to_fsba(fs, ip->i_number)),
	    (int)fs->e2fs_bsize, NOCRED, &ibp);
	if (error)
		goto out;

	/* Read superblock buffer */
	error = bread(ip->i_devvp, fsbtodb(fs, 1),
	    (int)fs->e2fs_bsize, NOCRED, &sbp);
	if (error)
		goto out;

	/* Save current orphan head */
	uint32_t old_head = fs->e2fs.e3fs_last_orphan;

	/* Update inode's i_dtime to point to old head */
	struct ext2fs_dinode *dip = (struct ext2fs_dinode *)((char *)ibp->b_data +
	    EXT2_INODE_SIZE(fs) * ino_to_fsbo(fs, ip->i_number));
	dip->e2di_dtime = htole32(old_head);

	/* Update superblock's s_last_orphan */
	fs->e2fs.e3fs_last_orphan = ip->i_number;
	ext2_sbupdate(ip->i_ump, MNT_WAIT);

	/* Write buffers - for now use synchronous to preserve ordering */
	if ((error = bwrite(ibp)) != 0)
		goto out;
	ibp = NULL;

	if ((error = bwrite(sbp)) != 0)
		goto out;
	sbp = NULL;

	/* Update in-memory state */
	fs->e2fs_softdep->sd_orphan_head = ip->i_number;

out:
	if (ibp)
		brelse(ibp);
	if (sbp)
		brelse(sbp);

	mtx_unlock(&sd->sd_orphan_lock);
	return (error);
}

int
ext2_orphan_remove(struct inode *ip)
{
	struct m_ext2fs *fs = ip->i_e2fs;
	struct ext2_softdep_mount *sd = fs->e2fs_softdep;
	struct buf *ibp = NULL, *prev_bp = NULL, *sbp = NULL;
	int error = 0;

	if (sd == NULL)
		return (ENODEV);

	mtx_lock(&sd->sd_orphan_lock);

	/* Read inode buffer to get next pointer */
	error = bread(ip->i_devvp,
	    fsbtodb(fs, ino_to_fsba(fs, ip->i_number)),
	    (int)fs->e2fs_bsize, NOCRED, &ibp);
	if (error)
		goto out;

	struct ext2fs_dinode *dip = (struct ext2fs_dinode *)((char *)ibp->b_data +
	    EXT2_INODE_SIZE(fs) * ino_to_fsbo(fs, ip->i_number));
	uint32_t next_ino = le32toh(dip->e2di_dtime);

	/* Find previous inode in list (or superblock if head) */
	if (fs->e2fs.e3fs_last_orphan == ip->i_number) {
		/* We are the head */
		fs->e2fs.e3fs_last_orphan = next_ino;
		if ((error = ext2_sbupdate(ip->i_ump, MNT_WAIT)) != 0)
			goto out;
	} else {
		/* Need to find predecessor - traverse list */
		ino_t prev_ino = fs->e2fs.e3fs_last_orphan;
		ino_t cur_ino = prev_ino;

		while (cur_ino != 0 && cur_ino != ip->i_number) {
			if (prev_bp)
				brelse(prev_bp);
			prev_ino = cur_ino;

			error = bread(ip->i_devvp,
			    fsbtodb(fs, ino_to_fsba(fs, cur_ino)),
			    (int)fs->e2fs_bsize, NOCRED, &prev_bp);
			if (error)
				goto out;

			dip = (struct ext2fs_dinode *)((char *)prev_bp->b_data +
			    EXT2_INODE_SIZE(fs) * ino_to_fsbo(fs, cur_ino));
			cur_ino = le32toh(dip->e2di_dtime);
		}

		if (cur_ino == ip->i_number) {
			/* Found predecessor */
			dip->e2di_dtime = htole32(next_ino);
			if ((error = bwrite(prev_bp)) != 0)
				goto out;
		} else {
			/* Not found in list - inconsistency */
			error = EIO;
			goto out;
		}
	}

	/* Clear i_dtime */
	dip = (struct ext2fs_dinode *)((char *)ibp->b_data +
	    EXT2_INODE_SIZE(fs) * ino_to_fsbo(fs, ip->i_number));
	dip->e2di_dtime = 0;
	if ((error = bwrite(ibp)) != 0)
		goto out;

out:
	if (ibp)
		brelse(ibp);
	if (prev_bp)
		brelse(prev_bp);
	if (sbp)
		brelse(sbp);

	mtx_unlock(&sd->sd_orphan_lock);
	return (error);
}

/*
 * Orphan recovery at mount time
 */
void
ext2_orphan_recovery(struct mount *mp)
{
	struct m_ext2fs *fs = VFSTOEXT2(mp);
	struct ext2_softdep_mount *sd = fs->e2fs_softdep;
	ino_t ino = fs->e2fs.e3fs_last_orphan;
	int error = 0;
	int iterations = 0;

	if (sd == NULL || ino == 0)
		return;

	printf("ext2_softdep: recovering orphan list (head=%ju)\n", (uintmax_t)ino);

	/* Bound traversal to prevent infinite loops */
	while (ino != 0 && iterations < EXT4_MAX_ORPHAN_RECOVERY) {
		iterations++;

		struct vnode *vp;
		error = ext2_vget(mp, ino, LK_EXCLUSIVE, &vp);
		if (error) {
			if (error == ENOENT) {
				/* Inode doesn't exist, remove from orphan list */
				struct inode *ip = NULL;
				/* We can't easily remove without inode, just advance */
				ino = fs->e2fs.e3fs_last_orphan;
				continue;
			}
			printf("ext2_softdep: orphan recovery: vget failed for ino %ju: %d\n",
			    (uintmax_t)ino, error);
			break;
		}

		struct inode *ip = VTOI(vp);

		/* Verify orphan linkage */
		if ((ip->i_mode == 0) || (ip->i_nlink > 0)) {
			/* Not a valid orphan - clear from list */
			ext2_orphan_remove(ip);
		} else {
			/* Truncate and free the orphan */
			ext2_truncate(vp, (off_t)0, IO_SYNC, NOCRED, curthread);
			ext2_vfree(vp, ip->i_number, ip->i_mode);
		}

		vput(vp);

		/* Read next orphan from updated superblock */
		ino = fs->e2fs.e3fs_last_orphan;
	}

	if (iterations >= EXT4_MAX_ORPHAN_RECOVERY) {
		printf("ext2_softdep: WARNING: orphan recovery exceeded max iterations\n");
	}

	if (fs->e2fs.e3fs_last_orphan != 0) {
		printf("ext2_softdep: orphan list not empty after recovery (head=%ju)\n",
		    (uintmax_t)fs->e2fs.e3fs_last_orphan);
	}

	/* Mark superblock clean if recovery completed */
	if (fs->e2fs.e3fs_last_orphan == 0) {
		fs->e2fs_fmod = 1;
		ext2_update(ITOV(fs->e2fs->e2fs_root), 1);
	}
}

if (fs->e2fs.e3fs_last_orphan == 0) {
		fs->e2fs_fmod = 1;
		ext2_update(ITOV(fs->e2fs->e2fs_root), 1);
	}
}

/*
 * Deferred write management
 */

/*
 * Check if a buffer can be written. Returns true if the buffer has
 * no blocking dependencies, false if it should be deferred.
 */
bool
ext2_can_write_buffer(struct buf *bp)
{
	if (bp->b_dep == NULL)
		return (true);

	struct ext2_dep *dep = bp->b_dep;

	/* If dependency is complete or cancelled, allow write */
	if (ext2_dep_is_complete(dep) || (dep->dep_state & EXT2_DEP_CANCELLED))
		return (true);

	/* If dependency is attached and not being written, block */
	if (dep->dep_state & EXT2_DEP_ATTACHED)
		return (false);

	return (true);
}

/*
 * Defer a buffer write due to unsatisfied dependency.
 * The buffer will be written when the dependency is satisfied.
 */
void
ext2_defer_buffer_write(struct buf *bp, struct ext2_dep *dep)
{
	struct ext2_softdep_mount *sd = dep->dep_mp;
	struct ext2_deferred_write *dw;

	KASSERT(bp != NULL && dep != NULL, ("ext2_defer_buffer_write: NULL args"));
	KASSERT(dep->dep_mp != NULL, ("ext2_defer_buffer_write: NULL mount"));

	/* Attach dependency to buffer if not already attached */
	if (bp->b_dep == NULL)
		bp->b_dep = dep;

	dw = malloc(sizeof(*dw), M_EXT2SOFTDEP, M_WAITOK | M_ZERO);
	if (dw == NULL) {
		/* Allocation failed - force write anyway */
		return;
	}

	dw->dw_bp = bp;
	dw->dw_dep = dep;
	dw->dw_callback = NULL; /* Will be called by normal completion */

	mtx_lock(&dep->dep_mp->sd_deferred_lock);
	TAILQ_INSERT_TAIL(&dep->dep_mp->sd_deferred_writes, dw, dw_list);
	dep->dep_mp->sd_writes_deferred++;
	mtx_unlock(&dep->dep_mp->sd_deferred_lock);
}

/*
 * Process deferred writes whose dependencies are now satisfied.
 * Called from ext2_dep_satisfy() and worklist processing.
 */
void
ext2_process_deferred_writes(struct ext2_softdep_mount *sd)
{
	struct ext2_deferred_write *dw, *next;

	mtx_lock(&sd->sd_deferred_lock);

	TAILQ_FOREACH_SAFE(dw, &sd->sd_deferred_writes, dw_list, next) {
		if (dw->dw_dep == NULL || ext2_can_write_buffer(dw->dw_bp)) {
			/* Dependency satisfied - write the buffer */
			TAILQ_REMOVE(&sd->sd_deferred_writes, dw, dw_list);
			sd->sd_writes_completed++;

			struct buf *bp = dw->dw_bp;
			free(dw, M_EXT2SOFTDEP);

			/* Release the lock before writing */
			mtx_unlock(&sd->sd_deferred_lock);

			/* Write the buffer - use bawrite for async */
			bawrite(bp);

			mtx_lock(&sd->sd_deferred_lock);
		}
	}

	mtx_unlock(&sd->sd_deferred_lock);
}

/*
 * Flush all deferred writes - used during unmount/shutdown.
 * Forces writes of all deferred buffers regardless of dependency state.
 */
void
ext2_flush_deferred_writes(struct ext2_softdep_mount *sd)
{
	struct ext2_deferred_write *dw, *next;

	mtx_lock(&sd->sd_deferred_lock);

	TAILQ_FOREACH_SAFE(dw, &sd->sd_deferred_writes, dw_list, next) {
		TAILQ_REMOVE(&sd->sd_deferred_writes, dw, dw_list);
		sd->sd_writes_completed++;

		struct buf *bp = dw->dw_bp;
		free(dw, M_EXT2SOFTDEP);

		mtx_unlock(&sd->sd_deferred_lock);

		/* Force synchronous write for shutdown */
		bwrite(bp);

		mtx_lock(&sd->sd_deferred_lock);
	}

	mtx_unlock(&sd->sd_deferred_lock);
}

/*
 * Inode dependency lookup
 */
struct ext2_inodedep *
ext2_inodedep_lookup(struct ext2_softdep_mount *sd, ino_t ino)
{
	/* TODO: implement hash table lookup */
	return (NULL);
}

struct ext2_inodedep *
ext2_inodedep_lookup_ip(struct inode *ip)
{
	/* TODO: implement using inode's softdep pointer */
	return (NULL);
}

/*
 * Debugging
 */
#ifdef INVARIANTS
void
ext2_dep_assert_valid(struct ext2_dep *dep)
{
	KASSERT(dep != NULL, ("ext2_dep_assert_valid: NULL dep"));
	KASSERT(dep->dep_type > EXT2_DEP_UNUSED && dep->dep_type < EXT2_DEP_MAX,
	    ("ext2_dep_assert_valid: invalid type %d", dep->dep_type));
	KASSERT(dep->dep_refcnt > 0, ("ext2_dep_assert_valid: refcnt %d", dep->dep_refcnt));
	KASSERT(dep->dep_mp != NULL, ("ext2_dep_assert_valid: NULL mount"));
}
#endif