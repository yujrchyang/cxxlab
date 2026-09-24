# cxxlab 架构总览

> 实现状态：持续更新（Phase 1–2 已完成，Phase 3 进行中，Phase 4 已完成）

## 1. 系统分层

```plaintext
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                        │
│  examples/                                                  │
├─────────────────────────────────────────────────────────────┤
│                    Storage Engine Layer                     │
│  ┌──────────────────┐  ┌─────────────────────────────────┐  │
│  │  BlueStore       │  │  BTier                          │  │
│  │  (Object Store)  │  │  (Tiered Store)                 │  │
│  │  bluestore/      │  │  btier/                         │  │
│  └────────┬─────────┘  └─────────────┬───────────────────┘  │
├───────────┼──────────────────────────┼──────────────────────┤
│           │   Filesystem + KV Layer  │                      │
│  ┌────────▼─────────┐  ┌─────────────▼──────────┐           │
│  │  bluefs          │  │  kv                     │           │
│  │  bluefs/         │  │  kv/                    │           │
│  └────────┬─────────┘  └────────────┬───────────┘           │
├───────────┼─────────────────────────┼───────────────────────┤
│           │     Block + IO Layer    │                       │
│  ┌────────▼─────────────────────────▼──────────┐            │
│  │  blk (dev + alloc)                          │            │
│  │  blk/                                       │            │
│  └────────────────┬────────────────────────────┘            │
│                   │                                         │
│  ┌────────────────▼────────────────────────────┐            │
│  │  common (bufferlist, denc, crc32, etc.)     │            │
│  │  common/                                    │            │
│  └─────────────────────────────────────────────┘            │
└─────────────────────────────────────────────────────────────┘
```

## 2. 模块依赖关系

```plaintext
common  ← 基础库 (bufferlist, denc, crc32, uuid, intarith, ...)
  │
  ├── blk  ← 块设备 + 空间分配器
  │     ├── BlockDevice / KernelDevice (libaio)
  │     ├── Allocator (Avl / Bitmap / Hybrid)
  │     └── IOContext / aio_t / aio_queue_t
  │
  ├── kv  ← 键值存储抽象层
  │     ├── KeyValueDB (抽象基类)
  │     ├── RocksDBStore (生产后端)
  │     ├── MemDB (调试后端)
  │     └── MergeOperator (XOR / Int64Array)
  │
  ├── bluefs  ← 用户态文件系统 (独立库)
  │     ├── BlueFS (mkfs/mount/dir/file/journal)
  │     ├── bluefs_types / bluefs_config / bluefs_volume_selector
  │     └── 无 kv/RocksDB 依赖，可独立复用
  │
  ├── bluestore  ← BlueStore 引擎 (含 FM + BlueRocksEnv)
  │     ├── FreelistManager / BitmapFreelistManager
  │     ├── BlueRocksEnv (rocksdb::Env 适配)
  │     └── bluestore_types (pextent, blob, onode, cnode)
  │
  └── btier  ← 分层存储引擎
        ├── BtierEngine (公共 API)
        ├── ExtentMap / KeyMap
        ├── ScoringEngine / MigrationEngine
        └── Journal (WAL)
```

### 编译依赖

| 库 | 依赖 (PUBLIC) | 依赖 (PRIVATE) | 构建产物 |
| ------ | -------------- | ---------------- | --------- |
| `common` | — | — | libcommon.so |
| `blk` | common, aio | — | libblk.so |
| `kv` | common | RocksDB | libkv.so |
| `bluefs` | common, blk | — | libbluefs.so |
| `bluestore` | common, kv, blk, bluefs | RocksDB | libbluestore.so |
| `btier` | common, blk | — | libbtier.so |

> bluefs 独立于 kv/RocksDB，任何基于 RocksDB 的应用可单独链接 libbluefs.so 获得用户态文件系统支撑。
>
> bluestore 不依赖 btier，btier 不依赖 bluestore/kv/bluefs。两者是平行的存储引擎。
>
> RocksDB 依赖说明： `kv` 和 `bluestore` 库均将 RocksDB 设为 PRIVATE。`kv` 仅 `RocksDBStore` 实现内部使用 RocksDB。`bluestore` 仅 `BlueRocksEnv` 实现内部使用 RocksDB，CMake 链接层面将 `RocksDB::RocksDB` 设为 PRIVATE，不向下游 target 传递链接依赖；但 `blue_rocks_env.h` 公开继承 `rocksdb::EnvWrapper` 并直接引入 RocksDB 头文件，头文件层面仍暴露 RocksDB 类型。
>
> FreelistManager 保留在 `bluestore` 库中（不迁入 `blk`），因为其实现本质是 KV 操作（bitmap XOR merge），属于引擎级持久化策略而非块设备原语。

## 3. 模块职责

| 模块 | 职责 | 设计文档 |
| ------ | ------ | --------- |
| common | bufferlist、DENC 序列化、CRC32、UUID、工具函数 | [common.md](common.md) |
| blk | 块设备抽象 (KernelDevice + libaio)、空间分配器 (Avl/Bitmap/Hybrid) | [block-device.md](block-device.md)、[allocator.md](allocator.md) |
| kv | KV 存储抽象层 (RocksDBStore + MemDB)、MergeOperator | [keyvalue-db.md](keyvalue-db.md) |
| bluefs | 用户态文件系统（mkfs/mount/dir/file/journal/log compaction） | [bluefs.md](bluefs.md) |
| bluestore | FreelistManager、BlueRocksEnv、BlueStore 类型定义与引擎 | [freelist-manager.md](freelist-manager.md)、[blue-rocks-env.md](blue-rocks-env.md)、[bluestore.md](bluestore.md) |
| btier | 分层存储引擎 (评分、迁移、压缩、WAL) | [btier.md](btier.md) |

## 4. KV 前缀定义

BlueStore 使用 RocksDB 存储元数据，不同类型的数据存储在不同的单字符前缀下。完整的前缀定义表（含常量名、使用者、Key 格式、实现状态）见 [keyvalue-db.md](keyvalue-db.md) §1.1。

## 5. 数据流路径

### 5.1 BlueStore 写路径

```plaintext
queue_transactions()
  → TransContext (STATE_PREPARE)
  → _do_write() → alloc->allocate() [blk] → bdev->aio_write() [blk]
  → STATE_AIO_WAIT → STATE_IO_DONE
  → _txc_finalize_kv() → fm->allocate/release() [bluestore] → db->submit_transaction() [kv]
  → STATE_KV_QUEUED → kv_sync_thread → STATE_KV_DONE
  → kv_finalize_thread → _txc_release_alloc() → alloc->release() [blk]
  → STATE_DONE
```

### 5.2 BlueFS 写路径

```plaintext
append_try_flush()
  → buffer 累积 → _flush_F() → bdev->aio_write() [blk]
  → fsync() → _flush_and_sync_log()
  → _consume_dirty() → log_.t (OP_FILE_UPDATE_INC)
  → _flush_and_sync_log_core() → log writer → bdev->aio_write() [blk]
  → _release_pending_allocations() → alloc->release() [blk]
```

### 5.3 BTier 写路径

```plaintext
put(key, value)
  → KeyMap::lookup() (旧 key)
  → ExtentMap::find_extent_with_space() / allocate_extent() [blk]
  → BlockDevice::write() [blk]
  → ExtentMap::record_io() (CAS, lock-free)
  → Journal::begin_txn() → append() → commit_txn() [blk write + fsync]
  → KeyMap::put() / ExtentMap::mark_dead_slot()
```

### 5.4 启动恢复路径

```plaintext
BlueStore:  bdev->open() → db->open_read_only() → _read_super_meta()
            → fm->init() → fm->enumerate_next() → alloc->init_add_free()
            → db->close() → db->open() → _open_collections()

BlueFS:     bdev->open() → _read_super() (CRC32C 校验)
            → _replay() (日志重放) → alloc->init_rm_free()

BTier:      bdev->open() → alloc->create() → Journal::recover()
            → replay records → ExtentMap rebuild → alloc->init_rm_free()
            → verify ExtentHeader CRC → MigrationEngine::start()
```

## 6. 开发计划概览

| 阶段 | 内容 | 状态 | 详细计划 |
| ------ | ------ | ------ | --------- |
| Phase 1 | BlueFS (1.1–1.11) | 已完成 | [plan.md](../plan.md) |
| Phase 2 | BlueRocksEnv (2.1–2.7) | 已完成 | [plan.md](../plan.md) |
| Phase 3 | BlueStore (3.1–3.15) | 进行中 | [plan.md](../plan.md) |
| Phase 4 | BTier (A1–A7, B1–B3, C1.1–C2.2) | 已完成 | [btier.md](btier.md) §9 |

## 7. 非功能性约束

| 维度 | 目标 | 说明 |
| ------ | ------ | ------ |
| 写延迟 | < 1ms（Direct I/O 路径） | 不含 KV sync 延迟，AIO 提交后即返回 |
| KV sync 延迟 | < 10ms | `kv_sync_thread` 单线程，批量提交事务 |
| Allocator 内存占用 | < 1% 设备大小 | AvlAllocator 节点数受 `range_count_cap` 限制；BitmapAllocator 三级位图压缩 |
| 并发模型 | 单线程 KV sync + 单线程 KV finalize | BlueStore 写路径：客户端线程准备 IO → AIO 回调 → KV 线程提交 |
| BlueFS 日志压缩 | 不阻塞前台 IO | 异步压缩 Step 2–5 释放 `log.lock`，允许并发写入 |
| BTier 迁移 | 不阻塞读写 | `MigrationHandle` 数据复制期间无锁，generation 中断协议保证一致性 |
| 持久化保证 | `fsync` 后 crash-safe | BlueFS: 数据 flush + log sync；BlueStore: AIO 完成 + KV sync；BTier: journal commit + fsync |

## 8. 架构决策记录 (ADR)

重大架构决策索引，每条关联到对应设计文档的详细说明：

| # | 决策 | 状态 | 关联文档 |
| --- | ------ | ------ | --------- |
| ADR-01 | 单 ColumnFamily，不做 hash 分片 | 已采纳 | [keyvalue-db.md](keyvalue-db.md) §4.2 |
| ADR-02 | 不实现 omap（4 个前缀标记"不实现"） | 已采纳 | [bluestore.md](bluestore.md) §8.1 |
| ADR-03 | 不实现压缩（zlib/zstd/lz4/snappy） | 已采纳 | [bluestore.md](bluestore.md) §8.1 |
| ADR-04 | 不实现 Shared Blob（无 clone/snapshot） | 已采纳 | [bluestore.md](bluestore.md) §8.1 |
| ADR-05 | 不实现 Deferred Write（初始版本全部同步 AIO） | 已采纳 | [bluestore.md](bluestore.md) §8.1 |
| ADR-06 | Null FM 有设计但不启用 | 设计完成 | [freelist-manager.md](freelist-manager.md) §6.2 |
| ADR-07 | BlueFS 使用 AvlAllocator 而非 BitmapAllocator | 已采纳 | [bluefs.md](bluefs.md) §7.2 |
| ADR-08 | Allocator 从 `bluestore/` 迁移到 `blk/`（Phase 0 重构） | 已采纳 | [allocator.md](allocator.md) §5.4 |
| ADR-09 | 不引入统一存储引擎抽象接口 | 已采纳 | — |
| ADR-10 | BlueFS 拆为独立 `libbluefs.so`，与 `libbluestore.so` 分离 | 已修订 | 见下方 §9 库边界决策 |
| ADR-11 | `bluestore` 将 RocksDB 设为 PRIVATE 依赖 | 已采纳 | 见下方 §9 库边界决策 |
| ADR-12 | FreelistManager 保留在 `libbluestore.so`，不迁入 `blk/` | 已采纳 | 见下方 §9 库边界决策 |

## 9. 库边界决策

### 9.1 BlueFS 拆为独立 `libbluefs.so` (ADR-10)

BlueFS 从 `libbluestore.so` 中拆出为独立的 `libbluefs.so`（SHARED）。BlueRocksEnv 和 FreelistManager 保留在 `libbluestore.so` 中。

拆分理由：

- BlueFS 对 kv/RocksDB **零依赖**，是真正独立的子系统
- 独立 `.so` 允许任何基于 RocksDB 的应用复用 BlueFS 作为用户态存储后端
- `bluefs_shared_alloc_context_t` 跨库传递 Allocator 指针，接口不变（通过 `add_block_device()` 参数）
- BTier 等引擎未来可复用 BlueFS（例如将 journal 放在 BlueFS 上）

BlueRocksEnv 保留在 `libbluestore.so` 的原因：

- 它是 BlueStore 的消费者（为 BlueStore 提供 `rocksdb::Env` 适配），不是 BlueFS 的消费者
- 它依赖 `rocksdb/env.h`，合入 `libbluefs.so` 会让 BlueFS 被迫依赖 RocksDB 头文件
- 代码量小（~200 行），不值得独立为第三个 `.so`

FreelistManager 保留在 `libbluestore.so` 的原因（ADR-12）：

- 其实现本质是 KV 操作（bitmap XOR merge、enumerate 扫描），属于引擎级持久化策略
- 迁入 `blk/` 会让 `libblk.so` 从纯 IO 原语层变为需要 kv 依赖的重量级库
- BTier 不使用 FreelistManager（通过 Journal 恢复分配状态），印证 FM 是引擎级而非设备级关注点

### 9.2 RocksDB 依赖隔离 (ADR-11)

`bluestore` 库将 `RocksDB::RocksDB` 设为 PRIVATE，与 `kv` 库保持一致。`BlueRocksEnv` 公开继承 `rocksdb::EnvWrapper` 并在 `blue_rocks_env.h` 中直接引入 RocksDB 头文件，因此头文件层面暴露 RocksDB 类型；但 CMake 链接层面不向下游 target 传递 `RocksDB::RocksDB` 链接依赖。

直接使用 `BlueRocksEnv` 的目标（如 `test_blue_rocks_env`）需自行链接 `RocksDB::RocksDB` 并 include `rocksdb/env.h`。

详见 [blue-rocks-env.md](blue-rocks-env.md) §10。

### 9.3 不引入统一存储引擎接口 (ADR-09)

BlueStore 和 BTier 提供不同语义（对象存储 vs KV 存储），且 BTier 不依赖 kv/RocksDB。引入统一 `StorageEngine` 抽象基类会：

- 强制 BTier 依赖 kv 层（违反 BTier 独立性设计目标）
- 增加不必要的抽象层（当前两个引擎无互换需求）

决策：暂不引入，overview.md 记录此决策避免后续重复讨论。

## 10. 参考

- [common.md](common.md) — 基础库（bufferlist, DENC, CRC32, interval\_set）
- [keyvalue-db.md](keyvalue-db.md) — KV 存储抽象层
- [freelist-manager.md](freelist-manager.md) — 空闲空间管理器
- [allocator.md](allocator.md) — 内存级空间分配器
- [block-device.md](block-device.md) — 块设备抽象层
- [bluefs.md](bluefs.md) — BlueFS 用户态文件系统
- [blue-rocks-env.md](blue-rocks-env.md) — BlueRocksEnv RocksDB 适配层
- [bluestore.md](bluestore.md) — BlueStore 存储引擎
- [btier.md](btier.md) — BTier 分层存储引擎
- [../plan.md](../plan.md) — 开发计划
