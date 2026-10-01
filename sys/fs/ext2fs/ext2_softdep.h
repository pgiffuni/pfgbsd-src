/*-
 * Copyright (c) 1991, 1993
 *		The Regents of the University of California.  All rights reserved.
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
/*
 * ext2 metadata dependency graph.
 *
 * The invariant the graph exists to express: a metadata update must not
 * reach the disk before the updates that make it safe have reached it.
 * The instance that matters here is a newly allocated block -- an indirect
 * block, an extent tree node -- which must be initialised on disk before
 * the parent that names it is allowed to name it.
 *
 * Every write ext2fs makes is synchronous today, so the graph does not
 * yet defer anything: a dependency is created, linked, and satisfied
 * within the operation that needs it, and the wrappers below reduce to
 * the raw write plus the checks that the ordering held.  What it buys
 * now is the assertion that the ordering is real, and a vocabulary in
 * which the writes can later be relaxed one class at a time.
 *
 * Buffer association uses b_fsprivate1, not b_dep.  b_dep is reached
 * through the single global struct bio_ops vtable, which ffs_softdep()
 * installs and which only one filesystem can own; using it would make
 * ext2fs and UFS mutually exclusive.
 */

#ifndef _FS_EXT2FS_EXT2_SOFTDEP_H_
#define	_FS_EXT2FS_EXT2_SOFTDEP_H_

#include <sys/queue.h>
#include <sys/types.h>

/*
 * Dependency types.
 *
 * EXT2_DEP_NEWBLK	guards a freshly allocated metadata block that is not
 *			yet reachable.  Its buffer must reach the disk before
 *			whatever names the block.
 * EXT2_DEP_METADATA	guards a metadata update that must not be written
 *			before the prerequisites linked to it.
 */
#define	EXT2_DEP_TYPES {						\
	{ "newblk",	EXT2_DEP_NEWBLK },				\
	{ "metadata",	EXT2_DEP_METADATA },				\
	{ NULL,		0 }						\
}

enum {
	EXT2_DEP_NEWBLK = 1,
	EXT2_DEP_METADATA
};

/*
 * Dependency states.  A dependency is created PENDING and reaches exactly
 * one terminal state, so it never has to be re-armed: dependencies are
 * per-operation objects, created and retired by the operation that needs
 * them, rather than long-lived objects hanging off an inode.
 */
enum {
	EXT2_DEP_PENDING = 0,
	EXT2_DEP_SATISFIED,
	EXT2_DEP_CANCELLED
};

/*
 * Longest prerequisite chain that will be walked before the graph is
 * declared broken.  Indirect block chains are three deep and extent
 * trees five; eight leaves room without being unbounded.
 */
#define	EXT2_DEP_MAXDEPTH	8

struct ext2_softdep_mount;

/*
 * One dependency: "dep_bp must not be written until dep_prereq is".
 *
 * A dependency holds a reference on its prerequisite, so a prerequisite
 * cannot be freed while something still waits for it.  It is freed when
 * its own references drop to zero, which happens once it is satisfied or
 * cancelled and nothing is waiting for it -- so the graph does not grow
 * with the size of the filesystem.
 */
struct ext2_dep {
	uint8_t			dep_type;
	uint8_t			dep_state;
	struct ext2_softdep_mount *dep_sd;
	struct buf		*dep_bp;	/* guarded, NULL once released */
	struct ext2_dep		*dep_prereq;	/* written before this one */
	struct ext2_dep		*dep_dependent;	/* waiting on this one */
	STAILQ_ENTRY(ext2_dep)	 dep_link;	/* mount accounting list */
	uint32_t		dep_refcnt;
};

struct ext2_softdep_mount {
	struct ext2mount	*sd_ump;
	struct m_ext2fs		*sd_fs;
	struct mtx		sd_lock;
	STAILQ_HEAD(, ext2_dep)	 sd_all;	/* live dependencies */
	int			sd_shutting_down;
};

/*
 * Counters are process-wide rather than per-mount so that they can be
 * read without walking the mount list; outstanding is the number of
 * dependency objects alive anywhere, and should settle back to zero.
 */
extern int ext2_softdep_created;
extern int ext2_softdep_satisfied;
extern int ext2_softdep_cancelled;
extern int ext2_softdep_outstanding;
extern int ext2_softdep_debug;
extern int ext2_softdep_async;

/*
 * Which classes of metadata write may be deferred rather than written
 * synchronously.  A bitmask so that one class can be enabled, measured
 * and reverted without touching the others; enabling several at once
 * hides which one regressed.
 */
#define	EXT2_SD_ASYNC_NEWBLK	0x01	/* new indirect block */
#define	EXT2_SD_ASYNC_EXTENT	0x02	/* extent tree metadata */
#define	EXT2_SD_ASYNC_DIR	0x04	/* directory metadata */

#define	EXT2_SOFTDEP_LOCK(sd)		mtx_lock(&(sd)->sd_lock)
#define	EXT2_SOFTDEP_UNLOCK(sd)		mtx_unlock(&(sd)->sd_lock)

/*
 * Buffer association.  One dependency per buffer, cleared before the
 * buffer is released by the write wrapper.
 */
#define	EXT2_BP_DEP(bp)		((struct ext2_dep *)((bp)->b_fsprivate1))
#define	EXT2_SET_BP_DEP(bp, d)	((bp)->b_fsprivate1 = (void *)(d))
#define	EXT2_BP_DEP_CLEAR(bp)	((bp)->b_fsprivate1 = NULL)

int	ext2_softdep_mount(struct ext2mount *, struct m_ext2fs *);
void	ext2_softdep_unmount(struct ext2mount *);

struct ext2_dep *ext2_dep_create(struct inode *, struct buf *, uint8_t);
void	ext2_dep_rele(struct ext2_dep *);
int	ext2_dep_link(struct ext2_dep *, struct ext2_dep *);
void	ext2_dep_satisfy(struct ext2_dep *);
void	ext2_dep_cancel(struct ext2_dep *);
void	ext2_dep_discard(struct buf *);
void	ext2_dep_biodone(struct buf *);
void	ext2_dep_defer(struct buf *, int);
int	ext2_dep_write(struct buf *, int);
int	ext2_dep_drive(struct buf *);

int	ext2_dep_bwrite(struct buf *);
void	ext2_dep_bdwrite(struct buf *);
int	ext2_dep_bawrite(struct buf *);

#endif /* !_FS_EXT2FS_EXT2_SOFTDEP_H_ */