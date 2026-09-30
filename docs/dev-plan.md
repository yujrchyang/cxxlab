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

### 3.4 Blob 内存管理 [✅]

| 文件 | 内容 |
| --- | --- |
| `bluestore/blob.h/cc` | `Blob` 类、`bluestore_blob_use_tracker_t` |

- 依赖: 3.1
- 测试: Blob 创建、`split()`、`get_ref()`/`put_ref()`、`used_in_blob` 引用追踪

### 3.5 ExtentMap [✅]

| 文件 | 内容 |
| --- | --- |
| `bluestore/extent_map.h/cc` | `ExtentMap`、`Extent`、`seek_lextent()`、`punch_hole()`、`add()`、`rm()`、`compress_extent_map()`、`needs_reshard()` |

- 依赖: 3.4
- 测试: 插入 extent → 按偏移查找 → 打孔 → 删除 → 重新映射

### 3.6 Onode + Collection（内存 + KV）[✅]

| 文件 | 内容 |
| --- | --- |
| `bluestore/onode.h/cc` | `Onode`、`write_to_kv()`、`read_from_kv()` |
| `bluestore/collection.h/cc` | `Collection`、`OnodeSpace`、`get_onode()`、`create_onode()`、`remove_onode()` |
| `bluestore/bluestore_constants.h` | KV 前缀常量定义 |

- 依赖: 3.3 + 3.5 + KV (RocksDBStore)
- 测试: Onode 编码 → KV 读写 → 解码验证 → LRU 缓存淘汰

### 3.7 mkfs + mount [✅]

| 文件 | 内容 |
| --- | --- |
| `bluestore/bluestore.h/cc` | `mkfs()`、`mount()`、`_open_bdev()`、`_open_db()`、`_open_fm()`、`_init_alloc()`、`_open_collections()`、`_read_super_meta()` |
| `bluestore/bluestore_config.h` | `BlueStoreConfig` 结构体 |

- 依赖: 3.6 + FreelistManager + Allocator + BlockDevice
- 测试: mkfs → mount → 验证超级块、collections、allocator 状态正确

### 3.8 TransContext + OpSequencer [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `TransContext` 状态机、`OpSequencer`、`queue_transactions()`、`_txc_state_proc()` |

- 依赖: 3.7
- 测试: 创建 TransContext → 状态推进 → OpSequencer 顺序保证

### 3.9 Small Write 路径 [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_do_write()`、`_choose_write_options()`、`_do_write_small()` |

- 依赖: 3.8 + ExtentMap + Allocator
- 测试: 写入 1 个 AU 内数据 → 验证 extent 正确 → 覆盖已有 extent → 验证旧 extent 进入 released

### 3.10 Big Write 路径 [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_do_write_big()`、`_do_alloc_write()`、`_wctx_finish()` |

- 依赖: 3.9
- 测试: 写入多 AU 对齐数据 → 验证 blob 的 physical extents → 校验和正确

### 3.11 读取路径 [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `read()`、`_do_read()`、`verify_csum()` |

- 依赖: 3.10 + BlockDevice 读
- 测试: 9 个测试用例覆盖基本读取、部分读取、未对齐读取、多 extent 读取、越界读取、校验和验证、不存在对象读取、空洞读取、交错空洞数据读取
- 简化实现: 跳过缓存层（P1 阶段实现），直接从块设备读取数据
- 校验和验证: 读取时自动验证 CRC32C 校验和，失败返回 -EIO
- 对齐处理: 读取时自动对齐到 block_size，结果裁剪返回用户请求范围
- Bug 修复: 修正了空洞与数据交错读取时的顺序错误（原实现将所有空洞放在前面，所有数据放在后面）
- Bug 修复: 修正了 checksum 验证偏移计算（使用 blob-relative offset 而非 physical offset）

### 3.12 KV 提交管道 [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_txc_write_nodes()`、`_txc_finalize_kv()`、`kv_sync_thread()`、`kv_finalize_thread()`、`_txc_finish()`、`_txc_release_alloc()` |

- 依赖: 3.11
- 测试: 完整写入事务 → KV 提交 → sync → finalize → alloc release 全路径
- 实现状态: 已在 Phase 3.8 中完成，包括 `_txc_write_nodes`、`_txc_finalize_kv`、`_txc_apply_kv`、`_txc_committed_kv`、`_txc_finish`、`_txc_release_alloc` 以及 `kv_sync_thread_main`、`kv_finalize_thread_main`、`finisher_thread_main` 三个后台线程

### 3.13 Zero + Remove + Attrs [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_do_zero()`、`_do_remove()`、`_do_setattr()`、`getattr()`、`getattrs()` |
| `onode.h/cc` | `set_attr()`、`remove_attr()` |
| `trans_context.h` | `zero()`、`setattr()` builder 方法，`note_removed_object()` |
| `extent_map.cc` | `punch_hole()` 修复：仅将打孔部分加入 `old_extents` |

- 依赖: 3.12
- 测试: 9 个测试用例覆盖 zero + read 验证、zero 扩展 size、remove 对象、remove 释放空间、setattr/getattr、setattrs/getattrs、attrs 持久化、不存在对象读取、zero 后 remove
- `_do_zero`: 调用 `punch_hole` 移除 extent，`_wctx_finish` 释放物理块，不写入新数据（空洞读取返回零）
- `_do_remove`: 释放所有 extent，删除 onode KV key，标记 `exists=false`
- `_do_setattr`: 设置单个属性到 `onode.attrs`，标记 onode dirty
- `getattr`/`getattrs`: 只读 API，直接从 `onode.attrs` 读取
- Bug 修复: `punch_hole` 原先将整个 extent 加入 `old_extents`，导致 `_wctx_finish` 释放整个 blob 的物理块（包括未打孔部分）
- Bug 修复: `_do_read` 按 checksum chunk 边界对齐读取，确保 `verify_csum` 校验正确

### 3.14 Collection List [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_collection_list()`、`get_coll_range()` |

- 依赖: 3.7 + KV iterator
- 测试: 创建多个 object → 按范围分页列出 → 验证结果

### 3.15 Deferred Write [✅]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `_get_deferred_op()`、`_deferred_queue()`、`_deferred_submit()`、`_deferred_aio_finish()`、`_deferred_replay()`、状态机扩展（DEFERRED_QUEUED/CLEANUP/DONE） |
| `bluestore_config.h` | `prefer_deferred_size` 配置项（默认 64KB） |

- 依赖: 3.12 (KV pipeline) + 3.9 (Small Write)
- 测试: 3 个新增测试（小写触发延迟写、持久化验证、多次小写）
- 实现状态: 已完成延迟写核心路径，包括 WAL 记录、延迟队列、批量提交、崩溃恢复重放
- 简化实现: 相比 Ceph 的复杂 iomap 合并，采用全局队列 + 立即提交的简化策略

### 3.16 OMap（对象级 key-value）[✅]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `omap_get()`、`omap_set()`、`omap_rmkeys()`、`omap_get_values()`、`omap_check_keys()` 等 11 个操作 |

- 依赖: 3.6 (Onode + Collection) + KV
- 测试: set/get/rmkeys 基本操作 → 迭代器遍历 → 边界条件（空 key、超长 key）
- 实现状态: 已完成 8 个测试用例，包括 SetAndGetKeys、SetAndGetHeader、RemoveKeys、Clear、GetFull、NonExistentObject、EmptyOMap、CheckKeys
- 简化实现: 使用 NID-based key 编码和三-分隔符方案（'-' header, '.' entries, '~' tail），相比 Ceph 简化了 per-OSR 批处理

### 3.17 FSCK（文件系统检查）[✅]

| 文件 | 内容 |
| --- | --- |
| `BlueStore.h/cc` | `fsck()` (SHALLOW/REGULAR/DEEP)、`repair()`、`quick_fix()`、`_fsck_check_collections()`、`_fsck_check_objects()`、`_fsck_check_freelist()` |

- 依赖: 3.7 (mkfs/mount) + 3.5 (ExtentMap) + 3.13 (Zero/Remove)
- 测试: 构造损坏元数据 → fsck 检测 → repair 修复 → 验证一致性
- 实现状态: 已完成 7 个测试用例，覆盖基础 FSCK、深度检查、快速修复、泄漏 extent 修复、extent 重叠检测、空 store、多 collection 场景
- 简化实现: 未实现 BlueStoreRepairer 和 StoreSpaceTracker，直接使用 FreelistManager 进行 extent 检查和修复

### 3.18 Buffer Cache [✅]

| 文件 | 内容 |
| --- | --- |
| `buffer_cache.h/cc` | `Buffer`、`BufferSpace`、`BufferCache` (全局 LRU) |
| `blob.h/cc` | `BufferSpace bc_` + `Collection*` 反向指针 |
| `bluestore.h/cc` | `_buffer_cache_write()`、`_finish_write()`、读/写/zero/remove 路径集成 |
| `bluestore_config.h` | `buffer_cache_size` (默认 64MiB)、`onode_cache_size` (默认 1024) |

- 依赖: 3.11 (Read) + 3.9/3.10 (Write)
- 测试: 11 个单元测试 + 8 个集成测试 = 19 个测试
- 实现状态: 已完成 chunk 级缓存，BufferSpace 挂在每个 Blob 上，全局 BufferCache 提供 LRU 淘汰
- 读路径: `_do_read()` 先查 `blob->bc().read()` → 命中直接返回 → 未命中走磁盘 + `did_read()` 填充缓存
- 写路径: `_do_alloc_write()` 调用 `_buffer_cache_write()` 创建 WRITING buffer → `_txc_committed_kv()` 或 `_deferred_aio_finish()` 调用 `_finish_write()` 提升为 CLEAN
- zero 路径: `_do_zero()` 对旧 extent 的 blob 调用 `bc().discard()` 失效缓存
- remove 路径: `_do_remove()` 对所有涉及 blob 调用 `bc().clear()` 清空缓存
- NOCACHE 标记: `finish_write()` 中 NOCACHE buffer 直接丢弃，不提升为 CLEAN
- WRITING 状态可读: 支持读后写一致性 (read-after-write)

### 3.19 完整集成测试 [✅]

| 文件 | 内容 |
| --- | --- |
| `test_fixture.h/cc` | 共享 Fixture 基类 `BlueStoreTestFixture` |
| `test_integration.cc` | 23 个集成测试覆盖全路径场景 |

- 依赖: 所有
- 测试: 23 个测试 — 全路径 (1) + 持久化 (7) + 混合操作 (3) + 压力 (4) + 边界 (4) + 崩溃恢复 (1) + FSCK (2) + 延迟写 (1)
- 实现状态: 已完成
- 共享 Fixture: `BlueStoreTestFixture` 提供 SetUp/TearDown (mkdtemp + block device 创建)、`make_config()`、`submit_and_wait()`、`close_and_reopen()`、`write_object()`、`read_and_verify()`、`make_oid()`
- `close_and_reopen()`: umount → 销毁 store → 新建 store → mount (同一路径)，验证持久化
- 混合操作: `MixedWorkloadState` 维护内存影子状态 (shadow)，随机执行 write/read/remove/setattr，与 store 实际数据对比验证
- 全路径测试 `FullPathSequence`: mkfs → mount → 创建 10 对象 → 写入 → 读回 → zero 前 3 个 → remove 第 4-6 个 → collection_list 验证 7 个 → umount → mount → 再次全量验证
- 崩溃恢复 `DeferredReplayAfterCrash`: 写入小数据触发延迟写路径 → umount → mount → 验证数据持久化
- 发现并修复 3 个生产代码 bug (详见已完成实现记录)

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
- BlueStore TransContext + OpSequencer (Phase 3.8): 12 tests
  - `trans_context.h`: `TransContext` (11-state machine), `OpSequencer` (per-collection sequencing), `BlueStoreTransaction` (op batch)
  - `collection.h/cc`: added `OpSequencer*` + `BlueStore*` back-pointer
  - `bluestore.h/cc`: full transaction pipeline — `_txc_create`, `_txc_state_proc`, `_txc_finish_io`, `_txc_write_nodes`, `_txc_finalize_kv`, `_txc_apply_kv`, `_txc_committed_kv`, `_txc_finish`, `_txc_release_alloc`
  - 3 threads: `kv_sync_thread` (submit + sync), `kv_finalize_thread` (post-commit state transitions), `finisher_thread` (on_commit callbacks)
  - AIO callback wired via `_aio_callback` → `txc_aio_finish` → `_txc_state_proc`
  - `_txc_finish_io` ordering: backward walk to find earliest consecutive IO_DONE, forward walk to process (matches Ceph's intrusive list approach using std::deque)
  - `queue_transactions()`: top-level entry point accepting `BlueStoreTransaction` vector + on_commit callback
- BlueStore Small Write Path (Phase 3.9): 11 tests
  - `bluestore_config.h`: added `max_blob_size` (64KB default), `csum_type` (CRC32C default)
  - `bluestore_types.h/cc`: `calc_csum()` using `calc_crc32()` from `common/crc32.h`
  - `extent_map.h/cc`: `set_lextent()` — creates extent + optional punch_hole
  - `trans_context.h`: `WriteContext` + `write_item` (tracks pending writes per transaction)
  - Write pipeline: `_do_write()` → `_do_write_data()` → `_do_write_small()` → `_do_alloc_write()` → `_wctx_finish()`
  - `_do_write_small()`: blob reuse via `can_reuse_blob()` (forward search), zero detection (punch hole), pad_zeros alignment
  - `_do_alloc_write()`: batch allocation, init_csum/calc_csum, set_lextent, AIO submission
  - `_wctx_finish()`: put_ref on old extents, record released physical extents in txc->released
  - `_do_write_data()`: splits cross-AU writes into per-chunk small writes (big write path deferred)
- BlueStore Big Write Path (Phase 3.10): 6 additional tests (19 total write tests)
  - `bluestore_config.h`: `max_blob_size` default 64KB → 256KB
  - `_do_write_data()`: rewritten as Ceph-style head/middle/tail three-way split
  - `_do_write_big()`: while loop over max_bsize-aligned chunks, forward+reverse blob reuse search, zero detection, new blob fallback
  - `_do_alloc_write()` already supports `blob_length > min_alloc_size` via batch allocation

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
- TransContext 11 态状态机保留完整枚举 (PREPARE→DONE)，3.8 实现非 deferred 路径 (DEFERRED_QUEUED/CLEANUP/DONE 在 3.15 实现)
- OpSequencer 使用 `std::deque<TransContext*>` 替代 Ceph 的 `boost::intrusive::list`，`_txc_finish_io` 用 `std::find` + 双向遍历实现顺序保证
- `_txc_finish_io` 顺序算法: 向后遍历找到最早连续 IO_DONE 的起点 (遇到 state < IO_DONE 返回阻塞，遇到 state > IO_DONE 停止回溯)，向前遍历处理所有 IO_DONE
- Collection 持有 `OpSequencer*` (堆分配，构造时 new，析构时 delete)，`BlueStore*` 反向指针在 mount/create_collection 时设置
- Finisher 用 `std::thread` + `std::deque<std::function<void()>>` 实现，简化替代 Ceph 的 Finisher 线程池
- BlueStoreTransaction 定义 OP_NOP/TOUCH/CREATE/WRITE/ZERO/REMOVE/SETATTR/SETATTRS 8 种 op，3.8 仅实现 NOP/TOUCH/CREATE/SETATTRS dispatch
- Small write path (3.9): blob reuse 仅搜索 offset 之前、blob_start <= offset 的 extent，简化 Ceph 的双向搜索
- `_do_write_data` 将跨 AU 写入拆分为 head(small) + middle(big) + tail(small)，big write 路径在 `_do_write_big` 内部 while 循环按 max_bsize chunking（方案 A，与 Ceph 一致）
- `_do_write_big` 对每个 chunk 做前向+反向 blob reuse 搜索，无复用时新建 blob，b_off 始终为 0
- `max_blob_size` 默认 256KB（Ceph 默认 512KB），使 big write 路径有意义地区别于 small write
- `_do_alloc_write` 对新 blob 调用 `allocated()` 替换 extents，对复用 blob 调用 `dirty_extents().push_back()` 追加新 extents
- `calc_csum` 接受 `dev_block_size` 参数，通过 `get_chunk_size()` 计算实际 chunk size（max of csum_chunk_size 和 dev_block_size）
- `_open_bdev` 传入 `_aio_callback` 静态函数作为 AIO 完成回调，替代之前的 nullptr（3.8 需要 AIO 回调驱动状态机）
- `WriteContext` 用 `std::vector<OldExtent>` 而非 Ceph 的 `boost::intrusive::list`，`_wctx_finish` 中调用 `put_ref` 释放旧 extent 空间
- Read path (3.11): 跳过 Ceph 的三阶段流水线（`_read_cache` / `_prepare_read_ioc` / `_generate_read_result_bl`），简化为单次 extent map 遍历 + 同步块设备读取
- `_do_read` 使用单次遍历模式：遍历 extent map 时同步读取数据并追加到 `bl`，空洞用 `append_zero` 内联填充，保证空洞与数据的正确交错顺序（修复了分两阶段处理导致的排序 bug）
- `verify_csum` 从 blob-relative offset (`req.blob_offset - front_pad`) 计算校验偏移，而非从 physical offset 计算（避免非连续 extent 场景下的偏移错误）
- Read 路径使用 buffered IO (`bdev->read(..., buffered=true)`)，跳过对齐检查，由内核页缓存处理未对齐请求
- Buffer Cache 架构 (Phase 3.18): `BufferSpace` 挂在每个 `Blob` 上 (per-Blob 缓存空间)，`BufferCache` 全局单例提供 LRU 淘汰
- Buffer Cache 粒度: chunk 级对齐 (按 `get_chunk_size()` 对齐)，与 Ceph 一致
- Buffer Cache 默认大小: 64 MiB (`buffer_cache_size`)，可通过配置调整
- OnodeSpace `max_size` 可配置: 通过 `onode_cache_size` 配置项传入 Collection 构造函数
- Buffer 状态: WRITING (写中) / CLEAN (已提交)，WRITING 状态对读可见 (read-after-write 一致性)
- Buffer NOCACHE 标记: `finish_write()` 中 NOCACHE buffer 直接丢弃，不提升为 CLEAN
- Buffer Cache 锁: BufferSpace 方法内部按需获取 cache lock (条件锁，cache=nullptr 时不锁)，避免调用方重复加锁
- `_finish_write()` 调用点: `_txc_committed_kv()` (直接写) + `_deferred_aio_finish()` (延迟写)
- Buffer Cache 淘汰: `trim()` 仅淘汰 CLEAN buffer (WRITING 跳过)，在 `add()` / `did_read()` / `finish_write()` 后自动触发
- 简化实现: 单 shard (无分片)，仅 LRU (无 2Q)，无 autotune，无 mempool 内存分类
- BlueStore Read Path (Phase 3.11): 9 tests
  - `bluestore_types.h/cc`: `verify_csum()` — mirrors `calc_csum()`, iterates chunks computing CRC32C and comparing against stored values
  - `bluestore.h/cc`: `read()` public API, `_do_read()` core read logic
  - `_do_read()`: single-pass extent map walk, synchronous block device reads, inline checksum verification, correct hole/data ordering
  - Checksum offset computed from blob-relative offset (`req.blob_offset - front_pad`), not physical offset (avoids non-contiguous extent bug)
  - Buffered read via `bdev_->read()` with `buffered=true` (skips alignment check, kernel page cache handles misalignment)
  - Hole handling: `bl.append_zero()` for gaps between extents, inline during walk to preserve ordering
- BlueStore Zero + Remove + Attrs (Phase 3.13): 9 tests
  - `_do_zero()`: punch hole in extent map, release physical blocks, no new data written
  - `_do_remove()`: release all extents, delete onode, mark non-existent
  - `_do_setattr()`: set single attribute on onode
  - `getattr()`/`getattrs()`: read-only public API for attributes
  - `extent_map::punch_hole()` bug fix: only add the punched portion to `old_extents`, not the full extent
  - `_do_read` alignment fix: align reads to checksum chunk boundaries for correct `verify_csum` validation
  - `trans_context.h`: added `zero()` and `setattr()` builders, `note_removed_object()` method
  - `onode.h/cc`: added `set_attr()` and `remove_attr()` methods
- BlueStore Collection List (Phase 3.14): 10 tests
  - `collection_list()` public API: iterate through objects in a collection with pagination support
  - Uses KV iterator with PREFIX_OBJ bounds to efficiently scan onodes
  - Filters by collection ID (pool field) and skips extent shard keys
  - Returns objects in ghobject_t comparison order (bitwise key order)
  - Supports range filtering with start/end bounds and max limit for pagination
  - Test coverage: empty collection, single/multiple objects, pagination, max limit, range filtering, collection isolation, null collection, zero max, persistence across remount
- BlueStore Deferred Write (Phase 3.15): 3 new tests (22 total write tests)
  - `bluestore_config.h`: added `prefer_deferred_size` config (default 64KB)
  - `bluestore.h/cc`: `_get_deferred_op()`, `_deferred_queue()`, `_deferred_submit()`, `_deferred_aio_finish()`, `_deferred_replay()`
  - State machine: full DEFERRED_QUEUED → DEFERRED_CLEANUP → FINISHING path
  - WAL record: serialized `bluestore_deferred_transaction_t` written to PREFIX_DEFERRED before KV commit
  - AIO callback: `txc_aio_finish()` routes to `_deferred_aio_finish()` for deferred writes
  - Mount recovery: `_deferred_replay()` replays all PREFIX_DEFERRED records after `_open_collections()`
  - `_do_alloc_write()`: deferred write decision based on `wi.bl.length() < cfg_.prefer_deferred_size`
  - Simplified from Ceph: global queue + immediate submit (no per-OSR batching, no aggressive mode, no iomap coalescing)
  - Test coverage: small write triggers deferred path, persistence across remount, multiple small writes
- BlueStore OMap (Phase 3.16): 7 tests
  - `omap_get()`, `omap_get_header()`, `omap_get_values()`, `omap_check_keys()` implementation
  - `_omap_setkeys()`, `_omap_setheader()`, `_omap_rmkeys()`, `_omap_clear()` implementation
  - NID-based OMap key encoding (`[nid] + '.' + user_key`)
  - Simplified: no BlueStoreRepairer or StoreSpaceTracker
- BlueStore FSCK (Phase 3.17): 7 tests
  - `fsck(depth, repair)` implementation, supports SHALLOW/REGULAR/DEEP depth levels
  - `_fsck_check_collections()` checks collection metadata integrity
  - `_fsck_check_objects()` checks object extent consistency, detects overlaps
  - `_fsck_check_freelist()` checks freelist consistency with actual used blocks
  - `repair()` fixes leaked extents (marks them as free)
  - `quick_fix()` quick check and fix for common issues
  - Simplified: directly uses FreelistManager for extent checking and repair, no BlueStoreRepairer or StoreSpaceTracker
- BlueStore Buffer Cache (Phase 3.18): 19 tests
  - `buffer_cache.h/cc`: `Buffer` (CLEAN/WRITING 状态 + FLAG_NOCACHE), `BufferSpace` (per-Blob 缓存空间), `BufferCache` (全局 LRU + 统计)
  - `blob.h/cc`: `BufferSpace bc_` 成员 + `Collection *coll_` 反向指针 + `get_cache()`
  - `bluestore.h/cc`: `_buffer_cache_write()` / `_finish_write()` 辅助方法
  - `_do_read()`: chunk 级缓存查询，未命中走磁盘 + `did_read()` 填充
  - `_do_alloc_write()`: 每次写入调用 `_buffer_cache_write()` 创建 WRITING buffer
  - `_txc_committed_kv()` / `_deferred_aio_finish()`: 调用 `_finish_write()` 将 WRITING 提升为 CLEAN
  - `_do_zero()`: 对旧 extent 的 blob 调用 `bc().discard()` 失效缓存
  - `_do_remove()`: 对所有涉及 blob 调用 `bc().clear()` 清空缓存
  - `bluestore_config.h`: `buffer_cache_size` (默认 64MiB) + `onode_cache_size` (默认 1024)
  - `collection.h/cc`: `OnodeSpace` max_size 可配置 + `BufferCache*` 指针 + `set_cache()` / `get_cache()`
  - `trans_context.h`: `std::set<Blob*> blobs_written` 跟踪写入的 blob
  - WRITING 状态对读可见 (支持 read-after-write 一致性)
  - NOCACHE 标记的 buffer 在 `finish_write()` 中直接丢弃
  - LRU 淘汰: `trim()` 在 `add()` / `did_read()` / `finish_write()` 后自动触发
  - 简化实现: 单 shard (无分片)，仅 LRU (无 2Q)，无 autotune，无 mempool
- BlueStore 集成测试 (Phase 3.19): 23 tests
  - `test_fixture.h/cc`: 共享 `BlueStoreTestFixture` 基类 (SetUp/TearDown/close_and_reopen/submit_and_wait/write_object/read_and_verify)
  - `test_integration.cc`: 23 个测试覆盖全路径/持久化/混合操作/压力/边界/崩溃恢复/FSCK
  - `MixedWorkloadState`: 内存影子状态 + 随机 write/read/remove/setattr 混合操作 + 对比验证
  - 全路径 `FullPathSequence`: mkfs → mount → 10 对象写入 → 读回 → zero → remove → list → umount → mount → 全量验证
  - 持久化: 7 个 remount 测试 (write/overwrite/zero/remove/attrs/omap/collection_list)
  - 压力: ManySmallWrites (50x64KB) + ManyObjectsWriteRead (200 对象) + LargeObjectChunked (4MB) + CacheEvictionUnderPressure
  - 边界: EmptyObjectReadWrite + ZeroLengthWrite + ReadBeyondSize + OverlappingWriteSameTransaction
  - 崩溃恢复: DeferredReplayAfterCrash (延迟写路径持久化验证)
  - FSCK: FsckAfterMixedOps (deep) + FsckAfterRemount
  - 发现并修复 3 个生产代码 bug:
    - `_do_alloc_write`: blob reuse 时 `need` 计算使用 `wi.blob_length` (未扩展) 而非 `logical_length` (已扩展)，导致新分配的 extent 为 0
    - `_do_alloc_write`: blob reuse 时 `new_alloc_len` 未按 `min_alloc_size` 对齐，导致 BitmapAllocator 断言失败
    - `can_reuse_blob`: `add_tail` 扩展量未按 `min_alloc_size` 对齐，导致 blob logical_length 非对齐
  - `compress_extent_map`: 添加物理连续性检查，防止合并非连续物理 extent (否则 `_do_read` 单次读取会跨越不连续的物理区域)
  - `BufferCache::trim()`: 跳过 WRITING buffer (移到 front) 继续淘汰 CLEAN buffer，防止 WRITING 堵住 LRU 尾部导致缓存无限膨胀
  - `BufferCache::flush()`: 同步清理 `BufferSpace::writing_` 列表，防止悬垂指针

## 阶段五：R7 可观测性与运维（P2，16 项）[✅]

> Ceph 参考：`src/common/perf_counters.h`（PerfCountersBuilder/PerfCounters 框架）、`src/os/bluestore/BlueStore.h:3314`（BSPerfTracker）、`BlueStore.h:3350-3378`（error injection）、`BlueStore.cc:5035`（`_init_logger` 注册约 60 个计数器）。
> cxxlab 简化基线：无 CephContext / PerfCountersCollection / ceph-mgr / histogram / mempool；avg_tracker 用 `sum+count` 原子做 long-running average；error injection 不含 SharedBlob/misreference/zombie（Deferred ADR-04）。

### 5.1 PerfCounters 基础框架 [✅]

| 文件 | 内容 |
| --- | --- |
| `common/perf_counter.h`、`common/perf_counter.cc` | `PerfCounter`（Type: `U64_COUNTER`/`U64_GAUGE`/`TIME_AVG`，atomic 存储）、`PerfCounters` 容器（name→idx，线程安全）、`PerfCountersBuilder`（`add_u64_counter`/`add_u64`/`add_time_avg`、`create_perf_counters()`）、`dump(Formatter*)` |

- Ceph ref: `perf_counters.h` `PerfCountersBuilder` + `perfcounter_type_d` + `PRIO_*`
- 简化: 无 CephContext 依赖；无 histogram（deferred）；TIME_AVG 用 `(sum, count)` 双原子做 long-running average
- R7 映射: #15 BSPerfTracker 容器、#1 基础性能计数器
- 测试: builder add → tick → dump 验证值正确；并发 inc 无竞争

### 5.2 BlueStore PerfCounters 集成 + BSPerfTracker [✅]

| 文件 | 内容 |
| --- | --- |
| `bluestore/bluestore.h` | 加 `PerfCounters *perf_` + `BSPerfTracker perf_tracker_` 成员 |
| `bluestore/bluestore.cc` | `_init_logger()`、插桩 hooks、`dump_perf_counters()` |

- 计数器: 状态机延迟（11 态：prepare/aio_wait/io_done/kv_queued/kv_committing/kv_done/finishing/done + deferred 3 态）、`txc_count`、`read_lat`/`read_eio`、`write_big`/`write_small` + bytes、`kv_flush`/`kv_commit`/`kv_sync`/`kv_final` 延迟、cache size/avail/onode
- 插桩点: `queue_transactions`（throttle/submit 起 tick）、`_txc_state_proc`（每次状态转移记前一态延迟）、`_kv_sync_thread_main`、`_kv_finalize_thread_main`、`read()`、`_do_alloc_write`
- BSPerfTracker: `commit_latency_ns`/`apply_latency_ns` 两个 avg，`update_from_perfcounters()` + `get_cur_stats()`
- Ceph ref: `BlueStore.h:3314` BSPerfTracker、`BlueStore.cc:5035` `_init_logger`
- 简化: 去掉 compression 统计（ADR-03 Deferred）、去掉 shared_blob tracker；约 30 个计数器（Ceph 约 60）
- R7 映射: #1、#2、#15、#22（设备统计部分）
- 测试: 跑 write/read → 验证 `txc_count`/`read_lat`/`write_small` 递增、延迟 > 0

### 5.3 最简 Benchmark 基线 [✅]

| 文件 | 内容 |
| --- | --- |
| `tests/bench/bench_bluestore.cc`、`tests/bench/CMakeLists.txt` | 复用 `BlueStoreTestFixture`；4 个 workload：small_write（4KB×N）、big_write（1MB×N）、read_random、mixed（50/50）；测 wall-clock p50/p99 延迟 + 吞吐 MB/s + 调 `dump_perf_counters` |

- 定位: 不是调优框架，只产出基线数字 + 给 5.2 计数器真实负载
- 与重构关系: 阶段六拆分 `bluestore.cc` 后用此基线对比验证行为无回归
- 测试: benchmark 跑通，产出非零数字

### 5.4 Error Injection 框架 [✅]

| 文件 | 内容 |
| --- | --- |
| `bluestore/error_injector.h`、`bluestore/error_injector.cc` | `ErrorInjector` 类：`inject_data_error(oid)`/`inject_mdata_error(oid)`（`std::set<ghobject_t>` + mutex）、`inject_leaked(len)`（alloc 不记 freelist）、`inject_false_free(coll,oid)`（标已分配为空闲）、`check_data_error`/`check_mdata_error`（read 路径查）、`clear()` |
| `bluestore/bluestore_config.h` | 加 `inject_read_err_rate`/`inject_write_err_rate`/`inject_kv_err_rate`（概率型，默认 0） |

- Ceph ref: `BlueStore.h:3351` `inject_data_error`/`inject_mdata_error`、`BlueStore.cc:10139` `inject_leaked`、`BlueStore.cc:10159` `inject_false_free`
- 简化: 不含 SharedBlob/misreference/zombie_spanning（依赖 Deferred ADR-04）；概率型替代 Ceph 的确定型 oid-set（更易覆盖随机路径）
- R7 映射: #10 框架
- 测试: 注入 data error → read 返回 -EIO；注入 leaked → fsck 检出；注入 false_free → fsck 检出

### 5.5 Error Injection 注入点（write/read/kv/device）[✅]

| 文件 | 内容 |
| --- | --- |
| `bluestore/bluestore.cc` | write/read/kv 注入 hooks |
| `blk/kernel_device.cc` | device 注入 |
| `bluestore/bluestore_config.h` | 注入概率配置项 |

- write 注入: `_do_alloc_write` 按 `inject_write_err_rate` 注入 `-ENOSPC`；`_wctx_finish` 偶发跳过 release（制造泄漏）
- read 注入: `_do_read` 前查 `check_data_error` → -EIO；onode 加载查 `check_mdata_error` → -EIO
- kv 注入: `_txc_write_nodes` 按 `inject_kv_err_rate` 注入 RocksDB put 失败（返回 -EIO），验证 txc 回滚
- device 注入: `KernelDevice::read`/`write` 按 `inject_*_err_rate` 注入 EIO
- R7 映射: #11、#12、#13、#14
- 测试: 各注入点触发预期错误 + 事务正确回滚（STATE_PREPARE→aborted），不残留半状态

### 5.6 BlueFS Perf Counters [✅]

| 文件 | 内容 |
| --- | --- |
| `bluefs/bluefs.h` | 加 `PerfCounters *perf_` 成员 |
| `bluefs/bluefs.cc` | `_init_perf()`、hooks、`dump_perf_counters()` |

- 计数器: `log_flush_count`/`log_flush_bytes`、`log_compact_count`、`alloc_bytes`/`free_bytes`、`read_bytes`/`write_bytes`、`files_open`
- 插桩点: `_flush_and_sync_log`、`_compact_log_async`、`_allocate`、`read`/`read_random`、`open_for_write`/`close_writer`
- R7 映射: #16
- 测试: BlueFS 写读 → 计数器递增；压缩触发 → compact_count 递增

### 5.7 空间/分配器统计 [✅]

| 文件 | 内容 |
| --- | --- |
| `bluestore/bluestore.cc` | `volatile_statfs`（运行时 used/avail） |
| `blk/allocator.h` | stats 暴露（fragmentation/alloc_count/release_count） |
| `bluestore/buffer_cache.h` | cache hit/miss/size 统计 |

- 内容: `volatile_statfs`（运行时 used/avail：sum `alloc_->get_alloc_stats` + `fm_` free）；分配器 fragmentation/alloc_count/release_count；cache hit/miss/size
- R7 映射: #5、#24（缓存统计）、#26（空间使用统计）
- 测试: 分配 N 字节 → statfs 反映；cache 命中率随访问模式变化

### 5.8 KV 统计 + PREFIX_STAT [✅]

| 文件 | 内容 |
| --- | --- |
| `kv/key_value_db.h` | stats 接口 |
| `kv/rocksdb_store.cc`、`kv/mem/mem_db.cc` | hooks |
| `kv/merge_op/` | Int64ArrayMergeOperator merge_count/merge_bytes |

- 内容: `PREFIX_STAT("T")` 统计前缀；`Int64ArrayMergeOperator` merge_count/merge_bytes；KV 层 get/put/delete/iter count + commit_lat
- Ceph ref: KeyValueDB stats、Int64ArrayMergeOperator
- R7 映射: #3、#4
- 测试: KV ops → 计数器；merge ops → merge 统计

### 5.9 FSCK 进度 + 配置热更新 [✅]

| 文件 | 内容 |
| --- | --- |
| `bluestore/bluestore.cc` | FSCK 进度回调 |
| `bluestore/bluestore_config.h` | `md_config_obs_t` 配置 reload |

- 内容: FSCK 进度回调（`_fsck_check_objects` 遍历时百分比回调）；`md_config_obs_t`（静态配置 reload：重读 config 文件，应用非 mount-time 设置如 throttle/cache_size）
- R7 映射: #9、#20、#21（per-pool 统计，简化为全局）
- 简化: per-pool 统计简化为全局（无 PG 概念）；config 热更新仅限运行时可变项
- 测试: fsck 带 progress 回调验证百分比递增；reload config → throttle 上限变化生效

### 阶段五依赖图

```plaintext
阶段五: R7 可观测性与运维

  5.1 PerfCounters framework
  5.2 BlueStore Perf + BSPerfTracker ─── 5.1
  5.3 Benchmark baseline ─── 5.2
  5.4 ErrorInjector framework ─── no dep
  5.5 Inject points ─── 5.4
  5.6 BlueFS perf ─── 5.1
  5.7 Space/alloc stats ─── 5.2
  5.8 KV stats + PREFIX_STAT ─── 5.1
  5.9 FSCK progress + config reload ─── 5.2
```

可并行: 5.1 完成后，5.4/5.6/5.8 可与 5.2 并行推进；5.3 必须等 5.2。

---

## 阶段五设计决策

- `get_tavg_ns()` / `get_avg()` 统一返回 `{sum, count}` 顺序（Ceph `get_tavg_ns` 返回 `{count, sum}`）。cxxlab 统一为 `{sum, count}` 使结构化绑定 `auto [sum, count]` 更直觉
- `PerfCounters` 私有构造 + `PerfCountersBuilder` 友元（替代 Ceph `make_unique` 无法访问私有构造的问题），Builder 内部用 `new PerfCounters(...)` 直接构造
- 所有原子操作使用 `memory_order_relaxed`（性能优先，perf 计数器允许轻微不一致）；`read_avg()` 双检循环保证读取 `(sum, count)` 一致对
- `enabled_` 原子标志按 store 粒度开关（替代 Ceph 的 `cct->_conf->perf` 全局开关）
- `reset()` 跳过纯 U64 gauge（`type == PERFCOUNTER_U64`），匹配 Ceph 语义：gauge 代表当前状态不清零，counter/avg 可重置
- 无 PerfCountersCollection：每个 store 持有自己的 `PerfCounters`，避免引入全局单例
- 新增 `get_avg(idx)` 方法（Ceph 无）：通用于任何 `LONGRUNAVG` 类型，`get_tavg_ns` 限 `TIME | LONGRUNAVG`

---

## 阶段六：重构（R7 落地后）[ ]

> 架构评审见 [docs/design/refactor-architecture-review.md](design/refactor-architecture-review.md)。核心方向: 用模块提取替代文件拆分——每次提取产出一个独立可测的深模块（自有 `.h`/`.cc`/test 文件，构造时不依赖 BlueStore），而非把 BlueStore 的成员函数搬到不同 `.cc` 文件。模块留在 `bluestore/` 目录下编入 `libbluestore.so`，与 `BufferCache`/`ExtentMap`/`Blob` 同级。

### 6.1 提取 DeferredWriter 模块 [ ]

将 deferred write 的数据结构合并、生命周期状态机、提交链路收敛为一个独立可测模块，替代原 T5+T6 分散在 BlueStore 成员中的方案。

cxxlab 现状: `_deferred_queue`（`bluestore.cc:1951`）立即 submit 无合并；`_deferred_submit`（`bluestore.cc:1964`）逐 extent `aio_write` 无 IO 聚合；无 pending/running 分离；`_deferred_aio_finish`（`bluestore.cc:1998`）有 bug（遍历已空的 `deferred_queue_` 删 key）；kv_sync 无 deferred_done→flush→stable 链路。

Ceph 参考: `BlueStore.h:2069-2095`（`DeferredBatch`）、`BlueStore.cc:3879-3899`（`prepare_write`）、`3901-3965`（`_discard`）、`13860-13903`（`_deferred_queue`）、`13941-13999`（`_deferred_submit_unlock`）、`14009-14063`（`_deferred_aio_finish`）、`13905-13939`（`deferred_try_submit`）、`13355-13403`（kv_sync flush）、`13607-13623`（kv_finalize deferred_stable）。

新增文件:

| 文件 | 内容 |
| --- | --- |
| `bluestore/deferred_writer.h` | `DeferredWriter` 类（窄接口 4 方法）+ `DeferredBatch`（嵌套或独立） |
| `bluestore/deferred_writer.cc` | 实现 |
| `tests/bluestore/test_deferred_writer.cc` | 单元测试（构造 DeferredWriter 直接测，无需 mount） |

`DeferredWriter` 接口（4 方法）:

- `queue(TransContext* txc)`: 获取 OSR 的 deferred_pending（不存在则新建），将 txc 加入 txcs，遍历 deferred ops 调 `prepare_write` 合并到 iomap
- `try_submit()`: 遍历全局 deferred_queue_osr_，对有 pending 且无 running 的 OSR 调 submit
- `flush_done()`: AIO 完成回调，清除 deferred_running，batch 加入 deferred_done_queue_
- `finalize_stable()`: kv_finalize 阶段处理 deferred_stable，对每个 txc 调 `_txc_state_proc`，删 PREFIX_DEFERRED key

`DeferredBatch` 内部结构（DeferredWriter 持有）:

- `iomap`(map<uint64_t, deferred_io>) 按 physical offset 排序
- `txcs`(list<TransContext*>)
- `ioc`(IOContext)
- `seq_bytes`(map<uint64_t, uint64_t>)
- `prepare_write(seq, offset, length, blp)`: 插入新 IO 前先 `_discard` 移除重叠的旧 IO
- `_discard(offset, length)`: 精确覆盖语义——新写与已 pending IO 部分重叠时保留非重叠 head/tail，丢弃重叠部分并扣减 seq_bytes

其他改动:

- `OpSequencer`（`trans_context.h`）新增 `deferred_pending`/`deferred_running` 指针 + `deferred_lock`
- `BlueStoreConfig`（`bluestore_config.h`）新增 `deferred_batch_ops` 字段（默认 16）
- `_deferred_submit` 内部: 切换 pending→running，遍历 iomap 合并连续 offset 的 IO 为单个 `aio_write`，最后统一 `aio_submit`
- `_kv_sync_thread_main` 新增 deferred_done→`bdev->flush()`→deferred_stable 链路
- `_kv_finalize_thread_main` 新增 deferred_stable 处理: 按 `deferred_batch_ops` 阈值触发 `try_submit`
- 修复原 `_deferred_aio_finish` 的删 key bug（改为 finalize_stable 阶段删）
- BlueStore 持有 `std::unique_ptr<DeferredWriter> deferred_writer_`，原 `_deferred_*` 5 个方法 + 4 个成员移除，替换为 4 方法调用

测试:

- `test_deferred_writer.cc`: 直接构造 DeferredWriter + mock BlockDevice，测 prepare_write 合并/discard 覆盖语义、连续 IO 聚合、pending/running 分离
- `test_write.cc` Deferred write 测试组 + 新增多 txc 合并提交测试

风险: 高（多线程状态机 + IO 保序）

### 6.2 提取 ReadPipeline 模块 [ ]

将读路径三阶段流水线 + 数据结构收敛为独立可测模块，替代原 T1 把 4 个 struct + 3 方法加进 BlueStore 成员的方案。

cxxlab 现状: `_do_read`（`bluestore.cc:1109-1200`）单函数单循环，cache 查询/同步读盘/csum 校验/组装全内联，逐 chunk 同步 `bdev_->read`，无 IO 合并、无 csum 重试。

Ceph 参考: `BlueStore.cc:11156-11281`（`_do_read` 主控）、`10893-10992`（`_read_cache`）、`10994-11061`（`_prepare_read_ioc`）、`11063-11154`（`_generate_read_result_bl`）。

新增文件:

| 文件 | 内容 |
| --- | --- |
| `bluestore/read_pipeline.h` | `ReadPipeline` 类 + 内部数据结构（不暴露到 `bluestore.h`） |
| `bluestore/read_pipeline.cc` | 实现 |
| `tests/bluestore/test_read_pipeline.cc` | 单元测试（mock BlockDevice + BufferCache，无需 mount） |

`ReadPipeline` 接口（1 方法）:

- `read(OnodeRef o, uint64_t offset, uint64_t length, BlockDevice* bdev, BufferCache* cache) → bufferlist`

内部数据结构（`read_pipeline.h` 内，不进 `bluestore.h`）:

- `region_t` = `{uint64_t logical_offset; uint32_t blob_xoffset; uint32_t length; uint32_t front}`
- `read_req_t` = `{uint32_t r_off; uint32_t r_len; bufferlist bl; std::vector<region_t> regs}`
- `regions2read_t` = `std::vector<read_req_t>`
- `blobs2read_t` = `std::map<BlobRef, regions2read_t>`
- `ready_regions_t` = `std::map<uint64_t, bufferlist>`

三阶段实现（ReadPipeline private 方法）:

- `_read_cache(OnodeRef, offset, length, ready_regions, blobs2read)`: 遍历 extent_map，查 BufferCache，命中进 ready_regions，未命中对齐 chunk_size 后合并进 blobs2read
- `_prepare_read_ioc(blobs2read, ioc)`: 遍历 blobs2read，按 `blob.map()` 映射物理偏移，注册 `aio_read` 到同一 IOContext
- `_generate_read_result_bl(OnodeRef, offset, length, ready_regions, blobs2read, buffered, csum_error, bl)`: csum 校验、cache 回填、组装结果 + hole 填零

`read()` 主控: 调三阶段 + `aio_submit` + `aio_wait` + csum error 重试（`retry_count` 递增）

其他改动:

- `bdev_->read`（同步）改为 `bdev_->aio_read` + 统一 `aio_submit`（需确认 `BlockDevice` 的 aio_read 接口）
- BlueStore 的 `read()` 委托给 `read_pipeline_->read(...)`，`_do_read` 从 `bluestore.cc` 移除

前置条件: 确认 `BlockDevice::aio_read` 可用（`blk/` 已有 libaio 支持）

测试:

- `test_read_pipeline.cc`: 直接构造 ReadPipeline + mock BlockDevice，测 cache hit/miss 混合、多 chunk 批量读、csum 重试、hole 填零
- `test_read.cc`（已有 9 测试）保留作为集成验证

### 6.3 迁移 TransContext 状态机 [ ]

将 11 态状态机从 BlueStore 的 `_txc_state_proc` 迁入 TransContext 自身，使其 own 自己的生命周期。

cxxlab 现状: `TransContext`（`trans_context.h:25-115`）持有 15 个 public 字段 + `state_`（private），但 11 态转换全由 BlueStore 的 `_txc_state_proc`（`bluestore.cc:492-558`）驱动，共 17 个 `txc->set_state()` 调用点分散在 `_txc_state_proc`/`_txc_finish_io`/`_txc_committed_kv`/`_kv_sync` 中。TransContext 不封装自己的转换。

改动文件: `trans_context.h`（TransContext）、`bluestore.cc`（移除 `_txc_state_proc`，17 个 set_state 点收敛）

改动内容:

1. 在 TransContext 新增 `advance(BlueStoreEnv& env)` 方法:
   - 内部持有 11 态转换表（state × condition → next_state + side_effect）
   - `BlueStoreEnv` 是一个窄 visitor/callback 接口，提供 BlueStore 的环境能力（`aio_submit`、`kv_sync_queue`、`finisher_queue`、`deferred_writer_queue` 等）
   - 每次调用 advance 根据当前 state + 条件决定转换 + 触发副作用

2. BlueStore 的 `_txc_state_proc` 改为薄壳: 构造 `BlueStoreEnv` 并调 `txc->advance(env)`

3. 17 个外部 `set_state` 点坍缩为 `advance()` 调用（副作用内聚到 TransContext）

4. TransContext 的 15 个 public 字段逐步 private 化（只暴露 advance 接口 + 必要的查询方法）

测试:

- `test_trans_context.cc`（已有 12 测试）补充: 直接构造 TransContext + mock BlueStoreEnv，测 11 态转换表的每个路径、非法状态拒绝、副作用触发顺序

风险: 高（17 调用点、11 状态、副作用）。建议在 6.1/6.2 之后做——前两次提取已缩小 BlueStore surface，迁移更安全。

### 6.4 blob reuse 交错搜索 + can_reuse_blob 对齐修复 [ ]（独立）

cxxlab 现状: `_do_write_big`（`bluestore.cc:1746-1783`）和 `_do_write_small`（`bluestore.cc:1638-1691`）用 forward 先穷尽、未命中再 backward 的两段式搜索。`can_reuse_blob`（`blob.cc:86`）多了 min_alloc_size 对齐。

Ceph 参考: `BlueStore.cc:15524-15568`（big 双向 do-while）、`14910-15211`（small 双向 do-while）、`2307-2379`（`can_reuse_blob`）。

改动内容:

1. `_do_write_big` 和 `_do_write_small` 的 forward+backward 搜索改为 `do-while` 交错模式: 每轮先查最近的 forward extent 再查最近的 backward extent，任一命中即退出，保证找到物理距离最近的复用 blob

2. 移除 `can_reuse_blob`（`blob.cc:86`）中多余的 `new_blen = (new_blen + min_alloc_size - 1) & ~(min_alloc_size - 1)` 对齐，与 Ceph 保持一致

验证: `tests/bluestore/test_write.cc`（已有 22 测试）补充"前后都有可复用 blob 时选最近"的场景

### 6.5 small write RMW + BigDeferredWriteContext [ ]（依赖 6.4）

cxxlab 现状: `_do_write_small` 仅 `can_reuse_blob` 单一复用路径，无 direct-write-unused、无 chunk 对齐 RMW。`_do_write_big` 无 `BigDeferredWriteContext`，大写中的小覆写无法延迟。

Ceph 参考: `BlueStore.cc:14963-14997`（direct-write-unused）、`15023-15091`（chunk RMW）、`BlueStore.h:2268-2288`（`BigDeferredWriteContext`）、`BlueStore.cc:15262-15316`（`can_defer`/`apply_defer`）、`15318-15379`（`_do_write_big_apply_deferred`）、`15418-15504`（big defer 路径）。

改动内容:

1. `_do_write_small` 补 direct-write-unused 路径: 写落在已分配+unused 的 chunk 对齐区域时直接 `aio_write` 或 deferred（若 `b_len < prefer_deferred_size`）。需确认 `bluestore_blob_t::is_unused()`/`is_allocated()`/`is_unallocated()` 在 `bluestore_types.h` 中是否已实现

2. `_do_write_small` 补 chunk 对齐 RMW 路径: 写落在已分配空间但不对齐 chunk 边界时，先读 head/tail 填充 chunk，再构造 `bluestore_deferred_op_t` 延迟覆写

3. 新增 `BigDeferredWriteContext` 结构体（`trans_context.h`）: 持有 `off`/`b_off`/`used`/`head_read`/`tail_read`/`blob_ref`/`blob_start`/`res_extents`，实现 `can_defer()`/`apply_defer()`

4. 新增 `_do_write_big_apply_deferred` 函数: 执行 RMW（读回 head/tail → 拼接 → `calc_csum` → `set_lextent` → 构造 deferred op）

5. `_do_write_big` 中补 `prefer_deferred_size` RMW 路径: 当 `l <= prefer_deferred_size*2` 时尝试 `can_defer`

验证: `tests/bluestore/test_write.cc` 补充小覆写 RMW + 大写小覆写延迟场景

风险: 高（RMW 逻辑复杂，涉及读回 + csum + deferred op 构造）

### 6.6 property/fuzz 测试 [ ]

| 测试目标 | 内容 | Ceph 参考 |
| --- | --- | --- |
| compress_extent_map | 随机生成 extent 序列，验证 compress 后物理连续性（相邻 extent 的 blob 物理偏移连续才可合并） | `BlueStore.cc` ExtentMap::compress_extent_map |
| punch_hole | 随机 offset+length punch hole，验证结果 extent 列表无重叠、无空洞残留 | `BlueStore.cc` ExtentMap::punch_hole |
| checksum 偏移 | 随机 blob offset 读，验证 csum 校验的 chunk 对齐与偏移映射正确 | `BlueStore.cc` `_verify_csum` |

测试框架: GoogleTest property-based（或简单 fuzz：随机种子 + N 轮随机操作序列），新增 `tests/bluestore/test_fuzz.cc`。

### T7 SharedBlob 间接层 — 跳过

一个 adapter = 假想 seam，无第二用例（clone/snapshot 不存在），高爆破半径（每个 `bc()` 调用点都要改）。跳过直到 clone/snapshot 到来，届时第二用例（跨 cloned onode 共享 blob cache）才能证明 seam 合理。

### 依赖与优先级

```plaintext
6.1 (DeferredWriter)  ─────── independent
6.2 (ReadPipeline)    ─────── independent
6.4 (blob reuse)      ─────── independent
6.3 (TransContext)    ─────── 6.1, 6.2
6.5 (write RMW)       ─────── 6.4
6.6 (fuzz)            ─────── 6.4, 6.5
```

建议顺序: 6.1 > 6.2 > 6.4 > 6.3 > 6.5 > 6.6。6.1/6.2/6.4 独立且收益明确，先做；6.3 在 6.1/6.2 缩小 surface 后更安全；6.5 依赖 6.4 的 blob reuse 改进；6.6 最后补 fuzz 验证。

验证手段: 5.5 注入点（write/read/kv/device error injection 验证回滚正确）+ 5.3 benchmark 基线对比（写放大 / 读延迟 / IO 合并数改善）。

### 与原方案的关键变化

| 方面 | 原方案 | 融合后 |
| --- | --- | --- |
| 6.1 | 拆 8 文件（pass-through） | 提取 DeferredWriter 模块 |
| read pipeline | BlueStore 成员 + 4 struct 进 bluestore.h | 提取 ReadPipeline 模块（struct 内部化） |
| deferred | DeferredBatch 作 helper + 5 方法留 BlueStore | 提取 DeferredWriter owning 生命周期 |
| TransContext | 未涉及 | 新增 6.3 迁移状态机 |
| blob reuse + write RMW | T2/T3/T4 分散 | 合并为 6.4 + 6.5（原样保留） |
| T7 SharedBlob | 可选实施 | 跳过 |
| 文件拆分 | 需要单独做 | 随模块提取后自然消解 |

---

## 下一步

阶段一至五全部完成（BlueFS + BlueRocksEnv + Throttle + BlueStore MVP/P1 + BTier + R7 可观测性与运维）。当前进入阶段六重构。起点: 6.1 提取 DeferredWriter 模块。
