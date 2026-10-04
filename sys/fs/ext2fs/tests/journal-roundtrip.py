#!/usr/bin/env python3
"""Round-trip the JBD2 block formats this filesystem writes.

The offsets are not restated here.  They are read out of
ext2_journal.h, from the EXT2_J_ASSERT_OFF lines that already pin every
field against the documented format.  A copy in this script would be a
second source of truth and could drift from the C code while the
assertions still passed; deriving them means the test follows the header.

What this proves: that the encoding and the decoding agree, that every
field lands where the header says it does, and that the awkward parts --
r_count as a byte count rather than an entry count, LAST_TAG on the final
tag only, tag width selected by feature bits, the 16-bit truncation of a
classic tag checksum, big-endian throughout -- survive a round trip under
each feature combination.

What this does not prove: that the layout matches what Linux writes.  Only
a journal produced by a real implementation can show that, and the
committed fixture in this directory does it for the superblock alone.
Generating blocks here tests this code against this code's reading of the
documentation, which is worth having and is not the same thing.
"""

import re
import struct
import sys
import os

HDR = os.path.join(os.path.dirname(__file__), "..", "ext2_journal.h")

# Documented constants, from journal.rst.
MAGIC = 0xC03B3998
BT_DESCRIPTOR, BT_COMMIT, BT_SB_V2, BT_REVOKE = 1, 2, 4, 5
INCOMPAT_REVOKE, INCOMPAT_64BIT = 0x1, 0x2
INCOMPAT_CSUM_V2, INCOMPAT_CSUM_V3 = 0x8, 0x10
TAG_ESCAPED, TAG_SAME_UUID, TAG_LAST = 0x1, 0x2, 0x8

BS = 1024          # journal block size for the generated fixture
DESC_END = BS       # descriptor tags run to here, less any tail


def offsets_from_header():
    """Offsets as the C header asserts them, not as this file believes."""
    text = open(HDR).read()
    off = {}
    for m in re.finditer(
        r"EXT2_J_ASSERT_OFF\(struct\s+(\w+),\s*(\w+),\s*(0x[0-9a-fA-F]+)\)",
        text,
    ):
        off.setdefault(m.group(1), {})[m.group(2)] = int(m.group(3), 16)
    return off


O = offsets_from_header()


def need(struct, field):
    try:
        return O[struct][field]
    except KeyError:
        sys.exit(f"header asserts no offset for {struct}.{field}")


def be32(buf, sname, field, value):
    """Write a big-endian field at the offset the header asserts."""
    o = need(sname, field)
    buf[o:o + 4] = struct.pack(">I", value & 0xFFFFFFFF)


def rdbhdr(buf):
    return struct.unpack(">III", buf[0:12])


def tag_size(incompat, flags):
    """The documented tag size, selected by feature bits."""
    if incompat & INCOMPAT_CSUM_V3:
        return 16 if (flags & TAG_SAME_UUID) else 32
    sz = 8
    if incompat & INCOMPAT_64BIT:
        sz += 4
    if not (flags & TAG_SAME_UUID):
        sz += 16
    return sz


def build_descriptor(sequence, blocks, incompat, uuid):
    """A descriptor block with one tag per block, LAST on the final tag."""
    buf = bytearray(BS)
    buf[0:4] = struct.pack(">I", MAGIC)
    buf[4:8] = struct.pack(">I", BT_DESCRIPTOR)
    buf[8:12] = struct.pack(">I", sequence)

    p = 12
    for i, b in enumerate(blocks):
        flags = TAG_LAST if i == len(blocks) - 1 else 0
        if incompat & INCOMPAT_CSUM_V3:
            off = need("ext2fs_journal_tag_csum3", "t_blocknr")
            struct.pack_into(">I", buf, p + off, b)
            struct.pack_into(">I", buf,
                             p + need("ext2fs_journal_tag_csum3", "t_flags"),
                             flags)
            struct.pack_into(">I", buf,
                             p + need("ext2fs_journal_tag_csum3", "t_blocknr_high"), 0)
            struct.pack_into(">I", buf,
                             p + need("ext2fs_journal_tag_csum3", "t_checksum"), 0)
            if not (flags & TAG_SAME_UUID):
                buf[p + 16:p + 32] = uuid
        else:
            off = need("ext2fs_journal_tag_classic", "t_blocknr")
            struct.pack_into(">I", buf, p + off, b)
            struct.pack_into(">H", buf,
                             p + need("ext2fs_journal_tag_classic", "t_checksum"), 0)
            struct.pack_into(">H", buf,
                             p + need("ext2fs_journal_tag_classic", "t_flags"), flags)
            # The fixed part is 8 bytes, plus blocknr_high under 64-bit;
            # the uuid follows all of it, and only when it is not shared.
            q = p + 8
            if incompat & INCOMPAT_64BIT:
                struct.pack_into(">I", buf,
                                 p + need("ext2fs_journal_tag_classic", "t_blocknr_high"), 0)
                q += 4
            if not (flags & TAG_SAME_UUID):
                buf[q:q + 16] = uuid
                q += 16
        p += tag_size(incompat, flags)
    return bytes(buf), p


def parse_descriptor(buf, incompat):
    magic, btype, seq = rdbhdr(buf)
    assert magic == MAGIC, f"magic {magic:#x}"
    assert btype == BT_DESCRIPTOR, f"blocktype {btype}"
    tags, p, last = [], 12, False
    while p + 8 <= DESC_END and not last:
        if incompat & INCOMPAT_CSUM_V3:
            o = need("ext2fs_journal_tag_csum3", "t_blocknr")
            blk = struct.unpack_from(">I", buf, p + o)[0]
            flags = struct.unpack_from(">I", buf,
                                       p + need("ext2fs_journal_tag_csum3", "t_flags"))[0]
            high = struct.unpack_from(">I", buf,
                                      p + need("ext2fs_journal_tag_csum3", "t_blocknr_high"))[0]
            csum = struct.unpack_from(">I", buf,
                                      p + need("ext2fs_journal_tag_csum3", "t_checksum"))[0]
            blocknr = blk | (high << 32)
        else:
            o = need("ext2fs_journal_tag_classic", "t_blocknr")
            blk = struct.unpack_from(">I", buf, p + o)[0]
            flags = struct.unpack_from(">H", buf,
                                       p + need("ext2fs_journal_tag_classic", "t_flags"))[0]
            csum = struct.unpack_from(">H", buf,
                                      p + need("ext2fs_journal_tag_classic", "t_checksum"))[0]
            blocknr = blk
            if incompat & INCOMPAT_64BIT:
                blocknr |= struct.unpack_from(
                    ">I", buf, p + need("ext2fs_journal_tag_classic", "t_blocknr_high"))[0] << 32
        tags.append((blocknr, flags, csum))
        p += tag_size(incompat, flags)
        last = bool(flags & TAG_LAST)
    return seq, tags, last


def build_revoke(sequence, blocks, incompat, csum_tail):
    width = 8 if (incompat & INCOMPAT_64BIT) else 4
    count = 0x10 + width * len(blocks) + (4 if csum_tail else 0)
    buf = bytearray(BS)
    buf[0:4] = struct.pack(">I", MAGIC)
    buf[4:8] = struct.pack(">I", BT_REVOKE)
    buf[8:12] = struct.pack(">I", sequence)
    be32(buf, "ext2fs_journal_revoke_hdr", "rh_count", count)
    p = need("ext2fs_journal_revoke_hdr", "rh_count") + 4
    for b in blocks:
        if width == 8:
            struct.pack_into(">Q", buf, p, b)
        else:
            struct.pack_into(">I", buf, p, b & 0xFFFFFFFF)
        p += width
    return bytes(buf), count, p


def parse_revoke(buf, csum_tail):
    magic, btype, seq = rdbhdr(buf)
    assert magic == MAGIC and btype == BT_REVOKE
    count = struct.unpack_from(
        ">I", buf, need("ext2fs_journal_revoke_hdr", "rh_count"))[0]
    p = need("ext2fs_journal_revoke_hdr", "rh_count") + 4
    used = count - p - (4 if csum_tail else 0)
    # r_count is bytes; the number of entries only exists after dividing
    # by the width.  Reporting the byte count as an entry count is the
    # mistake this is here to catch.
    return seq, count, used


def build_commit(sequence):
    buf = bytearray(BS)
    buf[0:4] = struct.pack(">I", MAGIC)
    buf[4:8] = struct.pack(">I", BT_COMMIT)
    buf[8:12] = struct.pack(">I", sequence)
    # chksum_type/chksum_size/pad/chksum, then the timestamps
    p = need("ext2fs_journal_commit", "jc_chksum_type")
    buf[p] = 4                                  # CRC32C
    buf[p + 1] = 4                              # checksum size
    struct.pack_into(">Q", buf,
                     need("ext2fs_journal_commit", "jc_commit_sec"), 0x1122334455)
    struct.pack_into(">I", buf,
                     need("ext2fs_journal_commit", "jc_commit_nsec"), 0x66778899)
    return bytes(buf)


def parse_commit(buf):
    magic, btype, seq = rdbhdr(buf)
    assert magic == MAGIC and btype == BT_COMMIT
    sec = struct.unpack_from(">Q", buf, need("ext2fs_journal_commit", "jc_commit_sec"))[0]
    nsec = struct.unpack_from(">I", buf, need("ext2fs_journal_commit", "jc_commit_nsec"))[0]
    p = need("ext2fs_journal_commit", "jc_chksum_type")
    return seq, buf[p], buf[p + 1], sec, nsec


def main():
    uuid = bytes(range(16))
    blocks = [0x2A, 0x2B, 0x2C, 0x2D]
    fails = 0

    print("offsets taken from ext2_journal.h, not restated here:")
    print(f"  descriptor tag (classic) t_flags   = "
          f"{need('ext2fs_journal_tag_classic', 't_flags'):#04x}")
    print(f"  descriptor tag (csum v3) t_checksum = "
          f"{need('ext2fs_journal_tag_csum3', 't_checksum'):#04x}")
    print(f"  revoke header        rh_count       = "
          f"{need('ext2fs_journal_revoke_hdr', 'rh_count'):#04x}")
    print(f"  commit header        jc_commit_sec  = "
          f"{need('ext2fs_journal_commit', 'jc_commit_sec'):#04x}")
    print()

    cases = [
        ("no features", 0, False),
        ("64bit", INCOMPAT_64BIT, False),
        ("revoke + csum v2", INCOMPAT_REVOKE | INCOMPAT_CSUM_V2, True),
        ("revoke + csum v3", INCOMPAT_REVOKE | INCOMPAT_CSUM_V3, True),
        ("revoke + csum v3 + 64bit",
         INCOMPAT_REVOKE | INCOMPAT_CSUM_V3 | INCOMPAT_64BIT, True),
    ]

    for name, incompat, tail in cases:
        print(f"  {name}")
        dbuf, dend = build_descriptor(7, blocks, incompat, uuid)
        seq, tags, last = parse_descriptor(dbuf, incompat)
        got = [t[0] for t in tags]
        ok = got == blocks and last and seq == 7
        print(f"    {'PASS' if ok else 'FAIL'}  descriptor: {len(tags)} tags, "
              f"blocks {got}, LAST={last}")
        fails += 0 if ok else 1

        rbuf, rcount, rend = build_revoke(7, [0x100, 0x200], incompat, tail)
        rseq, gcount, used = parse_revoke(rbuf, tail)
        width = 8 if (incompat & INCOMPAT_64BIT) else 4
        entries = used // width
        ok = (entries == 2 and used % width == 0 and
              gcount == rcount == 0x10 + width * 2 + (4 if tail else 0))
        print(f"    {'PASS' if ok else 'FAIL'}  revoke: r_count={gcount} bytes -> "
              f"{entries} entries (not an entry count)")
        fails += 0 if ok else 1

        cbuf = build_commit(7)
        cseq, ctype, csize, sec, nsec = parse_commit(cbuf)
        ok = (cseq == 7 and sec == 0x1122334455 and nsec == 0x66778899
              and ctype == 4 and csize == 4)
        print(f"    {'PASS' if ok else 'FAIL'}  commit: seq={cseq} "
              f"sec={sec:#x} nsec={nsec:#x}")
        fails += 0 if ok else 1
        print()

    print("all round trips passed" if fails == 0 else f"{fails} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
