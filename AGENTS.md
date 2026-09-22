# cxxlab

## Build & Toolchain

- **Build system:** CMake (in-source build dir at `build/`)
- **Formatter:** `.clang-format` (Google-based, 4-space indent, format-on-save via clangd)
- **LSP:** clangd (`--compile-commands-dir=${workspaceFolder}`)
- **Compiler artifacts** are gitignored (`.o`, `.so`, `.a`, `build/`, `compile_commands.json`, etc.)

## Development Rules

- **先读 Ceph，再写代码**：每步开发前必须分析 Ceph reference (`/home/yujrchyang/opensrc/ceph/`) 中对应模块的完整实现（.h + .cc），理解完整逻辑、数据结构、边界条件后再开始编码。禁止仅凭设计文档或记忆实现。
- **新源码文件名必须小写**：所有新增源码文件（`.h`/`.cc`）使用全小写字母命名（如 `bluefs.h`/`bluefs.cc`），禁止大写字母。
- **Markdown 必须通过 markdownlint**：每次创建或修改 `.md` 文件后，必须运行 `markdownlint <file>` 检查语法并修复全部告警，通过后才算完成。
- **ASCII 框线图内部保持纯英文**：`┌─┐`/`│ │`/`└─┘` 这类**闭合框线图**（有横向边框包围的对齐区域），框内内容只能用 ASCII 字符（英文/数字/符号）；中文字形在等宽字体中占 2 列，会导致框线错位。框外的中文标注/图例可自由使用。`│├└─` 纵向缩进流程树不在此列（连接符全为 ASCII 单宽，中文作为节点内容不影响对齐）。闭合框内必须含中文时改用 Markdown 列表/缩进格式表达，不使用 Mermaid/SVG 等替代方案。

## Headers

`IncludeBlocks: Preserve` — clang-format preserves user-defined groups
(separated by blank lines). Within each group, `IncludeCategories` define
the sort priority:

| Priority | Pattern | Example headers |
| ---------- | --------- | ----------------- |
| 1 | `<` + `.h` | `<fcntl.h>`, `<unistd.h>`, `<gtest/gtest.h>`, `<libaio.h>` |
| 2 | `<` + no `.h` or `.hpp` | `<vector>`, `<cstdlib>`, `<memory>` |
| 3 | `<` + `.hpp` | `<boost/container/small_vector.hpp>` |
| 4 | `""` | `"blk/aio.h"`, `"common/buffer.h"` |

Within each priority, headers are sorted alphabetically. To create or
remove a group, add or remove the blank line between blocks. Includes
never migrate across blank lines.

## Progress

### Goal

Implement BlueStore 引擎 (BlueFS + BlueRocksEnv + BlueStore) for cxxlab, modeled after Ceph's `src/os/bluestore/*`, layered on top of existing kv/ (RocksDBStore) and bluestore/ (FreelistManager + Allocator) infrastructure.

### Key Context

- `TOPNSPC` macro defined in `common/common_fwd.h` → expands to `cxxlab`
- `bufferlist` = `cxxlab::bufferlist`
- Key encoding: `prefix + '\0' + inner_key` (both backends, Ceph-compatible)
- RocksDB version: v7.10.2 (via submodule `third_party/rocksdb/`)
- Ceph reference: `/home/yujrchyang/opensrc/ceph/src/kv/*` and `src/os/bluestore/*`
- kv/ compiles as SHARED library (`libkv.so`), links `common` (PUBLIC) + `RocksDB::RocksDB` (PRIVATE)
- `kv/CMakeLists.txt` uses `-Wno-unused-parameter` due to RocksDB callback signatures

### Serialization: `common/denc.h` (DENC framework)

`common/denc.h` 是一个基于 traits 模板的编译期序列化框架，统一通过 `denc(o, p)` 入口调度 encode/decode。使用规则：

- **POD 定长类型**（uint64_t, int32_t 等用于 FreelistManager meta）：直接用 `bl.append((const char*)&v, sizeof(v))` / `p.copy(...)` 即可，不需要 `denc.h`
- **简单容器**（`vector<T>` 等）或 `string`：直接 `#include "common/denc.h"`，模板已内置支持
- **自定义复合类型**（onode_t、extent_map 等复杂的 BlueStore 结构化数据）：用 `WRITE_CLASS_DENC(T)` 宏或 `DENC(Type, v, p)` 宏实现成员级序列化，在 `topnspc` 命名空间内使用

用法示例：

```cpp
#include "common/denc.h"

// 方式 A: DENC 宏（推荐，结构体内联）
struct Extent {
    uint64_t offset;
    uint32_t length;
    DENC(Extent, v, p) {
        DENC_START(1, 1, p);
        denc(v.offset, p);
        denc(v.length, p);
        DENC_FINISH(p);
    }
};

// 方式 B: WRITE_CLASS_DENC（traits 特化）
struct Blob {
    uint64_t id;
    // 需定义 encode() / decode() / bound_encode() 成员
};
WRITE_CLASS_DENC(Blob);  // 在 topnspc 内

// 使用
bufferlist bl;
encode(extent, bl);       // denc(o, p) 顶层包装
Extent e;
decode(e, bl.cbegin());   // denc(o, p) 顶层包装
```

简单原则：**只有 uint64/int/string 等简单字段时直接手动序列化；出现嵌套结构体组合（onode_t 含多个成员 + map + vector）时上 DENC**。

### Done

- **RocksDBStore implementation:**
  - `RDBTransactionImpl`: builds `rocksdb::WriteBatch`, submitted via `db_->Write()` / `db_->Write({.sync=true})`
  - `RDBWholeSpaceIteratorImpl`: wraps `rocksdb::Iterator`, supports `ITERATOR_NOCACHE` via `fill_cache=false`, applies bounds from `WholeSpaceIteratorImpl::iterate_{lower,upper}_bound_` as `rocksdb::Slice*`
  - `RocksDBMergeAdapter`: `rocksdb::MergeOperator` adapter dispatching to `kv::MergeOperator` by key prefix
  - `rm_single_key`: uses `SingleDelete`
  - `rmkeys_by_prefix`: uses `WriteBatch::DeleteRange(prefix+'\0', prefix+'\xff')`
  - `open_read_only`: uses `rocksdb::DB::OpenForReadOnly`
  - `repair`: uses `rocksdb::RepairDB`
  - `compact_prefix` / `compact_range`: encode key and call `CompactRange` with Slice bounds
  - Factory: `create("rocksdb", dir, opts)` returns `RocksDBStore`
  - `set_merge_operator`: overridden to return `-EROFS` if `db_ != nullptr`
  - `init(options_str)`: parses `key=val;key=val` style string for RocksDB options
  - `delete_range_threshold`: configurable via options map, reserved for small-range optimization
- **MemDB implementation:**
  - `MDBTransactionImpl::Op`: refactored with explicit `end` field for `rm_range_keys` (removed reuse of `value`)
  - `_merge()`: uses `cxxlab_assert(mop)` instead of silent return
  - Iterator invalidation: `uint64_t seqno_` incremented on every mutation; `MDBWholeSpaceIteratorImpl` detects stale seqno on each seek/lower_bound/upper_bound and rebuilds snapshot from `std::map` under lock
- **PrefixIteratorImpl:**
  - Removed `skip_to_next_valid()`/`skip_to_prev_valid()` direction-check bug
  - Constructs `seek_lower_bound_`/`seek_upper_bound_` from prefix + bounds and passes to underlying iterator via `set_iterate_lower_bound` / `set_iterate_upper_bound`
- **Base interface (`kv/key_value_db.h`):**
  - `open_read_only`, `repair`, `compact_prefix`, `compact_prefix_async`, `compact_range`, `compact_range_async` added as virtual-with-default
  - `WholeSpaceIteratorImpl`: added `set_iterate_lower_bound(const std::string*)` / `set_iterate_upper_bound(const std::string*)` with protected `iterate_{lower,upper}_bound_` pointers
  - `key_size()` / `value_size()` added to `WholeSpaceIteratorImpl`
  - `make_iterator` now takes `IteratorBounds` parameter
  - `set_merge_operator` is no longer pure virtual (has default storage in `merge_ops_`)
  - `get_merge_ops()` protected accessor for subclasses
- **Tests:** Split `test_librocksdb.cc` (23 raw RocksDB tests) from `test_rocksdb.cc` (21 RocksDBStore tests); 36 MemDB tests in `test_memdb.cc`; all 80 pass
- **Ceph evaluation:** Compared `src/kv/*` (KeyValueDB, RocksDBStore, MemDB) and `src/os/bluestore/*` (KV usage patterns), identified 12 improvement items in 8 categories — all resolved
- **Allocator implementation:**
  - `Allocator` abstract base with `create()` factory, `init_add_free()`, `allocate()`, `release()`, `get_fragmentation()`, `get_alloc_stats()` interface
  - `AvlAllocator`: interval-tree (AVL) based via `range_seg_tree_t`, exact match allocation, `_spillover_range()` cap mechanism
  - `BitmapAllocator`: 2-level bitmap (`bdev_block_count` × `bitmap_granularity`), `ffs`/`ffz` scan, affinity hint via arena-weighted round-robin
  - `HybridAllocator`: wraps AvlAllocator + BitmapAllocator child, `_add_to_tree()` override to claim-free adjacent extents from bitmap before AVL insert
  - `Allocator::create()` factory with `"stupid"` (AvlAllocator), `"bitmap"`, `"hybrid"` type strings
- **Tests:** 21 AvlAllocator tests, 33 BitmapAllocator tests, 19 HybridAllocator tests — all pass
- **BlueFS Phase 1.1–1.9 (data structures through space allocation):** 33 tests total
  - `bluefs_types.h`: `bluefs_super_t`, `bluefs_fnode_t`, `bluefs_transaction_t`, `bluefs_extent_t` with DENC serialization
  - `BlueFSConfig` struct + `RocksDBBlueFSVolumeSelector` (WAL→DB→slow device fallback)
  - KernelDevice as block device backend with buffered IO support
  - Superblock layout (pad to 4KB at offset 0): `_write_super`/`_read_super` with CRC-32C
  - mkfs: allocate log file (4096 extents), write superblock
  - mount: read super, replay log (dirs + files), init allocators with `init_rm_free` for existing extents
  - umount: persist log metadata to superblock, flush + truncate log
  - Log replay: full `bluefs_transaction_t` with `op_bl` operations
  - Directory ops: `mkdir`, `rmdir`, `exists`, `readdir` with persistence
  - File ops: `open_for_read`, `open_for_write` (with/without truncate), `close_writer`/`close_reader`
  - `_flush_F`/`_flush_range_F`/`_flush_data`: buffer → extent-mapped data on block device
  - `_allocate`: AvlAllocator-based extent allocation per bdev
  - `_flush_and_sync_log`: dirty tracking, transaction encoding, log rotation
  - `read`, `read_random`: extent-based read with `preadv` via KernelDevice
  - `fsync`: `_flush_F(force=true)` + `_flush_and_sync_log`
  - Fixes: KernelDevice buffered IO alignment skip, `close_writer` double-flush, `umount` log metadata loss, `pending_release` vector resize, `_flush_F` buffer clear, fsync deadlock (release dirty lock before log sync)
  - **Code review fixes (Phase 1.11 completion):** `lock_file`/`unlock_file`/`invalidate_cache`/`flush_range`/`preallocate`/`get_used` all implemented; `OP_DIR_UNLINK` replay assertion (refs>0); `OP_FILE_UPDATE_INC` delta offset validation; truncate uses `op_file_update` instead of `op_file_update_inc`; `_flush_data` reverted to direct buffer write
  - 55 tests total, all pass
- **BlueRocksEnv Phase 2.1–2.7 implementation:**
  - `blue_rocks_env.h` / `blue_rocks_env.cc`: Full `BlueRocksEnv : rocksdb::EnvWrapper` implementation
  - `err_to_status()` helper converting POSIX errno → `rocksdb::Status`
  - `split()` helper parsing `"dir/file"` → `{dir, file}`
  - `BlueRocksSequentialFile` / `NewSequentialFile`: wraps BlueFS `FileReader`, supports Read/Skip/InvalidateCache
  - `BlueRocksRandomAccessFile` / `NewRandomAccessFile`: random reads via `read_random()`, GetUniqueId, Prefetch, Hint
  - `BlueRocksWritableFile` / `NewWritableFile`: Append/PositionedAppend/Truncate/Close/Flush/Sync/GetFileSize/GetUniqueId/InvalidateCache/RangeSync/Allocate
  - `ReuseWritableFile`: rename + open_for_write(overwrite=true)
  - `BlueRocksDirectory` / `NewDirectory`: Fsync → sync_metadata
  - `FileExists`, `GetChildren`, `DeleteFile`, `CreateDir`, `CreateDirIfMissing`, `DeleteDir`, `GetFileSize`, `GetFileModificationTime`, `RenameFile`, `AreFilesSame`, `LockFile`, `UnlockFile`, `GetAbsolutePath`, `GetTestDirectory`
  - `BlueFSRocksdbLogger`: stderr-based rocksdb::Logger, factory `CreateRocksdbLogger()`
  - Absolute path escape: files starting with `/` forwarded to POSIX Env
  - 29 tests covering all operations, all pass

### Key Decisions

- Single default ColumnFamily (no hash sharding, no `parse_sharding_def`)
- `set_merge_operator` must be called before `open()` / `create_and_open()` for RocksDBStore
- `close()` null-checks `db_` before delete, sets to `nullptr`, resets adapter
- DeleteRange threshold: configurable via `delete_range_threshold`, default 0 → always use DeleteRange
- MemDB iterator invalidation: seqno-based, automatically rebuilds snapshot on detection of concurrent writes
- Iterator bounds two-layer separation: `WholeSpaceIteratorImpl` stores `const std::string*`, backend converts to native type (`rocksdb::Slice*`) at seek time
- BitmapFreelistManager: `create()` allocates block 0 at mkfs time (caller must not double-allocate)
- Bitmap key encoding: 8 bytes big-endian uint64_t (memcmp-compatible, matches Ceph `_key_encode_u64`)
- BitmapFreelistManager links as STATIC library `libbluestore.a`
- bluestore/ subdirectory added to root CMakeLists.txt
- Allocator::create() type string `"stupid"` maps to AvlAllocator (not the original Ceph StupidAllocator)
- HybridAllocator allocation strategy: always try AVL first, bitmap as fallback (simplified from Ceph's conditional strategy)
- `_add_to_tree()` claim-free optimization reclaims adjacent free extents from bitmap child before AVL insertion
- BlueFS uses AvlAllocator (`"avl"` type) instead of BitmapAllocator — BitmapAllocator's 512MB L2 granularity is too coarse for small test devices (8MB), causing `init_rm_free` on any range within the first 512MB to clear the entire L2 bit
- `_flush_F` clears `h->buffer` after successful flush to prevent double-flush on `close_writer` calling `_flush_F` then `_flush_bdev`
- `close_writer` calls `_flush_F(h, true)` before `_flush_bdev()` then `_close_writer()` — ensures data flushed before writer destroyed
- `umount` saves `super_.log_fnode` and calls `_write_super()` before clearing `nodes_.file_map` — otherwise next mount gets stale log extents
- `fsync` lock ordering: release `dirty_.lock` before calling `_flush_and_sync_log` (which internally acquires both `log_.lock` and `dirty_.lock`)
- `dirty_.pending_release` vector resized to `MAX_BDEV` in `_init_alloc` (accessed as `pending_release[e.bdev]`); `_flush_and_sync_log` processes in-place instead of swap-and-discard to preserve vector size
- KernelDevice `write`/`read`: skip `is_valid_io` alignment check for buffered IO (kernel page cache handles misalignment)
- Allocator::create() type `"stupid"` maps to AvlAllocator
- **Allocator extracted from `bluestore/` to `blk/`** (Phase 0 refactoring): Allocator is pure memory management with no dependency on RocksDB or FreelistManager. `common/extent_types.h` created with `pextent_t`, `PExtentVector`, `interval_set<T>`. `bluestore/bluestore_types.h` now a thin wrapper. Allocator tests moved to `tests/blk/`. `bluestore` now links `blk` (PUBLIC).

### Development Plan (3 phases, bottom-up)

开发顺序 **BlueFS → BlueRocksEnv → BlueStore**，详见 `docs/plan.md`。

#### Phase 1: BlueFS (11 steps)

| # | Step | Test Strategy | Status |
| --- | ------ | --------------- | -------- |
| 1.1 | `bluefs_types` — 数据结构 + DENC | encode/decode roundtrip | ✅ |
| 1.2 | BlueFSConfig + RocksDBBlueFSVolumeSelector | 逻辑测试，无 IO | ✅ |
| 1.3 | 设备层 + 超级块 (add_block_device, _write_super) | tempfile 读写 | ✅ |
| 1.4 | mkfs + mount + 日志重放 | mkfs→mount→umount 循环 | ✅ |
| 1.5 | 目录操作 (mkdir, rmdir, lookup) | 创建/列出/删除 | ✅ |
| 1.6 | 文件创建/关闭 (open_for_write/read, close_writer/reader) | 文件生命周期 | ✅ |
| 1.7 | 文件读写 (append_try_flush, read, read_random, fsync) | 写入→读回验证 | ✅ |
| 1.8 | 日志持久化 (dirty tracking, flush_and_sync_log) | 写→umount→mount→验证 | ✅ |
| 1.9 | 空间分配 (_allocate, 设备回退，shared_alloc) | 多设备分配 | ✅ |
| 1.10 | 异步压缩 (_compact_log_async) | 增长→压缩→验证 | ✅ |
| 1.11 | 文件管理 (truncate, unlink, rename, stat) + 边界 | 错误路径 | ✅ |

#### Phase 2: BlueRocksEnv (7 steps)

| # | Step | Test Strategy | Status |
| --- | ------ | --------------- | -------- |
| 2.1 | 辅助函数 (err_to_status, split) | 单元测试 | ✅ |
| 2.2 | BlueRocksSequentialFile + NewSequentialFile | BlueFS 写入→Env 读取 | ✅ |
| 2.3 | BlueRocksRandomAccessFile + NewRandomAccessFile | 随机读 + GetUniqueId | ✅ |
| 2.4 | BlueRocksWritableFile + NewWritableFile | Append→Sync→Close 验证 | ✅ |
| 2.5 | 目录/锁/状态操作 (FileExists, GetChildren, LockFile 等) | 完整生命周期 | ✅ |
| 2.6 | BlueFSRocksdbLogger | dout 输出验证 | ✅ |
| 2.7 | BlueRocksEnv 集成 + EnvMirror | 与 POSIX Env 双路验证 | ✅ |

#### Phase 3: BlueStore (15 steps)

| # | Step | Test Strategy |
| --- | ------ | --------------- |
| 3.1 | bluestore_types (pextent_t, blob_t, onode_t, cnode_t) | DENC roundtrip |
| 3.2 | BlueStoreConfig | struct init + file load |
| 3.3 | Onode key 编码 (_key_encode/decode) | roundtrip + 排序 |
| 3.4 | Blob 内存管理 + use_tracker | split, get/put_ref |
| 3.5 | ExtentMap (seek_lextent, punch_hole, add/rm) | 插入/查找/删除 |
| 3.6 | Onode + Collection (KV 读写 + shard) | 编码→KV→解码 |
| 3.7 | mkfs + mount (FM + Alloc + BlockDev + BlueFS) | 全流程启动 |
| 3.8 | TransContext + OpSequencer (状态机) | 状态推进 + 顺序保证 |
| 3.9 | Small Write (≤1 AU, read-modify-write) | 写入→验证 extent |
| 3.10 | Big Write (多 AU 对齐) | 写入→验证校验和 |
| 3.11 | Read (_do_read, cache, csum verify) | 写入→读取→比较 |
| 3.12 | KV pipeline (kv_sync_thread, kv_finalize_thread, release) | 完整事务生命周期 |
| 3.13 | Zero + Remove + Attrs | 数据打孔→删除→空间释放 |
| 3.14 | Collection List | 对象分页列出 |
| 3.15 | 集成测试 | 压力 + 持久化 + 边界 |

### Next Steps

1. Phase 3.1: bluestore_types (bluestore_pextent_t, bluestore_blob_t, bluestore_onode_t, bluestore_cnode_t + DENC)
2. Phase 3.2: BlueStoreConfig struct init + file load

### Relevant Files

- `kv/key_value_db.h`: Abstract base (TransactionImpl, IteratorImpl, WholeSpaceIteratorImpl, PrefixIteratorImpl, KeyValueDB)
- `kv/key_value_db.cc`: PrefixIteratorImpl, KeyValueDB factory/create
- `kv/mem/mem_db.h` / `kv/mem/mem_db.cc`: MemDB backend
- `kv/rocksdb/rocksdb_store.h` / `kv/rocksdb/rocksdb_store.cc`: RocksDBStore backend
- `kv/merge_op/`: MergeOperator abstract base, Int64ArrayMergeOperator, XorMergeOperator
- `kv/CMakeLists.txt`: builds libkv.so (SHARED), links common (PUBLIC) + RocksDB::RocksDB (PRIVATE), uses `-Wno-unused-parameter`
- `bluestore/freelist_manager.h`: FreelistManager abstract base
- `bluestore/bitmap_freelist_manager.h` / `bluestore/bitmap_freelist_manager.cc`: BitmapFreelistManager implementation
- `bluestore/CMakeLists.txt`: builds libbluestore.a (STATIC)
- `bluestore/allocator.h` / `bluestore/allocator.cc`: Allocator abstract base + factory
- `bluestore/avl_allocator.h` / `bluestore/avl_allocator.cc`: AvlAllocator (interval-tree)
- `bluestore/bitmap_allocator.h` / `bluestore/bitmap_allocator.cc`: BitmapAllocator (2-level bitmap)
- `bluestore/hybrid_allocator.h` / `bluestore/hybrid_allocator.cc`: HybridAllocator (AVL + bitmap)
- `tests/kv/test_librocksdb.cc`: 23 raw RocksDB tests
- `tests/kv/test_rocksdb.cc`: 21 RocksDBStore tests
- `tests/kv/test_memdb.cc`: 36 MemDB tests
- `tests/bluestore/test_bitmap_freelist_manager.cc`: 15 BitmapFreelistManager tests
- `tests/bluestore/test_avl_allocator.cc`: 21 AvlAllocator tests
- `tests/bluestore/test_bitmap_allocator.cc`: 33 BitmapAllocator tests
- `tests/bluestore/test_hybrid_allocator.cc`: 19 HybridAllocator tests
- `tests/kv/CMakeLists.txt`: test targets linking kv + RocksDB + cxxlab_test_helpers + GTest
- `docs/design/keyvalue-db.md`: full design specification
- `docs/design/freelist-manager.md`: FreelistManager/BitmapFreelistManager design analysis (Ceph reference: `src/os/bluestore/FreelistManager.*`, `BitmapFreelistManager.*`)
- `docs/design/allocator.md`: Allocator design (Avl + Bitmap + Hybrid)
- `docs/design/block-device.md`: Block device abstraction design
- `docs/design/bluestore.md`: BlueStore engine design
- `docs/design/bluefs.md`: BlueFS user-space filesystem design
- `docs/plan.md`: Detailed development plan (3 phases, 33 steps)
