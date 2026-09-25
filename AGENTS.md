# cxxlab

## Build & Toolchain

- Build system: CMake (in-source build dir at `build/`)
- Formatter: `.clang-format` (Google-based, 4-space indent, format-on-save via clangd)
- LSP: clangd (`--compile-commands-dir=${workspaceFolder}`)
- Compiler artifacts are gitignored (`.o`, `.so`, `.a`, `build/`, `compile_commands.json`, etc.)

## Development Rules

- 先读 Ceph，再写代码：每步开发前必须分析 Ceph reference (`/home/yujrchyang/opensrc/ceph/`) 中对应模块的完整实现（.h + .cc），理解完整逻辑、数据结构、边界条件后再开始编码。禁止仅凭设计文档或记忆实现。
- 新源码文件名必须小写：所有新增源码文件（`.h`/`.cc`）使用全小写字母命名（如 `bluefs.h`/`bluefs.cc`），禁止大写字母。
- Markdown 必须通过 markdownlint：每次创建或修改 `.md` 文件后，必须运行 `markdownlint <file>` 检查语法并修复全部告警，通过后才算完成。
- Markdown 不使用加粗语法：所有 `.md` 文件（含 `AGENTS.md` 和 `docs/` 目录）禁止使用 `**text**` 加粗语法。使用标题层级、列表结构、代码标记等替代强调。
- ASCII 框线图内部保持纯英文：`┌─┐`/`│ │`/`└─┘` 这类闭合框线图（有横向边框包围的对齐区域），框内内容只能用 ASCII 字符（英文/数字/符号）；中文字形在等宽字体中占 2 列，会导致框线错位。框外的中文标注/图例可自由使用。`│├└─` 纵向缩进流程树不在此列（连接符全为 ASCII 单宽，中文作为节点内容不影响对齐）。闭合框内必须含中文时改用 Markdown 列表/缩进格式表达，不使用 Mermaid/SVG 等替代方案。

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

开发计划、状态标记、实现记录、设计决策见 [docs/dev-plan.md](docs/dev-plan.md)。

### Key Context

- `TOPNSPC` macro defined in `common/common_fwd.h` → expands to `cxxlab`
- `bufferlist` = `cxxlab::bufferlist`
- Key encoding: `prefix + '\0' + inner_key` (both backends, Ceph-compatible)
- RocksDB version: v7.10.2 (via submodule `third_party/rocksdb/`)
- Ceph reference: `/home/yujrchyang/opensrc/ceph/src/kv/*` and `src/os/bluestore/*`
- kv/ compiles as SHARED library (`libkv.so`), links `common` (PUBLIC) + `RocksDB::RocksDB` (PRIVATE)
- bluefs/ compiles as SHARED library (`libbluefs.so`), links `common` (PUBLIC) + `blk` (PUBLIC), no kv/RocksDB
- bluestore/ compiles as SHARED library (`libbluestore.so`), links `common` (PUBLIC) + `kv` (PUBLIC) + `blk` (PUBLIC) + `bluefs` (PUBLIC) + `RocksDB` (PRIVATE)
- `kv/CMakeLists.txt`: kv compiles with default warnings; RocksDB callback unused parameters handled by RocksDB's own build flags

### Serialization: `common/denc.h` (DENC framework)

`common/denc.h` 是一个基于 traits 模板的编译期序列化框架，统一通过 `denc(o, p)` 入口调度 encode/decode。使用规则：

- POD 定长类型（uint64_t, int32_t 等用于 FreelistManager meta）：直接用 `bl.append((const char*)&v, sizeof(v))` / `p.copy(...)` 即可，不需要 `denc.h`
- 简单容器（`vector<T>` 等）或 `string`：直接 `#include "common/denc.h"`，模板已内置支持
- 自定义复合类型（onode_t、extent_map 等复杂的 BlueStore 结构化数据）：用 `WRITE_CLASS_DENC(T)` 宏或 `DENC(Type, v, p)` 宏实现成员级序列化，在 `topnspc` 命名空间内使用

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

简单原则：只有 uint64/int/string 等简单字段时直接手动序列化；出现嵌套结构体组合（onode_t 含多个成员 + map + vector）时上 DENC。

### Relevant Files

- `kv/key_value_db.h`: Abstract base (TransactionImpl, IteratorImpl, WholeSpaceIteratorImpl, PrefixIteratorImpl, KeyValueDB)
- `kv/key_value_db.cc`: PrefixIteratorImpl, KeyValueDB factory/create
- `kv/mem/mem_db.h` / `kv/mem/mem_db.cc`: MemDB backend
- `kv/rocksdb/rocksdb_store.h` / `kv/rocksdb/rocksdb_store.cc`: RocksDBStore backend
- `kv/merge_op/`: MergeOperator abstract base, Int64ArrayMergeOperator, XorMergeOperator
- `kv/CMakeLists.txt`: builds libkv.so (SHARED), links common (PUBLIC) + RocksDB::RocksDB (PRIVATE), uses `-Wno-unused-parameter`
- `bluefs/bluefs.h` / `bluefs/bluefs.cc`: BlueFS user-space filesystem
- `bluefs/bluefs_types.h` / `bluefs/bluefs_types.cc`: bluefs_super_t, bluefs_fnode_t, bluefs_transaction_t, bluefs_extent_t, bluefs_shared_alloc_context_t
- `bluefs/bluefs_config.h` / `bluefs/bluefs_config.cc`: BlueFSConfig struct
- `bluefs/bluefs_volume_selector.h` / `bluefs/bluefs_volume_selector.cc`: BlueFSVolumeSelector + RocksDBBlueFSVolumeSelector
- `bluefs/CMakeLists.txt`: builds libbluefs.so (SHARED), links common (PUBLIC) + blk (PUBLIC), no kv/RocksDB
- `bluestore/freelist_manager.h`: FreelistManager abstract base
- `bluestore/bitmap_freelist_manager.h` / `bluestore/bitmap_freelist_manager.cc`: BitmapFreelistManager implementation
- `bluestore/blue_rocks_env.h` / `bluestore/blue_rocks_env.cc`: BlueRocksEnv (rocksdb::Env adapter for BlueFS)
- `bluestore/bluestore_types.h`: bluestore_pextent_t alias
- `bluestore/CMakeLists.txt`: builds libbluestore.so (SHARED), links common (PUBLIC) + kv (PUBLIC) + blk (PUBLIC) + bluefs (PUBLIC) + RocksDB (PRIVATE)
- `blk/allocator.h` / `blk/allocator.cc`: Allocator abstract base + factory
- `blk/avl_allocator.h` / `blk/avl_allocator.cc`: AvlAllocator (interval-tree)
- `blk/bitmap_allocator.h` / `blk/bitmap_allocator.cc`: BitmapAllocator (2-level bitmap)
- `blk/hybrid_allocator.h` / `blk/hybrid_allocator.cc`: HybridAllocator (AVL + bitmap)
- `tests/kv/test_librocksdb.cc`: 23 raw RocksDB tests
- `tests/kv/test_rocksdb.cc`: 25 RocksDBStore tests
- `tests/kv/test_memdb.cc`: 36 MemDB tests
- `tests/bluefs/test_bluefs_types.cc`: BlueFS types DENC roundtrip tests
- `tests/bluefs/test_bluefs_volume_selector.cc`: VolumeSelector tests
- `tests/bluefs/test_bluefs.cc`: BlueFS functional tests (64 tests)
- `tests/bluestore/test_bitmap_freelist_manager.cc`: 15 BitmapFreelistManager tests
- `tests/bluestore/test_blue_rocks_env.cc`: BlueRocksEnv tests (29 tests)
- `tests/blk/test_avl_allocator.cc`: 18 AvlAllocator tests
- `tests/blk/test_bitmap_allocator.cc`: 23 BitmapAllocator tests
- `tests/blk/test_hybrid_allocator.cc`: 17 HybridAllocator tests
- `tests/kv/CMakeLists.txt`: test targets linking kv + RocksDB + cxxlab_test_helpers + GTest
- `common/throttle.h` / `common/throttle.cc`: Throttle class for resource rate limiting
- `docs/design/keyvalue-db.md`: full design specification
- `docs/design/freelist-manager.md`: FreelistManager/BitmapFreelistManager design analysis (Ceph reference: `src/os/bluestore/FreelistManager.*`, `BitmapFreelistManager.*`)
- `docs/design/allocator.md`: Allocator design (Avl + Bitmap + Hybrid)
- `docs/design/block-device.md`: Block device abstraction design
- `docs/design/bluestore.md`: BlueStore engine design
- `docs/design/bluefs.md`: BlueFS user-space filesystem design
- `docs/dev-plan.md`: Detailed development plan (3 phases, 33 steps)
