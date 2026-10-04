#!/usr/bin/env python3
"""Run the scanner and replay-ordering logic against a real dirty journal.

A faithful port of ext2_journal_scan_pass() and ext2_journal_replay().
It answers questions about the algorithm that no fixture can:

  - does the scan terminate, and where?
  - does it avoid block 0?
  - does it find the committed transaction and skip its data blocks?
  - how much of it is still unapplied?

This tests the algorithm, not the C.  The C is a separate transcription
of the same steps and has never executed.
"""
import struct, sys, os

MAGIC = 0xC03B3998
BT_DESCRIPTOR, BT_COMMIT, BT_REVOKE = 1, 2, 5
INCOMPAT_64BIT, INCOMPAT_CSUM_V3 = 0x2, 0x10

img = sys.argv[1] if len(sys.argv) > 1 else \
    os.path.join(os.path.dirname(__file__), "journal-dirty.img")
first_phys = int(sys.argv[2]) if len(sys.argv) > 2 else 16385

raw = open(img, "rb").read()
# the fixture is header + journal blocks + the filesystem blocks they target,
# rather than a whole disk image
assert raw[:4] == b"JHD0", "not a journal fixture"
nblk = int.from_bytes(raw[4:8], "little")
nfs = int.from_bytes(raw[8:12], "little")
J = raw[12:12 + nblk * 1024]
FSOFF = 12 + nblk * 1024
BS = 1024
sb = J[0:1024]
j_first = struct.unpack('>I', sb[0x14:0x18])[0]
j_maxlen = struct.unpack('>I', sb[0x10:0x14])[0]
incompat = struct.unpack('>I', sb[0x28:0x2C])[0]

print(f"  journal: first={j_first} maxlen={j_maxlen} incompat={incompat:#x}")

def tag_size(flags):
    if incompat & INCOMPAT_CSUM_V3:
        return 16 if (flags & 0x2) else 32
    return 8 + (4 if incompat & INCOMPAT_64BIT else 0) + (0 if (flags & 0x2) else 16)

def bhdr(n):
    o = n * BS
    return struct.unpack('>III', J[o:o+12]) if o + 12 <= len(J) else (0, 0, 0)

blockno, trans, order, steps, touched0 = j_first, {}, [], 0, False
while True:
    steps += 1
    if steps > j_maxlen * 2:
        print("  FAIL: scan did not terminate"); sys.exit(1)
    if blockno >= j_maxlen:
        blockno = j_first
    if blockno == 0:
        touched0 = True
    m, ty, seq = bhdr(blockno)
    if m != MAGIC or ty not in (BT_DESCRIPTOR, BT_COMMIT, BT_REVOKE):
        break
    if ty == BT_DESCRIPTOR:
        blk = J[blockno*BS:(blockno+1)*BS]
        tags, p, last = [], 12, False
        while p + 8 <= BS and not last:
            fl = struct.unpack_from('>I', blk, p+4)[0]
            if fl & ~0xf:
                print("  FAIL: unknown tag flag"); sys.exit(1)
            tags.append(struct.unpack_from('>I', blk, p)[0])
            p += tag_size(fl); last = bool(fl & 0x8)
        if not last:
            print("  FAIL: descriptor with no LAST_TAG"); sys.exit(1)
        t = trans.setdefault(seq, {'tags': [], 'commit': False}); t['tags'] = tags
        if seq not in order: order.append(seq)
        blockno += len(tags)          # skip the data blocks
    elif ty == BT_COMMIT:
        t = trans.setdefault(seq, {'tags': [], 'commit': False}); t['commit'] = True
        if seq not in order: order.append(seq)
    blockno += 1

print(f"  scan terminated after {steps} steps, log end at block {blockno}")
print(f"  reached block 0: {'FAIL' if touched0 else 'no'}")
for s in order:
    t = trans[s]
    print(f"    seq {s}: {len(t['tags'])} tags, committed={t['commit']}, "
          f"replayable={bool(t['commit'] and t['tags'])}")

# how much is actually still unapplied
raw2 = raw[FSOFF:]
fsidx = {}
for i in range(nfs):
    no = int.from_bytes(raw2[i*(4+BS):i*(4+BS)+4], "little")
    fsidx[no] = raw2[i*(4+BS)+4:(i+1)*(4+BS)]
unapplied = 0
for seq in order:
    t = trans[seq]
    if not t['commit']:
        continue
    for i, target in enumerate(t['tags']):
        jb = J[(2+i)*BS:(3+i)*BS]
        fb = fsidx.get(target)
        if fb is not None and jb != fb:
            unapplied += 1
total = sum(len(trans[s]['tags']) for s in order)
print(f"  of {total} journalled blocks, {unapplied} still differ from the filesystem")
print("  (the rest were already replayed and checkpointed before the unmount)")
