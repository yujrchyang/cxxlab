# 开发计划

## 项目目标

Implement BlueStore 引擎 (BlueFS + BlueRocksEnv + BlueStore) for cxxlab, modeled after Ceph's `src/os/bluestore/*`, layered on top of existing kv/ (RocksDBStore) and bluestore/ (FreelistManager + Allocator) infrastructure. BlueFS 已拆分为独立 `libbluefs.so`。

开发顺序：BlueFS → BlueRocksEnv → Throttle → BlueStore → BTier，每阶段按内部依赖细分子步骤，每步可独立测试。

> BTier 是独立的分层存储引擎，与 BlueStore 并行开发。详细设计见 [docs/design/btier.md](design/btier.md)。

---

## 阶段一：BlueFS [✅]

### 1.1 bluefs_types（纯数据结构）[✅]

| 文件 | 内容 |
| --- | --- |
| `bluefs_types.h/cc` | `bluefs_extent_t`、`bluefs_fnode_t`、`bluefs_super_t`、`bluefs_transaction_t` + DENC 序列化 |

- 纯数据结构，无外部依赖
- 测试: DENC encode/decode roundtrip，验证 `fnode_t::make_delta()` / `append_extent()` / `recalc_allocated()`

### 1.2 BlueFSConfig + VolumeSelector [✅]

| 文件 | 内容 |
| --- | --- |
| `bluefs_config.h` | `BlueFSConfig` 结构体，默认值 + `load_from_file()` |
| `bluefs_volume_selector.h/cc` | `BlueFSVolumeSelector` 抽象基类 + `RocksDBBlueFSVolumeSelector` 实现 |

- 逻辑层，无需块设备
- 测试: 构造不同容量组合，验证 `select_prefer_bdev()` 在 DB 满时正确 spill 到 Slow/WAL

### 1.3 设备层 + 超级块 [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `add_block_device()`、`add_shared_device()`、`_init_alloc()`、`_write_super()`、`_open_super()` |

- 依赖: Allocator（已完成）、BlockDevice（已完成）
- 测试: 在临时文件上写超级块、读回验证 CRC32

### 1.4 mkfs + mount（核心框架）[✅]

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `mkfs()`、`mount()`、`umount()`，日志重放逻辑 `_replay()` |

- 依赖: 1.3
- 测试: mkfs → mount → umount，验证 log 文件 ino=1 正确创建、OP_INIT 可重放

### 1.5 目录操作 [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `mkdir()`、`rmdir()`、`lookup()` |

- 依赖: 1.4
- 测试: 创建目录 → 列出所有目录 → 删除 → 验证不存在

### 1.6 文件创建/关闭 [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `open_for_write()`、`open_for_read()`、`close_writer()`、`close_reader()` |

- 依赖: 1.5
- 测试: 创建文件 → 打开读句柄 → 关闭 → 验证 inode 正确

### 1.7 文件读写 [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `append_try_flush()`、`_flush_F()`、`_flush_data()`、`flush()`、`fsync()`、`read()`、`read_random()` |

- 依赖: 1.6 + BlockDevice AIO 接口
- 测试: 写入数据 → fsync → 读回比较 → 随机读验证

### 1.8 日志持久化 [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `_signal_dirty_to_log()`、`_flush_and_sync_log()`、`_consume_dirty()`、`_clear_dirty_set_stable()`、`_release_pending_allocations()` |

- 依赖: 1.7
- 测试: 写入文件 → fsync → umount → mount → 验证文件内容和元数据恢复

### 1.9 空间分配 [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `_allocate()`、`_maybe_extend_log()`、设备回退链 WAL→DB→Slow、shared_alloc 集成 |

- 依赖: 1.8 + Allocator
- 测试: 分配耗尽 WAL → 验证自动回退到 DB；共享设备分配验证 `bluefs_used` 计数

### 1.10 异步日志压缩 [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `_compact_log_async()`、`_compact_log_dump_metadata()`、`_maybe_compact_log()` |

- 依赖: 1.9
- 测试: 反复写入产生大量日志 → 触发压缩 → 验证压缩后日志可正确重放 → 旧空间已释放

### 1.11 文件管理 + 边界 [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `truncate()`、`unlink()`、`rename()`、`stat()`、`get_total()`、`get_free()` |

- 依赖: 1.8
- 测试: 文件创建 → 改名 → 查询 stat → 删除 → 边界情况（空文件、大文件、重名等）

---

## 阶段二：BlueRocksEnv [✅]

### 2.1 辅助函数 [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueRocksEnv.h/cc` | `err_to_status()`、`split()` |

- 无依赖
- 测试: 各种路径字符串解析、错误码转换

### 2.2 SequentialFile [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueRocksEnv.h/cc` | `BlueRocksSequentialFile`（内部类）+ `NewSequentialFile()` |

- 依赖: BlueFS 文件读接口
- 测试: 通过 BlueFS 写入文件 → 通过 Env `NewSequentialFile` + `Read()/Skip()` 读取 → 验证

### 2.3 RandomAccessFile [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueRocksEnv.h/cc` | `BlueRocksRandomAccessFile`（内部类）+ `NewRandomAccessFile()` |

- 依赖: BlueFS `read_random()`
- 测试: 写入多块数据 → `Read(offset)` 随机位置读取 → `GetUniqueId()` 验证

### 2.4 WritableFile [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueRocksEnv.h/cc` | `BlueRocksWritableFile`（内部类）+ `NewWritableFile()`、`ReuseWritableFile()` |

- 依赖: BlueFS 文件写接口
- 测试: `Append()` → `Sync()` → `Close()` → 通过 BlueFS 读回验证 → `GetFileSize()` 正确性

### 2.5 目录 + 文件状态操作 [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueRocksEnv.h/cc` | `NewDirectory()`、`FileExists()`、`GetChildren()`、`DeleteFile()`、`CreateDir()`、`DeleteDir()`、`GetFileSize()`、`RenameFile()`、`LockFile()`、`UnlockFile()` |

- 依赖: BlueFS 目录操作
- 测试: 完整目录/文件生命周期：创建目录 → 创建文件 → 查询存在 → 获取子项 → 改名 → 删除

### 2.6 Logger [✅]

| 文件 | 内容 |
| --- | --- |
| `RocksDBStore.h/cc` 或独立文件 | `CephRocksdbLogger` + `NewLogger()` |

- 无 BlueFS 依赖
- 测试: 构造 Logger，写入日志消息，验证输出

### 2.7 BlueRocksEnv 集成测试 [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueRocksEnv.h/cc` | `BlueRocksEnv` 完整实现 + 路径分发逻辑（绝对路径逃逸） |

- 依赖: 2.2–2.6
- 测试: 使用 `EnvMirror` 同时验证 BlueRocksEnv 与 POSIX Env 行为一致

---

## 阶段 2.5：Throttle 基础库 [✅]

### 2.5.1 Throttle 核心实现 [已完成]

| 文件 | 内容 |
| --- | --- |
| `common/throttle.h/cc` | `Throttle` 类：`get()`、`get_or_fail()`、`try_get()`、`take()`、`put()`、`reset_max()`、`reset()` |

- 无外部依赖，纯 std::mutex + std::condition_variable
- 测试: 并发获取/释放、超时机制、上限保护、资源耗尽场景

### 2.5.2 Throttle 集成验证 [已完成]

| 文件 | 内容 |
| --- | --- |
| `tests/common/test_throttle.cc` | 多线程压测：100 线程并发获取、超时等待、资源释放 |

- 依赖: 2.5.1
- 测试: 高并发场景下无死锁、无资源泄漏、超时正确触发

---

## 阶段三：BlueStore 核心引擎

> 功能裁剪：201 项 → 104 项 MVP (52%) + 41 项 P1 + 28 项 P2 + 28 项 Deferred
> 详细分析见 [design/bluestore.md](design/bluestore.md) §8-§11

### 3.1 bluestore_types（纯数据结构）[MVP]

| 文件 | 内容 |
| --- | --- |
| `bluestore_types.h/cc` | `bluestore_pextent_t`、`bluestore_blob_t`、`bluestore_onode_t`、`bluestore_cnode_t` + DENC 序列化 |

- 无外部依赖
- 测试: 各类型 DENC encode/decode roundtrip，`blob_t::map()` / `verify_csum()` / `allocated()` / `split()`

### 3.2 BlueStoreConfig [MVP]

| 文件 | 内容 |
| --- | --- |
| `bluestore_config.h` | `BlueStoreConfig` 结构体，默认值 + `load_from_file()` |

### 3.3 Onode key 编码 [MVP]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_key_encode_prefix()`、`_key_encode_object()`、`_key_decode_object()`、`append_escaped()` |

- 依赖: 3.1
- 测试: encode/decode roundtrip，排序一致性验证

### 3.4 Blob 内存管理 [MVP]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `Blob` 类、`bluestore_blob_use_tracker_t` |

- 依赖: 3.1
- 测试: Blob 创建、`split()`、`get_ref()`/`put_ref()`、`used_in_blob` 引用追踪

### 3.5 ExtentMap [MVP]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `ExtentMap`、`Extent`、`seek_lextent()`、`punch_hole()`、`add()`、`rm()`、`compress_extent_map()`、`needs_reshard()` |

- 依赖: 3.4
- 测试: 插入 extent → 按偏移查找 → 打孔 → 删除 → 重新映射

### 3.6 Onode + Collection（内存 + KV）[MVP]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `Onode`、`Collection`、`get_onode()`、`write_onode()` 到 KV、shard index 管理 |

- 依赖: 3.3 + 3.5 + KV (RocksDBStore)
- 测试: Onode 编码 → KV 读写 → 解码验证 → shard 分片加载/保存

### 3.7 mkfs + mount [MVP]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `mkfs()`、`mount()`、`_open_db_and_around()`、`_open_collections()`、`_read_super_meta()` |

- 依赖: 3.6 + FreelistManager + Allocator + BlockDevice + BlueFS（先 mount BlueFS）
- 测试: mkfs → mount → 验证超级块、collections、allocator 状态正确

### 3.8 TransContext + OpSequencer [MVP]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `TransContext` 状态机、`OpSequencer`、`queue_transactions()`、`_txc_state_proc()` |

- 依赖: 3.7
- 测试: 创建 TransContext → 状态推进 → OpSequencer 顺序保证

### 3.9 Small Write 路径 [MVP]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_do_write()`、`_choose_write_options()`、`_do_write_small()` |

- 依赖: 3.8 + ExtentMap + Allocator
- 测试: 写入 1 个 AU 内数据 → 验证 extent 正确 → 覆盖已有 extent → 验证旧 extent 进入 released

### 3.10 Big Write 路径 [MVP]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_do_write_big()`、`_do_alloc_write()`、`_wctx_finish()` |

- 依赖: 3.9
- 测试: 写入多 AU 对齐数据 → 验证 blob 的 physical extents → 校验和正确

### 3.11 读取路径 [MVP]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_do_read()`、`_read_cache()`、`_prepare_read_ioc()`、`_generate_read_result_bl()` |

- 依赖: 3.10 + ExtentMap fault_range + BlockDevice 读
- 测试: 写入 → 读取 → 比较数据 → 校验和验证 → 部分读取（offset/length 不对齐）

### 3.12 KV 提交管道 [MVP]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_txc_write_nodes()`、`_txc_finalize_kv()`、`kv_sync_thread()`、`kv_finalize_thread()`、`_txc_finish()`、`_txc_release_alloc()` |

- 依赖: 3.11
- 测试: 完整写入事务 → KV 提交 → sync → finalize → alloc release 全路径

### 3.13 Zero + Remove + Attrs [MVP]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_do_zero()`、`_do_remove()`、`_do_setattrs()`、`_do_getattrs()` |

- 依赖: 3.12
- 测试: 写入 → zero 部分区域 → 读取验证 → 删除 object → 验证空间释放

### 3.14 Collection List [MVP]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_collection_list()`、`get_coll_range()` |

- 依赖: 3.7 + KV iterator
- 测试: 创建多个 object → 按范围分页列出 → 验证结果

### 3.15 Deferred Write [MVP]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `DeferredWriteQueue`、`submit_deferred()`、`process_deferred()`、状态机扩展（DEFERRED_QUEUED/CLEANUP/DONE） |

- 依赖: 3.12 (KV pipeline) + 3.9 (Small Write)
- 测试: 小写触发延迟写 → 批量合并 → 刷盘 → 崩溃恢复重放

### 3.16 OMap（对象级 key-value）[P1]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `omap_get()`、`omap_set()`、`omap_rmkeys()`、`omap_get_values()`、`omap_check_keys()` 等 11 个操作 |

- 依赖: 3.6 (Onode + Collection) + KV
- 测试: set/get/rmkeys 基本操作 → 迭代器遍历 → 边界条件（空 key、超长 key）

### 3.17 FSCK（文件系统检查）[P1]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `fsck()` (SHALLOW/REGULAR/DEEP)、`repair()`、`quick_fix()`、`BlueStoreRepairer`、`StoreSpaceTracker` |

- 依赖: 3.7 (mkfs/mount) + 3.5 (ExtentMap) + 3.13 (Zero/Remove)
- 测试: 构造损坏元数据 → fsck 检测 → repair 修复 → 验证一致性

### 3.18 Buffer Cache [P1]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `BufferCache`、`OnodeCache`、LRU 淘汰策略、`_read_cache()`、`_write_cache()` |

- 依赖: 3.11 (Read) + 3.9/3.10 (Write)
- 测试: 读取命中缓存 → 写入更新缓存 → LRU 淘汰验证 → 并发访问正确性

### 3.19 完整集成测试 [MVP]

| 文件 | 内容 |
| --- | --- |
| `test_bluestore.cc` | 全路径场景：mkfs → mount → 多次写入 → 读取 → zero → remove → collection list → umount → mount → 验证持久化 |

- 依赖: 所有
- 测试: 压力测试、崩溃恢复、边界条件

---

## 依赖图概览

```plaintext
阶段一: BlueFS
  1.1 bluefs_types ──────────────────────────────────────────────
  1.2 Config + VolumeSelector                                    │
  1.3 Device + Superblock ─── Allocator, BlockDevice (已完成)    │
  1.4 mkfs + mount ◄─────────────────────────────────────────────┤
  1.5 Directory ops ◄────────────────────────────────────────────┤
  1.6 File create/close ◄────────────────────────────────────────┤
  1.7 File read/write ◄──────────────────────────────────────────┤
  1.8 Log persistence ◄──────────────────────────────────────────┤
  1.9 Space allocation ◄─────────────────────────────────────────┤
  1.10 Async compaction ◄────────────────────────────────────────┤
  1.11 File mgmt + edge ◄────────────────────────────────────────┘

阶段二: BlueRocksEnv
  2.1 Helpers (split, err_to_status) ──── 无依赖
  2.2 SequentialFile ──── BlueFS read
  2.3 RandomAccessFile ──── BlueFS read_random
  2.4 WritableFile ──── BlueFS write
  2.5 Dir + Status ops ──── BlueFS dir/file ops
  2.6 Logger ──── 无 BlueFS 依赖
  2.7 BlueRocksEnv 集成 ──── 2.2–2.6

阶段 2.5: Throttle
  2.5.1 Throttle 核心实现 ──── 无依赖
  2.5.2 集成验证 ──── 2.5.1

阶段三: BlueStore
  3.1 bluestore_types ─────────────────────────────────────────
  3.2 Config                                                    │
  3.3 Onode key encoding ─── KV                                │
  3.4 Blob (内存)                                            │
  3.5 ExtentMap ◄──────────────────────────────────────────────┤
  3.6 Onode + Collection ◄────────── KV (RocksDBStore 已完成)  │
  3.7 mkfs + mount ◄────────── FM + Alloc + BlockDev + BlueFS  │
  3.8 TransContext + OpSequencer ◄─────────────────────────────┤
  3.9 Small Write ◄────────────────────────────────────────────┤
  3.10 Big Write ◄─────────────────────────────────────────────┤
  3.11 Read ◄──────────────────────────────────────────────────┤
  3.12 KV pipeline ◄───────────────────────────────────────────┤
  3.13 Zero + Remove + Attrs ◄─────────────────────────────────┤
  3.14 Collection List ◄───────────────────────────────────────┤
  3.15 Deferred Write ◄────────── 3.12 + 3.9                   │
  3.16 OMap [P1] ◄────────── 3.6 + KV                          │
  3.17 FSCK [P1] ◄────────── 3.7 + 3.5 + 3.13                 │
  3.18 Buffer Cache [P1] ◄────────── 3.11 + 3.9/3.10           │
  3.19 集成测试 ◄──────────────────────────────────────────────┘
```

---

## 阶段四：BTier（分层存储引擎）[✅]

> BTier 是独立的块级分层存储引擎，不依赖 BlueStore/kv/RocksDB。详细设计见 [design/btier.md](design/btier.md)。
> 开发顺序：A (I/O 路径) → B (双层 + 评分) → C1 (迁移) → C2 (压缩 + 集成)

### 阶段 A：核心 I/O 路径

| 步骤 | 文件 | 实现内容 | 依赖 |
| --- | --- | --- | --- |
| A1 | `btier/btier_types.h` | `Tier`、`DiskLocation`、`ExtentMetrics`、`ExtentHeader`(4KB+CRC)、`KeyLocation`、`IoOp` | common/denc.h |
| A2 | `btier/config.h/cc` | `WeightSet`、`BtierConfig` + JSON load/save | 自包含 JSON parser |
| A3 | `btier/extent_map.h/cc` | `ExtentEntry`、`ExtentMap` 基础（单层 + 生命周期 + deferred-free） | A1 + blk/ |
| A4 | `btier/extent_map.h/cc` | 多键 packing（`append_slot` + `mark_dead_slot` + `record_io` CAS） | A3 |
| A5 | `btier/key_map.h/cc` | `KeyMap`（key→extent + 反向索引 + stride tracking） | A1 |
| A6 | `btier/journal.h/cc` | `Journal`（WAL 事务 + checkpoint + recover + 循环缓冲区） | A1 + blk/ |
| A7 | `btier/btier.h/cc` + CMake | `BtierEngine`（init/recover/put/get/del/sync/shutdown） | A1–A6 |

### 阶段 B：双层分配 + 评分

| 步骤 | 文件 | 实现内容 | 依赖 |
| --- | --- | --- | --- |
| B1 | `btier/extent_map.h/cc` | 双层分配（FAST→SLOW fallback）+ `fast_watermark()` | A7 |
| B2 | `btier/scoring_engine.h/cc` | `ScoringEngine`（4D 公式 + 权重自适应） | A2 |
| B3 | `btier/btier.cc` | 评分集成 + randomness refresh | B1 + B2 |

### 阶段 C1：迁移

| 步骤 | 文件 | 实现内容 | 依赖 |
| --- | --- | --- | --- |
| C1.1 | `btier/extent_map.h/cc` | `MigrationHandle` 协议（begin/commit/abort/check） | B3 |
| C1.2 | `btier/migration_engine.h/cc` | `MigrationEngine`（migrate_tier + 后台线程 + main_loop） | C1.1 |
| C1.3 | `btier/btier.cc` | 评分驱动迁移集成 | C1.2 |
| C1.4 | `btier/btier_observer.h/cc` | 可观测性（spdlog + stats + trace） | C1.3 |

### 阶段 C2：压缩 + 集成

| 步骤 | 文件 | 实现内容 | 依赖 |
| --- | --- | --- | --- |
| C2.1 | `btier/migration_engine.h/cc` | `compact()`（copy live data + batch_update KeyMap + free old） | C1.4 |
| C2.2 | `tests/btier/test_e2e.cc` | 端到端集成测试 | C2.1 |

### 依赖图

```plaintext
阶段 A: I/O 路径
  A1 btier_types.h ──────────────────────────────────────────
  A2 config.h/cc ──── 无依赖（与 A1 并行）                    │
  A3 extent_map 基础 ─── A1 + blk/                             │
  A4 extent_map packing ─── A3                                 │
  A5 key_map ─── A1                                           │
  A6 journal ─── A1 + blk/                                    │
  A7 btier + CMake + 测试 ─── A1–A6 ──────────────────────────┤

阶段 B: 双层 + 评分
  B1 extent_map 双层 ─── A7                                    │
  B2 scoring_engine ─── A2                                    │
  B3 评分集成 ─── B1 + B2 ────────────────────────────────────┤

阶段 C1: 迁移
  C1.1 MigrationHandle ─── B3                                  │
  C1.2 migration_engine ─── C1.1                             │
  C1.3 集成 ─── C1.2                                          │
  C1.4 observer ─── C1.3 ─────────────────────────────────────┤

阶段 C2: 压缩 + 集成
  C2.1 compact() ─── C1.4                                      │
  C2.2 端到端测试 ─── C2.1 ──────────────────────────────────┘
```

---

## 已完成实现记录

- RocksDBStore implementation:
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
  - `delete_range_threshold`: threshold-based small-range optimization (iterate + per-key Delete with SavePoint/Rollback, fallback to DeleteRange)
- MemDB implementation:
  - `MDBTransactionImpl::Op`: refactored with explicit `end` field for `rm_range_keys` (removed reuse of `value`)
  - `_merge()`: uses `if (!mop) return -ENOENT;` for null merge operator check
  - Iterator invalidation: `uint64_t seqno_` incremented on every mutation; `MDBWholeSpaceIteratorImpl` detects stale seqno on each seek/lower_bound/upper_bound and rebuilds snapshot from `std::map` under lock
  - `submit_transaction_sync`: explicitly overridden to call `submit_transaction` (sync == async for in-memory)
- PrefixIteratorImpl:
  - Removed `skip_to_next_valid()`/`skip_to_prev_valid()` direction-check bug
  - Constructs `seek_lower_bound_`/`seek_upper_bound_` from prefix + bounds and passes to underlying iterator via `set_iterate_lower_bound` / `set_iterate_upper_bound`
- Base interface (`kv/key_value_db.h`):
  - `open_read_only`, `repair`, `compact_prefix`, `compact_prefix_async`, `compact_range`, `compact_range_async` added as virtual-with-default
  - `WholeSpaceIteratorImpl`: added `set_iterate_lower_bound(const std::string*)` / `set_iterate_upper_bound(const std::string*)` with protected `iterate_{lower,upper}_bound_` pointers
  - `key_size()` / `value_size()` added to `WholeSpaceIteratorImpl`
  - `make_iterator` now takes `IteratorBounds` parameter
  - `set_merge_operator` is no longer pure virtual (has default storage in `merge_ops_`)
  - `get_merge_ops()` protected accessor for subclasses
  - `submit_transaction_sync` changed to pure virtual (forces explicit sync semantics per backend)
  - `encode_key` / `decode_key` centralized as public static methods (previously duplicated in RocksDBStore + MemDB + PrefixIteratorImpl)
- Tests: Split `test_librocksdb.cc` (23 raw RocksDB tests) from `test_rocksdb.cc` (25 RocksDBStore tests); 36 MemDB tests in `test_memdb.cc`; all 84 pass
- Ceph evaluation: Compared `src/kv/*` (KeyValueDB, RocksDBStore, MemDB) and `src/os/bluestore/*` (KV usage patterns), identified 12 improvement items in 8 categories — all resolved
- Allocator implementation:
  - `Allocator` abstract base with `create()` factory, `init_add_free()`, `allocate()`, `release()`, `get_fragmentation()`, `get_alloc_stats()` interface
  - `AvlAllocator`: interval-tree (AVL) based via `range_seg_tree_t`, exact match allocation, `_spillover_range()` cap mechanism
  - `BitmapAllocator`: 2-level bitmap (`bdev_block_count` x `bitmap_granularity`), `ffs`/`ffz` scan, affinity hint via arena-weighted round-robin; Pimpl pattern hides `AllocatorLevel01Loose`/`AllocatorLevel02` internals from public header
  - `HybridAllocator`: wraps AvlAllocator + BitmapAllocator child, `_add_to_tree()` override to claim-free adjacent extents from bitmap before AVL insert
  - `Allocator::create()` factory with `"stupid"` (AvlAllocator), `"bitmap"`, `"hybrid"` type strings
- Tests: 18 AvlAllocator tests, 23 BitmapAllocator tests, 17 HybridAllocator tests — all pass
- BlueFS Phase 1.1-1.11 (data structures through file management + edge cases): 64 tests total
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
  - Code review fixes (Phase 1.11 completion): `lock_file`/`unlock_file`/`invalidate_cache`/`flush_range`/`preallocate`/`get_used` all implemented; `OP_DIR_UNLINK` replay assertion (refs>0); `OP_FILE_UPDATE_INC` delta offset validation; truncate uses `op_file_update` instead of `op_file_update_inc`; `_flush_data` reverted to direct buffer write
- BlueRocksEnv Phase 2.1-2.7 implementation: 29 tests
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
- Throttle implementation: 20 tests
  - `common/throttle.h` / `common/throttle.cc`: Generic resource rate limiting with FIFO fair queuing
  - Per-waiter condition variable pattern (from Ceph Throttle), oversized request handling, timeout support
- BTier implementation: 118 tests total (all phases A-C2)
  - `btier_types.h`: `Tier`, `DiskLocation`, `ExtentMetrics`, `ExtentHeader` (4KB+CRC), `KeyLocation`, `IoOp`, `MigrationStats`
  - `BtierConfig` + JSON load/save (self-contained parser, no external JSON dependency)
  - `ExtentMap`: single + dual-tier allocation, multi-key packing, deferred-free, migration handle protocol, `refresh_randomness()`
  - `KeyMap`: key→extent mapping, reverse index, stride tracking
  - `Journal`: WAL transactions, checkpoint, recover, circular buffer
  - `ScoringEngine`: 4D formula (recency/frequency/randomness/write-penalty) + weight adaptation
  - `MigrationEngine`: migrate_tier + compact + background thread
  - `BtierObserver`: spdlog + stats + trace
  - `recover_internal`: single-pass replay with switch dispatch (was 5-pass)

## 设计决策

- Single default ColumnFamily (no hash sharding, no `parse_sharding_def`)
- `set_merge_operator` must be called before `open()` / `create_and_open()` for RocksDBStore
- `close()` null-checks `db_` before delete, sets to `nullptr`, resets adapter
- DeleteRange threshold: configurable via `delete_range_threshold`, default 0 → always use DeleteRange; threshold > 0 → small ranges use per-key Delete with SavePoint, large ranges fallback to DeleteRange
- MemDB iterator invalidation: seqno-based, automatically rebuilds snapshot on detection of concurrent writes
- Iterator bounds two-layer separation: `WholeSpaceIteratorImpl` stores `const std::string*`, backend converts to native type (`rocksdb::Slice*`) at seek time
- BitmapFreelistManager: `create()` allocates block 0 at mkfs time (caller must not double-allocate)
- Bitmap key encoding: 8 bytes big-endian uint64_t (memcmp-compatible, matches Ceph `_key_encode_u64`)
- BitmapFreelistManager compiles into `libbluestore.so` (SHARED)
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
- Allocator extracted from `bluestore/` to `blk/` (Phase 0 refactoring): Allocator is pure memory management with no dependency on RocksDB or FreelistManager. `blk/extent_types.h` created with `pextent_t`, `PExtentVector`. `common/interval_set.h` extracted as separate file. `bluestore/bluestore_types.h` now a thin wrapper. Allocator tests moved to `tests/blk/`. `bluestore` now links `blk` (PUBLIC).
- BlueStore 功能裁剪 (Phase 3 评审): 201 个功能点分 4 优先级 — 104 项 MVP (52%) + 41 项 P1 + 28 项 P2 + 28 项 Deferred。详见 `docs/design/bluestore.md`
- Deferred Write 纳入 MVP: 小写性能关键路径，状态机从 8 态恢复为完整 11 态
- FSCK 全功能纳入 P1: 数据安全关键，含 SHALLOW/REGULAR/DEEP 三级检查 + repair + quick_fix
- Throttle 提取到 `common/`: 通用限流基础库（Phase 2.5），不绑定 BlueStore，供 BTier/kv 等组件复用
- OMap 纳入 P1: 对象级 key-value 存储，11 个操作方法
- BitmapAllocator Pimpl: `AllocatorLevel01Loose`/`AllocatorLevel02` 移入 .cc，header 从 166→52 行，bitmap 内部实现不再泄漏给 includer
- btier stats 统一: `MigrationStats` 定义在 `btier_types.h`，`MigrationEngine`/`BtierEngine` 共用，消除三重定义 + 字段级拷贝
- btier recover_internal 单次遍历: 5 次 for 循环合并为 1 次 for + switch，op 处理顺序不变

## 下一步

1. Phase 3.1: bluestore_types (bluestore_pextent_t, bluestore_blob_t, bluestore_onode_t, bluestore_cnode_t + DENC)
2. Phase 3.2: BlueStoreConfig struct init + file load
