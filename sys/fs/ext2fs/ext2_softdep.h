/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * This file defines the ext2fs Soft-Updates-style dependency system.
 * It is an ext2-native implementation inspired by UFS Soft Updates concepts
 * but adapted for ext2/ext3/ext4 on-disk format and FreeBSD VFS.
 */

#ifndef _FS_EXT2FS_EXT2_SOFTDEP_H_
#define _FS_EXT2FS_EXT2_SOFTDEP_H_

#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mount.h>
#include <sys/buf.h>
#include <sys/vnode.h>

/* Forward declarations */
struct ext2_softdep_mount;
struct ext2_dep;
struct ext2_inodedep;
struct ext2_pagedep;
struct ext2_newblk;
struct ext2_allocdirect;
struct ext2_indirdep;
struct ext2_allocindir;
struct ext2_freeblks;
struct ext2_freefile;
struct ext2_diradd;
struct ext2_dirrem;
struct ext2_mkdir;
struct ext2_bmsafemap;
struct ext2_sbdep;
struct ext2_orphan_add;
struct ext2_orphan_remove;

/*
 * Dependency type identifiers
 */
typedef enum ext2_dep_type {
	EXT2_DEP_UNUSED = 0,

	/* Metadata block dependencies */
	EXT2_DEP_NEWBLK,		/* Newly allocated block */
	EXT2_DEP_ALLOCDIRECT,		/* Direct block allocation */
	EXT2_DEP_INDIRDEP,		/* Indirect block dependency */
	EXT2_DEP_ALLOCINDIR,		/* Indirect block allocation */
	EXT2_DEP_EXTENTDEP,		/* Extent tree node dependency */
	EXT2_DEP_ALLOCEXTENT,		/* Extent allocation */

	/* Inode dependencies */
	EXT2_DEP_INODEDEP,		/* Inode update dependencies */

	/* Bitmap dependencies */
	EXT2_DEP_BMSAFEMAP,		/* Bitmap safe map */

	/* Free dependencies */
	EXT2_DEP_FREEBLKS,		/* Block freeing from truncation */
	EXT2_DEP_FREEFILE,		/* Inode deallocation (orphan) */

	/* Directory dependencies */
	EXT2_DEP_DIRADD,		/* Directory entry addition */
	EXT2_DEP_DIRREM,		/* Directory entry removal */
	EXT2_DEP_MKDIR,			/* Directory creation */

	/* Superblock dependencies */
	EXT2_DEP_SBDEP,			/* Superblock update */

	/* Orphan list dependencies */
	EXT2_DEP_ORPHAN_ADD,		/* Orphan list insertion */
	EXT2_DEP_ORPHAN_REMOVE,		/* Orphan list removal */

	EXT2_DEP_MAX
} ext2_dep_type_t;

/*
 * Dependency state flags
 */
#define	EXT2_DEP_ATTACHED	0x00000001  /* Data not currently being written */
#define	EXT2_DEP_UNDONE		0x00000002  /* Rolled back for safe write */
#define	EXT2_DEP_COMPLETE	0x00000004  /* Written to disk */
#define	EXT2_DEP_DEPCOMPLETE	0x00000008  /* Dependent operations complete */
#define	EXT2_DEP_UNLINKED	0x00000010  /* Inode unlinked, on orphan list */
#define	EXT2_DEP_UNLINKNEXT	0x00000020  /* Valid i_dtime (next orphan) */
#define	EXT2_DEP_UNLINKPREV	0x00000040  /* Predecessor points to us */
#define	EXT2_DEP_UNLINKONLIST	0x00000080  /* On disk orphan list */
#define	EXT2_DEP_GOINGAWAY	0x00000100  /* Frozen from further change */
#define	EXT2_DEP_ONWORKLIST	0x00000200  /* On worklist */
#define	EXT2_DEP_INPROGRESS	0x00000400  /* Being processed */
#define	EXT2_DEP_CANCELLED	0x00000800  /* Cancelled due to error */

#define	EXT2_DEP_ALLCOMPLETE	(EXT2_DEP_ATTACHED | EXT2_DEP_COMPLETE | EXT2_DEP_DEPCOMPLETE)

/*
 * Worklist structure - MUST be first member of each dependency object
 */
struct ext2_dep {
	LIST_ENTRY(ext2_dep)	dep_list;	/* List linkage */
	struct ext2_softdep_mount *dep_mp;	/* Mount we live in */
	ext2_dep_type_t		dep_type;	/* Type of dependency */
	uint32_t		dep_state;	/* State flags */
	uint32_t		dep_refcnt;	/* Reference count */
	LIST_ENTRY(ext2_dep)	dep_deps;	/* List of dependent deps */
	struct ext2_dep		*dep_parent;	/* Parent dependency */

	/* Debugging */
#ifdef INVARIANTS
	const char		*dep_func;	/* Function where added/removed */
	int			dep_line;	/* Line where added/removed */
#endif
};

#define	DEP_DATA(dep) ((void *)(dep))

/*
 * Worklist head types
 */
LIST_HEAD(ext2_dep_list, ext2_dep);
TAILQ_HEAD(ext2_dep_tailq, ext2_dep);

/*
 * Deferred write item for buffers with unsatisfied dependencies
 */
struct ext2_deferred_write {
	TAILQ_ENTRY(ext2_deferred_write) dw_list;
	struct buf		*dw_bp;			/* Deferred buffer */
	struct ext2_dep		*dw_dep;		/* Dependency blocking write */
	void			(*dw_callback)(struct buf *, int); /* Completion callback */
};

TAILQ_HEAD(ext2_deferred_write_head, ext2_deferred_write);

/*
 * Mount-private dependency state
 */
struct ext2_softdep_mount {
	struct mount		*sd_mp;		/* Associated mount point */
	struct mtx		sd_lock;	/* Mount-wide dependency lock */

	/* Worklists */
	struct ext2_dep_list	sd_worklist[EXT2_DEP_MAX];	/* Per-type worklists */
	TAILQ_HEAD(, ext2_dep)	sd_all_deps;	/* All dependencies */

	/* Deferred writes - buffers waiting for dependencies */
	struct ext2_deferred_write_head sd_deferred_writes;
	struct mtx		sd_deferred_lock;	/* Deferred write lock */

	/* Orphan list state */
	uint32_t		sd_orphan_head;	/* In-memory copy of s_last_orphan */
	struct mtx		sd_orphan_lock;	/* Orphan list lock */

	/* Statistics */
	uint64_t		sd_deps_allocated;
	uint64_t		sd_deps_freed;
	uint64_t		sd_workitems_processed;
	uint64_t		sd_writes_deferred;
	uint64_t		sd_writes_completed;

	/* Shutdown state */
	bool			sd_shutting_down;	/* Unmount in progress */
};

/*
 * Dependency object type-specific structures
 */

/* Inode dependency - tracks all deps associated with an inode */
struct ext2_inodedep {
	struct ext2_dep	id_dep;			/* Base dependency */

	/* Inode identification */
	ino_t		id_ino;			/* Dependent inode number */
	uint32_t	id_mode;		/* Inode mode */

	/* Link count tracking */
	int32_t		id_nlinkdelta;		/* Saved effective link count delta */
	int32_t		id_nlinkwrote;		/* i_nlink that we wrote to disk */
	int32_t		id_savednlink;		/* Link saved during rollback */

	/* Worklists */
	struct ext2_dep_list	id_inowait;	/* Operations waiting for inode update */
	struct ext2_dep_list	id_bufwait;	/* Operations after inode written */
	struct ext2_dep_list	id_newinoupdt;	/* Block updates before inode write */
	struct ext2_dep_list	id_inoupdt;	/* Block updates at inode write */
	struct ext2_dep_list	id_freeblklst;	/* List of partial truncates */

	/* Saved inode state for rollback */
	struct ext2fs_dinode	*id_savedino;	/* Saved dinode contents */

	/* Buffer association */
	struct buf		*id_bp;		/* Associated inode buffer */
};

/* Page (directory block) dependency */
struct ext2_pagedep {
	struct ext2_dep	pd_dep;			/* Base dependency */

	/* Directory block identification */
	ino_t		pd_ino;			/* Associated file (directory) */
	uint32_t	pd_lbn;			/* Logical block number within file */

	/* Worklists */
	struct ext2_dep_list	pd_diraddhd;	/* diradd waiting for this page */
	struct ext2_dep_list	pd_dirremhd;	/* dirrem waiting for this page */
	struct ext2_dep_list	pd_pendinghd;	/* Directory entries awaiting write */
};

/* Newly allocated block */
struct ext2_newblk {
	struct ext2_dep	nb_dep;			/* Base dependency */

	/* Block identification */
	daddr_t		nb_blkno;		/* Physical block number */
	ufs_lbn_t	nb_lbn;			/* Logical block number */
	ino_t		nb_ino;			/* Owning inode */
};

/* Direct block allocation */
struct ext2_allocdirect {
	struct ext2_dep	ad_dep;			/* Base dependency */

	/* Block identification */
	daddr_t		ad_blkno;		/* Physical block number */
	ufs_lbn_t	ad_lbn;			/* Logical block number */
	ino_t		ad_ino;			/* Owning inode */

	/* Previous block (for replacement) */
	daddr_t		ad_oldblkno;		/* Old physical block */
};

/* Indirect block dependency */
struct ext2_indirdep {
	struct ext2_dep	ir_dep;			/* Base dependency */

	/* Indirect block identification */
	daddr_t		ir_blkno;		/* Physical block number */
	ufs_lbn_t	ir_lbn;			/* Logical block number */
	ino_t		ir_ino;			/* Owning inode */
	int		ir_level;		/* Indirection level (1,2,3) */

	/* Parent indirect block */
	struct ext2_indirdep	*ir_parent;	/* Parent indirect block */

	/* Freeblks that free this indirect block */
	struct ext2_freeblks	*ir_freeblks;
};

/* Indirect block allocation */
struct ext2_allocindir {
	struct ext2_dep	ai_dep;			/* Base dependency */

	/* Block identification */
	daddr_t		ai_blkno;		/* Physical block number */
	ufs_lbn_t	ai_lbn;			/* Logical block number */
	ino_t		ai_ino;			/* Owning inode */
	int		ai_level;		/* Indirection level */

	/* Parent */
	struct ext2_indirdep	*ai_parent;
};

/* Extent tree node dependency */
struct ext2_extentdep {
	struct ext2_dep	ed_dep;			/* Base dependency */

	/* Extent block identification */
	daddr_t		ed_blkno;		/* Physical block number */
	uint32_t	ed_depth;		/* Tree depth */
	ino_t		ed_ino;			/* Owning inode */

	/* Parent extent block */
	struct ext2_extentdep	*ed_parent;
};

/* Extent allocation */
struct ext2_allocextent {
	struct ext2_dep	ae_dep;			/* Base dependency */

	/* Extent identification */
	daddr_t		ae_pblk;		/* Physical block start */
	uint32_t	ae_len;			/* Extent length */
	ufs_lbn_t	ae_lblk;		/* Logical block start */
	ino_t		ae_ino;			/* Owning inode */
};

/* Block freeing from truncation */
struct ext2_freeblks {
	struct ext2_dep	fb_dep;			/* Base dependency */

	/* Free operation */
	ino_t		fb_ino;			/* Inode being truncated */
	off_t		fb_oldsize;		/* Old file size */
	off_t		fb_newsize;		/* New file size */

	/* Lists of blocks to free */
	struct ext2_dep_list	fb_blklist;	/* List of blocks to free */

	/* Parent freeblks for nested truncates */
	struct ext2_freeblks	*fb_parent;
};

/* Inode deallocation (orphan) */
struct ext2_freefile {
	struct ext2_dep	fx_dep;			/* Base dependency */

	/* Inode identification */
	ino_t		fx_ino;			/* Inode being freed */
	uint32_t	fx_mode;		/* Inode mode */
	struct vnode	*fx_devvp;		/* Filesystem device vnode */
};

/* Directory entry addition */
struct ext2_diradd {
	struct ext2_dep	da_dep;			/* Base dependency */

	/* Directory entry */
	ino_t		da_parent_ino;		/* Parent directory inode */
	ino_t		da_child_ino;		/* Child inode number */
	uint32_t	da_offset;		/* Offset in directory block */
	bool		da_is_mkdir;		/* Is mkdir (needs extra deps) */

	/* Previous entry (for rename) */
	struct ext2_dirrem	*da_previous;
};

/* Directory entry removal */
struct ext2_dirrem {
	struct ext2_dep	dr_dep;			/* Base dependency */

	/* Directory entry */
	ino_t		dr_parent_ino;		/* Parent directory inode */
	ino_t		dr_child_ino;		/* Child inode number */
	uint32_t	dr_offset;		/* Offset in directory block */
};

/* Directory creation (mkdir) */
struct ext2_mkdir {
	struct ext2_dep	md_dep;			/* Base dependency */

	/* Mkdir specific */
	ino_t		md_parent_ino;		/* Parent directory */
	ino_t		md_child_ino;		/* New directory inode */
	struct ext2_diradd	*md_diradd;	/* Associated diradd */
};

/* Bitmap safe map */
struct ext2_bmsafemap {
	struct ext2_dep	sm_dep;			/* Base dependency */

	/* Cylinder group */
	int		sm_cg;			/* Cylinder group number */
	struct buf	*sm_buf;		/* Associated CG buffer */

	/* Dependencies waiting for this bitmap */
	struct ext2_dep_list	sm_newblks;	/* newblk deps */
	struct ext2_dep_list	sm_allocdirects;	/* allocdirect deps */
	struct ext2_dep_list	sm_allocindirs;	/* allocindir deps */
	struct ext2_dep_list	sm_inodedeps;	/* inodedep deps */
};

/* Superblock update dependency */
struct ext2_sbdep {
	struct ext2_dep	sb_dep;			/* Base dependency */

	/* Superblock buffer */
	struct buf	*sb_buf;		/* Superblock buffer */
};

/* Orphan list insertion */
struct ext2_orphan_add {
	struct ext2_dep	oa_dep;			/* Base dependency */

	/* Orphan inode */
	ino_t		oa_ino;			/* Orphan inode number */
	ino_t		oa_prev_head;		/* Previous list head */

	/* Buffers involved */
	struct buf	*oa_inode_bp;		/* Inode buffer */
	struct buf	*oa_sb_bp;		/* Superblock buffer */
};

/* Orphan list removal */
struct ext2_orphan_remove {
	struct ext2_dep	or_dep;			/* Base dependency */

	/* Orphan inode */
	ino_t		or_ino;			/* Orphan inode number */
	ino_t		or_prev_ino;		/* Previous inode in list (0 if head) */

	/* Buffers involved */
	struct buf	*or_inode_bp;		/* Inode buffer */
	struct buf	*or_prev_bp;		/* Previous inode buffer (if any) */
	struct buf	*or_sb_bp;		/* Superblock buffer (if head) */
};

/*
 * Dependency API
 */

/* Mount initialization/shutdown */
int	ext2_softdep_mount(struct mount *mp, struct m_ext2fs *fs);
void	ext2_softdep_unmount(struct mount *mp);
void	ext2_softdep_initialize(void);
void	ext2_softdep_uninitialize(void);

/* Dependency object allocation */
struct ext2_dep *ext2_dep_alloc(struct ext2_softdep_mount *sd, ext2_dep_type_t type);
void	ext2_dep_free(struct ext2_dep *dep);
void	ext2_dep_ref(struct ext2_dep *dep);
void	ext2_dep_rele(struct ext2_dep *dep);

/* Dependency state management */
void	ext2_dep_set_state(struct ext2_dep *dep, uint32_t flags);
void	ext2_dep_clear_state(struct ext2_dep *dep, uint32_t flags);
bool	ext2_dep_has_state(struct ext2_dep *dep, uint32_t flags);
bool	ext2_dep_is_complete(struct ext2_dep *dep);

/* Dependency graph */
void	ext2_dep_add_dependency(struct ext2_dep *pred, struct ext2_dep *succ);
void	ext2_dep_remove_dependency(struct ext2_dep *pred, struct ext2_dep *succ);
void	ext2_dep_satisfy(struct ext2_dep *dep);

/* Worklist management */
void	ext2_worklist_insert(struct ext2_softdep_mount *sd, struct ext2_dep *dep);
void	ext2_worklist_remove(struct ext2_softdep_mount *sd, struct ext2_dep *dep);
void	ext2_worklist_process(struct ext2_softdep_mount *sd);

/* Buffer association */
void	ext2_dep_attach_buffer(struct ext2_dep *dep, struct buf *bp);
void	ext2_dep_detach_buffer(struct ext2_dep *dep);
void	ext2_buf_write_start(struct buf *bp);
void	ext2_buf_write_complete(struct buf *bp, int error);

/* Orphan list operations */
int	ext2_orphan_add(struct inode *ip);
int	ext2_orphan_remove(struct inode *ip);
void	ext2_orphan_recovery(struct mount *mp);

/* Inode dependency lookup */
struct ext2_inodedep *ext2_inodedep_lookup(struct ext2_softdep_mount *sd, ino_t ino);
struct ext2_inodedep *ext2_inodedep_lookup_ip(struct inode *ip);

/* Deferred write management */
bool	ext2_can_write_buffer(struct buf *bp);
void	ext2_defer_buffer_write(struct buf *bp, struct ext2_dep *dep);
void	ext2_process_deferred_writes(struct ext2_softdep_mount *sd);
void	ext2_flush_deferred_writes(struct ext2_softdep_mount *sd);

/* Inode dependency lookup */
struct ext2_inodedep *ext2_inodedep_lookup(struct ext2_softdep_mount *sd, ino_t ino);
struct ext2_inodedep *ext2_inodedep_lookup_ip(struct inode *ip);

/* Locking */
#define	EXT2_SOFTDEP_LOCK(sd)		mtx_lock(&(sd)->sd_lock)
#define	EXT2_SOFTDEP_UNLOCK(sd)		mtx_unlock(&(sd)->sd_lock)
#define	EXT2_SOFTDEP_LOCK_ASSERT(sd)	mtx_assert(&(sd)->sd_lock, MA_OWNED)

/* Debugging */
#ifdef INVARIANTS
void	ext2_dep_assert_valid(struct ext2_dep *dep);
#define	EXT2_DEP_ASSERT_VALID(dep)	ext2_dep_assert_valid(dep)
#else
#define	EXT2_DEP_ASSERT_VALID(dep)	do {} while (0)
#endif

#endif /* !_FS_EXT2FS_EXT2_SOFTDEP_H_ */