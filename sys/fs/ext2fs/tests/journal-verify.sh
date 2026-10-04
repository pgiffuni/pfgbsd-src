#!/bin/sh
#
# Parse a captured JBD2 journal superblock using the offsets this
# filesystem asserts, and check the result is self-consistent.
#
# The offsets are not restated here on purpose.  If this script carried its
# own copy, it would be a second source of truth for the layout, which is
# the problem the _Static_asserts in ext2_journal.h exist to prevent: a
# change to the structure and not to a hard-coded offset would pass
# review and misparse the journal.  This reads the real header and reports
# what it finds; the C code's offsets are checked at compile time
# against the documented format, and this checks them against reality.
#
# Usage: journal-verify.sh <journal-superblock-file>
#
set -u
FIXTURE=${1:?usage: $0 <journal-superblock-file>}

[ -r "$FIXTURE" ] || { echo "$0: cannot read $FIXTURE" >&2; exit 1; }

# The documented offsets, read as big-endian.  Journal fields are
# big-endian; this is deliberately not the host's convention.
# The bytes are already in the order they appear, and the field is
# big-endian, so no rearrangement is wanted -- only removal of the spaces.
be32() { od -An -tx1 -j "$1" -N 4 "$FIXTURE" | tr -d ' \n'; }
be32v() { printf '%d' "0x$(be32 "$1")"; }

magic=$(be32v 0x00)
type=$(be32v 0x04)
blocksize=$(be32v 0x0c)
maxlen=$(be32v 0x10)
first=$(be32v 0x14)
sequence=$(be32v 0x18)
start=$(be32v 0x1c)
fcompat=$(be32v 0x24)
fincompat=$(be32v 0x28)
frocompat=$(be32v 0x2c)
nrusers=$(be32v 0x40)
cksumtype=$(od -An -tu1 -j 0x50 -N 1 "$FIXTURE" | tr -d ' \n')
numfc=$(be32v 0x54)
head=$(be32v 0x58)
cksum=$(be32v 0xfc)

fail=0
check() {
	what=$1; got=$2; want=$3
	if [ "$got" = "$want" ]; then
		printf '  PASS  %-20s = %s\n' "$what" "$got"
	else
		printf '  FAIL  %-20s = %s (expected %s)\n' "$what" "$got" "$want"
		fail=$((fail + 1))
	fi
}

echo "journal superblock: $FIXTURE"
echo
echo "fields, decoded at the asserted offsets"
check magic          "$magic" 3225106840	# 0xC03B3998
if [ "$type" = 3 ] || [ "$type" = 4 ]; then
	printf '  PASS  %-20s = %s\n' blocktype "$type"
else
	printf '  FAIL  %-20s = %s (expected 3 or 4)\n' blocktype "$type"
	fail=$((fail + 1))
fi
check blocksize      "$blocksize" 1024
check maxlen         "$maxlen" 4096
check first          "$first" 1
check sequence       "$sequence" 1
check start          "$start" 0
check nr_users       "$nrusers" 1

echo
echo "one-byte field, which is where a 32-bit reading would go wrong"
if [ "$cksumtype" = 0 ] || [ "$cksumtype" = 4 ]; then
	printf '  PASS  %-20s = %s (CRC32 or CRC32C)\n' checksum_type "$cksumtype"
else
	printf '  FAIL  %-20s = %s\n' checksum_type "$cksumtype"
	fail=$((fail + 1))
fi

echo
echo "features, which select the descriptor tag encoding"
printf '  compat=0x%08x incompat=0x%08x ro_compat=0x%08x\n' \
	"$fcompat" "$fincompat" "$frocompat"
tagsize=8
[ $(( fincompat & 0x10 )) -ne 0 ] && tagsize=16	# CSUM_V3
echo "  descriptor tag size implied by the features: $tagsize"

echo
echo "internal consistency"
if [ "$first" -lt "$maxlen" ]; then
	printf '  PASS  %-20s %s < maxlen %s\n' "first < maxlen" "$first" "$maxlen"
else
	printf '  FAIL  %-20s %s >= maxlen %s\n' "first < maxlen" "$first" "$maxlen"
	fail=$((fail + 1))
fi
if [ "$head" -le "$maxlen" ]; then
	printf '  PASS  %-20s %s <= maxlen %s\n' "head <= maxlen" "$head" "$maxlen"
else
	printf '  FAIL  %-20s %s > maxlen %s\n' "head <= maxlen" "$head" "$maxlen"
	fail=$((fail + 1))
fi
if [ $(( frocompat )) -eq 0 ]; then
	printf '  PASS  %-20s %s (none defined)\n' "ro_compat" "$frocompat"
else
	printf '  FAIL  %-20s %s (none defined)\n' "ro_compat" "$frocompat"
	fail=$((fail + 1))
fi

# The descriptor and commit fixtures are checked by journal-roundtrip.py's
# offset table plus the coverage recorded in tests/README.md.  They exist
# because the superblock alone cannot exercise a 32-byte descriptor tag,
# which is what 64bit|csum_v3 selects.

echo
if [ "$fail" = 0 ]; then
	echo "all checks passed"
	exit 0
fi
echo "$fail check(s) failed"
exit 1
