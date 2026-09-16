/*-
 *  modified for Lites 1.1
 *
 *  Aug 1995, Godmar Back (gback@cs.utah.edu)
 *  University of Utah, Department of Computer Science
 */
/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1982, 1986, 1989, 1993
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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/mount.h>
#include <sys/sdt.h>
#include <sys/vnode.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/ext2_mount.h>

SDT_PROVIDER_DECLARE(ext2fs);

static int
ext2_ext_balloc(struct inode *ip, uint32_t lbn, int size,
    struct ucred *cred, struct buf **bpp, int flags)
{
	struct m_ext2fs *fs;
	struct buf *bp = NULL;
	struct vnode *vp = ITOV(ip);
	daddr_t newblk;
	int blks, error, allocated;

	fs = ip->i_e2fs;
	blks = howmany(size, fs->e2fs_bsize);

	error = ext4_ext_get_blocks(ip, lbn, blks, cred, NULL, &allocated, &newblk);
	if (error)
		return (error);

	if (allocated) {
		bp = getblk(vp, lbn, fs->e2fs_bsize, 0, 0, 0);
		if(!bp)
			return (EIO);
	} else {
		error = bread(vp, lbn, fs->e2fs_bsize, NOCRED, &bp);
		if (error) {
			return (error);
		}
	}

	bp->b_blkno = fsbtodb(fs, newblk);
	if (flags & BA_CLRBUF)
		vfs_bio_clrbuf(bp);

	*bpp = bp;

	return (error);
}

/*
 * Balloc defines the structure of filesystem storage
 * by allocating the physical blocks on a device given
 * the inode and the logical block number in a file.
 */
int
ext2_balloc(struct inode *ip, e2fs_lbn_t lbn, int size, struct ucred *cred,
    struct buf **bpp, int flags)
{
	struct m_ext2fs *fs;
	struct ext2mount *ump;
	struct buf *bp, *nbp;
	struct vnode *vp = ITOV(ip);
	struct indir indirs[EXT2_NIADDR + 2];
	struct ext2_alloc_context ctx;
	e2fs_daddr_t *bap;
	e4fs_daddr_t bpref, pref, newb;
	int num, i, error;

	*bpp = NULL;
	if (lbn < 0)
		return (EFBIG);
	fs = ip->i_e2fs;
	ump = ip->i_ump;

	/*
	 * check if this is a sequential block allocation.
	 * If so, increment i_next_alloc_block to allow ext2_blkpref
	 * to make a good guess.
	 *
	 * Note: i_next_alloc_goal is no longer incremented here —
	 * ext2_commit_allocated_block() now sets it to
	 * par_physical_start + par_length after each allocation,
	 * which correctly points to the preferred continuation block.
	 */
	if (lbn == ip->i_next_alloc_block + 1)
		ip->i_next_alloc_block++;

	if (ip->i_flag & IN_E4EXTENTS)
		return (ext2_ext_balloc(ip, lbn, size, cred, bpp, flags));

	/*
	 * The first EXT2_NDADDR blocks are direct blocks
	 */
	if (lbn < EXT2_NDADDR) {
		pref = ip->i_db[lbn];
		if (pref != 0) {
			error = bread(vp, lbn, fs->e2fs_bsize, NOCRED, &bp);
			if (error) {
				return (error);
			}
			bp->b_blkno = fsbtodb(fs, pref);
			if (ip->i_size >= (lbn + 1) * fs->e2fs_bsize) {
				*bpp = bp;
				return (0);
			}
		} else {
			EXT2_LOCK(ump);
			error = ext2_alloc(ip, lbn,
			    ext2_blkpref(ip, lbn, (int)lbn, &ip->i_db[0], 0),
			    fs->e2fs_bsize, cred, &newb);
			if (error)
				return (error);
			/*
			 * If the newly allocated block exceeds 32-bit limit,
			 * we can not use it in file block maps.
			 */
			if (newb > UINT_MAX) {
				/*
				 * Block is unpublished: ext2_alloc() returned
				 * it via newb but we have not yet installed it
				 * in i_db[lbn].  ext2_rollback_unpublished()
				 * handles lock/re-accounting/free atomically.
				 */
				ext2_rollback_unpublished(ip, newb, 1);
				return (EFBIG);
			}
			bp = getblk(vp, lbn, fs->e2fs_bsize, 0, 0, 0);
			bp->b_blkno = fsbtodb(fs, newb);
			if (flags & BA_CLRBUF)
				vfs_bio_clrbuf(bp);
		}
		ip->i_db[lbn] = dbtofsb(fs, bp->b_blkno);
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
		*bpp = bp;
		return (0);
	}
	/*
	 * Determine the number of levels of indirection.
	 */
	bpref = 0;
	if ((error = ext2_getlbns(vp, lbn, indirs, &num)) != 0)
		return (error);
#ifdef INVARIANTS
	if (num < 1)
		panic("ext2_balloc: ext2_getlbns returned indirect block");
#endif
	/*
	 * Fetch the first indirect block allocating if necessary.
	 */
	--num;
	pref = ip->i_ib[indirs[0].in_off];
	if (pref == 0) {
		EXT2_LOCK(ump);
		bpref = ext2_blkpref(ip, lbn, indirs[0].in_off +
		    EXT2_NDIR_BLOCKS, &ip->i_db[0], 0);
		error = ext2_alloc(ip, lbn, bpref, fs->e2fs_bsize, cred,
		    &newb);
		if (error)
			return (error);
		if (newb > UINT_MAX) {
			/*
			 * Block is unpublished: ext2_alloc() returned it but
			 * we have not yet installed it in i_ib[].
			 */
			ext2_rollback_unpublished(ip, newb, 1);
			return (EFBIG);
		}
		pref = newb;
		bp = getblk(vp, indirs[1].in_lbn, fs->e2fs_bsize, 0, 0, 0);
		bp->b_blkno = fsbtodb(fs, pref);
		vfs_bio_clrbuf(bp);
		/*
		 * Write synchronously so that indirect blocks
		 * never point at garbage.
		 */
		if ((error = bwrite(bp)) != 0) {
			/*
			 * bwrite failed after allocating the indirect block.
			 * The block is unpublished: pref was set but not yet
			 * stored in ip->i_ib[].  bwrite already released bp.
			 */
			ext2_rollback_unpublished(ip, pref, 1);
			return (error);
		}
		ip->i_ib[indirs[0].in_off] = pref;
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
	}
	/*
	 * Fetch through the indirect blocks, allocating as necessary.
	 */
	for (i = 1;;) {
		error = bread(vp,
		    indirs[i].in_lbn, (int)fs->e2fs_bsize, NOCRED, &bp);
		if (error) {
			return (error);
		}
		bap = (e2fs_daddr_t *)bp->b_data;
		pref = le32toh(bap[indirs[i].in_off]);
		if (i == num)
			break;
		i += 1;
		if (pref != 0) {
			bqrelse(bp);
			continue;
		}
		EXT2_LOCK(ump);
		if (bpref == 0)
			bpref = ext2_blkpref(ip, lbn, indirs[i].in_off, bap,
			    bp->b_lblkno);
		error = ext2_alloc(ip, lbn, bpref, (int)fs->e2fs_bsize, cred, &newb);
		if (error) {
			brelse(bp);
			return (error);
		}
		if (newb > UINT_MAX) {
			/*
			 * Block is unpublished: ext2_alloc() returned it but
			 * we have not yet installed it in bap[].  bp (parent
			 * indirect block) is still held and must be released.
			 */
			ext2_rollback_unpublished(ip, newb, 1);
			brelse(bp);
			return (EFBIG);
		}
		pref = newb;
		nbp = getblk(vp, indirs[i].in_lbn, fs->e2fs_bsize, 0, 0, 0);
		nbp->b_blkno = fsbtodb(fs, pref);
		vfs_bio_clrbuf(nbp);
		/*
		 * Write synchronously so that indirect blocks
		 * never point at garbage.
		 */
		if ((error = bwrite(nbp)) != 0) {
			/*
			 * bwrite failed after allocating the indirect block.
			 * The block is unpublished (pref not yet stored in
			 * bap[]).  bwrite already released nbp; bp is still
			 * held as the parent indirect block and must be
			 * released.
			 */
			ext2_rollback_unpublished(ip, pref, 1);
			brelse(bp);
			return (error);
		}
		bap[indirs[i - 1].in_off] = htole32(pref);
		/*
		 * If required, write synchronously, otherwise use
		 * delayed write.  bwrite() and bdwrite() both release
		 * 'bp' internally; the loop re-assigns bp via bread()
		 * at the top of the next iteration.
		 */
		if (flags & IO_SYNC) {
			if ((error = bwrite(bp)) != 0) {
				/*
				 * bwrite of the parent indirect block
				 * failed after the child indirect block
				 * (pref) was allocated, i_blocks was
				 * incremented, and the child was written.
				 * The child is now orphaned: it exists on
				 * disk but is not reachable from the
				 * inode.  Roll it back.
				 */
				ext2_rollback_unpublished(ip, pref, 1);
				bp = NULL;
				return (error);
			}
		} else {
			if (bp->b_bufsize == fs->e2fs_bsize)
				bp->b_flags |= B_CLUSTEROK;
			bdwrite(bp);
			bp = NULL;
		}
	}
	/*
	 * Get the data block, allocating if necessary.
	 */
	if (pref == 0) {
		/*
		 * Allocate the data block.  In the traditional (non-extent)
		 * mapping path, only one logical block is mapped per call,
		 * so we use EXT2_ALLOC_DATA_RAND to avoid silently
		 * preallocating blocks the caller cannot install into the
		 * indirect block array.
		 */
		EXT2_LOCK(ump);
		if (fs->e2fs_fbcount == 0)
			goto nospace;
		bpref = ext2_blkpref(ip, lbn, indirs[i].in_off, &bap[0],
		    bp->b_lblkno);
		error = ext2_alloc_run(ip, lbn, 1, bpref,
		    EXT2_ALLOC_DATA_RAND, cred, &ctx);
		/*
		 * Lock contract: ext2_alloc_run returns with lock held.
		 * On failure, goto nospace (which unlocks).
		 */
		if (error)
			goto nospace;
		/*
		 * ext2_alloc_run succeeded; lock is still held.
		 * Commit inode accounting (i_blocks, hints, flags),
		 * then install the block.
		 */
		pref = ctx.run.par_physical_start;
		ext2_commit_allocated_block(ip, &ctx);
		if (pref > UINT_MAX) {
			/*
			 * ext2_alloc_run succeeded with lock held, and
			 * ext2_commit_allocated_block() incremented
			 * i_blocks.  The block is unpublished: pref
			 * was assigned but not yet stored in bap[] or
			 * nbp's b_blkno.  Release the lock, then let
			 * ext2_rollback_allocation() re-lock, re-account,
			 * and free.
			 */
			EXT2_UNLOCK(ump);
			ext2_rollback_allocation(&ctx);
			brelse(bp);
			return (EFBIG);
		}
		nbp = getblk(vp, lbn, fs->e2fs_bsize, 0, 0, 0);
		nbp->b_blkno = fsbtodb(fs, pref);
		if (flags & BA_CLRBUF)
			vfs_bio_clrbuf(nbp);
		bap[indirs[i].in_off] = htole32(pref);
		ext2_alloc_transition(&ctx, EXT2_ALLOC_MAPPED);
		/*
		 * If required, write synchronously, otherwise use
		 * delayed write.
		 */
		if (flags & IO_SYNC) {
			if ((error = bwrite(bp)) != 0) {
				/*
				 * bwrite failed for the parent indirect
				 * block after the data block was
				 * allocated, committed, and its pointer
				 * installed in the in-memory indirect
				 * block.  Roll back the allocation,
				 * release the data buffer, and return
				 * the I/O error.
				 *
				 * Safe to rollback: state is MAPPED
				 * (not yet PUBLISHED) — the
				 * PUBLISHED transition has not
				 * occurred because bwrite failed.
				 */
				EXT2_UNLOCK(ump);
				ext2_rollback_allocation(&ctx);
				brelse(nbp);
				return (error);
			}
			ext2_alloc_transition(&ctx, EXT2_ALLOC_PUBLISHED);
		} else {
			if (bp->b_bufsize == fs->e2fs_bsize)
				bp->b_flags |= B_CLUSTEROK;
			bdwrite(bp);
		}
		*bpp = nbp;
		EXT2_UNLOCK(ump);
		return (0);
	}
	brelse(bp);
	if (flags & BA_CLRBUF) {
		int seqcount = (flags & BA_SEQMASK) >> BA_SEQSHIFT;

		if (seqcount && (vp->v_mount->mnt_flag & MNT_NOCLUSTERR) == 0) {
			error = cluster_read(vp, ip->i_size, lbn,
			    (int)fs->e2fs_bsize, NOCRED,
			    MAXBSIZE, seqcount, 0, &nbp);
		} else {
			error = bread(vp, lbn, (int)fs->e2fs_bsize, NOCRED, &nbp);
		}
		if (error) {
			brelse(nbp);
			return (error);
		}
	} else {
		nbp = getblk(vp, lbn, fs->e2fs_bsize, 0, 0, 0);
		nbp->b_blkno = fsbtodb(fs, pref);
	}
	*bpp = nbp;
	return (0);
nospace:
	/*
  * Reached only via goto from pre-check failures (fbcount == 0)
  * or ext2_alloc_run() failure, both of which leave EXT2_LOCK held.
	 */
	mtx_assert(EXT2_MTX(ump), MA_OWNED);
	brelse(bp);
	EXT2_UNLOCK(ump);
	SDT_PROBE2(ext2fs, , alloc, trace, 1, "cannot allocate data block");
	return (ENOSPC);
}
