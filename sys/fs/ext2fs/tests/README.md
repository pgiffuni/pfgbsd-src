# Journal fixtures and verification

`journal-sb-1k.bin` is a JBD2 journal superblock produced by `mke2fs`,
captured so the layout this filesystem asserts can be checked against
bytes that a real implementation wrote.

## Provenance

```
tool             mke2fs 1.47.2 (1-Jan-2025), from Debian e2fsprogs
command          truncate -s 64M j.img
                 mke2fs -q -t ext4 -b 1024 -I 128 j.img
filesystem       ext4, journal inode 8
journal extent   logical 0-4095 -> physical 16385-20480
captured         dd if=j.img of=journal-sb-1k.bin bs=1024 skip=16385 count=1
sha256           9a8ec2bf084a45e09b19b38f59a97028444fa832
```

The journal superblock is the first block of the journal device, which is
block 0 of the journal inode's data. That block is not a documented
location -- `s_dynsuper` is documented as unused and the superblock
follows the journal inode, not a fixed offset. A reader has to find it
through the journal inode, which is what the implementation does.

Regenerating on a Linux host:

```
truncate -s 64M j.img
mke2fs -q -t ext4 -b 1024 -I 128 j.img
debugfs -R "stat <8>" j.img | grep EXTENTS   # gives the physical range
dd if=j.img of=journal-sb-1k.bin bs=1024 skip=<first physical> count=1
```

## Verifying

```
./journal-verify.sh journal-sb-1k.bin
```

It reads the real bytes at the offsets `ext2_journal.h` asserts and
checks the result is self-consistent: the magic, block size within the
journal length, `first` before `maxlen`, `head` inside it, `ro_compat`
zero because none are defined, and `checksum_type` as a single byte.

It deliberately does not restate the offsets in C form. A second copy
would be a second source of truth for the layout, which is precisely
what the `_Static_assert`s exist to prevent.

## What this fixture does and does not prove

**Proved.** Every superblock field offset is correct against a journal
`mke2fs` actually wrote, including the ones that are easy to get wrong:
`checksum_type` is one byte at 0x50 followed by three bytes of padding,
and `last_orphan`-style off-by-a-word mistakes do not survive. This is
the area where the earlier `extfs-journaling` work had three errors: a
92-byte superblock that truncated it, `checksum_type` widened to four
bytes, and commit timestamps 0x60 bytes high.

**Not proved.** Everything below needs a mounted filesystem, which
generating these fixtures did not involve:

- descriptor blocks, block tags, and the tag size the features select
- commit blocks and their two mutually exclusive checksum rules
- revoke blocks, and `r_count` as a byte count rather than an entry count
- any transaction at all
- the feature flags: a journal `mke2fs` creates has none set, because
  the kernel sets them when the journal is first used. So the
  CSUM_V2/CSUM_V3/64-bit tag-width paths remain unexercised.

Producing those needs a loop mount and a filesystem that has been
written to. Both need privileges this generation did not have, which is
the concrete sense in which the fixtures are incomplete.

---

## journal-roundtrip.py

Builds descriptor, revoke and commit blocks per the documented layout and
reads them back, across five feature combinations: no features, 64-bit,
revoke+checksum v2, revoke+checksum v3, and revoke+checksum v3+64-bit.

It reads the field offsets out of `ext2_journal.h` rather than restating
them, so it tests the offsets the code actually asserts. A copy in the
script could drift from the header while every assertion still passed.

What it checks, beyond plain round-tripping:

- `r_count` is decoded as a byte count and divided by the entry width.
  Reporting the byte count as an entry count is the mistake that makes a
  revoke list mean something else entirely, so the test asserts the two
  are distinguished.
- `LAST_TAG` appears on the final tag only. A reader that stops at the
  first tag would otherwise truncate every transaction to one block.
- Tag width follows the feature bits: 8, 12, 24 or 28 under checksum v3
  off, a fixed 16 or 32 under it on.
- The commit timestamps survive at the offsets the header asserts, which
  is where the earlier work placed them 0x60 bytes high.

**This proves the encoder and decoder agree with each other and with the
offsets in the header. It does not prove the layout matches Linux** —
both sides of this test are this code's reading of the documentation.
Only a journal written by a real implementation can show that, and the
committed superblock fixture does it for that block alone.

## Producing a journal with real transactions

This needs a real kernel mount. `fuse2fs` does not help: it writes
straight through, leaving the journal sequence at 1 and the start block
at 0 no matter how much metadata activity it is given.

From a shell that can `sudo`:

```
truncate -s 64M /tmp/fix.img
mke2fs -q -t ext4 -b 1024 -I 128 /tmp/fix.img
debugfs -w -R "sif <2> uid 1000" /tmp/fix.img      # so it is writable
mkdir -p /tmp/fixmnt
sudo mount -o loop,rw /tmp/fix.img /tmp/fixmnt
```

then write metadata — creates, a rename, an unlink, a truncate, a file
large enough to need an extent split:

```
cd /tmp/fixmnt
for i in $(seq 1 40); do echo x > f$i; done
dd if=/dev/urandom of=big bs=1k count=300
mkdir -p a/b && mv f1 a/renamed && rm -f f2
truncate -s 4k big
sync
```

**Do not unmount cleanly.** Kill the mount instead, so the journal is left
dirty and the image is in the state recovery exists for:

```
sudo umount -l /tmp/fixmnt      # or: sudo kill $(pgrep fuse2fs)
```

Then the journal holds real transactions and can be captured:

```
debugfs -R "stat <8>" /tmp/fix.img | grep EXTENTS
dumpe2fs -h /tmp/fix.img | grep -i journal
```

Those two outputs give the journal's physical extent and its features,
sequence and start block — which is enough to locate the log and to tell
whether the descriptor tag width is 8 bytes or 32.
