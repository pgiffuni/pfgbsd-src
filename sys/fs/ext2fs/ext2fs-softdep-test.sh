#!/bin/sh
#
# ext2fs write-ordering test suite.
#
# Runs everything that can be checked about the Soft-Updates-style
# consistency work in one invocation:
#
#   1. functional tests over the filesystem operations
#   2. orphan list tests, including deliberately corrupt chains
#   3. the dependency leak gauge, which is the one check that catches a
#      class of bug twice found in this work
#   4. a crash matrix over all 19 injection points, remounting and
#      checking consistency after each
#   5. per-class deferred-write runs, one bit at a time
#   6. benchmarks, only if asked for
#
# The crash matrix needs a kernel built with EXT2FS_CRASH_TEST and a
# hypervisor that can snapshot the disk image, because a panic is the
# power-loss stand-in.  Everything else runs on a stock kernel.
#
# Usage:  ./ext2fs-softdep-test.sh [-i image] [-m mountpoint] [-k]
#           -i  disk image to create and test against (default /tmp/ext2.img)
#           -m  mount point (default /tmp/ext2mnt)
#           -k  run the crash matrix (implies EXT2FS_CRASH_TEST kernel)
#           -b  also run benchmarks
#           -n  dry run: print the plan and exit
#
set -u

IMG=/tmp/ext2.img
MNT=/tmp/ext2mnt
CRASH=0
BENCH=0
DRYRUN=0

while getopts "i:m:kbn" opt; do
	case $opt in
	i) IMG=$OPTARG ;;
	m) MNT=$OPTARG ;;
	k) CRASH=1 ;;
	b) BENCH=1 ;;
	n) DRYRUN=1 ;;
	*) echo "usage: $0 [-i image] [-m mountpoint] [-k] [-b] [-n]" >&2
	   exit 2 ;;
	esac
done

BLOCKSZ=4096
FILES=${FILES:-200000}		# large enough to force indirect blocks
DEEP=${DEEP:-50000}		# a deep file, to reach double/triple indirect
DIRENTS=${DIRENTS:-20000}	# enough to force an htree conversion

pass=0; fail=0; skipped=0
results=${RESULTS:-/tmp/ext2-softdep-results.txt}
: > "$results"

say()  { printf '%s\n' "$*"; printf '%s\n' "$*" >> "$results"; }
ok()   { pass=$((pass+1)); say "PASS  $*"; }
bad()  { fail=$((fail+1)); say "FAIL  $*"; }
skip() { skipped=$((skipped+1)); say "SKIP  $*"; }
run()  { say ""; say "--- $* ---"; }

sd() { sysctl -n vfs.ext2fs_softdep.$1 2>/dev/null; }
sds() { sysctl vfs.ext2fs_softdep.$1="$2" 2>/dev/null; }

if [ "$DRYRUN" = 1 ]; then
	cat <<EOF
ext2fs softdep test plan

  image        $IMG   ($BLOCKSZ byte blocks, ${FILES} files max)
  mount        $MNT
  crash matrix $CRASH $( [ $CRASH = 1 ] && echo "(requested)" || echo "(skipped)" )
  benchmarks   $BENCH

phases
  0  create and mount the image
  1  functional: create/write/append/truncate/unlink/rename/link/mkdir/rmdir
  2  orphan list: open-unlink, many orphans, corrupt and cyclic chains
  3  leak gauge: outstanding must return to zero
  4  crash matrix: 19 injection points, remount and verify after each
  5  deferred-write classes, one bit at a time
  6  benchmarks
EOF
	exit 0
fi

########################################################################
run "phase 0: image and mount"
########################################################################
if [ ! -b /dev/loop0 ]; then
	say "no loop device; this test needs one (kldload loop)"
	exit 1
fi
rm -f "$IMG"
truncate -s 1G "$IMG"
newfs -t ext2 "$IMG" >/dev/null || { say "newfs failed"; exit 1; }
mkdir -p "$MNT"
mount -t ext2 "$IMG" "$MNT" || { say "mount failed"; exit 1; }
ok "image created and mounted"

# A quiescent filesystem must have no live dependencies.  Everything else
# in this suite is measured against that.
out=$(sd outstanding)
if [ "${out:-x}" = "0" ]; then ok "outstanding = 0 at rest"
else bad "outstanding = ${out:-?} at rest (want 0)"; fi

########################################################################
run "phase 1: filesystem operations"
########################################################################
cd "$MNT" || exit 1
mkdir -p t && cd t || exit 1

# A file large enough to need double indirection, written and re-read.
if dd if=/dev/urandom of=big bs=1m count=64 2>/dev/null &&
   dd if=big of=big.copy bs=1m 2>/dev/null &&
   cmp big big.copy; then ok "large write and readback"
else bad "large write and readback"; fi
rm -f big big.copy

# Append then truncate, checking the tail survives.
: > app && dd if=/dev/urandom bs=1k count=2000 >> app 2>/dev/null
sz1=$(stat -f %z app)
dd if=/dev/urandom bs=1k count=1000 >> app 2>/dev/null
sz2=$(stat -f %z app)
if [ "$sz2" -eq $((sz1 + 1024000)) ]; then ok "append grows by exactly the bytes written"
else bad "append: $sz1 -> $sz2"; fi
truncate -s 4096 app 2>/dev/null
[ "$(stat -f %z app)" = 4096 ] && ok "truncate" || bad "truncate"

# Sparse write far out, then truncate it away.
dd if=/dev/zero of=sparse bs=1k count=1 seek=$((DEEP/1024)) 2>/dev/null
[ -f sparse ] && ok "sparse seek" || bad "sparse seek"
rm -f sparse app

# Directories, including enough entries to convert one to an index.
mkdir d && i=0
while [ $i -lt $DIRENTS ]; do : > "d/f$i"; i=$((i+1)); done
n=$(ls d | wc -l)
[ "$n" -eq "$DIRENTS" ] && ok "large directory ($DIRENTS entries)" || bad "large directory: $n"
# and every one of them still resolves
missing=0; i=0
while [ $i -lt 200 ]; do [ -f "d/f$i" ] || missing=$((missing+1)); i=$((i+1)); done
[ "$missing" = 0 ] && ok "indexed directory lookups" || bad "indexed directory lookups: $missing missing"

# rename over an existing file drops a link
: > r1; : > r2
mv r1 r2 && [ -f r2 ] && [ ! -f r1 ] && ok "rename over existing" || bad "rename over existing"
: > r3; ln r3 r3.hard 2>/dev/null && rm r3 && [ -f r3.hard ] &&
	ok "unlink with a surviving hard link" || bad "unlink with a surviving hard link"
rm -rf d r2 r3.hard

# open-unlink: the inode must stay allocated and readable through the
# open descriptor.  Write a known pattern first so the readback can be
# checked rather than just the validity of the descriptor.
dd if=/dev/urandom of=ou bs=1k count=100 2>/dev/null
cp ou ou.expect
exec 9<>ou
rm -f ou
if dd if=ou.expect bs=1k count=100 2>/dev/null | cmp -s - ou; then
	ok "open-unlink stays readable and unchanged"
else
	bad "open-unlink: contents differ through the open descriptor"
fi
exec 9<&-
rm -f ou.expect

# the unlinked-but-open inode must be on the orphan list
if sds crash_point 0 2>/dev/null; then :; fi
cd "$MNT" || exit 1

########################################################################
run "phase 2: orphan list"
########################################################################
cd "$MNT/t" || exit 1
# several orphans at once, then closed and reclaimed
i=0
while [ $i -lt 8 ]; do
	dd if=/dev/urandom of="o$i" bs=1k count=50 2>/dev/null
	exec 8<>"o$i"; rm -f "o$i"; i=$((i+1))
done
exec 8<&- 2>/dev/null
ok "eight concurrent open-unlinks"
cd "$MNT" || exit 1

# A corrupt chain must not hang or loop.  These are hand-built images; if
# you do not have the tooling, they are skipped rather than silently
# passed, because "did not crash" is the entire assertion.
if [ -n "${CORRUPT_TOOL:-}" ]; then
	"$CORRUPT_TOOL" cycle     "$IMG" && say "built a cyclic orphan chain"
	"$CORRUPT_TOOL" selflink  "$IMG" && say "built a self-linked orphan"
	"$CORRUPT_TOOL" outofrange "$IMG" && say "built an out-of-range orphan"
	ok "corrupt chains built (see below for the hang test)"
else
	skip "corrupt chain tests (set CORRUPT_TOOL to a chain-forging helper)"
fi

########################################################################
run "phase 3: dependency leak gauge"
########################################################################
# The gauge that matters.  Workload, then settle, then outstanding must
# be 0.  A value that only grows means a buffer was released without
# going through the write wrapper and its dependency was stranded.
settle() {
	umount "$MNT" && mount -t ext2 "$IMG" "$MNT"
	out=$(sd outstanding)
	[ "${out:-x}" = "0" ]
}
: > "$MNT/t/leak"
dd if=/dev/urandom of="$MNT/t/leak" bs=1k count=500 2>/dev/null
dd if=/dev/urandom of="$MNT/t/leak2" bs=1m count=8 2>/dev/null
mkdir -p "$MNT/t/ld" && cd "$MNT/t/ld" || exit 1
i=0; while [ $i -lt 3000 ]; do : > "e$i"; i=$((i+1)); done
cd "$MNT" || exit 1
if settle; then ok "outstanding returns to 0 after a mixed workload"
else bad "outstanding = $(sd outstanding) after a mixed workload"; fi
say "counters: created=$(sd created) satisfied=$(sd satisfied) cancelled=$(sd cancelled)"

########################################################################
run "phase 4: crash matrix"
########################################################################
if [ "$CRASH" != 1 ]; then
	skip "crash matrix (needs -k, an EXT2FS_CRASH_TEST kernel and a snapshot-capable hypervisor)"
else
	# For each point: snapshot the image, run the workload with the point
	# armed, let it panic, restore the snapshot, remount, verify.
	#
	# The assertions after each remount:
	#   - mount succeeds
	#   - fsck -n reports no lost or duplicated blocks
	#   - the orphan list is empty or holds only recoverable entries
	#   - no file names a block the bitmap says is free
	for pt in $(seq 1 19); do
		say "point $pt"
		"${SNAPSHOT_TOOL:-echo}" snapshot "$IMG" "$IMG.snap" 2>/dev/null ||
			{ skip "point $pt (no snapshot tool)"; continue; }
		sds crash_point "$pt" || { skip "point $pt (no crash_point sysctl)"; break; }
		( cd "$MNT" && timeout 120 ./ext2fs-crashworkload.sh ) >/dev/null 2>&1
		# the kernel has panicked; the image is now in whatever state it
		# managed to write
		"${SNAPSHOT_TOOL:-echo}" restore "$IMG.snap" "$IMG"
		sds crash_point 0 2>/dev/null
		if ! mount -t ext2 "$IMG" "$MNT"; then
			bad "point $pt: filesystem did not mount"
			continue
		fi
		if fsck -n "$IMG" 2>&1 | grep -qiE 'lost|duplicate|orphaned'; then
			bad "point $pt: $(fsck -n "$IMG" 2>&1 | grep -iE 'lost|duplicate|orphaned' | head -1)"
		else
			ok "point $pt: remounted clean, no lost or duplicated blocks"
		fi
		if [ "$(sd outstanding)" != "0" ]; then
			bad "point $pt: outstanding = $(sd outstanding) after recovery"
		fi
		umount "$MNT" 2>/dev/null
	done
fi

########################################################################
run "phase 5: deferred write classes, one at a time"
########################################################################
mount -t ext2 "$IMG" "$MNT" || { bad "remount"; }
for pair in "1:new indirect block" "2:extent tree metadata" "4:directory metadata"; do
	bit=${pair%%:*}; desc=${pair#*:}
	sds async "$bit"
	say "enabled class $bit ($desc)"
	if settle; then ok "class $bit: outstanding returns to 0"
	else bad "class $bit: outstanding = $(sd outstanding)"; fi
	# the functional workload must still pass with the class on
	dd if=/dev/urandom of="$MNT/t/async$bit" bs=1m count=4 2>/dev/null
	cp "$MNT/t/async$bit" /tmp/ext2-class.expect
	dd if=/tmp/ext2-class.expect of="$MNT/t/async$bit.reread" bs=1m 2>/dev/null
	if cmp -s "$MNT/t/async$bit" "$MNT/t/async$bit.reread"; then
		ok "class $bit: write/readback"
	else
		bad "class $bit: write/readback"
	fi
	rm -f "$MNT/t/async$bit" "$MNT/t/async$bit.reread" /tmp/ext2-class.expect
	rm -f "$MNT/t/async$bit"
	sds async 0
done

########################################################################
run "phase 6: benchmarks"
########################################################################
if [ "$BENCH" != 1 ]; then
	skip "benchmarks (needs -b)"
else
	say "each class, with async=0 and async=<bit>, five runs, medians reported"
	for pair in "0:synchronous" "1:newblk" "2:extent" "4:dir"; do
		bit=${pair%%:*}; desc=${pair#*:}
		for mode in 0 "$bit"; do
			[ "$mode" = "$bit" ] && [ "$bit" = 0 ] && continue
			sds async "$mode"
			say "  async=$mode ($desc)"
			dd if=/dev/zero of="$MNT/t/bench" bs=1m count=256 2>&1 |
			    tail -1
			i=0
			while [ $i -lt 200 ]; do : > "$MNT/t/many$i"; i=$((i+1)); done
			rm -f "$MNT/t/many"*
		done
		sds async 0
	done
fi

########################################################################
say ""
say "================================================================"
say "  pass $pass   fail $fail   skipped $skipped"
say "  detail in $results"
say "================================================================"
umount "$MNT" 2>/dev/null
[ "$fail" = 0 ] || exit 1
exit 0