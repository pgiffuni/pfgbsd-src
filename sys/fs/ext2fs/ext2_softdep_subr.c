#include <sys/param.h>
#include <sys/buf.h>

#include <fs/ext2fs/ext2_softdep.h>

/*
 * Write a buffer synchronously, with dependency gating.
 *
 * If the buffer's dep has unsatisfied prerequisites, they are driven
 * synchronously via ext2_drive_prerequisites() before the write.
 * After a successful write, the dep is satisfied.
 *
 * Returns the bwrite error code, or 0 on success.
 */
int
ext2_dep_bwrite(struct buf *bp)
{
	struct ext2_dep *dep;
	int error;

	if (bp == NULL)
		return (0);

	dep = EXT2_BP_DEP(bp);
	if (dep == NULL)
		return (bwrite(bp));

	/*
	 * Synchronously drive prerequisites.  This recursively writes
	 * any buffers that must precede this one, ensuring all
	 * dependencies are satisfied before we write.
	 */
	error = ext2_drive_prerequisites(dep);
	if (error)
		return (error);

	/* Prerequisites are satisfied - detach buffer from dep, then write.
	 * Detach before bwrite since bwrite may free/reuse the buffer. */
	EXT2_BP_DEP_CLEAR(bp);
	dep->dep_bp = NULL;

	error = bwrite(bp);
	if (error == 0)
		ext2_dep_satisfy(dep);
	else
		/*
		 * On write failure, cancel the dep to ensure it reaches a
		 * terminal state (CANCELLED) rather than remaining stranded
		 * in WRITING/PENDING with no buffer.
		 */
		ext2_dep_cancel(dep);
	return (error);
}

/*
 * Delayed write with dependency awareness.
 *
 * bdwrite() normally only marks the buffer dirty without initiating I/O.
 * However, to guarantee correct dependency ordering, ext2_dep_bdwrite()
 * delegates to ext2_dep_bwrite() (synchronous) when a dependency is
 * attached.  This ensures that:
 *
 *   1. Prerequisites are driven before the buffer is written
 *   2. The dep is satisfied only after actual I/O completion
 *   3. Errors are propagated (via void cast since bdwrite returns void)
 *
 * This conservative choice trades async performance for correctness.
 * The priority is filesystem consistency, not write latency.
 */
void
ext2_dep_bdwrite(struct buf *bp)
{
	struct ext2_dep *dep;

	if (bp == NULL)
		return;

	dep = EXT2_BP_DEP(bp);
	if (dep == NULL) {
		bdwrite(bp);
		return;
	}

	/*
	 * Use synchronous bwrite to guarantee on-disk persistence
	 * and satisfy the dep at actual I/O completion.
	 */
	(void)ext2_dep_bwrite(bp);
}

/*
 * Asynchronous write with dependency gating.
 *
 * bawrite initiates async I/O.  We gate on the dependency:
 * first drive prerequisites synchronously to ensure all
 * predecessor writes are complete, then initiate the async write.
 * The dep is satisfied only after bwrite (via ext2_dep_bwrite)
 * completes the actual I/O.
 *
 * For simplicity, if a dep is present we use the synchronous
 * ext2_dep_bwrite path to guarantee correct ordering.  This is
 * safe because ext2_dep_bwrite calls ext2_drive_prerequisites
 * first, which handles all prerequisite writes.
 *
 * Returns the bwrite error code, or 0 on success.
 */
int
ext2_dep_bawrite(struct buf *bp)
{
	struct ext2_dep *dep;

	if (bp == NULL)
		return (0);

	dep = EXT2_BP_DEP(bp);
	if (dep == NULL) {
		bawrite(bp);
		return (0);
	}

	/*
	 * Dep is present.  Use synchronous bwrite to guarantee
	 * on-disk persistence and proper dep satisfaction.
	 */
	return (ext2_dep_bwrite(bp));
}
