# FreelistManager — 空闲空间管理器

> 实现状态：已实现（BitmapFreelistManager + null_manager 模式）

## 1. 需求分析

### 1.1 背景

在 BlueStore 的单机引擎架构中，`FreelistManager` 负责持久化追踪 block device 上哪些空间已分配（allocated）、哪些空闲（free）。它与 `Allocator`（内存中负责分配决策的组件）共同构成双层空间管理：

| 组件 | 角色 | 存储位置 |
| --- | --- | --- |
| `Allocator` | 运行时的分配决策（B-tree / 位图） | RAM |
| `FreelistManager` | 分配状态的持久化追踪 | KV store (RocksDB) |

两者的关系是：运行时 IO 路径中 Allocator 决定分配哪些 block，事务提交时将变更同步到 FreelistManager；重启恢复时通过遍历 FreelistManager 中所有空闲区间来重建 Allocator 的内存状态。

### 1.2 约束条件

- 操作系统：仅 Linux（x86\_64 + AArch64）
- 后端存储：依赖 `kv::KeyValueDB` 抽象层（当前已有 RocksDBStore / MemDB 实现）
- 不支持 SMR 设备：`ZonedFreelistManager` 相关代码不做移植；`create()` 接口中移除 `zone_size` / `first_sequential_zone` 参数

### 1.3 核心功能

| 功能 | 说明 |
| --- | --- |
| `allocate(offset, length, txn)` | 标记指定范围为已分配 |
| `release(offset, length, txn)` | 标记指定范围为空闲 |
| `enumerate_reset` / `enumerate_next` | 遍历所有空闲区间（用于启动恢复） |
| `create(size, granularity, ...)` | mkfs 时初始化持久化元数据 |
| `init(kvdb, ...)` | 上电时从 KV 加载配置参数 |

## 2. 架构设计

### 2.1 整体架构

```plaintext
┌────────────────────────────────────────────────────────────┐
│                    FreelistManager (abstract)              │
│  allocate / release / enumerate / create / init / shutdown │
│  set_null_manager / is_null_manager                        │
└──────────────┬─────────────────────────────────────────────┘
               │ inherit
┌──────────────▼─────────────────────────────────────────────┐
│                 BitmapFreelistManager                      │
│  bitmap-based, persisted to KeyValueDB                     │
│  1 bit = 1 block, 1 = allocated, 0 = free                  │
│  each KV pair stores blocks_per_key (default 128) bits     │
│  XOR MergeOperator for atomic toggle                       │
│                                                            │
│  + null_manager: allocate/release are no-ops at runtime,   │
│    state kept in Allocator (RAM), persisted by BlueFS      │
└────────────────────────────────────────────────────────────┘
```

### 2.2 KV 数据模型

BitmapFreelistManager 使用两个 KV prefix：

| Prefix | 含义 | 内容 |
| --- | --- | --- |
| `meta_prefix` (如 `"B"`) | 元数据 | `bytes_per_block`, `blocks_per_key`, `blocks`, `size` |
| `bitmap_prefix` (如 `"b"`) | 位图数据 | `key = offset` → `value = bitmask` |

Key 编码：

- Key 为 uint64_t 类型的偏移量（byte offset），编码为 10 字节 binary string（与 `_key_encode_u64` 编码一致）
- Key 按 `bytes_per_key` 对齐（`bytes_per_key = bytes_per_block * blocks_per_key`）

Value 编码：

- Value 为 `blocks_per_key >> 3` 字节的 bitmask
- 每个 bit 代表一个 block：1 = allocated（已分配），0 = free（空闲）
- 默认 `blocks_per_key = 128`，故默认 value 长度为 16 字节

示例：一个 4KB block size、128 blocks/key、1TB 设备

```plaintext
bytes_per_block   = 4096 (4KB)
blocks_per_key    = 128
bytes_per_key     = 4096 * 128 = 524288 (512KB)
blocks            = 1TB / 4KB = 256M, 向上对齐到 128 → 256M

KV 条目数          = 256M / 128 = 2M 条
```

### 2.3 分配与释放的数学本质

```plaintext
allocate(offset, len)  → bitmap := bitmap XOR mask      (1 XOR 1 = 0: 从 free 变 allocated)
release(offset, len)   → bitmap := bitmap XOR mask      (0 XOR 1 = 1: 从 allocated 变 free)
```

由于 XOR 的对称性，allocate 和 release 调用的是同一个 `_xor()` 方法。mask 在范围内为 1，范围外为 0。XOR MergeOperator 在 KV 层完成原子性合并：

```cpp
// XorMergeOperator.merge():
// result[i] = existing_byte[i] ^ delta_byte[i]
for (size_t i = 0; i < rlen; ++i)
    (*new_value)[i] ^= rdata[i];
```

### 2.4 边界处理：跨 KV pair 的范围操作

当 `allocate/release` 的范围跨越多个 KV pair 时，`_xor()` 将其分为三段处理：

```plaintext
|<── first_key ──>|<── middle keys ──>|<── last_key ──>|
┌─────────────────┬──────────────────┬─────────────────┐
│  Partial        │   Full           │   Partial        │
│  gen local mask │   all_set_bl     │  gen local mask  │
│  merge(txn)     │  merge(txn)      │  merge(txn)      │
└─────────────────┴──────────────────┴─────────────────┘
```

- first_key：从 `s = offset % blocks_per_key` 到 `blocks_per_key - 1` 的 mask
- middle keys：整 key 全 1 的 mask（`all_set_bl` 预计算）
- last_key：从 0 到 `e = end % blocks_per_key` 的 mask

### 2.5 磁盘大小对齐

```cpp
// blocks_per_key 对齐
uint64_t size_2_block_count(uint64_t target_size) const {
    auto blocks = target_size / bytes_per_block;           // block 数
    // 向上对齐到 blocks_per_key 的整数倍
    if (blocks % blocks_per_key != 0)
        blocks = (blocks / blocks_per_key + 1) * blocks_per_key;
    return blocks;
}
```

对齐后超出实际磁盘空间的区域（`size ~ blocks * bytes_per_block`）在 `create()` 时通过 `_xor()` 标记为 allocated，防止被错误分配。

## 3. 组件详情

### 3.1 FreelistManager

抽象基类，定义以下接口：

- 工厂方法：`create(type, meta_prefix, bitmap_prefix)` — 当前支持 `"bitmap"` 类型
- 生命周期：`create(size, granularity, txn)` 在 mkfs 时持久化配置参数；`init(kvdb, read_only, cfg_reader)` 在 mount 时从 KV 加载配置恢复运行时状态；`shutdown()` 清理
- 启动恢复：`enumerate_reset()` / `enumerate_next(kvdb, &offset, &length)` 遍历所有空闲区间（用于启动恢复）
- 运行时：`allocate(offset, length, txn)` / `release(offset, length, txn)` 标记分配/释放
- 查询：`get_size()` / `get_alloc_units()` / `get_alloc_size()`
- 元数据导出：`get_meta(target_size, *)` 供写 bdev label
- null_manager 模式：`set_null_manager()` / `is_null_manager()` 控制是否跳过 KV 写入

### 3.2 BitmapFreelistManager

基于 bitmap + XOR MergeOperator 的实现，核心成员：

- KV prefix：`meta_prefix`（如 `"B"`）存参数，`bitmap_prefix`（如 `"b"`）存位图
- 几何参数：`size`（设备大小）、`bytes_per_block`（block size）、`blocks_per_key`（默认 128）、`bytes_per_key`（= bytes_per_block × blocks_per_key）、`blocks`（对齐后总 block 数）
- 对齐掩码：`block_mask` / `key_mask`
- 预计算：`all_set_bl` — 全 1 mask 的 bufferlist，用于 middle keys
- 枚举状态：`enumerate_p`（iterator）、`enumerate_offset`、`enumerate_bl`、`enumerate_bl_pos` — 游标
- 线程安全：`std::mutex lock` 保护 enumerate 系列操作

### 3.3 MergeOperator 注册

`XorMergeOperator`（定义在 `kv/merge_op/xor_merge_op.h`）需在创建/打开 KV store 之前注册到 `bitmap_prefix`，通过 `db->set_merge_operator(bitmap_prefix, std::make_shared<kv::XorMergeOperator>())` 完成。

### 3.4 null_manager 模式

当设备满足以下条件时可启用 null_manager 优化：

- 非 SMR 设备
- 存在 BlueFS（分配状态可写文件）
- 非 read_only 模式

启用后 FM 行为变更：

```cpp
void BitmapFreelistManager::allocate(uint64_t offset, uint64_t length,
                                     KeyValueDB::Transaction txn) {
    if (!is_null_manager())
        _xor(offset, length, txn);   // 正常模式：写 KV
    // null 模式：空操作，仅更新 Allocator (内存)
}
void BitmapFreelistManager::release(uint64_t offset, uint64_t length,
                                     KeyValueDB::Transaction txn) {
    if (!is_null_manager())
        _xor(offset, length, txn);   // 正常模式：写 KV
    // null 模式：空操作，仅更新 Allocator (内存)
}
```

启动/关闭流程差异：

| 阶段 | 正常模式 | null_manager 模式 |
| --- | --- | --- |
| mkfs | `fm->create()` 初始化 bitmap KV | 相同（回退时 KV 数据完整） |
| mount 时重建 Allocator | `fm->enumerate_next()` 扫描 KV | 从 BlueFS 文件 `restore_allocator()` 加载 |
| 运行时 allocate/release | `_txc_finalize_kv` 中写 KV | 跳过 `_txc_finalize_kv`，仅更新 Allocator |
| 关闭时持久化 | 无需额外操作（已写 KV） | `store_allocator()` 写入 BlueFS 文件 |

模式切换（null → bitmap）：先 `shutdown()` + `delete` 旧 FM，然后以 `freelist_type = "bitmap"` 重新 `_open_fm(txn, true, true, true)`（fm_restore=true 重新 allocate 全部设备），最后将 Allocator 中当前空闲区间写入新 FM。

## 4. 关键流程

### 4.1 mkfs 流程

```plaintext
BlueStore::mkfs()
  │
  ├── 1. 确定参数
  │     size = bdev->get_size()
  │     bytes_per_block = min_alloc_size (e.g., 4KB/16KB/64KB)
  │     blocks_per_key = bluestore_freelist_blocks_per_key (default 128)
  │
  ├── 2. db->set_merge_operator("b", XorMergeOperator)
  ├── 3. db->create_and_open(out)
  │
  ├── 4. t = db->get_transaction()
  ├── 5. fm->create(size, bytes_per_block, t)
  │     ├── 计算 blocks = size_2_block_count(size)
  │     ├── 预分配超出 EOF 的区域: _xor(size, extra, t)
  │     ├── bufferlist bl; bl.append((const char*)&v, sizeof(v));
  │     │   txn->set("B", "bytes_per_block", bl)   // 固定 8 字节小端
  │     ├── 同上: blocks_per_key, blocks, size 写入 meta_prefix
  │
  └── 6. db->submit_transaction_sync(t)
```

### 4.2 mount 流程

```plaintext
BlueStore::_open_db()
  ├── db->open(out)                // 打开 KV store
  └── db->set_merge_operator("b", XorMergeOperator)  // 注册 XOR merge op

BlueStore::_open_fm()
  ├── fm->init(kvdb, read_only, cfg_reader)
  │     ├── _read_cfg(cfg_reader)     // 从 bdev label 读取参数 (可选)
  │     ├── if fail: _load_from_db()  // 从 meta_prefix 读取
  │     ├── _sync()                   // 检查 size 变化，必要时 expand (初始版本 deferred)
  │     └── _init_misc()              // 计算 key_mask, all_set_bl 等
  │
  ├── 分支: 重建 Allocator
  │     ├── [正常模式] fm->enumerate_next() 逐个扫描 free extents
  │     │               alloc->init_add_free(offset, length)
  │     └── [null]     restore_allocator(alloc)  // 从 BlueFS 文件恢复
  │
  └── fm->enumerate_reset()
```

### 4.3 运行时 IO 路径

```plaintext
写入 IO (buffer writes, new extent allocation)

  txc->allocated.insert(offset, length)
  txc->released.insert(...)

BlueStore::_txc_finalize_kv(txc, t)
  │
  ├── 跳过: [null_manager 模式] 无需写 KV
  │
  ├── [正常模式]
  │     ├── 处理 allocated ↔ released 重叠区间
  │     ├── for each in allocated:
  │     │     fm->allocate(off, len, t)    → t->merge("b", key, xor_mask)
  │     ├── for each in released:
  │     │     fm->release(off, len, t)     → t->merge("b", key, xor_mask)
  │     └── db->submit_transaction(t)      // 事务提交，KV 持久化
  │
  └── [null 模式] 仅 Allocator 变化, 关闭时一次写入 BlueFS

BlueStore::_txc_release_alloc(txc)
  │
  └── alloc->release(off, len)       // 还回 Allocator (内存)
```

### 4.4 enumerate 扫描流程

```plaintext
fm->enumerate_reset()
  enumerate_offset = 0, enumerate_bl_pos = 0
  enumerate_bl.clear(), enumerate_p.reset()

fm->enumerate_next(db, &offset, &length) → bool
  │
  ├── 首次: 创建 iterator, 定位到第一个 key
  │        assert(第一个 bit == 1)  // block 0 永远已分配
  │
  ├── 找 free 区间的起点:
  │     while (true)
  │       enumerate_bl_pos = get_next_clear_bit(bl, pos)  // 找 0
  │       if found: break
  │       else: iterator->next() 或跳出
  │     offset = key_offset + bl_pos * bytes_per_block
  │
  ├── 找 free 区间的终点:
  │     while (true)
  │       enumerate_bl_pos = get_next_set_bit(bl, pos)    // 找 1
  │       if found: end = key_offset + bl_pos * bytes_per_block; break
  │       else: iterator->next()
  │     length = end - offset
  │
  └── 返回 true (有下一个空闲区间)
```

## 5. 线程安全

BitmapFreelistManager 的线程安全模型：

| 操作 | 保护方式 |
| --- | --- |
| `allocate/release` | 无需加锁 — 所有修改通过 `Transaction::merge()` 在 KV 层原子完成 |
| `enumerate_reset/next` | `lock` mutex 保护 — 多个线程同时遍历会破坏内部状态 |

`allocate` / `release` 本身是纯函数式的（生成 delta mask → merge），不修改内部状态，因此不需要锁。`enumerate` 系列操作维护游标状态（`enumerate_p`, `enumerate_offset` 等），需要互斥。

## 6. 设计简化

| Ceph 实现 | cxxlab 处理方式 |
| --- | --- |
| `_read_cfg()` 从 bdev label 读取配置 | 保留可选 cfg_reader 接口，主要依赖 `_load_from_db()` |
| `_sync()` / `_expand()` 版本兼容逻辑 | 初始版本不做 expand（固定磁盘大小），可后续补充 |
| `ZonedFreelistManager` | 不实现 |
| `get_meta()` 写 bdev label | 保留接口，暂不使用 |

## 7. 构建

BitmapFreelistManager 作为 `bluestore` 动态库的组件编译（`libbluestore.so`），链接 `common`（bufferlist, assert 等）、`kv`（获取 KeyValueDB 接口）、`blk`（Allocator + extent_types）、`bluefs`（均为 PUBLIC）和 `RocksDB`（PRIVATE，仅 BlueRocksEnv 使用）。

命名空间策略：各模块统一在 `TOPNSPC`（即 `cxxlab`）命名空间内，`bufferlist` 等类型可直接使用。Allocator 已从 `bluestore/` 迁移到 `blk/`（Phase 0 重构），引用时使用 `blk/extent_types.h` 和 `blk/allocator.h`。

## 8. 文件

| 文件 | 角色 |
| --- | --- |
| `bluestore/freelist_manager.h` | `FreelistManager` 抽象基类 + 工厂方法 |
| `bluestore/bitmap_freelist_manager.h` | `BitmapFreelistManager` 声明 |
| `bluestore/bitmap_freelist_manager.cc` | `BitmapFreelistManager` 实现 |

## 9. 参考

- Ceph source: `src/os/bluestore/FreelistManager.h` / `.cc`
- Ceph source: `src/os/bluestore/BitmapFreelistManager.h` / `.cc`
- Ceph source: `src/os/bluestore/BlueStore.cc`（`_open_fm`、`_txc_finalize_kv`、`_init_alloc`、`_close_fm`）
- 本项目 [docs/design/overview.md](overview.md): 架构总览
- 本项目 [docs/design/keyvalue-db.md](keyvalue-db.md): KV 层设计文档
- 本项目 [docs/design/allocator.md](allocator.md): Allocator 设计
- 本项目 `bluestore/freelist_manager.h`: FreelistManager 抽象接口
- 本项目 `bluestore/bitmap_freelist_manager.h`: BitmapFreelistManager 声明
- 本项目 `kv/merge_op/xor_merge_op.h`: XorMergeOperator
