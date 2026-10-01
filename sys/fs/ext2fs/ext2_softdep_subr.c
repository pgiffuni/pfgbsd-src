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

	KASSERT(bp != NULL, "ext2_dep_bwrite: NULL buffer");
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
	KASSERT(bp != NULL, "ext2_dep_bdwrite: NULL buffer");

	if (EXT2_BP_DEP(bp) == NULL) {
		bdwrite(bp);
		return;
	}
	(void)ext2_dep_bwrite(bp);
}

/*
 * Asynchronous write, honouring dependencies.  Returns an error like
 * bawrite(); a dependency-carrying buffer is written synchronously.
 */
int
ext2_dep_bawrite(struct buf *bp)
{

	KASSERT(bp != NULL, "ext2_dep_bawrite: NULL buffer");
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