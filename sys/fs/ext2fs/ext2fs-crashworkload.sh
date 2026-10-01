#!/bin/sh
#
# Workload for the crash matrix.
#
# Exercises the paths whose write ordering the crash points sit between.
# It is run with a crash point armed and is expected not to return; what
# matters is the state it leaves the filesystem in, not its output.
#
# Kept separate from the test driver so that the same workload can be
# run by hand while iterating on a point:
#
#	sysctl vfs.ext2fs_softdep.crash_point=6
#	sysctl vfs.ext2fs_softdep.crash_enable=1
#	cd /tmp/ext2mnt && ./ext2fs-crashworkload.sh
#
set -u
MNT=${MNT:-/tmp/ext2mnt}
DEEP=${DEEP:-50000}
DIRENTS=${DIRENTS:-4000}

cd "$MNT" || exit 1
mkdir -p cw && cd cw || exit 1

# A file past the single-indirect limit, so a new indirect block is
# allocated and written, then named by its parent.  Points 5, 6, 7 and 8
# sit around exactly that.
dd if=/dev/urandom of=deep bs=1k count="$DEEP" 2>/dev/null

# Enough of a directory to force a conversion to a hash index, so a new
# directory block and the index root are written and then named.
mkdir -p big
i=0
while [ $i -lt "$DIRENTS" ]; do
	: > "big/f$i"
	i=$((i+1))
done

# A create: the inode is initialised, written, and only then entered into
# the directory.  Points 4, 9, 10, 11 and 12 sit around that.
dd if=/dev/urandom of=created bs=1k count=64 2>/dev/null

# An unlink of a file that is still open, which is what puts an inode on
# the orphan list.  Points 13 through 15 and 18, 19 sit in that sequence.
dd if=/dev/urandom of=orphan bs=1k count=32 2>/dev/null
exec 9<>orphan
rm -f orphan
dd if=/dev/urandom of=orphan2 bs=1k count=32 2>/dev/null
exec 8<>orphan2
rm -f orphan2

# Close them, which is what drives the orphan list to be drained.
exec 9<&-
exec 8<&-

# A truncate, which is the block-free path: points 16 and 17 sit either
# side of the bitmap update.
truncate -s 0 deep 2>/dev/null

# A rename over an existing name.
: > rename_src
: > rename_dst
mv rename_src rename_dst 2>/dev/null

sync
exit 0