# kv — Key-Value Storage Abstraction Layer

> 实现状态：已实现（80 tests，RocksDBStore + MemDB + MergeOperator）

## 1. 需求分析

### 1.1 背景

cxxlab 需要在 block device 层之上实现一个类 BlueStore 的对象存储引擎。BlueStore 依赖一个 KV 存储作为元数据与状态持久化引擎，承担以下职责：

| 数据类别 | KV Prefix | 内容 | 实现状态 |
| --- | --- | --- | --- |
| 超级块 | `PREFIX_SUPER` ("S") | nid/blobid 计数器、分配参数 | 已实现 |
| 统计信息 | `PREFIX_STAT` ("T") | 全局/每池 statfs 数据 | 计划中 |
| 集合 (Collection) | `PREFIX_COLL` ("C") | 集合名 → cnode_t | 已实现 |
| 对象元数据 (Onode) | `PREFIX_OBJ` ("O") | 对象名 → onode_t（含 extent map） | 计划中 |
| Omap (旧) | `PREFIX_OMAP` ("M") | nid + key → value | 不实现 |
| Omap (按 PG) | `PREFIX_PERPG_OMAP` ("p") | pool + hash + nid + key → value | 不实现 |
| Omap (按 pool) | `PREFIX_PERPOOL_OMAP` ("m") | pool + nid + key → value | 不实现 |
| Omap (meta PG) | `PREFIX_PGMETA_OMAP` ("P") | meta PG 的 omap | 不实现 |
| Deferred WAL | `PREFIX_DEFERRED` ("L") | seq → deferred_transaction_t | 计划中 |
| Freelist (extent) | `PREFIX_ALLOC` ("B") | offset → length | 已实现 |
| Freelist (bitmap) | `PREFIX_ALLOC_BITMAP` ("b") | 位图分配元数据 | 已实现 |
| 共享 Blob | `PREFIX_SHARED_BLOB` ("X") | sb_id → shared_blob_t | 不实现 |

"使用者"列区分前缀的实际拥有者——BlueStore 直接使用 `S`/`T`/`C`/`O`/`L` 等前缀，FreelistManager 使用 `B`/`b` 前缀（在 `fm->init()` 时注册）。"已实现"表示该前缀已在代码中使用；"计划中"表示在 BlueStore Phase 3 开发计划中；"不实现"表示已明确决定不移植。

### 1.2 约束条件

- 操作系统：仅 Linux（x86\_64 + AArch64）
- 后端实现：
  - RocksDB — 生产级持久化引擎
  - MemDB — 纯内存实现，仅用于调试/演示/单元测试，无持久化保障
- 接口要求：必须支持 MergeOperator，这是 BlueStore 原子分配和统计更新的基础

### 1.3 功能需求

| 需求 | 说明 |
| --- | --- |
| Point Read | `get(prefix, key) → value` |
| Batch Read | `get(prefix, keys) → map<key, value>` |
| Point Write | `set(prefix, key, value)` |
| Point Delete | `rmkey(prefix, key)` |
| Single-Key Delete (LSM opt) | `rm_single_key(prefix, key)` — 仅删除最新版本 |
| Prefix Delete | `rmkeys_by_prefix(prefix)` |
| Range Delete | `rm_range_keys(prefix, start, end)` |
| Merge | `merge(prefix, key, delta)` — 原子增量操作 |
| 前缀迭代 | `get_iterator(prefix)` — 正反向遍历 |
| 全空间迭代 | `get_wholespace_iterator()` — 遍历全部 prefix |
| 迭代器边界 | `IteratorBounds` — lower\_bound / upper\_bound 过滤 |
| NOCACHE 扫描 | `ITERATOR_NOCACHE` — 不污染缓存的大规模扫描 |
| 事务提交 | `submit_transaction(t)` — 异步 |
| 同步提交 | `submit_transaction_sync(t)` — 用于 bootstrap / batch |
| Compact | `compact()` / `compact_async()` — 全量 SST 合并 |
| Merge Op 注册 | `set_merge_operator(prefix, op)` — open 前调用 |
| 空间估算 | `get_estimated_size()` — 磁盘占用统计 |

## 2. 架构设计

### 2.1 整体架构

```plaintext
┌────────────────────────────────────────────────────────────┐
│                      KeyValueDB (abstract)                 │
│  Base: factory / get / iterator_bounds / merge_op_reg      │
│  Inner: PrefixIteratorImpl (WholeSpaceIterator prefix      │
│         filter wrapper)                                    │
└─────────────┬───────────────────────────────┬──────────────┘
              │ inherit                       │ inherit       
┌─────────────▼──────────────┐  ┌─────────────▼──────────────┐
│       RocksDBStore         │  │          MemDB             │
│  rocksdb::DB wrapper       │  │  std::map + std::mutex     │
│  key: prefix+'\0'+key      │  │  pure memory, no persist   │
│  supports MergeOperator    │  │  supports MergeOperator    │
│  supports IteratorBounds   │  │  iterator invalidation(smp)│
└────────────────────────────┘  └────────────────────────────┘
```

`PrefixIteratorImpl` 继承 `IteratorImpl`，包裹一个 `WholeSpaceIterator`，在 `valid()` 中调用 `raw_key_is_prefixed(prefix)` 确保没有越界到其他 prefix。构造时将 bounds 信息（`lower_bound` / `upper_bound`）合并到编码 key 中，并通过 `set_iterate_lower_bound` / `set_iterate_upper_bound` 传递给底层 RocksDB 迭代器。`make_iterator()` 是基类提供的 protected 工厂方法，子类只需实现 `get_wholespace_iterator()` 即可获得 prefix 过滤迭代器。

### 2.2 构建组织

```plaintext
kv/
├── CMakeLists.txt            ← 构建 libkv.so
├── key_value_db.h            ← 抽象基类 (接口 + 内部 PrefixIteratorImpl)
├── key_value_db.cc           ← create() 工厂方法 + PrefixIteratorImpl 实现
├── mem/
│   ├── mem_db.h
│   └── mem_db.cc
├── rocksdb/
│   ├── rocksdb_store.h
│   └── rocksdb_store.cc
└── merge_op/
    ├── int64_array_merge_op.h  ← 用于 PREFIX_STAT
    ├── xor_merge_op.h          ← 用于 PREFIX_ALLOC_BITMAP
    └── merge_op.h              ← 抽象基类
```

### 2.3 依赖关系

```plaintext
libkv.so
  ├── libcommon.so (bufferlist, cassert, error)
  ├── librocksdb (仅 RocksDBStore 链接，PRIVATE)
  └── Boost (仅 RocksDBStore: intrusive list for cache)
```

## 3. 组件详情

### 3.1 MemDB

- 存储：`std::map<std::string, std::string>`
- Key 编码：`prefix + '\0' + key`
- 锁：`std::mutex` 保护所有读写操作
- 事务：`MDBTransactionImpl` 将操作记录到 `vector<Op>`，提交时加锁依次回放
- 迭代器：创建时获取 `std::map` 的 snapshot（排序后的 `vector<pair>`），并通过 `uint64_t seqno_` 检测写操作是否发生过。检测到变更后重新 snapshot 并重定位到之前的 key 位置。支持 `set_iterate_lower_bound` / `set_iterate_upper_bound`（自 `WholeSpaceIteratorImpl` 基类继承）。
- Merge：实现完整的 merge 语义，查找已注册的 MergeOperator 并调用 `merge_nonexistent` / `merge`。未注册的 prefix 返回 `-ENOENT`。
- 持久化：无。重启后数据丢失

### 3.2 RocksDBStore

- 编译：需要 `-Wno-unused-parameter`（RocksDB 接口回调中大量未使用参数）
- 存储：单个 `rocksdb::DB` 实例，不启用 ColumnFamily 分片
- Key 编码：`prefix + '\0' + key`（与 MemDB 一致，确保两者 Key 空间可互相转换）
- ColumnFamily：仅使用默认 CF（`rocksdb::DB::Open` 的单 CF 模式），不实现 hash 分片逻辑
- Merge：通过 `RocksDBMergeAdapter`（继承 `rocksdb::MergeOperator`）将 `KeyValueDB::MergeOperator` 分发到各 prefix。`set_merge_operator()` 必须在 `open()` / `create_and_open()` 之前调用，否则返回 `-EROFS`。
- IteratorBounds：两层分离：
  - `WholeSpaceIteratorImpl` 基类持有 `const std::string* iterate_lower_bound_` / `iterate_upper_bound_`
  - `PrefixIteratorImpl` 构造时通过 `set_iterate_lower_bound` / `set_iterate_upper_bound` 将 bounds 注入基类
  - `RDBWholeSpaceIteratorImpl` 在每次 `seek` / `lower_bound` / `upper_bound` 时调用 `apply_bounds()`，将 `std::string*` 转为 `rocksdb::Slice*` 写入 `ReadOptions`
- open\_read\_only：通过 `rocksdb::DB::OpenForReadOnly()` 实现
- repair：通过 `rocksdb::RepairDB()` 实现
- compact\_prefix / compact\_range：编码 key 后调用 `CompactRange()` 限定 `[start, end)` Slice
- DeleteRange 阈值：通过构造函数 options `delete_range_threshold` 配置，预留用于小范围逐条删除以减少宽墓碑开销，当前初始版本始终使用 `DeleteRange`
- Init 选项解析：`init(options_str)` 解析 `key=val;key=val` 格式字符串，支持设置 `write_buffer_size`、`max_write_buffer_number`、`max_bytes_for_level_base`、`target_file_size_base` 等 RocksDB 参数
- ITERATOR\_NOCACHE：通过 `rocksdb::ReadOptions::fill_cache = false` 实现
- Compact：调用 `rocksdb::DB::CompactRange()`，同步版本
- submit\_transaction\_sync：设置 `rocksdb::WriteOptions::sync = true` 实现 WAL 同步
- key\_size / value\_size：委托给 `rocksdb::Iterator::key().size()` / `value().size()`

### 3.3 设计简化

| 原始实现 | cxxlab 处理方式 |
| --- | --- |
| `CompactThread` 异步压缩 | 移除 |
| `BinnedLRUCache` / `PriorityCache` 集成 | 移除 |
| `Resharding` 相关逻辑 | 移除 |
| `CephContext` / `PerfCounters` / `Formatter` 依赖 | 移除 |
| ColumnFamily sharding 及多 CF 管理 | 不实现，单 CF 模式 |
| 自定义 block cache 配置 | 移除 |
| MemDB `_save()`/`_load()` 文件持久化 | 移除 |
| MemDB `PerfCounters` / `btree_map` 支持 | 移除 |

### 3.4 MergeOperator 实现

`Int64ArrayMergeOperator`（用于 `PREFIX_STAT`）做逐元素 int64 加法：

```cpp
class Int64ArrayMergeOperator : public MergeOperator {
  const char *name() const override { return "int64_array"; }
  void merge_nonexistent(const char *rdata, size_t rlen,
                          std::string *new_value) override {
    *new_value = std::string(rdata, rlen);  // exist = delta
  }
  void merge(const char *ldata, size_t llen,
             const char *rdata, size_t rlen,
             std::string *new_value) override {
    auto existing = reinterpret_cast<const int64_t*>(ldata);
    auto delta   = reinterpret_cast<const int64_t*>(rdata);
    size_t count = std::min(llen, rlen) / sizeof(int64_t);
    std::vector<int64_t> result(count);
    for (size_t i = 0; i < count; i++)
      result[i] = existing[i] + delta[i];
    new_value->assign(reinterpret_cast<char*>(result.data()),
                      count * sizeof(int64_t));
  }
};
```

`XorMergeOperator`（用于 `PREFIX_ALLOC_BITMAP`）做位级别异或：

```cpp
class XorMergeOperator : public MergeOperator {
  const char *name() const override { return "xor"; }
  void merge_nonexistent(const char *rdata, size_t rlen,
                          std::string *new_value) override {
    *new_value = std::string(rdata, rlen);
  }
  void merge(const char *ldata, size_t llen,
             const char *rdata, size_t rlen,
             std::string *new_value) override {
    size_t len = std::min(llen, rlen);
    new_value->resize(len);
    for (size_t i = 0; i < len; i++)
      (*new_value)[i] = ldata[i] ^ rdata[i];
  }
};
```

## 4. IO 流程

### 4.1 写入流程

```plaintext
BlueStore::_txc_apply_kv()
        │
        ▼
txc->t->set(PREFIX_OBJ, onode_key, onode_bl)     ← onode 更新
txc->t->set(PREFIX_COLL, cid, cnode_bl)           ← collection 更新
txc->t->merge(PREFIX_STAT, stat_key, delta_bl)    ← statfs 增量
txc->t->set(PREFIX_OMAP, omap_key, omap_bl)       ← omap 更新
txc->t->rm_range_keys(PREFIX_OMAP, head, tail)    ← omap 范围删除
fm->allocate(release, t)  → t->merge("b", k, bl)  ← 位图分配
fm->release(alloc, t)     → t->merge("b", k, bl)  ← 位图释放
        │
        ▼
KeyValueDB::submit_transaction(t)
        │
  ┌─────┴──────┐
  │            │
  ▼            ▼
RocksDB      MemDB
WriteBatch   lock + replay ops
-> Write()   -> map insert/erase
```

RocksDBStore 提交路径（`submit_transaction`）：

```plaintext
submit_transaction(t)
  │
  ├── 遍历 TransactionImpl 操作, 构建 rocksdb::WriteBatch
  │     ├── set → batch.Put(encode_key(prefix, k), value)
  │     ├── rmkey → batch.Delete(encode_key(prefix, k))
  │     ├── rm_single_key → batch.SingleDelete(encode_key(prefix, k))
  │     ├── rmkeys_by_prefix → batch.DeleteRange(prefix+'\0', prefix+'\xff')
  │     ├── rm_range_keys → batch.DeleteRange(encode_key(prefix, start), encode_key(prefix, end))
  │     └── merge → batch.Merge(encode_key(prefix, k), value)
  │
  └── db->Write(woptions, &batch)
```

MemDB 提交路径（`submit_transaction`）：

```plaintext
submit_transaction(t)
  │
  ├── 加锁 m_lock
  ├── 遍历 MDBTransactionImpl.ops
  │     ├── SET → _setkey(prefix, k, bl)
  │     ├── RMKEY → _rmkey(prefix, k)
  │     ├── RMKEY_BY_PREFIX → 遍历 map 删除匹配前缀
  │     ├── RM_RANGE_KEYS → 遍历 map 删除 [start, end)
  │     └── MERGE → _merge(prefix, k, bl)
  └── 释放锁
```

### 4.2 读取流程

```plaintext
BlueStore::_onode_map_lookup(key)
        │
        ▼
KeyValueDB::get(PREFIX_OBJ, key, &bl)
        │
  ┌─────┴──────┐
  │            │
  ▼            ▼
RocksDB      MemDB
db->Get()    map.find()
```

### 4.3 扫描流程

```plaintext
BlueStore::generate_stats()
        │
        ▼
KeyValueDB::get_wholespace_iterator(opts = ITERATOR_NOCACHE)
        │
        ▼
it->seek_to_first()
while (it->valid()) {
    auto [prefix, key] = it->raw_key();   ← 获取 prefix + 内部 key
    auto value = it->value();
    it->next();
}
```

### 4.4 启动恢复流程

```plaintext
BlueStore::_open_db()
  db = KeyValueDB::create("rocksdb", path, opts)
  db->set_merge_operator("T", Int64ArrayMergeOperator)
  FreelistManager::setup_merge_operators(db, "bitmap")    ← 设置 "b" 的 XOR merge
  db->create_and_open(out)                                 ← KV 存储初始化
        │
        ▼
BlueStore::_open_fm()
  遍历 PREFIX_ALLOC 或 PREFIX_ALLOC_BITMAP 重建 freelist
        │
        ▼
BlueStore::_open_collections()
  遍历 PREFIX_COLL 重建所有 Collection
        │
        ▼
BlueStore::_replay()
  get_iterator(PREFIX_DEFERRED) 遍历重放未完成的 deferred 写
```

## 5. 使用示例

### 5.1 创建及打开

```cpp
auto db = KeyValueDB::create("rocksdb", "/var/lib/cxxlab/store", {});
// 必须在 open 前注册 MergeOperator
db->set_merge_operator("T",
    std::make_shared<Int64ArrayMergeOperator>());

int r = db->create_and_open(std::cerr);
```

### 5.2 写入

```cpp
auto t = db->get_transaction();

// Point write
bufferlist val;
val.append("hello");
t->set("O", "obj_key", val);

// Range delete
t->rm_range_keys("M", "head_prefix", "tail_prefix");

// Merge (statfs delta)
bufferlist delta;
encode(int64_t(1), delta);
t->merge("T", "global", delta);

db->submit_transaction_sync(t);
```

### 5.3 读取

```cpp
bufferlist value;
int r = db->get("O", "obj_key", &value);
if (r == 0) {
    // value contains the data
}
```

### 5.4 扫描

```cpp
// Prefix-scoped iteration with bounds
auto it = db->get_iterator("O", 0,
    KeyValueDB::IteratorBounds{"start_key", "end_key"});
it->lower_bound("start_key");
while (it->valid()) {
    process(it->key(), it->value());
    it->next();
}

// Full-space scan (e.g. debug)
auto wit = db->get_wholespace_iterator(
    KeyValueDB::ITERATOR_NOCACHE);
wit->seek_to_first();
while (wit->valid()) {
    auto [prefix, key] = wit->raw_key();
    fmt::print("{}:{}\n", prefix, key);
    wit->next();
}
```

### 5.5 调试模式

```cpp
// 完全在内存中运行，不产生任何磁盘写入
auto db = KeyValueDB::create("memdb", "/tmp/unused", {});
db->create_and_open(std::cerr);
// 所有操作与 RocksDB 模式语义完全一致
```

## 6. 参考

- Ceph source: `src/kv/KeyValueDB.h` / `.cc`
- Ceph source: `src/kv/RocksDBStore.h` / `.cc`
- Ceph source: `src/kv/MemDB.h` / `.cc`
- 本项目 [docs/design/overview.md](overview.md): 架构总览
- 本项目 `kv/key_value_db.h`: KeyValueDB 抽象层
- 本项目 `kv/rocksdb/rocksdb_store.h`: RocksDBStore 后端
- 本项目 `kv/mem/mem_db.h`: MemDB 后端
- 本项目 `kv/merge_op/`: MergeOperator 实现
