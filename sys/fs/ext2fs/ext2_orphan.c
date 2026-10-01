/*-
 * Copyright (c) 1991, 1993
 *		The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Network Information Systems Group.
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
 * ext2 orphan list.
 *
 * When the last link to a file goes away while the file is still open,
 * the inode cannot be freed: the file is still being read and written
 * through, and its blocks are still its own.  The inode is put on an
 * orphan list instead, so that if the system crashes before the file is
 * closed and the inode reclaimed, the blocks and the inode can be found
 * again without scanning the whole inode table.
 *
 * The list is the one ext2 already provides for this purpose and uses no
 * storage of its own:
 *
 *	s_last_orphan	(superblock)	first inode on the list
 *	i_dtime		(inode)		the previous entry, 0 at the tail
 *
 * Each orphan is therefore reachable from the superblock through the
 * inodes it links.  Nothing else is recorded, and nothing needs to be:
 * a crash mid-unlink leaves exactly this structure behind, because the
 * inode write and the superblock write are ordered so that the head is
 * never published before the inode that carries the linkage.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/endian.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/ext2_extattr.h>
#include <fs/ext2fs/ext2_extern.h>

/*
 * Revision level from which ext2fs itself stopped maintaining an orphan
 * list.  ext4 has its own orphan file, so s_last_orphan is dead weight
 * there and must not be written.
 */
#define	EXT2_REV_EXT4	0x40000

/*
 * Hard ceiling on the number of entries walked before a chain is declared
 * corrupt.  A well-formed list cannot be longer than the inode table, so
 * anything at or above this is a symptom rather than a filesystem state.
 */
#define	EXT2_ORPHAN_WALK_MAX	1048576

static uint64_t ext2_orphan_ninodes(struct m_ext2fs *fs);
static int ext2_orphan_supported(struct m_ext2fs *fs);
static int ext2_orphan_valid(struct m_ext2fs *fs, uint32_t ino);
static int ext2_orphan_unlink(struct inode *ip, uint32_t next);

/*
 * Total number of inode slots in the filesystem.  Used both to bound the
 * walk and to reject inode numbers that cannot exist.
 */
static uint64_t
ext2_orphan_ninodes(struct m_ext2fs *fs)
{

	return ((uint64_t)fs->e2fs_ipg * fs->e2fs_gcount);
}

/*
 * Whether this filesystem keeps an orphan list at all.
 */
static int
ext2_orphan_supported(struct m_ext2fs *fs)
{

	return (le32toh(fs->e2fs->e2fs_rev) < EXT2_REV_EXT4);
}

/*
 * Whether an inode number can appear in the chain.  Zero is the list
 * terminator and is checked separately by callers.
 */
static int
ext2_orphan_valid(struct m_ext2fs *fs, uint32_t ino)
{
	uint64_t max = ext2_orphan_ninodes(fs);

	if (ino < EXT2_ROOTINO || (uint64_t)ino > max)
		return (0);
	if (ino < EXT2_FIRST_INO(fs) && ino != EXT2_ROOTINO)
		return (0);
	return (1);
}

/*
 * Add an inode to the orphan list.
 *
 * The inode keeps its blocks and its link count of zero; it is simply no
 * longer reachable from a directory, which is what makes it an orphan and
 * what this list records.
 *
 * Called with the vnode locked for write.  Returns 0 on success.
 */
int
ext2_orphan_add(struct inode *ip)
{
	struct m_ext2fs *fs;
	struct ext2mount *ump;
	uint32_t prev;
	int error;

	fs = ip->i_e2fs;
	ump = ip->i_ump;

	if (fs->e2fs_ronly || !ext2_orphan_supported(fs))
		return (0);
	if (ip->i_nlink > 0)
		return (0);

	EXT2_LOCK(ump);
	prev = le32toh(fs->e2fs->e3fs_last_orphan);
	EXT2_UNLOCK(ump);
	if (prev == ip->i_number)
		return (0);
	if (prev != 0 && !ext2_orphan_valid(fs, prev)) {
		/*
		 * Publishing onto a chain we cannot trust would extend
		 * garbage.  Leave the filesystem as it is; the inode stays
		 * allocated, which costs space rather than correctness.
		 */
		return (EINVAL);
	}

	EXT2_LOCK(ump);
	ip->i_dtime = prev;
	ip->i_flag |= IN_CHANGE;
	fs->e2fs->e3fs_last_orphan = htole32(ip->i_number);
	EXT2_UNLOCK(ump);

	/*
	 * The linkage lives in the inode, the head lives in the superblock.
	 * Write the inode first: the reverse order would publish a head
	 * pointing at an inode whose predecessor has not reached the disk,
	 * and recovery would follow a link that is not there yet.  This way
	 * an interruption leaves an inode that is allocated and orphaned
	 * but not yet listed, which fsck reclaims.
	 */
	error = ext2_update(ip->i_vnode, 1);
	if (error)
		return (error);

	return (ext2_sbupdate(ump, MNT_WAIT));
}

/*
 * Record an inode that has just lost its last link while still open.
 *
 * Every path that drops a link count routes through here, so that
 * unlink, rename replacing a name, and rename moving a directory out of
 * another one all leave the same record behind.  Inodes that are not
 * open are ignored: they are about to be reclaimed through
 * ext2_inactive(), which frees them in the normal course of events.
 */
void
ext2_orphan_maybe_add(struct inode *ip)
{
	int error;

	if (ip->i_nlink != 0 || ip->i_vnode == NULL)
		return;
	if (ip->i_vnode->v_usecount == 0)
		return;

	error = ext2_orphan_add(ip);
	if (error != 0) {
		/*
		 * The name is already gone, so this cannot be reported as a
		 * failed operation.  The inode stays allocated and
		 * unreachable, which costs space rather than correctness.
		 */
		printf("ext2fs: %s: could not add unlinked inode %lu to "
		    "the orphan list (%d)\n", ip->i_e2fs->e2fs_fsmnt,
		    (u_long)ip->i_number, error);
	}
}

/*
 * Detach an inode from the orphan chain, splicing 'next' into the slot
 * the inode occupied.
 *
 * The chain is updated before the caller's own i_dtime is cleared.  If the
 * clear were durable first, a crash would leave the chain terminating at
 * this inode and everything behind it unreachable.
 */
static int
ext2_orphan_unlink(struct inode *ip, uint32_t next)
{
	struct m_ext2fs *fs;
	struct ext2mount *ump;
	struct vnode *pvp;
	struct inode *pip;
	uint64_t bound;
	uint32_t cur;
	int error;

	fs = ip->i_e2fs;
	ump = ip->i_ump;

	EXT2_LOCK(ump);
	cur = le32toh(fs->e2fs->e3fs_last_orphan);
	EXT2_UNLOCK(ump);

	if (cur == 0)
		return (ENOENT);

	/*
	 * If this inode is the head, the superblock holds the linkage and
	 * no inode has to be searched.
	 */
	if (cur == ip->i_number) {
		EXT2_LOCK(ump);
		fs->e2fs->e3fs_last_orphan = htole32(next);
		EXT2_UNLOCK(ump);
		return (ext2_sbupdate(ump, MNT_WAIT));
	}

	if (!ext2_orphan_valid(fs, cur))
		return (ENOENT);

	/*
	 * Otherwise some earlier inode names this one, and that inode has
	 * to be found and rewritten.  Walk from the head.  The list is
	 * short in practice and this is not a hot path, so the walk is
	 * bounded by the inode table rather than maintained incrementally.
	 */
	bound = ext2_orphan_ninodes(fs);
	if (bound > EXT2_ORPHAN_WALK_MAX)
		bound = EXT2_ORPHAN_WALK_MAX;

	error = VFS_VGET(ump->um_mountp, cur, LK_EXCLUSIVE, &pvp);
	if (error)
		return (error);
	pip = VTOI(pvp);

	error = ENOENT;
	for (uint64_t i = 0; i < bound && pvp != NULL; i++) {
		uint32_t nxt = pip->i_dtime;

		if (nxt == ip->i_number) {
			pip->i_dtime = next;
			pip->i_flag |= IN_CHANGE;
			error = ext2_update(pvp, 1);
			break;
		}
		if (nxt == 0 || !ext2_orphan_valid(fs, nxt))
			break;

		cur = nxt;
		vput(pvp);
		pvp = NULL;
		if (VFS_VGET(ump->um_mountp, cur, LK_EXCLUSIVE, &pvp) != 0)
			break;
		pip = VTOI(pvp);
	}
	if (pvp != NULL)
		vput(pvp);

	return (error);
}

/*
 * Release an inode whose last link is gone.
 *
 * The inode is truncated, detached from the orphan list if it is on it,
 * and its number returned to the allocator.  Called with the vnode locked
 * for write; on success the inode may not be referenced again.
 *
 * Ordering, all of it synchronous:
 *
 *	truncate			blocks go back to the bitmap
 *	write the emptied inode		the directory can no longer reach it
 *	rewrite chain or superblock	no on-disk record names it
 *	ext2_vfree()			the number may be reused
 *
 * Freeing the number before the emptied inode is on disk would let a crash
 * hand the number to a new inode while the old one's blocks are still
 * described by the old contents of that slot.
 */
int
ext2_orphan_release(struct inode *ip)
{
	struct m_ext2fs *fs;
	struct vnode *vp;
	uint32_t next;
	int mode, error;

	fs = ip->i_e2fs;
	vp = ip->i_vnode;
	next = ip->i_dtime;

	if (ip->i_nlink > 0)
		return (0);
	if (fs->e2fs_ronly || !ext2_orphan_supported(fs))
		return (0);

	error = ext2_truncate(vp, 0, 0, NOCRED, curthread);
	if (error)
		return (error);

	ext2_extattr_free(ip);

	ip->i_rdev = 0;
	mode = ip->i_mode;
	ip->i_mode = 0;
	ip->i_flag |= IN_CHANGE | IN_UPDATE;

	/*
	 * The emptied inode must reach the disk before its number is
	 * returned to the allocator.  The other order lets a crash hand
	 * the number to a new inode while the old slot still describes
	 * blocks that have already been freed.
	 *
	 * i_dtime is deliberately left alone for now, so that the inode is
	 * still recognisable as the list entry until it is detached.
	 */
	error = ext2_update(vp, 1);
	if (error)
		return (error);

	/*
	 * Detach before clearing i_dtime.  Clearing it first would, on a
	 * crash, leave the head pointing at an inode whose successor has
	 * just become zero, stranding every orphan behind it.  Detaching
	 * first means an interruption leaves this inode listed but already
	 * empty, which the next pass reclaims without freeing anything
	 * twice.
	 *
	 * ENOENT means the inode was not on the list at all, which is the
	 * normal case for an unlinked-but-never-orphaned inode.
	 */
	error = ext2_orphan_unlink(ip, next);
	if (error != 0 && error != ENOENT)
		return (error);

	ip->i_dtime = 0;
	ip->i_flag |= IN_CHANGE;
	error = ext2_update(vp, 1);
	if (error)
		return (error);

	ext2_vfree(vp, ip->i_number, mode);
	return (0);
}

/*
 * Walk the orphan list and release every entry.
 *
 * 'context' names the caller in diagnostics.  The walk is bounded and
 * stops at the first entry that cannot be trusted: an out-of-range number,
 * a self-link, an unreadable inode, or more entries than the inode table
 * could hold.  A corrupt chain must not be able to loop, and must not be
 * followed into arbitrary blocks.
 */
void
ext2_orphan_drain(struct ext2mount *ump, const char *context)
{
	struct m_ext2fs *fs;
	struct mount *mp;
	struct vnode *vp;
	struct inode *ip;
	uint64_t bound, n;
	uint32_t ino, next, head;
	int error, failed;

	fs = ump->um_e2fs;
	mp = ump->um_mountp;

	if (fs->e2fs_ronly || !ext2_orphan_supported(fs))
		return;

	bound = ext2_orphan_ninodes(fs);
	if (bound > EXT2_ORPHAN_WALK_MAX)
		bound = EXT2_ORPHAN_WALK_MAX;

	EXT2_LOCK(ump);
	ino = le32toh(fs->e2fs->e3fs_last_orphan);
	EXT2_UNLOCK(ump);

	failed = 0;
	for (n = 0; ino != 0 && n < bound; n++) {
		if (!ext2_orphan_valid(fs, ino)) {
			printf("ext2fs: %s: orphan inode %u out of range, "
			    "stopping at entry %ju\n", context, ino,
			    (uintmax_t)n);
			break;
		}
		if (VFS_VGET(mp, ino, LK_EXCLUSIVE, &vp) != 0) {
			printf("ext2fs: %s: orphan inode %u unreadable, "
			    "stopping at entry %ju\n", context, ino,
			    (uintmax_t)n);
			break;
		}
		ip = VTOI(vp);
		next = ip->i_dtime;

		if (next == ino) {
			printf("ext2fs: %s: orphan inode %u links to itself, "
			    "stopping at entry %ju\n", context, ino,
			    (uintmax_t)n);
			vput(vp);
			break;
		}
		if (next != 0 && !ext2_orphan_valid(fs, next)) {
			printf("ext2fs: %s: orphan inode %u points at %u, "
			    "stopping at entry %ju\n", context, ino, next,
			    (uintmax_t)n);
			vput(vp);
			break;
		}

		if (ip->i_vnode->v_usecount > 0) {
			/*
			 * Never reclaim a file that is still open.  Nothing
			 * should be open by the time the list is drained,
			 * but truncating a live inode would discard writes
			 * that have not reached the disk, so leave it listed
			 * and say so.
			 */
			printf("ext2fs: %s: orphan inode %u still open, "
			    "leaving it listed\n", context, ino);
			vput(vp);
			ino = next;
			continue;
		}

		error = ext2_orphan_release(ip);
		vput(vp);
		if (error) {
			/*
			 * The entry is still on the list and still owns
			 * blocks; releasing the successor would leave a gap
			 * nothing records.  Stop and let the next pass
			 * retry from the same head.
			 */
			printf("ext2fs: %s: orphan inode %u could not be "
			    "released (%d), stopping at entry %ju\n",
			    context, ino, error, (uintmax_t)n);
			failed = 1;
			break;
		}
		ino = next;
	}

	if (ino == 0)
		return;

	/*
	 * Anything still listed means the walk stopped early.  Report the
	 * head rather than guessing how much is left, and leave the chain
	 * in place: a list we refused to trust is fsck's problem, and
	 * clearing it here would destroy the only record of the blocks
	 * these inodes still hold.
	 */
	EXT2_LOCK(ump);
	head = le32toh(fs->e2fs->e3fs_last_orphan);
	EXT2_UNLOCK(ump);
	printf("ext2fs: %s: stopped after %ju orphan(s), head %u%s\n",
	    context, (uintmax_t)n, head,
	    failed ? " (release failed)" : "");
}

/*
 * Reclaim orphans left behind by an unclean shutdown.
 *
 * Called once during mount, before the filesystem is handed out.  Runs on
 * a read-only mount too, to the extent that it can: the list is walked and
 * validated so that the condition is reported, but nothing is written,
 * because an inode cannot be unlinked and reclaimed on a read-only mount.
 */
void
ext2_orphan_recovery(struct ext2mount *ump)
{
	struct m_ext2fs *fs;
	uint32_t ino;

	fs = ump->um_e2fs;

	if (!ext2_orphan_supported(fs))
		return;

	EXT2_LOCK(ump);
	ino = le32toh(fs->e2fs->e3fs_last_orphan);
	EXT2_UNLOCK(ump);

	if (ino == 0)
		return;

	if (fs->e2fs_ronly) {
		printf("ext2fs: %s: orphan list is not empty (head %u); "
		    "nothing reclaimed, filesystem mounted read-only\n",
		    fs->e2fs_fsmnt, ino);
		return;
	}

	ext2_orphan_drain(ump, "recovery");
}