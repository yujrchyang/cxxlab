# 开发计划

开发顺序：**BlueFS → BlueRocksEnv → BlueStore → BTier**，每阶段按内部依赖细分子步骤，每步可独立测试。

> BTier 是独立的分层存储引擎，与 BlueStore 并行开发。详细设计见 [docs/design/btier.md](design/btier.md)。

---

## 阶段一：BlueFS

### 1.1 bluefs_types（纯数据结构）

| 文件 | 内容 |
| --- | --- |
| `bluefs_types.h/cc` | `bluefs_extent_t`、`bluefs_fnode_t`、`bluefs_super_t`、`bluefs_transaction_t` + DENC 序列化 |

- 纯数据结构，无外部依赖
- **测试**: DENC encode/decode roundtrip，验证 `fnode_t::make_delta()` / `append_extent()` / `recalc_allocated()`

### 1.2 BlueFSConfig + VolumeSelector

| 文件 | 内容 |
| --- | --- |
| `bluefs_config.h` | `BlueFSConfig` 结构体，默认值 + `load_from_file()` |
| `bluefs_volume_selector.h/cc` | `BlueFSVolumeSelector` 抽象基类 + `RocksDBBlueFSVolumeSelector` 实现 |

- 逻辑层，无需块设备
- **测试**: 构造不同容量组合，验证 `select_prefer_bdev()` 在 DB 满时正确 spill 到 Slow/WAL

### 1.3 设备层 + 超级块

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `add_block_device()`、`add_shared_device()`、`_init_alloc()`、`_write_super()`、`_open_super()` |

- 依赖: Allocator（已完成）、BlockDevice（已完成）
- **测试**: 在临时文件上写超级块、读回验证 CRC32

### 1.4 mkfs + mount（核心框架）

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `mkfs()`、`mount()`、`umount()`，日志重放逻辑 `_replay()` |

- 依赖: 1.3
- **测试**: mkfs → mount → umount，验证 log 文件 ino=1 正确创建、OP_INIT 可重放

### 1.5 目录操作

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `mkdir()`、`rmdir()`、`lookup()` |

- 依赖: 1.4
- **测试**: 创建目录 → 列出所有目录 → 删除 → 验证不存在

### 1.6 文件创建/关闭

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `open_for_write()`、`open_for_read()`、`close_writer()`、`close_reader()` |

- 依赖: 1.5
- **测试**: 创建文件 → 打开读句柄 → 关闭 → 验证 inode 正确

### 1.7 文件读写

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `append_try_flush()`、`_flush_F()`、`_flush_data()`、`flush()`、`fsync()`、`read()`、`read_random()` |

- 依赖: 1.6 + BlockDevice AIO 接口
- **测试**: 写入数据 → fsync → 读回比较 → 随机读验证

### 1.8 日志持久化

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `_signal_dirty_to_log()`、`_flush_and_sync_log()`、`_consume_dirty()`、`_clear_dirty_set_stable()`、`_release_pending_allocations()` |

- 依赖: 1.7
- **测试**: 写入文件 → fsync → umount → mount → 验证文件内容和元数据恢复

### 1.9 空间分配

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `_allocate()`、`_maybe_extend_log()`、设备回退链 WAL→DB→Slow、shared_alloc 集成 |

- 依赖: 1.8 + Allocator
- **测试**: 分配耗尽 WAL → 验证自动回退到 DB；共享设备分配验证 `bluefs_used` 计数

### 1.10 异步日志压缩

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `_compact_log_async()`、`_compact_log_dump_metadata()`、`_maybe_compact_log()` |

- 依赖: 1.9
- **测试**: 反复写入产生大量日志 → 触发压缩 → 验证压缩后日志可正确重放 → 旧空间已释放

### 1.11 文件管理 + 边界

| 文件 | 内容 |
| --- | --- |
| `BlueFS.h/cc` | `truncate()`、`unlink()`、`rename()`、`stat()`、`get_total()`、`get_free()` |

- 依赖: 1.8
- **测试**: 文件创建 → 改名 → 查询 stat → 删除 → 边界情况（空文件、大文件、重名等）

---

## 阶段二：BlueRocksEnv

### 2.1 辅助函数

| 文件 | 内容 |
| --- | --- |
| `BlueRocksEnv.h/cc` | `err_to_status()`、`split()` |

- 无依赖
- **测试**: 各种路径字符串解析、错误码转换

### 2.2 SequentialFile

| 文件 | 内容 |
| --- | --- |
| `BlueRocksEnv.h/cc` | `BlueRocksSequentialFile`（内部类）+ `NewSequentialFile()` |

- 依赖: BlueFS 文件读接口
- **测试**: 通过 BlueFS 写入文件 → 通过 Env `NewSequentialFile` + `Read()/Skip()` 读取 → 验证

### 2.3 RandomAccessFile

| 文件 | 内容 |
| --- | --- |
| `BlueRocksEnv.h/cc` | `BlueRocksRandomAccessFile`（内部类）+ `NewRandomAccessFile()` |

- 依赖: BlueFS `read_random()`
- **测试**: 写入多块数据 → `Read(offset)` 随机位置读取 → `GetUniqueId()` 验证

### 2.4 WritableFile

| 文件 | 内容 |
| --- | --- |
| `BlueRocksEnv.h/cc` | `BlueRocksWritableFile`（内部类）+ `NewWritableFile()`、`ReuseWritableFile()` |

- 依赖: BlueFS 文件写接口
- **测试**: `Append()` → `Sync()` → `Close()` → 通过 BlueFS 读回验证 → `GetFileSize()` 正确性

### 2.5 目录 + 文件状态操作

| 文件 | 内容 |
| --- | --- |
| `BlueRocksEnv.h/cc` | `NewDirectory()`、`FileExists()`、`GetChildren()`、`DeleteFile()`、`CreateDir()`、`DeleteDir()`、`GetFileSize()`、`RenameFile()`、`LockFile()`、`UnlockFile()` |

- 依赖: BlueFS 目录操作
- **测试**: 完整目录/文件生命周期：创建目录 → 创建文件 → 查询存在 → 获取子项 → 改名 → 删除

### 2.6 Logger

| 文件 | 内容 |
| --- | --- |
| `RocksDBStore.h/cc` 或独立文件 | `CephRocksdbLogger` + `NewLogger()` |

- 无 BlueFS 依赖
- **测试**: 构造 Logger，写入日志消息，验证输出

### 2.7 BlueRocksEnv 集成测试

| 文件 | 内容 |
| --- | --- |
| `BlueRocksEnv.h/cc` | `BlueRocksEnv` 完整实现 + 路径分发逻辑（绝对路径逃逸） |

- 依赖: 2.2–2.6
- **测试**: 使用 `EnvMirror` 同时验证 BlueRocksEnv 与 POSIX Env 行为一致

---

## 阶段三：BlueStore 核心引擎

### 3.1 bluestore_types（纯数据结构）

| 文件 | 内容 |
| --- | --- |
| `bluestore_types.h/cc` | `bluestore_pextent_t`、`bluestore_blob_t`、`bluestore_onode_t`、`bluestore_cnode_t` + DENC 序列化 |

- 无外部依赖
- **测试**: 各类型 DENC encode/decode roundtrip，`blob_t::map()` / `verify_csum()` / `allocated()` / `split()`

### 3.2 BlueStoreConfig

| 文件 | 内容 |
| --- | --- |
| `bluestore_config.h` | `BlueStoreConfig` 结构体，默认值 + `load_from_file()` |

### 3.3 Onode key 编码

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_key_encode_prefix()`、`_key_encode_object()`、`_key_decode_object()`、`append_escaped()` |

- 依赖: 3.1
- **测试**: encode/decode roundtrip，排序一致性验证

### 3.4 Blob 内存管理

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `Blob` 类、`bluestore_blob_use_tracker_t` |

- 依赖: 3.1
- **测试**: Blob 创建、`split()`、`get_ref()`/`put_ref()`、`used_in_blob` 引用追踪

### 3.5 ExtentMap

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `ExtentMap`、`Extent`、`seek_lextent()`、`punch_hole()`、`add()`、`rm()`、`compress_extent_map()`、`needs_reshard()` |

- 依赖: 3.4
- **测试**: 插入 extent → 按偏移查找 → 打孔 → 删除 → 重新映射

### 3.6 Onode + Collection（内存 + KV）

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `Onode`、`Collection`、`get_onode()`、`write_onode()` 到 KV、shard index 管理 |

- 依赖: 3.3 + 3.5 + KV (RocksDBStore)
- **测试**: Onode 编码 → KV 读写 → 解码验证 → shard 分片加载/保存

### 3.7 mkfs + mount

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `mkfs()`、`mount()`、`_open_db_and_around()`、`_open_collections()`、`_read_super_meta()` |

- 依赖: 3.6 + FreelistManager + Allocator + BlockDevice + BlueFS（先 mount BlueFS）
- **测试**: mkfs → mount → 验证超级块、collections、allocator 状态正确

### 3.8 TransContext + OpSequencer

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `TransContext` 状态机、`OpSequencer`、`queue_transactions()`、`_txc_state_proc()` |

- 依赖: 3.7
- **测试**: 创建 TransContext → 状态推进 → OpSequencer 顺序保证

### 3.9 Small Write 路径

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_do_write()`、`_choose_write_options()`、`_do_write_small()` |

- 依赖: 3.8 + ExtentMap + Allocator
- **测试**: 写入 1 个 AU 内数据 → 验证 extent 正确 → 覆盖已有 extent → 验证旧 extent 进入 released

### 3.10 Big Write 路径

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_do_write_big()`、`_do_alloc_write()`、`_wctx_finish()` |

- 依赖: 3.9
- **测试**: 写入多 AU 对齐数据 → 验证 blob 的 physical extents → 校验和正确

### 3.11 读取路径

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_do_read()`、`_read_cache()`、`_prepare_read_ioc()`、`_generate_read_result_bl()` |

- 依赖: 3.10 + ExtentMap fault_range + BlockDevice 读
- **测试**: 写入 → 读取 → 比较数据 → 校验和验证 → 部分读取（offset/length 不对齐）

### 3.12 KV 提交管道

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_txc_write_nodes()`、`_txc_finalize_kv()`、`kv_sync_thread()`、`kv_finalize_thread()`、`_txc_finish()`、`_txc_release_alloc()` |

- 依赖: 3.11
- **测试**: 完整写入事务 → KV 提交 → sync → finalize → alloc release 全路径

### 3.13 Zero + Remove + Attrs

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_do_zero()`、`_do_remove()`、`_do_setattrs()`、`_do_getattrs()` |

- 依赖: 3.12
- **测试**: 写入 → zero 部分区域 → 读取验证 → 删除 object → 验证空间释放

### 3.14 Collection List

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_collection_list()`、`get_coll_range()` |

- 依赖: 3.7 + KV iterator
- **测试**: 创建多个 object → 按范围分页列出 → 验证结果

### 3.15 完整集成测试

| 文件 | 内容 |
| --- | --- |
| `test_bluestore.cc` | 全路径场景：mkfs → mount → 多次写入 → 读取 → zero → remove → collection list → umount → mount → 验证持久化 |

- 依赖: 所有
- **测试**: 压力测试、崩溃恢复、边界条件

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
  3.15 集成测试 ◄──────────────────────────────────────────────┘
```

---

## 阶段四：BTier（分层存储引擎）

> BTier 是独立的块级分层存储引擎，不依赖 BlueStore/kv/RocksDB。详细设计见 [design/btier.md](design/btier.md)。
> 开发顺序：**A (I/O 路径) → B (双层 + 评分) → C1 (迁移) → C2 (压缩 + 集成)**

### 阶段 A：核心 I/O 路径

| 步骤 | 文件 | 实现内容 | 依赖 |
| --- | --- | --- | --- |
| A1 | `btier/btier_types.h` | `Tier`、`DiskLocation`、`ExtentMetrics`、`ExtentHeader`(4KB+CRC)、`KeyLocation`、`IoOp` | common/denc.h |
| A2 | `btier/config.h/cc` | `WeightSet`、`BtierConfig` + JSON load/save | nlohmann/json |
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
