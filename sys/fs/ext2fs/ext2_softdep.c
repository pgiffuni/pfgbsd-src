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
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mount.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/vnode.h>
#include <sys/sysctl.h>
#include <machine/atomic.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/ext2_softdep.h>

static const char *ext2_dep_typenames[] = {
	[EXT2_DEP_NEWBLK]	= "newblk",
	[EXT2_DEP_METADATA]	= "metadata",
};

int ext2_softdep_created;
int ext2_softdep_satisfied;
int ext2_softdep_cancelled;
int ext2_softdep_outstanding;
int ext2_softdep_debug;
int ext2_softdep_async;
int ext2_crash_point;
int ext2_crash_enabled;

static void ext2_dep_free(struct ext2_dep *);
static void ext2_dep_ref_locked(struct ext2_dep *);
static void ext2_dep_unlink(struct ext2_dep *);

/*
 * Create the per-mount dependency state.  Called from mount, before the
 * filesystem is reachable, so that no operation can race the first
 * registration.
 */
int
ext2_softdep_mount(struct ext2mount *ump, struct m_ext2fs *fs)
{
	struct ext2_softdep_mount *sd;

	sd = malloc(sizeof(*sd), M_EXT2SOFTSDEP, M_WAITOK | M_ZERO);
	if (sd == NULL)
		return (ENOMEM);

	sd->sd_ump = ump;
	sd->sd_fs = fs;
	mtx_init(&sd->sd_lock, "ext2fs sd", MTX_DEF, 0);
	STAILQ_INIT(&sd->sd_all);
	fs->e2fs_softdep = sd;
	return (0);
}

/*
 * Tear the dependency state down.  Every dependency must already be gone:
 * an outstanding one would mean a buffer still refers to memory that is
 * about to be freed, which is why this reports rather than cleans up.
 */
void
ext2_softdep_unmount(struct ext2mount *ump)
{
	struct m_ext2fs *fs = ump->um_e2fs;
	struct ext2_softdep_mount *sd = fs->e2fs_softdep;
	struct ext2_dep *dep;
	int remaining;

	if (sd == NULL)
		return;

	EXT2_SOFTDEP_LOCK(sd);
	sd->sd_shutting_down = 1;
	remaining = STAILQ_FIRST(&sd->sd_all) != NULL;
	if (remaining) {
		for (dep = STAILQ_FIRST(&sd->sd_all); dep != NULL;
		    dep = STAILQ_NEXT(dep, dep_link)) {
			printf("ext2fs: %s: dependency %p (%s) still pending "
			    "at unmount, state %d\n", fs->e2fs_fsmnt,
			    (void *)dep,
			    ext2_dep_typenames[dep->dep_type], dep->dep_state);
		}
	}
	EXT2_SOFTDEP_UNLOCK(sd);

	/*
	 * A dependency that outlives the mount points at this state, and
	 * a write that is still in flight calls back through b_iodone,
	 * which takes sd_lock.  Freeing it here would leave that callback
	 * locking a destroyed mutex over freed memory.
	 *
	 * Trying to write the outstanding buffers instead is not an option:
	 * the reason one is outstanding is that it was invalidated rather
	 * than written, and waiting on a buffer the cache has already
	 * discarded is how an unmount hangs.
	 *
	 * So the state is deliberately leaked when anything remains.  It is
	 * a few hundred bytes per mount, the leak is reported above, and a
	 * later completion still satisfies the dependency properly and
	 * frees it.  A use-after-free is not a trade worth making here.
	 */
	if (remaining)
		return;

	mtx_destroy(&sd->sd_lock);
	free(sd, M_EXT2SOFTSDEP);
	fs->e2fs_softdep = NULL;
}

/*
 * Create a dependency guarding a buffer.
 *
 * The dependency is owned by the association: it lives exactly as long as
 * the buffer carries it, and is retired when the wrapper that finds it
 * writes or discards the buffer.  The caller does not hold a reference
 * and must not release one; linking the new dependency to a prerequisite
 * takes a reference on the prerequisite, not on this.
 *
 * Returns NULL when the filesystem has no dependency state, or when the
 * allocation fails.  A NULL return is not an error: the caller falls back
 * to an unwrapped write, which is correct because every write ext2fs makes
 * today is synchronous.
 */
struct ext2_dep *
ext2_dep_create(struct inode *ip, struct buf *bp, uint8_t type)
{
	struct ext2_softdep_mount *sd;
	struct ext2_dep *dep;

	if (ip == NULL || bp == NULL)
		return (NULL);
	sd = ip->i_e2fs->e2fs_softdep;
	if (sd == NULL)
		return (NULL);
	if (EXT2_BP_DEP(bp) != NULL)
		return (NULL);

	/*
	 * Refuse once unmount has begun.  A dependency created then would
	 * be attached to a mount state that is being torn down.
	 */
	EXT2_SOFTDEP_LOCK(sd);
	if (sd->sd_shutting_down) {
		EXT2_SOFTDEP_UNLOCK(sd);
		return (NULL);
	}
	EXT2_SOFTDEP_UNLOCK(sd);

	dep = malloc(sizeof(*dep), M_EXT2SOFTSDEP, M_WAITOK | M_ZERO);
	if (dep == NULL)
		return (NULL);

	dep->dep_type = type;
	dep->dep_state = EXT2_DEP_PENDING;
	dep->dep_sd = sd;
	dep->dep_refcnt = 1;
	dep->dep_bp = bp;

	EXT2_SOFTDEP_LOCK(sd);
	STAILQ_INSERT_TAIL(&sd->sd_all, dep, dep_link);
	atomic_add_int(&ext2_softdep_created, 1);
	atomic_add_int(&ext2_softdep_outstanding, 1);
	EXT2_SET_BP_DEP(bp, dep);
	EXT2_SOFTDEP_UNLOCK(sd);

	return (dep);
}

static void
ext2_dep_ref_locked(struct ext2_dep *dep)
{

	KASSERT(dep->dep_refcnt > 0, ("ext2_dep_ref: refcount underflow"));
	dep->dep_refcnt++;
}

/*
 * Drop a reference.  The dependency is freed when the last one goes, which
 * is what keeps the graph from growing with the number of blocks ever
 * allocated.
 */
void
ext2_dep_rele(struct ext2_dep *dep)
{
	struct ext2_softdep_mount *sd;
	int terminal;

	if (dep == NULL)
		return;
	sd = dep->dep_sd;

	EXT2_SOFTDEP_LOCK(sd);
	KASSERT(dep->dep_refcnt > 0, ("ext2_dep_rele: refcount underflow"));
	dep->dep_refcnt--;
	terminal = dep->dep_refcnt == 0;
	EXT2_SOFTDEP_UNLOCK(sd);

	if (terminal)
		ext2_dep_free(dep);
}

/*
 * Release the memory.  Reaching here with references outstanding
 * means something still points at a dependency that is going away.
 */
static void
ext2_dep_free(struct ext2_dep *dep)
{
	struct ext2_softdep_mount *sd = dep->dep_sd;

	EXT2_SOFTDEP_LOCK(sd);
	STAILQ_REMOVE(&sd->sd_all, dep, ext2_dep, dep_link);
	atomic_add_int(&ext2_softdep_outstanding, -1);
	if (dep->dep_state == EXT2_DEP_SATISFIED)
		atomic_add_int(&ext2_softdep_satisfied, 1);
	else if (dep->dep_state == EXT2_DEP_CANCELLED)
		atomic_add_int(&ext2_softdep_cancelled, 1);
	if (ext2_softdep_debug && dep->dep_refcnt > 0) {
		printf("ext2fs: %s: dependency %p (%s) freed with %u ref(s)\n",
		    sd->sd_fs->e2fs_fsmnt, (void *)dep,
		    ext2_dep_typenames[dep->dep_type], dep->dep_refcnt);
	}
	EXT2_SOFTDEP_UNLOCK(sd);

	free(dep, M_EXT2SOFTSDEP);
}

/*
 * Record that 'dep' must not be written until 'prereq' has been.
 *
 * A link to a dependency that has already reached a terminal state is
 * accepted and ignored: there is nothing left to wait for, and refusing
 * would mean failing an operation whose ordering is already satisfied.
 *
 * A link that would close a cycle is refused.  The chain walk is bounded,
 * and a chain longer than the bound is treated as broken rather than
 * trusted.
 */
int
ext2_dep_link(struct ext2_dep *dep, struct ext2_dep *prereq)
{
	struct ext2_softdep_mount *sd;
	struct ext2_dep *cur;
	unsigned int depth;
	int error;

	if (dep == NULL || prereq == NULL)
		return (0);
	if (dep == prereq)
		return (ELOOP);
	sd = dep->dep_sd;

	EXT2_SOFTDEP_LOCK(sd);
	if (prereq->dep_state != EXT2_DEP_PENDING) {
		EXT2_SOFTDEP_UNLOCK(sd);
		return (0);
	}

	cur = prereq;
	for (depth = 0; cur != NULL; depth++) {
		if (cur == dep) {
			EXT2_SOFTDEP_UNLOCK(sd);
			return (ELOOP);
		}
		if (depth >= EXT2_DEP_MAXDEPTH) {
			EXT2_SOFTDEP_UNLOCK(sd);
			return (ELOOP);
		}
		cur = cur->dep_prereq;
	}
	if (dep->dep_prereq != NULL) {
		EXT2_SOFTDEP_UNLOCK(sd);
		return (EINVAL);
	}

	dep->dep_prereq = prereq;
	if (prereq->dep_dependent != NULL) {
		dep->dep_prereq = NULL;
		EXT2_SOFTDEP_UNLOCK(sd);
		return (EINVAL);
	}
	prereq->dep_dependent = dep;
	ext2_dep_ref_locked(prereq);
	error = 0;
	EXT2_SOFTDEP_UNLOCK(sd);

	return (error);
}

/*
 * Detach from the prerequisite chain and give up the reference held on it.
 */
static void
ext2_dep_unlink(struct ext2_dep *dep)
{
	struct ext2_softdep_mount *sd;
	struct ext2_dep *pre;

	if (dep == NULL)
		return;
	sd = dep->dep_sd;

	EXT2_SOFTDEP_LOCK(sd);
	pre = dep->dep_prereq;
	dep->dep_prereq = NULL;
	if (pre != NULL && pre->dep_dependent == dep)
		pre->dep_dependent = NULL;
	EXT2_SOFTDEP_UNLOCK(sd);

	if (pre != NULL)
		ext2_dep_rele(pre);
}

/*
 * The dependency's buffer has reached the disk.  Anything waiting on it
 * may now proceed.
 */
void
ext2_dep_satisfy(struct ext2_dep *dep)
{
	struct ext2_softdep_mount *sd;

	if (dep == NULL)
		return;
	sd = dep->dep_sd;

	EXT2_SOFTDEP_LOCK(sd);
	KASSERT(dep->dep_state == EXT2_DEP_PENDING,
	    ("ext2_dep_satisfy: %s already terminal", dep->dep_type));
	dep->dep_state = EXT2_DEP_SATISFIED;
	if (dep->dep_bp != NULL) {
		EXT2_BP_DEP_CLEAR(dep->dep_bp);
		dep->dep_bp = NULL;
	}
	EXT2_SOFTDEP_UNLOCK(sd);

	ext2_dep_unlink(dep);
	ext2_dep_rele(dep);
}

/*
 * The dependency's buffer failed to reach the disk.  Anything waiting on
 * it must not proceed as though it had.
 */
void
ext2_dep_cancel(struct ext2_dep *dep)
{
	struct ext2_softdep_mount *sd;

	if (dep == NULL)
		return;
	sd = dep->dep_sd;

	EXT2_SOFTDEP_LOCK(sd);
	KASSERT(dep->dep_state == EXT2_DEP_PENDING,
	    ("ext2_dep_cancel: %s already terminal", dep->dep_type));
	dep->dep_state = EXT2_DEP_CANCELLED;
	if (dep->dep_bp != NULL) {
		EXT2_BP_DEP_CLEAR(dep->dep_bp);
		dep->dep_bp = NULL;
	}
	EXT2_SOFTDEP_UNLOCK(sd);

	ext2_dep_unlink(dep);
	ext2_dep_rele(dep);
}
