# ext2_journal_mining.md

Mining record for adding a Linux-compatible JBD2 journal to FreeBSD ext2fs,
layered on the existing Soft Updates work.

Scope of this document: **Phase 0 only.** No code has been written. Sections
F, G and H are deliberately incomplete; §O records why.

---

## 0. Bottom line

| Question | Answer |
|---|---|
| Can the JBD2 on-disk format be implemented independently? | **Yes.** The published journal documentation gives complete byte-level layouts for every structure required. |
| Can fast commit be implemented independently? | **No, not now.** The value-structure layouts are unpublished. Deferred, §O. |
| Does Soft Updates already cover extent operations? | **No.** No dependency is registered on any extent path, §J.3. |
| Can a journaled ext4 filesystem be mounted today? | **No.** `EXT2F_INCOMPAT_JOURNAL_DEV` is not in the supported mask, §A.4.1. |
| Is an ext3 filesystem's journal honoured today? | **No, and silently so.** `EXT2F_COMPAT_HASJOURNAL` is never tested, §A.4.2. |

---

## A. JBD2 on-disk structures

### A.1 Provenance and endianness

Source for this entire section:

```
Linux kernel documentation, ext4 "Journal (jbd2)"
https://docs.kernel.org/filesystems/ext4/journal.html
```

That page states, verbatim: *"All fields in jbd2 are written to disk in
big-endian order. This is the opposite of ext4."*

This is a citation, not an inference, and it is the single most important
constraint in this document. The surrounding filesystem is little-endian
throughout. Every JBD2 field is therefore serialised and deserialised
explicitly; no native structure is ever cast onto a journal block.

Nothing in this section was derived from Linux source.

### A.2 Common journal block header

12 bytes, present at offset 0 of every journal block.

| Offset | Size | Encoding | Field | Meaning |
|---|---|---|---|---|
| 0x00 | 4 | big-endian | magic | `0xC03B3998` |
| 0x04 | 4 | big-endian | block type | see below |
| 0x08 | 4 | big-endian | sequence | transaction ID |

Documented block types:

| Value | Meaning |
|---|---|
| 1 | descriptor |
| 2 | commit |
| 3 | journal superblock, v1 |
| 4 | journal superblock, v2 |
| 5 | revoke |

### A.3 Journal superblock

1024 bytes. All numeric fields big-endian.

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0x00 | 12 | header | common block header, magic `0xC03B3998`, type 3 or 4 |
| 0x0C | 4 | blocksize | journal device block size |
| 0x10 | 4 | maxlen | total blocks in this journal |
| 0x14 | 4 | first | first block of log information |
| 0x18 | 4 | sequence | first commit ID expected in log |
| 0x1C | 4 | start | block number of start of log. **Zero does not imply clean** |
| 0x20 | 4 | errno | error value, set by journal abort |
| 0x24 | 4 | feature_compat | see A.4 |
| 0x28 | 4 | feature_incompat | see A.4 |
| 0x2C | 4 | feature_ro_compat | documentation states there are none currently |
| 0x30 | 16 | uuid | journal UUID; compared against the ext4 superblock copy at mount |
| 0x40 | 4 | nr_users | filesystems sharing this journal |
| 0x44 | 4 | dynsuper | dynamic superblock location (documented as unused) |
| 0x48 | 4 | max_transaction | journal blocks per transaction limit (documented as unused) |
| 0x4C | 4 | max_trans_data | data blocks per transaction limit (documented as unused) |
| 0x50 | 1 | checksum_type | see A.5 |
| 0x51 | 3 | padding2 | |
| 0x54 | 4 | num_fc_blocks | fast commit block count |
| 0x58 | 4 | head | first unused block; only up to date when the journal is empty |
| 0x5C | 160 | padding | 40 × u32 |
| 0xFC | 4 | checksum | checksum of the entire superblock **with this field zero** |
| 0x100 | 768 | users | 16 × 48-byte UUIDs of filesystems sharing the log |

`uuid` and everything below 0x30 is common to v1 and v2 superblocks; the
feature, checksum and fast-commit fields are documented as *"only valid in
a v2 superblock"*.

### A.4 Feature flags

Compatible:

| Value | Name |
|---|---|
| 0x00000001 | `JBD2_FEATURE_COMPAT_CHECKSUM` — journal maintains checksums on data blocks |

Incompatible:

| Value | Name |
|---|---|
| 0x00000001 | `JBD2_FEATURE_INCOMPAT_REVOKE` |
| 0x00000002 | `JBD2_FEATURE_INCOMPAT_64BIT` |
| 0x00000004 | `JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT` |
| 0x00000008 | `JBD2_FEATURE_INCOMPAT_CSUM_V2` |
| 0x00000010 | `JBD2_FEATURE_INCOMPAT_CSUM_V3` |
| 0x00000020 | `JBD2_FEATURE_INCOMPAT_FAST_COMMIT` |

No read-only-compatible features are documented.

#### A.4.1 A journaled ext4 filesystem cannot be mounted today

This is a blocker discovered while mining, not a journal design question.

`ext2fs.h:245` defines `EXT2F_INCOMPAT_JOURNAL_DEV` (0x0008).
`ext2fs.h:332` defines `EXT2F_INCOMPAT_SUPP` and **does not include it**.
`ext2_check_sb_compat()` tests the incompat mask at `ext2_vfsops.c:304`.

So mounting an ext4 filesystem that uses a journal device fails with
*"mount of %s denied due to unsupported optional features: journal_dev"*.

Widen `EXT2F_INCOMPAT_SUPP` only when the journal reader can actually
service that configuration. Advertising the bit without the
implementation is exactly what the feature rules forbid.

#### A.4.2 An ext3 journal is silently ignored today

`ext2fs.h:219` defines `EXT2F_COMPAT_HASJOURNAL` (0x0004). It is not in
`EXT2F_COMPAT_SUPP` (`ext2fs.h:324`), but **`ext2_check_sb_compat()` never
tests the compat mask at all** — only incompat (`:304`) and rocompat
(`:316`).

Consequence: a filesystem with an internal journal mounts today, and the
journal is neither used nor rejected. The filesystem is relying entirely
on Soft Updates ordering for a filesystem whose on-disk state Linux would
recover through JBD2. This is a correctness observation in its own right,
independent of this project.

### A.5 Checksum type codes

| Value | Algorithm |
|---|---|
| 1 | CRC32 |
| 2 | MD5 |
| 3 | SHA1 |
| 4 | CRC32C |

The documentation states crc32 and crc32c are the most likely choices.
CRC32C is the likely initial choice for FreeBSD, subject to §B and the
fixture work in §L. No FreeBSD-specific checksum encoding will be
invented.

### A.6 Descriptor blocks and block tags

Descriptor blocks are **open coded**, not a fixed structure: *"Descriptor
blocks consume at least 36 bytes, but use a full block"*. They begin with
the 12-byte common header followed by an array of tags filling the rest of
the block. Serialisation must therefore be an explicit loop, not a struct
cast. A descriptor block uses a whole block; 36 bytes is a minimum
occupancy, not a size.

Two tag encodings are documented.

#### A.6.1 Classic tag — used when CSUM_V3 is **not** set

Size is 8, 12, 24 or 28 bytes.

| Offset | Size | Encoding | Field |
|---|---|---|---|
| 0x0 | 4 | big-endian | `t_blocknr` — low 32 bits of the final location |
| 0x4 | 2 | big-endian | `t_checksum` — **only the low 16 bits are stored** |
| 0x6 | 2 | big-endian | `t_flags` |
| 0x8 | 4 | big-endian | `t_blocknr_high` — **only if the superblock advertises 64-bit** |
| 0x8 or 0xC | 16 | — | `uuid` — **omitted if the SAME_UUID flag is set** |

The documentation is explicit that the uuid field *"appears to be open
coded"* and *"always comes at the end of the tag"*. Tag size is therefore a
function of two independent conditions, and must be computed, not assumed:

```
size = 8
     + (64BIT ? 4 : 0)
     + (SAME_UUID ? 0 : 16)
```

#### A.6.2 Checksum-v3 tag — used when CSUM_V3 **is** set

Size is 16 or 32 bytes.

| Offset | Size | Encoding | Field |
|---|---|---|---|
| 0x0 | 4 | big-endian | `t_blocknr` |
| 0x4 | 4 | big-endian | `t_flags` |
| 0x8 | 4 | big-endian | `t_blocknr_high` — zero if 64BIT is not enabled |
| 0xC | 4 | big-endian | `t_checksum` |
| 0x10 | 16 | — | `uuid` — omitted if SAME_UUID |

Note the structural difference from the classic tag: under CSUM_V3 the
checksum is a full 32-bit field and the flags are 32-bit, so the tag is
**fixed** at 16 bytes plus the optional uuid. That is the documented
purpose of the feature: *"the journal block tag size is fixed regardless
of the size of block numbers."*

### A.7 Descriptor tag flags

| Value | Name | Documented meaning | Consequence |
|---|---|---|---|
| 0x1 | ESCAPED | the data block's first four bytes happened to match the jbd2 magic | those four bytes were replaced with zeroes in the journal copy; restore them on replay |
| 0x2 | SAME_UUID | same UUID as the previous tag | uuid field omitted; tag is 16 bytes shorter |
| 0x4 | DELETED | the data block was deleted by the transaction | documented as *"Not used?"* |
| 0x8 | LAST_TAG | last tag in this descriptor block | terminates the array |

An unknown flag must **not** be ignored. The size of the following tag
depends on SAME_UID, so misreading it desynchronises the whole array.

### A.8 Commit block

32-byte logical header in a full block.

| Offset | Size | Encoding | Field |
|---|---|---|---|
| 0x0 | 12 | — | common journal header |
| 0xC | 1 | — | `h_chksum_type` |
| 0xD | 1 | — | `h_chksum_size` — *"Most likely 4"* |
| 0xE | 2 | — | `h_padding` |
| 0x10 | 32 | — | `h_chksum` space, `JBD2_CHECKSUM_BYTES` |
| 0x30 | 8 | big-endian | `h_commit_sec` |
| 0x38 | 4 | big-endian | `h_commit_nsec` |

The commit block is the **only** durable completion marker. A transaction
without a valid commit record must not be treated as committed.

### A.9 Revoke block

At least 16 bytes, uses a full block.

| Offset | Size | Encoding | Field |
|---|---|---|---|
| 0x0 | 12 | — | common journal header |
| 0xC | 4 | big-endian | `r_count` — number of **bytes** used in this block |
| 0x10 | 4 or 8 | big-endian | `blocks[]` — 8 bytes each if 64BIT, else 4 |

`r_count` is a **byte** count, not an entry count. Entry width therefore
depends on the 64-bit feature, and a reader that assumes the wrong width
misparses the remainder of the block.

Revoke exists to prevent this:

```
metadata block journalled
    -> metadata block freed
    -> same physical block reused as file data
    -> crash
    -> journal replay writes stale metadata over the new owner
```

The documentation is emphatic that revoke does **not** mean *"superseded
by another journal block"*, and that adding a block to a transaction
removes all existing revocation records for that block.

Directly relevant to truncate, extent deletion, tree-node freeing, block
reuse and relocation — §I.

### A.10 Block tails

Present when CSUM_V2 or CSUM_V3 is set.

Descriptor tail, at the end of the descriptor block:

| Offset | Size | Encoding | Field |
|---|---|---|---|
| 0x0 | 4 | big-endian | `t_checksum` — UUID + descriptor block, **with this field zero** |

Revoke tail, at the end of the revoke block:

| Offset | Size | Encoding | Field |
|---|---|---|---|
| 0x0 | 4 | big-endian | `r_checksum` — UUID + revoke block |

---

## B. Feature matrix

Initial compatibility target, and why each row is what it is.

| Feature | Flag | Read | Write | Initial | Rationale |
|---|---|---|---|---|---|
| data-block checksums | COMPAT_CHECKSUM 0x1 | yes | yes | **yes** | torn-write detection; also the compat bit whose presence selects commit-block checksum semantics (§C) |
| revoke | INCOMPAT_REVOKE 0x1 | yes | yes | **yes** | without it, block reuse after a journaled free is unsafe. §I, §Q |
| 64-bit block numbers | INCOMPAT_64BIT 0x2 | yes | yes | **yes** | mke2fs enables 64bit by default on modern images; tag size and revoke width both depend on it |
| async commit | INCOMPAT_ASYNC_COMMIT 0x4 | no | no | **no** | §36 requires the first implementation to be synchronous |
| checksum v2 | INCOMPAT_CSUM_V2 0x8 | determine | determine | **fixture-determined** | see below |
| checksum v3 | INCOMPAT_CSUM_V3 0x10 | determine | determine | **fixture-determined** | see below |
| fast commit | INCOMPAT_FAST_COMMIT 0x20 | deferred | deferred | **no** | §O |

**CSUM_V2 vs CSUM_V3 is not decidable from documentation.** Both are
documented; which one a given `mke2fs` invocation selects is a property of
the image, not of the specification. The decision procedure is:

1. Generate images with `mke2fs` across representative configurations (§L).
2. Read the resulting `s_feature_incompat` from the filesystem superblock
   and the journal superblock's own copy.
3. Whatever appears in real images must be readable. The written feature is
   the one the target `mke2fs` produces by default.

This is fixture work and belongs to Phase 1. It is recorded here as an open
item rather than guessed, because guessing it wrong makes every journal we
write unparseable by the other side.

---

## C. Checksum rules

Journal checksums are **separate** from ext4 filesystem metadata checksums.
FreeBSD already has the latter (`ext2_csum.c`, metadata_csum feature); it
does not transfer. JBD2 checksums are big-endian and cover different
things.

| What | Coverage | Field zeroed during computation |
|---|---|---|
| journal superblock | the entire 1024-byte structure | `s_checksum` at 0xFC |
| descriptor block tail | journal UUID + the descriptor block | `t_checksum` |
| data block tag (classic) | journal UUID + sequence number + the data block — **low 16 bits stored** | n/a |
| data block tag (CSUM_V3) | journal UUID + sequence number + the data block — full 32 bits | n/a |
| revoke tail | journal UUID + the revoke block | `r_checksum` |
| commit block, CSUM_V2/V3 | journal UUID + the **entire** commit block | the first checksum word |
| commit block, COMPAT_CHECKSUM only | crc32 of all blocks already written in the transaction | n/a |

The last two rows are mutually exclusive and selected by feature flags,
not by preference. Both must be implemented to read real images.

Note the asymmetry worth getting wrong deliberately: with CSUM_V2/V3 the
commit checksum covers the commit block itself; with only
COMPAT_CHECKSUM it covers the transaction's data blocks. A reader that
applies the wrong one will reject valid journals.

---

## D. Transaction semantics

### D.1 Model

```
begin transaction
    -> obtain journal write access for a metadata buffer
    -> modify metadata
    -> mark metadata dirty
    -> stop transaction
    -> commit
    -> checkpoint
```

A transaction that exists only in memory is not durable. Dirtied buffers
are not a commit. The commit record is the boundary.

### D.2 FreeBSD-native handle

No Linux `handle_t`, `journal_t`, `transaction_t` or `struct journal_s`
is exposed. A FreeBSD-native handle carries only what is needed:

| Field | Purpose |
|---|---|
| journal reference | owning journal, for lock and lifetime |
| transaction identifier | the sequence number this transaction will commit under |
| credit reservation | blocks reserved, and reserved-but-unused |
| error state | sticky; set by any failure, checked at commit |
| commit requirements | synchronous, and whether a checkpoint is required |
| linked metadata list | buffers with write access, valid until commit or abort |

### D.3 Metadata access

Before modifying metadata belonging to a transaction, take journal write
access; after modifying, mark it dirty. The buffer association must remain
valid until the transaction commits or aborts.

Per §42, buffer ownership must be explicit. The existing Soft Updates
candidate's answer — deliberately leak rather than risk a use-after-free —
is the precedent for any lifetime the journal cannot fully close.

### D.4 Commit

```
serialise descriptor tags
    -> write descriptor block
    -> write journaled metadata blocks
    -> write commit block
    -> ensure stable-storage ordering
    -> transaction is recoverable
```

Submitting a write is not committing. See §R.

### D.5 Checkpoint

Commit and checkpoint are different events. Commit means the journal holds
a durable record; checkpoint means the metadata reached its final location.
Recovery must tolerate committed-but-not-checkpointed, checkpoint-in-
progress, and crash-during-checkpoint.

### D.6 Abort

Abort on journal I/O failure, checksum failure, corruption, or metadata I/O
failure. After an unrecoverable journal error the filesystem must not
continue normal operation. Per §25 a journal-required operation that cannot
obtain its transaction **fails the filesystem operation** and rolls back
only allocations still in `ALLOCATED` or `MAPPED`. It is never silently
downgraded to an unwritten operation.

---

## E. Recovery semantics

### E.1 The rule

```
valid committed transaction  -> replay
transaction without commit   -> discard
```

### E.2 Phases

Recovery must be explicitly phased, and must **not** call the ordinary
write paths — replay would otherwise start new transactions of its own
(§44).

```
journal discovery
    -> journal superblock validation
    -> feature flag validation
    -> sequence identification
    -> transaction scanning
    -> revoke collection
    -> committed transaction identification
    -> metadata replay
    -> checkpoint / journal reset
```

The documented JBD2 recovery model performs multiple passes: find the log
end, assemble revoke information, then replay non-revoked blocks. That
algorithm is followed, not replaced with a simpler one.

### E.3 Revoke during replay

A block revoked by a committed transaction must not be written back from
that transaction's journal contents.

### E.4 Fail closed

Recovery must tolerate and reject: incomplete transaction, invalid
descriptor, invalid checksum, malformed revoke block, invalid block
number, unsupported feature, wraparound, corruption.

Data that merely *parses* is not accepted. Malformed journal data is
refused.

### E.5 Revoke collection is global

The documentation states that any block added to a transaction removes
existing revocation records for that block. Revoke state is therefore
accumulated across the whole replay scan, not per transaction.

---

## F. Fast commit — DEFERRED

**No fast-commit writer or replay code is to be written in the JBD2
phase.** See §O for the decision and the reasoning.

What the published documentation does establish:

- The fast-commit area is *"organized as a log of tag length values"*.
- Each TLV begins with a tag/length pair covering the entire field,
  followed by a variable-length tag-specific value.
- The tag set: `HEAD`, `ADD_RANGE`, `DEL_RANGE`, `CREAT`, `LINK`,
  `UNLINK`, `PAD`, `TAIL`.
- The value structures are named: `ext4_fc_head`, `ext4_fc_add_range`,
  `ext4_fc_del_range`, `ext4_fc_dentry_info`, `ext4_fc_tail`.
- A full JBD2 commit invalidates the fast commits preceding it.
- Fast commit must be enabled at filesystem creation time.
- Fast commit is *"a filesystem-specific delta log"* — not merely
  "journal fewer buffers".

### F.1 What is NOT published

Byte offsets, sizes, numeric tag values, the HEAD TID encoding, the TAIL
CRC computation, the value-structure layouts, and the replay algorithm.

These exist only in GPL Linux source. Implementing them from there would
violate §1 and §50. They are therefore **not implemented**, and their
provenance is **not claimed**.

---

## G. Fast-commit eligibility — DEFERRED

Documented: fast commits apply to suitable operations; a full commit is
performed when fast commit is impossible, when the fast-commit area fills,
or on the JBD2 commit timer; and a full commit invalidates preceding fast
commits.

Not yet mined: exact per-operation eligibility, extent and directory
eligibility, transaction and dependency constraints, space rules, and
fallback triggers.

**Current FreeBSD policy until that is resolved:** every journal-required
operation uses full JBD2. An extent operation goes to full JBD2, never to
a guessed fast commit.

---

## H. Fast-commit replay — DEFERRED

Documented: replay must be idempotent, and records represent resulting
state rather than procedure. The `rm A; mv B A` example in the
documentation shows why — a procedural log replays incorrectly after a
crash mid-sequence, while an outcome-oriented log converges.

That is an architectural requirement and is retained. It is **not** enough
to implement the binary format. Replay implementation is deferred.

---

## I. Extent metadata mining

All references are to the current tree at `d905dead5b7`.

### I.1 `ext4_ext_dirty()` — the bridge point

`ext2_extents.c:813-856`. Current implementation:

| Step | Line | Action |
|---|---|---|
| 1 | 828 | `getblk()` the tree block at `path->ep_blk` |
| 2 | 832 | `ext4_ext_fill_path_buf()` — copy the in-memory path into the buffer |
| 3 | 833 | `ext2_extent_blk_csum_set()` |
| 4 | **834** | **`ext2_dep_write(bp, EXT2_SD_ASYNC_EXTENT)`** |
| 5 | 835-849 | on error, re-`getblk` + `bundirty` + `brelse` to discard the dirty copy |
| — | 851-852 | if `path->ep_data == NULL` (the inode root), set `IN_CHANGE\|IN_UPDATE` and `ext2_update(vp, 1)` |

It has **no** journal or transaction concept. It creates no dependency, so
`ext2_dep_write()` falls straight through to `bwrite()`. Twelve call
sites: 901, 1026, 1077, 1159, 1245, 1254, 1390, 1631, 1715, 1876, 1911.

This is the natural integration point (§10): when no transaction is
active, preserve the existing path exactly; when one is, take journal
write access before step 1 and mark dirty after step 3.

### I.2 Extent operation map

| Operation | Buffers modified | Allocates | Frees | Lines |
|---|---|---|---|---|
| insertion | leaf (or inode root); index blocks only when the modified extent is the leaf's first and depth≠0; inode root if `ep_data == NULL` | tree nodes if leaf full | no | 1390, 1245, 1254, 851 |
| deletion | leaf; parent index when a leaf empties; index blocks for border correction | no | data blocks, tree nodes | 1715, 1735-1737, 1631, 1633 |
| split | new leaf, new index nodes, old leaf, old index nodes, top parent | `depth - at` blocks | no | 1017, 1026, 1068, 1077, 1085 |
| merge | **one** — the leaf (or inode root) | no | no | 1390 (merged at 1287-1292, 1362-1380) |
| truncate | per-extent leaves, emptied parents, tree nodes; inode at the end | no | data blocks, tree nodes | 1715, 1631, 1633, 1870-1879 |
| relocation | **one** — the leaf holding the extent | new run | old run, after publication | 1911, 2057-2061 |

Two functions named in the original design **do not exist**:

- there is no `ext4_ext_truncate()`; the path is `ext2_ext_truncate()`
  (`ext2_inode.c:471`) → `ext4_ext_remove_space()`
  (`ext2_extents.c:1794`) → `ext4_ext_rm_leaf()` / `ext4_ext_rm_index()`
- there is no standalone merge function; merge is inlined into
  `ext4_ext_insert_extent()`, with only the predicate
  `ext4_can_extents_be_merged()` (`ext2_extents.c:772`) separate

Merge touches exactly one buffer. It is the cheapest extent operation and
does not need a journal transaction on buffer count alone — but see §K.

### I.3 Ordering already present in the extent code

The extent paths are already correctly ordered by program order and
synchronous writes. This was established and fixed during the Soft Updates
work:

- **deletion**: `ext4_ext_dirty()` at 1715 **before**
  `ext4_remove_blocks()` at 1719. The rationale is in the comment at
  1696-1703.
- **tree-node removal**: `ext4_ext_dirty()` at 1631 before
  `ext4_ext_blkfree()` at 1633.
- **split** is strictly bottom-up:
  ```
  1017  new LEAF written
  1026  old LEAF written (ecount reduced)
  1068  new INDEX node written
  1077  old INDEX node written
  1085  PARENT index written   -> ext4_ext_dirty @ 901
  ```
- **relocation**: the single `ext4_ext_dirty()` at 1911 is the publication
  point; old blocks are freed at 2057-2061, strictly after
  `ext2_alloc_transition(PUBLISHED)` at 2047.

So the journal is **not** being added to fix a known ordering bug in the
extent code. It is being added because §J.3 shows there is no *mechanism*
there — the ordering is incidental to synchronous writes, not enforced.

### I.4 `ext4_reallocblks()` — priority integration test

`ext2_extents.c:1944-2072`.

| Step | Line | Action |
|---|---|---|
| 1 | 2016-2018 | `ext2_alloc_run(EXT2_ALLOC_DATA_RAND)` → `ALLOCATED` |
| 2 | 2023 | release lock |
| 3 | — | deliberately **no** `ext2_commit_allocated_block()`; a relocation does not change `i_blocks` |
| 4 | 2033-2036 | short-run check, else `ext2_rollback_allocation()` |
| 5 | **2039** | `ext2_alloc_transition(MAPPED)` |
| 6 | 2040-2041 | `ext4_ext_realloc_extent()` — the leaf write |
| 6a | 2043 | on failure `ext2_rollback_allocation()` → `ROLLED_BACK` |
| 7 | **2047** | `ext2_alloc_transition(PUBLISHED)` — only after a successful write |
| 8 | 2057-2061 | old blocks freed, interleaved with buffer repointing |

This is the operation with the most to go wrong: it allocates, maps,
publishes and frees in one call, and the old blocks must not become
reusable until the mapping that replaces them is durable. It is therefore
the first operation to journal, and the one §17 singles out.

Under JBD2 the whole operation — new run allocation, leaf rewrite,
publication, and old-run release — belongs in **one** transaction. The
unit is the operation, not the leaf buffer (§7, §S).

---

## J. Soft Updates coverage mining

### J.1 Current model

| Property | Value |
|---|---|
| Dependency classes | `EXT2_DEP_NEWBLK`, `EXT2_DEP_METADATA` |
| Dependency states | `PENDING`, `SATISFIED`, `CANCELLED` |
| Ownership | owned by the buffer association; retired when the wrapper writes or discards the buffer |
| Reference counting | a dependency holds a reference on its prerequisite |
| Buffer association | `b_fsprivate1` |
| Enforcement point | `ext2_strategy()`, before any I/O starts |
| Completion | `bp->b_iodone` |
| Cycles | rejected at link time, bounded walk |
| Crash injection | 19 points, runtime-gated |
| Asynchrony | **all classes default off; every wrapped write is a `bwrite()`** |

### J.2 What Soft Updates covers

| Operation | Covered | Mechanism |
|---|---|---|
| direct block allocation | yes | allocator state machine + ordered synchronous writes |
| single indirect allocation | yes | `NEWBLK` dependency on the new block, `METADATA` on the parent (`ext2_balloc.c:264-265`) |
| multi-level indirect | yes | same, per level, inside the allocation loop |
| cluster relocation (indirect) | yes | synchronous pointer rewrite before `ext2_blkfree()` (`ext2_alloc.c:740-756`) |
| directory entry add/remove | yes | writes routed through the wrappers; ordering is program order plus synchronous writes |

### J.3 What it does not cover — the justification for JBD2

**No dependency is registered on any extent path.** `ext2_dep_create()`
appears in exactly one file:

```
sys/fs/ext2fs/ext2_balloc.c
```

`ext2_extents.c` references the write wrappers but never creates a
dependency. Consequently:

- the wrappers reduce to `bwrite()` at every extent call site
- the ordering documented in §I.3 is enforced only by synchronous writes
  and program order
- there is no mechanism that would keep a child extent node ahead of its
  parent if either write were made asynchronous

This is not a claim that extent ordering is currently wrong — §I.3 shows
it is right. It is that the guarantee is **incidental**, and cannot survive
the asynchronous relaxation the design otherwise contemplates.

That is the honest justification for JBD2 on extents: not a known bug, but
an absent mechanism.

### J.4 Notes for the journal layer

- `IN_MODIFIED` is **never** set by any metadata-mutating routine. It is set
  only by `ext2_itimes_locked()` (`ext2_vnops.c:229`), `ext2_setattr()`
  (`:490`) and synthesised in `ext2_reclaim()` (`ext2_inode.c:648`). It
  carries **no ordering information** and is not a usable journal hook.
- `IN_HASHED`, `IN_SPACECOUNTED` and `IN_LAZYACCESS` are dead. Not a base
  for anything.
- `ext2_update()`, `ext2_blkfree()`, `ext2_alloccg()`, `ext2_nodealloccg()`
  and `ext2_vfree()` all use bare `bwrite`/`bdwrite` and are **not**
  softdep-wrapped. A journal must hook these or the transaction will not
  see the metadata.
- Orphan handling (`ext2_orphan.c`) is complete and uses explicit
  synchronous ordering: inode before superblock on add, detach before
  clearing `i_dtime` on remove. Per §20 it is not to be expanded while the
  journal is built.

---

## K. Operation classification

The authoritative policy table. Each row states the **decision**, not a
claim that it has been proven; backing evidence and tests are named.

| Operation | Soft Updates | Full JBD2 | Fast commit | Basis |
|---|---|---|---|---|
| direct block allocation | **yes** | no | deferred | §J.2, dependency + state machine |
| single indirect allocation | **yes** | no | deferred | §J.2, NEWBLK dependency |
| multi-level indirect | **yes** | no | deferred | §J.2, per-level dependency |
| cluster relocation (indirect) | **yes** | no | deferred | §J.2 |
| directory create | **yes** | no | deferred | §J.2, inode written before dirent (`ext2_vnops.c:2002-2006`) |
| directory link | **yes** | no | deferred | §J.2, inode before dirent (`:719-723`) |
| directory unlink | **yes** | no | deferred | §J.2, orphan list handles the open-file case |
| directory expansion | **yes** | no | deferred | §J.2, linear and htree both order children first |
| htree index change | **yes** | no | deferred | §J.2, deepest level written first |
| rename | **yes** | no | deferred | composition of the above |
| orphan handling | **yes** | no | deferred | §20, explicitly frozen |
| extent insertion | insufficient | **yes** | deferred | §J.3 no mechanism; §I.2 multiple buffers |
| extent deletion | insufficient | **yes** | deferred | §J.3 |
| extent split | insufficient | **yes** | deferred | §J.3; up to `depth` new blocks |
| extent merge | insufficient | **yes** | deferred | one buffer, but no mechanism; classify with the rest for uniformity |
| truncate (extent-mapped) | insufficient | **yes** | deferred | §J.3 |
| extent relocation | insufficient | **yes** | deferred | §I.4, and §17 priority |
| xattrs | **yes** | no | deferred | already ordered synchronously after the Series 5 fixes |

"Insufficient" means *no enforcement mechanism exists*, not that the current
behaviour is wrong.

The classifier must be a single explicit function. Inferring the decision
independently in several places is how the two mechanisms end up
disagreeing (§40).

---

## L. Interoperability fixtures

Linux is used strictly as a black-box oracle: create images, run workloads,
read bytes, compare recovery behaviour. No Linux source is read for these.

Required JBD2 fixtures, each recorded with byte offsets, endian
representation, sequence numbers, checksums, feature flags, and expected
parser and recovery results:

| # | Fixture | Exercises |
|---|---|---|
| 1 | empty journal | clean mount, no replay |
| 2 | one full transaction | descriptor/data/commit, replay |
| 3 | multiple transactions | sequence handling, log-end discovery |
| 4 | revoke transaction | revoke parsing and suppression |
| 5 | checksummed transaction | tag, commit and revoke checksums |
| 6 | 64-bit block numbers | tag width, revoke width |
| 7 | journal wraparound | log-end discovery across the end |
| 8 | incomplete transaction | discard, no replay |
| 9 | corrupted descriptor | fail closed |
| 10 | corrupted commit | fail closed |
| 11 | invalid checksum | fail closed |
| 12 | revoked then reused block | the §19 scenario |
| 13 | journal requiring recovery | mount-time replay path |
| 14 | corrupt superblock | fail closed |
| 15 | unsupported feature set | mount refused, §30 |

Each fixture records: tool, tool version, filesystem feature set, journal
feature set, workload, raw bytes, expected result. Fixtures are evidence,
not source.

Fast-commit fixtures are deferred with §F.

---

## M. FreeBSD implementation decisions

### M.1 Files

```
sys/fs/ext2fs/ext2_journal.h
sys/fs/ext2fs/ext2_journal.c            state machine, transactions
sys/fs/ext2fs/ext2_journal_format.c     serialisation only
sys/fs/ext2fs/ext2_journal_recovery.c   recovery
```

No `ext2_fast_commit.c` until §O is revisited.

Serialisation is kept separate from state-machine logic so that the
big-endian conversions can be reviewed in one place and asserted
independently (§26).

### M.2 Licensing

All new files: 2-Clause BSD. No Linux GPL code is copied or translated.
Every on-disk structure carries a provenance comment naming the
documentation it came from, and stating that no Linux structure is reused.

### M.3 Native structures

FreeBSD-native types for journal state, transactions, handles, buffer
tracking, recovery state and revoke tracking. Only the **serialised**
representation matches JBD2. Linux type names are not exposed.

### M.4 Serialisation rules

Explicit host↔big-endian conversion in both directions. No casting of
native structures onto disk blocks. No reliance on compiler packing.
Compile-time assertions for sizes and offsets of internal serialisation
structures.

### M.5 Lock order

Observed in the current code:

1. inode lock (`vn_lock`)
2. `EXT2_MTX(ump)` — filesystem-wide; taken by the allocator, returned held
   by `ext2_alloc_run()`
3. `sd_lock` — Soft Updates per-mount
4. buffer `b_lock`

`sd_lock` is **never** held while `EXT2_MTX` is held: `ext2_dep_*` is
called after the allocator lock is released. The journal lock must slot in
without inverting this.

Proposed order, to be verified before any journal locking is added:

```
inode lock  ->  EXT2_MTX  ->  journal lock  ->  sd_lock  ->  buffer lock
```

with the rule that no journal lock may be held across a blocking I/O
call, and that `sd_lock` may not be taken while a journal lock is held.

**Open item:** `ext2_alloc_meta()` (`ext2_alloc.c:158-175`) takes
`EXT2_LOCK` and returns with it dropped, because `ext2_alloccg()` unlocks
internally without re-acquiring on return. `ext2_alloc_run()` does
re-acquire (`ext2_alloc.c:312,325`). So there is currently **no
allocator-level serialisation spanning an extent-tree metadata
allocation**. Journal work must not assume one.

### M.6 Shutdown

Journal state must not be freed while a transaction, buffer, I/O
completion, recovery or dependency can reference it. Follow §42 and the
existing Soft Updates precedent: leak deliberately rather than risk a
use-after-free, and report it.

### M.7 Mount integration point

Journal recovery must run between `ext2_vfsops.c:968-969` (the first
`ext2_sbupdate`, which clears the clean bit) and `:974-975`
(`ext2_softdep_mount`). The existing comment at 970-973 already reserves
this slot: *"This has to precede any wrapped metadata write, including the
recovery below."*

Two consequences:

- The superblock write at 968 currently happens **before** any recovery.
  A journal must be opened and replayed before it, or the clean bit is
  cleared with an unreplayed journal present.
- The read-only→read-write transition at `ext2_vfsops.c:183-226` performs
  no `ext2_mountfs()` and therefore no recovery hook. It needs one.

Unmount needs the mirror point: journal stop and final commit before
`E2FS_ISCLEAN` is set at 1051-1053 and the superblock written at 1054.

---

## N. Source-provenance record

| Item | Status | Authority |
|---|---|---|
| block header | **complete** | journal.rst §3.6.3 |
| block types | **complete** | journal.rst §3.6.3 |
| journal superblock | **complete** | journal.rst §3.6.4 |
| feature flags | **complete** | journal.rst §3.6.4 |
| checksum type codes | **complete** | journal.rst §3.6.4 |
| classic descriptor tag | **complete** | journal.rst §3.6.5 |
| CSUM_V3 descriptor tag | **complete** | journal.rst §3.6.5 |
| tag flags | **complete** | journal.rst §3.6.5 |
| descriptor block tail | **complete** | journal.rst §3.6.5 |
| revoke block | **complete** | journal.rst §3.6.7 |
| revoke tail | **complete** | journal.rst §3.6.7 |
| commit block | **complete** | journal.rst §3.6.8 |
| endian rule | **complete** | journal.rst, stated explicitly |
| checksum coverage | **complete** | journal.rst §3.6.4-3.6.8 |
| fast-commit TLV framing | **partial, documented** | journal.rst §3.6.9 |
| fast-commit tag set and meanings | **partial, documented** | journal.rst §3.6.9 |
| fast-commit idempotence model | **partial, documented** | journal.rst §3.6.10 |
| fast-commit full-commit invalidation | **partial, documented** | journal.rst §3.6.9 |
| fast-commit value structures | **incomplete** | unpublished |
| fast-commit numeric tag values | **incomplete** | unpublished |
| fast-commit replay algorithm | **incomplete** | unpublished |

Secondary, non-normative, used only for behavioural context and cited as
such: LWN, *"Fast commits for ext4"*, 15 January 2021
(`https://lwn.net/Articles/842385/`). It supplied the design rationale for
omitting derivable updates, the concrete set of supported operations, the
named fallback trigger (extended attributes), the recovery ordering
(JBD2 first, then fast commits), and the constraint that `fsync()` remains
a global barrier. **It supplied no byte layout**, and none was taken from
it.

---

## O. Fast-commit decision

```
DEFER FAST COMMIT.
```

1. The JBD2 core format is fully documented, so the reader, writer and
   recovery can be built without copying GPL source.
2. Fast commit is a layer on top of JBD2, not a separate mechanism.
3. The published documentation does not specify the binary value
   structures or the replay algorithm.
4. Deriving them from GPL source would violate §1 and §50.
5. Fast commit is not required to prove the correctness of the JBD2 core.
6. The plan already places fast commit after the core is proven (§48).

Therefore: Phases 1-5 are JBD2 only. Phases 6-8 revisit fast commit only
after a separate provenance decision, recorded here.

Per §L.4, if fixture-based derivation is chosen later it must be declared
as a distinct provenance methodology, not folded silently into this
document.

---

## P. Initial implementation scope

Contains: JBD2 reader, writer, transaction management, metadata access,
commit, checkpoint, revoke, checksum validation, recovery, and extent
integration.

Does not contain: fast-commit writer, fast-commit replay, or any guessed
fast-commit structure or tag value.

---

## Q. Allocator/journal boundary

The allocator keeps exactly four states — `ALLOCATED`, `MAPPED`,
`PUBLISHED`, `ROLLED_BACK` — with the transitions validated by
`ext2_alloc_transition()` (`ext2_alloc.c:498-522`). `PUBLISHED` and
`ROLLED_BACK` are terminal.

The journal must not introduce `JOURNALED`, `COMMITTED`, `CHECKPOINTED`,
`ACCOUNTED` or `PERSISTENT` as allocator states. The allocator asks one
question — *has publication become durable?* — and the consistency layer
answers it.

---

## R. Publication rule

`MAPPED -> PUBLISHED` may occur only after the selected mechanism has
reached its real publication boundary:

- Soft Updates: the ordered metadata write completed
- full JBD2: the required commit record is durable

It is **not** publication when metadata is modified in memory, when journal
records are constructed, when journal I/O is submitted, or when a buffer
is marked dirty.

---

## S. Mixed Soft Updates / JBD2 rule

The unit of journaling is the filesystem operation. An extent insertion
touching inode, index and leaf belongs in one transaction. The same applies
to a split touching the root, two index blocks and a leaf.

If a journaled update depends on an Soft-Updates-ordered update, prefer
putting the complete metadata set in the journal transaction over creating
an ordering relationship between two independently scheduled mechanisms.
Where that is impossible, the relationship must be documented and tested,
not assumed.

---

## T. Open items carried into Phase 1

| # | Item | Why it is open |
|---|---|---|
| 1 | CSUM_V2 or CSUM_V3 as the written feature | decidable only from fixtures, §B |
| 2 | `EXT2F_INCOMPAT_SUPP` widening for `JOURNAL_DEV` | must not precede a working reader, §A.4.1 |
| 3 | `EXT2F_COMPAT_HASJOURNAL` currently untested | mount must either honour or reject, §A.4.2 |
| 4 | Journal lock position | proposed in §M.5, unverified |
| 5 | Credit estimation per operation | §41; needs the buffer counts in §I.2 |
| 6 | Read-only→read-write recovery hook | §M.7 |
| 7 | `ext2_alloc_meta()` lock asymmetry | §M.5, affects serialisation assumptions |
| 8 | which of `ext2_update`/`ext2_blkfree`/`ext2_alloccg`/`ext2_nodealloccg`/`ext2_vfree` the journal must hook | §J.4 |
