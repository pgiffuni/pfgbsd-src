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
