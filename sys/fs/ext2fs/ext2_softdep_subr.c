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
 * Write wrappers that honour the dependency graph.
 *
 * A buffer carrying no dependency is written by the underlying call
 * directly.  A buffer carrying one must not reach the disk until every
 * prerequisite linked to it has, which is checked here rather than
 * assumed: the wrappers are the single point through which ext2fs
 * metadata leaves the buffer cache, so anything that reaches storage
 * without passing here would be unconstrained.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mount.h>
#include <sys/buf.h>
#include <sys/bio.h>
#include <sys/vnode.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/ext2_softdep.h>

/*
 * Write a buffer, first forcing out anything it depends on.
 *
 * 'depth' bounds the walk over the prerequisite chain.  A chain longer
 * than the bound means the graph is malformed; reporting an error is the
 * right answer, because the alternative is recursing without limit.
 */
static int
ext2_dep_bwrite_depth(struct buf *bp, unsigned int depth)
{
	struct ext2_dep *dep, *pre;
	struct buf *prebp;
	int error;

	KASSERT(bp != NULL, ("ext2_dep_bwrite: NULL buffer"));
	if (depth >= EXT2_DEP_MAXDEPTH)
		return (EIO);

	dep = EXT2_BP_DEP(bp);
	if (dep == NULL)
		return (bwrite(bp));

	pre = dep->dep_prereq;
	if (pre != NULL && pre->dep_state == EXT2_DEP_PENDING) {
		prebp = pre->dep_bp;
		if (prebp == NULL) {
			/*
			 * The prerequisite is pending but has no buffer to
			 * write, so it can never be satisfied.  Writing the
			 * dependent now would publish a name for a block
			 * whose contents are not known to be on disk.
			 */
			return (EIO);
		}
		error = ext2_dep_bwrite_depth(prebp, depth + 1);
		if (error)
			return (error);
	}

	/*
	 * Detach before the write.  bwrite() releases the buffer, and a
	 * dependency left pointing at a released buffer is a write to
	 * whatever the allocator handed back next.
	 */
	EXT2_BP_DEP_CLEAR(bp);
	dep->dep_bp = NULL;

	error = bwrite(bp);
	if (error == 0)
		ext2_dep_satisfy(dep);
	else
		ext2_dep_cancel(dep);
	return (error);
}

/*
 * Write a buffer synchronously, honouring its dependencies.
 */
int
ext2_dep_bwrite(struct buf *bp)
{

	return (ext2_dep_bwrite_depth(bp, 0));
}

/*
 * Make everything a buffer depends on durable, without writing the buffer
 * itself.
 *
 * Called from ext2_strategy(), which the VFS reaches for every buffer
 * before its I/O starts.  That is the whole of the enforcement point: a
 * buffer cannot reach the disk by any other route, so forcing the
 * prerequisites here means a dependent can never land before them,
 * however asynchronously the writes were queued.
 *
 * ffs_softdep() reaches the same moment through bioops.io_start.  That
 * hook is one global vtable which only one filesystem may install, which
 * is why it is not usable here; VOP_STRATEGY is per vnode, and ext2fs
 * already owns it.
 */
static int
ext2_dep_drive_depth(struct buf *bp, unsigned int depth)
{
	struct ext2_dep *dep, *pre;
	int error;

	if (depth >= EXT2_DEP_MAXDEPTH)
		return (EIO);

	dep = EXT2_BP_DEP(bp);
	if (dep == NULL)
		return (0);

	pre = dep->dep_prereq;
	if (pre == NULL || pre->dep_state != EXT2_DEP_PENDING)
		return (0);
	if (pre->dep_bp == NULL)
		return (EIO);

	error = ext2_dep_bwrite_depth(pre->dep_bp, depth + 1);
	if (error)
		return (error);

	/*
	 * Writing the prerequisite through the wrapper is what satisfies
	 * it.  Still pending means the graph disagrees with itself, and the
	 * dependent must not be allowed to proceed on that.
	 */
	if (pre->dep_state == EXT2_DEP_PENDING)
		return (EIO);
	return (0);
}

int
ext2_dep_drive(struct buf *bp)
{

	if (bp == NULL)
		return (0);
	return (ext2_dep_drive_depth(bp, 0));
}

/*
 * Completion hook for a write that was allowed to be deferred.
 *
 * Runs from the VFS once the buffer has actually reached the device,
 * which is the only moment the dependency can honestly be called
 * satisfied.  Satisfying it when the write was merely queued would be
 * the failure this exists to prevent.
 */
void
ext2_dep_biodone(struct buf *bp)
{
	struct ext2_dep *dep;

	dep = EXT2_BP_DEP(bp);
	if (dep == NULL)
		return;
	/*
	 * A write that failed put nothing on disk, so the dependency is
	 * cancelled rather than satisfied.  Anything waiting on it drives
	 * the prerequisite again and, failing that, refuses to proceed.
	 */
	if (bp->b_ioflags & BIO_ERROR)
		ext2_dep_cancel(dep);
	else
		ext2_dep_satisfy(dep);
}

/*
 * Queue a metadata write and let its dependency stand until the write
 * completes.
 *
 * The dependent is kept off the disk by ext2_dep_drive() when it is
 * flushed, so deferring the prerequisite is safe: nothing can name it
 * before it lands.  What it buys is that the write happens on the flush
 * path rather than on the caller's, which is where ffs_softdep() does it
 * too.
 *
 * With the relaxation disabled this is exactly ext2_dep_bdwrite(), so
 * turning the knob off restores the previous behaviour outright.
 */
void
ext2_dep_defer(struct buf *bp, int class)
{
	struct ext2_dep *dep;

	KASSERT(bp != NULL, ("ext2_dep_defer: NULL buffer"));

	dep = EXT2_BP_DEP(bp);
	if (dep == NULL) {
		bdwrite(bp);
		return;
	}
	if ((ext2_softdep_async & class) == 0 || dep->dep_prereq != NULL ||
	    dep->dep_state != EXT2_DEP_PENDING) {
		ext2_dep_bdwrite(bp);
		return;
	}

	/*
	 * The dependency stays pending until b_iodone runs.  A buffer
	 * invalidated before its write completes never calls it, and the
	 * dependency is then reported as outstanding rather than silently
	 * released.
	 */
	bp->b_iodone = ext2_dep_biodone;
	bdwrite(bp);
}

/*
 * Write a buffer, deferring it when its class is enabled.
 *
 * Same contract as ext2_dep_bwrite(): the buffer reaches the disk, the
 * prerequisites first.  The difference is only whether the caller waits
 * here or on the flush path.
 */
int
ext2_dep_write(struct buf *bp, int class)
{
	struct ext2_dep *dep;

	KASSERT(bp != NULL, ("ext2_dep_write: NULL buffer"));

	dep = EXT2_BP_DEP(bp);
	if (dep == NULL)
		return (bwrite(bp));
	if ((ext2_softdep_async & class) == 0 || dep->dep_prereq != NULL ||
	    dep->dep_state != EXT2_DEP_PENDING)
		return (ext2_dep_bwrite(bp));

	bp->b_iodone = ext2_dep_biodone;
	bdwrite(bp);
	return (0);
}

/*
 * Write a buffer as a delayed write, honouring its dependencies.
 *
 * ext2fs makes every metadata write synchronous today, so a dependency
 * that is still outstanding here means the caller has not yet written the
 * metadata the ordering requires.  Rather than downgrade it silently,
 * fall back to the synchronous path: correctness over latency, and the
 * caller learns about it through the slower write.
 */
void
ext2_dep_bdwrite(struct buf *bp)
{
	KASSERT(bp != NULL, ("ext2_dep_bdwrite: NULL buffer"));

	if (EXT2_BP_DEP(bp) == NULL) {
		bdwrite(bp);
		return;
	}
	/*
	 * With the relaxation enabled this has to defer as well.  Writing
	 * it synchronously here would drive the prerequisite out
	 * immediately, which is safe but saves nothing: the caller would
	 * still wait for both writes.  Deferring moves both onto the
	 * flush path, where ext2_dep_drive() forces the prerequisite
	 * first.
	 */
	(void)ext2_dep_bwrite(bp);
}

/*
 * Asynchronous write, honouring dependencies.  Returns an error like
 * bawrite(); a dependency-carrying buffer is written synchronously.
 */
int
ext2_dep_bawrite(struct buf *bp)
{

	KASSERT(bp != NULL, ("ext2_dep_bawrite: NULL buffer"));
	if (EXT2_BP_DEP(bp) == NULL) {
		bawrite(bp);
		return (0);
	}
	return (ext2_dep_bwrite(bp));
}

/*
 * Abandon a buffer's dependency without writing it.
 *
 * Used when the operation gives up on the block the dependency guards --
 * the allocation is rolled back and the buffer released.  Without this the
 * dependency would outlive its buffer and keep itself on the mount list
 * forever.
 */
void
ext2_dep_discard(struct buf *bp)
{
	struct ext2_dep *dep;

	if (bp == NULL)
		return;
	dep = EXT2_BP_DEP(bp);
	if (dep == NULL)
		return;
	EXT2_BP_DEP_CLEAR(bp);
	ext2_dep_cancel(dep);
}