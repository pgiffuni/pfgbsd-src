# ext2fs Soft-Updates-Style Consistency: Series 1 Documentation

## Current ext2fs Metadata Update Paths

### 1. Inode Updates

#### ext2_update (ext2_inode.c:69)
- Called from: VOP_UPDATE, VOP_INACTIVE, explicit fsync
- Reads inode block via `bread()`
- Converts in-core inode to on-disk format via `ext2_i2ei()`
- **Synchronous write**: `bwrite(bp)` if `waitfor && !DOINGASYNC(vp)`
- **Asynchronous write**: `bdwrite(bp)` otherwise
- Clears IN_MODIFIED, IN_LAZYMOD, IN_LAZYACCESS flags before write

#### ext2_itimes (ext2_inode.c:100+)
- Updates access/change/modification times
- Sets IN_ACCESS, IN_CHANGE, IN_UPDATE flags
- Called from: ext2_update, VOP_ACCESS, VOP_GETATTR

#### Inode Flag Transitions
- IN_ACCESS: atime needs update
- IN_CHANGE: ctime needs update (metadata change)
- IN_UPDATE: mtime needs update (data change)
- IN_MODIFIED: inode needs write
- IN_LAZYMOD: force async write later
- IN_LAZYACCESS: force async atime update later

### 2. Block Allocation

#### ext2_alloc_run (ext2_alloc.c)
- Multi-block physical allocator
- Returns contiguous physical run via context-based state machine
- States: ALLOCATED → MAPPED → PUBLISHED → ROLLED_BACK
- Credential-threaded reserved-block policy

#### ext2_balloc (ext2_balloc.c:100)
- Main block allocation entry point
- Direct blocks (lbn < EXT2_NDADDR): stores in i_db[]
- Indirect blocks: allocates via ext2_alloc(), installs in i_ib[] or indirect block arrays
- **Synchronous writes**: indirect blocks written via `bwrite()` when `IO_SYNC`
- **Asynchronous writes**: `bdwrite()` for delayed write

#### ext4_ext_get_blocks / ext4_new_blocks (ext2_extents.c)
- Extent-tree based allocation
- ext4_new_blocks → ext2_alloc_run for physical allocation
- ext4_ext_insert_extent for extent insertion
- **Synchronous**: extent tree blocks written via bwrite() in ext4_ext_dirty()

### 3. Block Freeing

#### ext2_truncate (ext2_vnops.c)
- Truncates file to specified length
- Calls ext2_ext_truncate or ext2_indir_trunc
- Frees blocks via ext2_blkfree()

#### ext2_blkfree (ext2_alloc.c)
- Frees physical blocks to bitmap
- Updates free block counts in superblock and group descriptors
- **Synchronous**: superblock updated via bdwrite()

#### ext2_vfree (ext2_vnops.c)
- Frees inode
- Updates inode bitmap and free inode counts
- **Synchronous**: superblock updated via bdwrite()

### 4. Directory Operations

#### ext2_direnter (ext2_vnops.c)
- Adds directory entry
- Increments target inode link count
- **Synchronous**: calls ext2_update on target inode with waitfor=1

#### ext2_dirremove (ext2_lookup.c)
- Removes directory entry
- Decrements target inode link count
- Sets IN_CHANGE flag

#### ext2_rename (ext2_vnops.c)
- Complex rename with multiple link count adjustments
- Creates/removes directory entries
- Updates ".." entries for directory moves

#### ext2_mkdir / ext2_rmdir (ext2_vnops.c)
- Create/remove directories
- Link count manipulation for "." and ".."
- **Synchronous**: inode updates

### 5. Extent Tree Operations

#### ext4_ext_insert_extent (ext2_extents.c:1322)
- Inserts extent into extent tree
- Splits nodes via ext4_ext_split()
- Grows tree depth via ext4_ext_grow_indepth()
- **Synchronous**: bwrite() for modified extent blocks

#### ext4_ext_split (ext2_extents.c:944)
- Splits extent tree node
- Allocates new metadata blocks
- Copies/moves extents between nodes
- Uses scalar rollback for cleanup on error

#### ext4_ext_dirty (ext2_extents.c)
- Marks extent block dirty
- **Synchronous**: bwrite() if waitfor, else bdwrite()

### 6. Indirect Block Operations

#### ext2_balloc indirect path (ext2_balloc.c:180+)
- Allocates indirect blocks when needed
- **Synchronous**: `bwrite()` for newly allocated indirect blocks
- Parent indirect block: bdwrite() or bwrite() depending on IO_SYNC

#### ext2_getlbns / ext2_bmaparray (ext2_bmap.c)
- Block mapping for indirect blocks
- Reads indirect blocks via bread()

---

## Current Synchronous Writes (Must Remain Safe in Series 3)

| Operation | Function | Buffer Write | Condition |
|-----------|----------|--------------|-----------|
| Inode update | ext2_update | bwrite | waitfor=1, !DOINGASYNC |
| Direct block alloc | ext2_balloc | bwrite | never (returns buffer) |
| Indirect block alloc | ext2_balloc | bwrite | new indirect block |
| Parent indirect block | ext2_balloc | bwrite/bdwrite | IO_SYNC → bwrite |
| Data block (sync) | ext2_balloc | bwrite | IO_SYNC flag |
| Extent tree block | ext4_ext_dirty | bwrite | waitfor=1 |
| Inode free | ext2_vfree | bdwrite | superblock |
| Block free | ext2_blkfree | bdwrite | superblock |
| Directory entry | ext2_direnter | bwrite | inode update waitfor=1 |
| Rename | ext2_rename | bwrite | multiple inode updates |

---

## Current Orphan Handling

### Existing On-Disk Support (Unused)
- Superblock: `e3fs_last_orphan` (ext2fs.h:88) - start of orphan list
- Inode: `e2di_dtime` (ext2_dinode.h:112) - deletion time / next orphan pointer
- Inode: `e2di_nlink` (ext2_dinode.h:114) - link count

### Current Behavior (ext2_inode.c:596-627)
```c
ext2_inactive():
  if (ip->i_nlink <= 0) {
      ext2_extattr_free(ip);
      error = ext2_truncate(vp, 0, ...);  // IMMEDIATE truncate
      ip->i_mode = 0;
      ip->i_flag |= IN_CHANGE | IN_UPDATE;
      ext2_vfree(vp, ip->i_number, mode);  // IMMEDIATE free
  }
```
**Problem**: No orphan list - immediate truncation and free. If crash occurs mid-truncate, filesystem inconsistent.

### Missing Orphan List Operations
1. **Orphan insertion**: When i_nlink reaches 0 but vnode still referenced
2. **Orphan removal**: After safe truncation and block freeing
3. **Mount-time recovery**: Read s_last_orphan, traverse list, complete cleanup

### UFS Soft Updates Orphan Model (Reference)
- `di_freelink` in on-disk inode stores next unlinked inode number
- `inodedep` tracks UNLINKED, UNLINKNEXT, UNLINKPREV, UNLINKONLIST flags
- `freefile` structure for deferred inode reclamation
- `softdep_setup_unlink()` registers unlink dependency

---

## Current Buffer Operations

### Buffer Types Used
1. **Inode buffers**: bread/bwrite/bdwrite for inode table blocks
2. **Indirect block buffers**: bread/bwrite/bdwrite for indirect blocks
3. **Extent block buffers**: bread/bwrite/bdwrite for extent tree nodes
4. **Directory block buffers**: bread/bwrite/bdwrite for directory data
5. **Superblock buffer**: bdwrite for superblock updates
6. **Bitmap buffers**: bdwrite for block/inode bitmaps
7. **Group descriptor buffers**: bdwrite for group descriptor updates

### Buffer Flags
- B_DELWRI: delayed write
- B_CACHE: cacheable
- B_RELBUF: release after write
- B_NOCACHE: don't cache

### Buffer Lifecycle
1. `bread()` - synchronous read
2. `getblk()` - get buffer (may be new or cached)
3. Modify buffer data
4. `bwrite()` - synchronous write, waits for completion
5. `bdwrite()` - delayed write, marks B_DELWRI
6. `bawrite()` - async write, no wait
7. `brelse()` / `bqrelse()` - release buffer

### VFS Writeback Integration
- `VOP_FSYNC` → ext2_fsync → ext2_update (waitfor=1)
- `VFS_SYNC` → sync all vnodes
- `bdflush` / `buf_daemon` → periodic delayed write flush
- Cluster write: `cluster_write()`, `cluster_wbuild()`

---

## UFS Soft Updates Concepts Mapping

### Dependency Object Types (UFS → ext2fs)

| UFS Type | ext2fs Equivalent | Purpose |
|----------|-------------------|---------|
| D_INODEDEP | ext2_inodedep | Tracks inode dependencies |
| D_PAGEDEP | ext2_pagedep | Tracks directory page dependencies |
| D_NEWBLK | ext2_newblk | Newly allocated data/metadata block |
| D_ALLOCDIRECT | ext2_allocdirect | Direct block allocation |
| D_INDIRDEP | ext2_indirdep | Indirect block allocation |
| D_ALLOCINDIR | ext2_allocindir | Indirect block allocation dependency |
| D_FREEBLKS | ext2_freeblks | Block freeing from truncation |
| D_FREEFILE | ext2_freefile | Inode deallocation (orphan) |
| D_DIRADD | ext2_diradd | Directory entry addition |
| D_DIRREM | ext2_dirrem | Directory entry removal |
| D_MKDIR | ext2_mkdir | Directory creation |
| D_BMSAFEMAP | ext2_bmsafemap | Bitmap safe map dependency |
| D_SBDEP | ext2_sbdep | Superblock update dependency |
| (NEW) | ext2_orphan_add | Orphan list insertion |
| (NEW) | ext2_orphan_remove | Orphan list removal |

### Dependency State Flags (UFS → ext2fs)

| UFS Flag | ext2fs Flag | Meaning |
|----------|-------------|---------|
| ATTACHED | EXT2_DEP_ATTACHED | Data not currently being written |
| UNDONE | EXT2_DEP_UNDONE | Rolled back for safe write |
| COMPLETE | EXT2_DEP_COMPLETE | Written to disk |
| DEPCOMPLETE | EXT2_DEP_DEPCOMPLETE | Dependent operations complete |
| UNLINKED | EXT2_DEP_UNLINKED | Inode unlinked, on orphan list |
| UNLINKNEXT | EXT2_DEP_UNLINKNEXT | Valid di_freelink (i_dtime) |
| UNLINKPREV | EXT2_DEP_UNLINKPREV | Predecessor points to us |
| UNLINKONLIST | EXT2_DEP_UNLINKONLIST | On disk orphan list |
| GOINGAWAY | EXT2_DEP_GOINGAWAY | Frozen from further change |
| ONWORKLIST | EXT2_DEP_ONWORKLIST | On worklist |
| INPROGRESS | EXT2_DEP_INPROGRESS | Being processed |

### Worklists (UFS → ext2fs)
- `inodedep->id_inowait`: ops waiting for inode update
- `inodedep->id_bufwait`: ops waiting for inode buffer write
- `inodedep->id_newinoupdt`: block updates before inode write
- `inodedep->id_inoupdt`: block updates at inode write
- `inodedep->id_freeblklst`: partial truncates
- `pagedep->pd_diraddhd`: diradd waiting for directory page
- `pagedep->pd_dirremhd`: dirrem waiting for directory page
- `pagedep->pd_pendinghd`: dir entries awaiting write
- Mount worklists: per-type lists for background processing

---

## ext2-Specific Invariants

### On-Disk Format Invariants
1. **Orphan list integrity**: `s_last_orphan` chain via `i_dtime` must be valid (no cycles, in-range inodes)
2. **Link count consistency**: `i_nlink` matches actual directory entries
3. **Block bitmap consistency**: Allocated blocks marked used, free blocks marked free
4. **Inode bitmap consistency**: Allocated inodes marked used
5. **Extent tree integrity**: Tree structure valid, no overlapping extents
6. **Indirect block integrity**: All indirect blocks reachable from inode valid
6. **Directory integrity**: "." and ".." entries correct, no dangling entries

### Runtime Invariants (Must Hold Before Buffer Write)
1. **New metadata block**: Contents initialized before parent pointer written
2. **Block free**: All persistent references removed before bitmap cleared
3. **Directory entry add**: Referenced inode valid and persisted before entry written
4. **Directory entry remove**: Link count updated, orphan inserted if needed
5. **Inode free**: Only after all blocks freed, link count zero, not on orphan list
6. **Truncation**: Block freeing ordered after mapping removal
7. **Extent operations**: Split/merge maintains tree invariants

### Dependency Ordering Invariants
1. **Allocation**: bitmap update → block init → mapping install → parent pointer
2. **Free**: mapping removal → inode update → bitmap update
3. **Directory**: inode persist → dir entry write
4. **Unlink**: dir entry remove → link count update → orphan insert → truncate → inode free
5. **Rename**: target removal dependencies → source link → source unlink
6. **Orphan**: list insertion ordered before truncation permitted

---

## Files Requiring Changes

### New Files
- `sys/fs/ext2fs/ext2_softdep.c` - Dependency framework implementation
- `sys/fs/ext2fs/ext2_softdep.h` - Dependency structures and APIs

### Existing Files to Modify
- `sys/fs/ext2fs/ext2_inode.c` - Inode update, inactive, orphan handling
- `sys/fs/ext2fs/ext2_balloc.c` - Block allocation dependencies
- `sys/fs/ext2fs/ext2_alloc.c` - Allocator integration
- `sys/fs/ext2fs/ext2_extents.c` - Extent tree dependencies
- `sys/fs/ext2fs/ext2_vnops.c` - Directory operations, link/unlink
- `sys/fs/ext2fs/ext2_vfsops.c` - Mount/unmount, sync
- `sys/fs/ext2fs/ext2_subr.c` - Subroutine support
- `sys/fs/ext2fs/ext2_bmap.c` - Block mapping
- `sys/fs/ext2fs/ext2_lookup.c` - Directory lookup
- `sys/fs/ext2fs/inode.h` - Inode structure additions
- `sys/fs/ext2fs/ext2fs.h` - Superblock structure additions
- `sys/fs/ext2fs/ext2_extern.h` - External declarations

---

## Next Steps (Series 2)

1. Create `ext2_softdep.h` with dependency object definitions
2. Create `ext2_softdep.c` with:
   - Mount-private dependency state
   - Dependency allocation/release
   - Reference counting
   - Worklists
   - Locking
   - Shutdown handling
3. Register no behavior-changing dependencies initially
4. Compile and test normal behavior unchanged

---

## References

- UFS Soft Updates: `sys/ufs/ffs/ffs_softdep.c`, `sys/ufs/ffs/softdep.h`
- ext2fs current: `sys/fs/ext2fs/*.c`
- Specification: See initial prompt for complete implementation spec