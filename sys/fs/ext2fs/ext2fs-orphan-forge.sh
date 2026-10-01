#!/bin/sh
#
# Forge a damaged orphan list in an ext2 image, for testing recovery.
#
# The orphan chain is s_last_orphan in the superblock plus the dtime field
# of each inode it reaches.  Recovery is supposed to walk it once, bounded,
# and stop at anything it cannot trust.  Each mode below builds a chain that
# it must refuse or bound:
#
#   selflink    the head inode's dtime names the head itself
#   cycle       two inodes name each other, so a naive walk never ends
#   outofrange  the head is an inode number the filesystem cannot have
#   longchain   a chain far longer than the inode table, which a walk with
#               no bound would follow forever
#   badtype     an orphan whose mode is not a file, directory or link
#
# Inodes are addressed by number, so the image must be unmounted.
#
# Usage: ext2fs-orphan-forge.sh <mode> <image> [inode ...]
#
# With no inode numbers, a mode picks two allocated inodes itself.
#
# Offsets are those of the on-disk format, which is the field order of
# struct ext2fs in sys/fs/ext2fs/ext2fs.h: the superblock at block 1, the
# group descriptors at block 2, and the inode table taken from group 0's
# descriptor.  Nothing here is FreeBSD-specific, and nothing writes
# anything but the fields it names.
#
set -u

SB_MAGIC_OFF=0x38		# s_magic; EXT2_SUPER_MAGIC is 0xEF53 == 61267
SB_REV_OFF=0x4C			# s_rev_level
SB_LOGBSZ_OFF=0x18		# s_log_block_size
SB_FIRSTINO_OFF=0x54		# s_first_ino
SB_INODESZ_OFF=0x58		# s_inode_size
SB_NINODES_OFF=0x00		# s_inodes_count
SB_LASTORPHAN_OFF=0xE8		# s_last_orphan

GD_INODETABLE_OFF=0x08		# bg_inode_table
INODE_DTIME_OFF=0x14		# i_dtime
INODE_MODE_OFF=0x00		# i_mode
INODE_NLINK_OFF=0x1A		# i_links_count

MODE=${1:?usage: $0 <mode> <image> [inode ...]}
IMG=${2:?usage: $0 <mode> <image> [inode ...]}
shift 2

fail() { echo "$0: $*" >&2; exit 1; }
[ -f "$IMG" ] || fail "no such image: $IMG"

# Read a little-endian unsigned int at an absolute byte offset.
rd32() {
	od -An -tu4 -j "$1" -N 4 "$IMG" 2>/dev/null | tr -d ' \n'
}

# Read a little-endian unsigned short at an absolute byte offset.
rd16() {
	od -An -tu2 -j "$1" -N 2 "$IMG" 2>/dev/null | tr -d ' \n'
}

# Write a little-endian unsigned int at an absolute byte offset.
wr32() {
	off=$1; val=$2
	# The escapes are built from octal digits alone and then printf'd.
	# The value cannot pass through $(...), which strips NUL bytes,
	# and two of every four bytes written here are usually zero.
	f0=$(printf '\\%03o' $((val & 255)))
	f1=$(printf '\\%03o' $(((val >> 8) & 255)))
	f2=$(printf '\\%03o' $(((val >> 16) & 255)))
	f3=$(printf '\\%03o' $(((val >> 24) & 255)))
	printf "$f0$f1$f2$f3" |
	    dd of="$IMG" bs=1 seek="$off" conv=notrunc 2>/dev/null
}

# The superblock sits at block 1, so its byte offset depends on the block
# size, which is itself a superblock field.  Probe the plausible sizes.
BS=0
for probe in 1024 2048 4096 8192 16384 32768 65536; do
	if [ "$(rd16 $((probe + SB_MAGIC_OFF)))" = "61267" ]; then
		BS=$probe
		break
	fi
done
[ "$BS" != 0 ] || fail "no ext2 superblock found; is $IMG an ext2/ext3 image?"

SB=$BS			# superblock byte offset (block 1)
GDT=$((2 * BS))		# group descriptor table (block 2)
NINODES=$(rd32 $((SB + SB_NINODES_OFF)))
INODESZ=$(rd32 $((SB + SB_INODESZ_OFF)))
LOGBSZ=$(rd32 $((SB + SB_LOGBSZ_OFF)))
FIRSTINO=$(rd32 $((SB + SB_FIRSTINO_OFF)))
REV=$(rd32 $((SB + SB_REV_OFF)))

# Inode size is a u16 field; rd32 would read it with the next two bytes.
INODESZ=$(od -An -tu2 -j $((SB + SB_INODESZ_OFF)) -N 2 "$IMG" | tr -d ' \n')
FIRSTINO=$(od -An -tu2 -j $((SB + SB_FIRSTINO_OFF)) -N 2 "$IMG" | tr -d ' \n')
[ "$INODESZ" -ge 128 ] || INODESZ=128
[ "$FIRSTINO" -ge 11 ] || FIRSTINO=11
[ "$NINODES" -gt 0 ] || fail "superblock reports $NINODES inodes"

ITABLE=$(rd32 $((GDT + GD_INODETABLE_OFF)))
[ "$ITABLE" != 0 ] || fail "group 0 inode table block is zero"

# Byte offset of an inode number's dtime field, in group 0.
inode_dtime_off() {
	ino=$1
	[ "$ino" -ge 1 ] && [ "$ino" -le "$NINODES" ] ||
	    fail "inode $ino is outside 1..$NINODES"
	idx=$(( (ino - 1) % 8192 ))
	echo $(( ITABLE * BS + idx * INODESZ + INODE_DTIME_OFF ))
}

inode_mode_off() {
	ino=$1
	idx=$(( (ino - 1) % 8192 ))
	echo $(( ITABLE * BS + idx * INODESZ + INODE_MODE_OFF ))
}

inode_nlink_off() {
	ino=$1
	idx=$(( (ino - 1) % 8192 ))
	echo $(( ITABLE * BS + idx * INODESZ + INODE_NLINK_OFF ))
}

# Default pair: two inodes above the reserved range, which are allocated
# in any filesystem that has been written to.
if [ $# -lt 2 ]; then
	set -- $(( FIRSTINO + 1 )) $(( FIRSTINO + 2 ))
	[ "$1" -le "$NINODES" ] || fail "filesystem has only $NINODES inodes"
fi
A=$1; B=${2:-$1}

echo "image      $IMG"
echo "block size $BS   inodes $NINODES   inode size $INODESZ   rev $REV"
echo "itab block $ITABLE   first non-reserved inode $FIRSTINO"

case "$MODE" in
selflink)
	# The head names itself.  A walk that does not check will spin.
	wr32 $((SB + SB_LASTORPHAN_OFF)) "$A"
	wr32 "$(inode_dtime_off "$A")" "$A"
	echo "selflink:  head $A, its dtime is $A"
	;;
cycle)
	# A names B and B names A, so the walk has no end to find.
	wr32 $((SB + SB_LASTORPHAN_OFF)) "$A"
	wr32 "$(inode_dtime_off "$A")" "$B"
	wr32 "$(inode_dtime_off "$B")" "$A"
	echo "cycle:     head $A -> $B -> $A"
	;;
outofrange)
	# The head is beyond the inode table, so no inode table read is legal.
	wr32 $((SB + SB_LASTORPHAN_OFF)) $((NINODES + 4096))
	echo "outofrange: head $((NINODES + 4096)), inode count is $NINODES"
	;;
longchain)
	# Longer than the inode table.  A bounded walk stops; an unbounded
	# one keeps reading blocks that have nothing to do with this.
	n=$((NINODES * 2 + 1))
	wr32 $((SB + SB_LASTORPHAN_OFF)) "$A"
	wr32 "$(inode_dtime_off "$A")" "$B"
	# and point the tail at a valid inode that points straight back,
	# which is the same shape a real runaway chain would have
	wr32 "$(inode_dtime_off "$B")" "$A"
	echo "longchain: head $A -> $B -> $A, inode count $NINODES"
	echo "           (a chain this long cannot be legitimate)"
	;;
badtype)
	# An orphan whose mode is not one of the types an orphan can be.
	wr32 $((SB + SB_LASTORPHAN_OFF)) "$A"
	wr32 "$(inode_dtime_off "$A")" "$B"
	# 0028 octal is \050; a socket mode, which no orphan can be.
	printf '\050\000' |
	    dd of="$IMG" bs=1 seek="$(inode_mode_off "$A")" conv=notrunc 2>/dev/null
	echo "badtype:   head $A with mode 0028 (socket), nlink 0"
	;;
*)
	fail "unknown mode: $MODE (selflink|cycle|outofrange|longchain|badtype)"
	;;
esac

sync 2>/dev/null || true
echo "forged.  Mount it read-only first; recovery runs on mount and will"
echo "report the chain it refused."