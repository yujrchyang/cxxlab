# BlueStore — 单机键值存储引擎

> 实现状态：设计完成，Phase 3 开发中（§3.3 `pextent_t` 已实现；§3.1–3.2, §3.4–3.10 数据结构及 §4–7 引擎逻辑尚未实现）

## 1. 概述

BlueStore 是一个直接管理原始块设备的单机键值存储引擎，绕过了传统本地文件系统（如 XFS/ext4）。其核心思路是将元数据存放在 KV 存储（RocksDB）中，而数据直接写入裸块设备。

### 1.1 分层架构

```plaintext
┌──────────────────────────────────────────────────────────────────┐
│                     BlueStore (ObjectStore)                      │
│                                                                  │
│  Object API: read / write / zero / remove / clone / setattrs     │
│  Collection API: list_collections / collection_list              │
│  Transaction API: queue_transactions                             │
├─────────────────┬──────────────────┬─────────────────────────────┤
│   KV            │   Allocator      │   Block Device              │
│  (RocksDBStore) │  (Avl/Bitmap)    │  (KernelDevice + libaio)    │
└─────────────────┴──────────────────┴─────────────────────────────┘
```

### 1.2 依赖组件

| 组件 | 角色 | 对应 cxxlab 实现 | 状态 |
| --- | --- | --- | --- |
| `KeyValueDB` | 元数据持久化 | `kv/rocksdb_store.h` (RocksDBStore) | 已实现 |
| `FreelistManager` | 分配状态持久化 | `bluestore/bitmap_freelist_manager.h` | 已实现 |
| `Allocator` | 运行时内存分配决策 | `blk/{avl,bitmap,hybrid}_allocator.h` | 已实现 |
| `BlockDevice` | 块设备读写 | `blk/kernel_device.h` (libaio) | 已实现 |
| `BlueFS` | BlueStore 内部日志与元数据文件系统 | `bluestore/bluefs.h` | 已实现 |
| `BlueRocksEnv` | RocksDB 文件操作适配 BlueFS | `bluestore/blue_rocks_env.h` | 已实现 |

### 1.3 约束条件

| 维度 | 说明 |
| --- | --- |
| 操作系统 | 仅 Linux (x86\_64 + AArch64) |
| KV 后端 | RocksDB v7.10.2 |
| 压缩 | 暂不实现 |
| Shared Blob | 不实现（无 clone/snapshot） |
| Zoned (SMR) | 暂不实现 |
| Null FM | 有设计（见 [freelist-manager.md](freelist-manager.md) §6.2），BlueStore 初始版本暂不启用 |
| 配置方式 | 通过 `BlueStoreConfig` 结构体传入，各参数提供默认值 |

## 2. KV 存储布局

### 2.1 前缀定义

BlueStore 将不同类型的数据存储在不同的 RocksDB 前缀（Prefix）下，前缀为单个字符。完整的前缀定义及实现状态见 [overview.md](overview.md) §4。BlueStore 当前使用的前缀：

| 前缀 | 常量 | 用途 | Key 格式 | Value 类型 |
| --- | --- | --- | --- | --- |
| `"S"` | `PREFIX_SUPER` | 超级块元数据 | field name (string) | bufferlist |
| `"C"` | `PREFIX_COLL` | Collection (PG) 元数据 | collection name (string) | `bluestore_cnode_t` |
| `"O"` | `PREFIX_OBJ` | Object onode + extent shard | 编码的 `ghobject_t` + suffix | `bluestore_onode_t` / shard |
| `"L"` | `PREFIX_DEFERRED` | 延迟写入 WAL | `u64 seq` | `bluestore_deferred_transaction_t` |

> cxxlab 简化： omap 相关前缀（`PREFIX_OMAP`、`PREFIX_PGMETA_OMAP`、`PREFIX_PERPOOL_OMAP`、`PREFIX_PERPG_OMAP`）暂不实现。

### 2.2 Object Key 编码

Onode key 位于 `PREFIX_OBJ` 下，编码格式保证 lexicographic 排序与 `ghobject_t` 一致：

```plaintext
[1 byte: shard_id + 0x80]
[8 bytes: pool_id + 0x8000000000000000]   // big-endian
[4 bytes: hash (bitwise reversed)]
[escaped string: namespace terminated with '!']
[escaped string: key or object name terminated with '!']
[1 byte: '<', '=', or '>']                 // key 与 name 的关系
[escaped string: object name if not '=']
[8 bytes: snap (big-endian u64)]
[8 bytes: generation (big-endian u64)]
[1 byte: ONODE_KEY_SUFFIX = 'o']           // 区分 onode 与 shard
```

Key 前缀固定 13 字节（`ENCODED_KEY_PREFIX_LEN = 1 + 8 + 4`）。

### 2.3 Extent Shard Key 编码

当一个 object 的 extent map 过大时，分片存储到多个 KV 条目。Shard key 以 onode key 为前缀：

```plaintext
<onode_key> + [4 bytes: u32 offset] + [1 byte: EXTENT_SHARD_SUFFIX = 'x']
```

- `'x'` (0x78) > `'o'` (0x6f)，因此 shard 条目排列在 onode 条目之后
- 分片信息索引记录在 `bluestore_onode_t::extent_map_shards` 中

### 2.4 字符串转义

Key 中的 namespace、key、object name 需转义，以保证 lexicographic 排序正确：

- 字符 `<= '#'` → 转义为 `#XX`（两位 hex）
- 字符 `>= '~'` → 转义为 `~XX`（两位 hex）
- 字符串以 `'!'` 终止

### 2.5 超级块 (PREFIX_SUPER)

| Field | 说明 |
| --- | --- |
| `"min_alloc_size"` | 最小分配单元 |
| `"max_alloc_size"` | 最大分配单元 |
| `"nid_max"` / `"nid_last"` | Object ID 分配器 |
| `"blobid_max"` / `"blobid_last"` | Blob ID 分配器 |
| `"freelist_type"` | Freelist 类型（固定 `"bitmap"`） |
| `"mkfs_done"` | 初始化完成标记 |
| `"csum_type"` | 默认校验和类型 |
| `"csum_order"` | 默认校验和块 order |
| `"bluefs_extents"` | BlueFS 占用空间（保留） |

## 3. 核心数据结构

> 实现状态说明：本节描述的数据结构属于 Phase 3 设计规格。其中 §3.3 `bluestore_pextent_t`（别名 `pextent_t`）已实现于 `blk/extent_types.h` + `bluestore/bluestore_types.h`。其余结构（§3.1–3.2, §3.4–3.10）尚未实现，将在 Phase 3.1 (`bluestore_types.h/cc`) 中逐步落地。`bluestore_types.h` 当前仅包含 `pextent_t` 别名。

### 3.1 bluestore_bdev_label_t

块设备标签，存储在设备的首个 4KB 扇区中：

```cpp
struct bluestore_bdev_label_t {
    uuid_d osd_uuid;
    uint64_t size;
    string description;
    map<string, string> meta;     // "min_alloc_size", "mkfs_done", ...
    DENC(bluestore_bdev_label_t, v, p) { ... }
};
```

### 3.2 bluestore_cnode_t

Collection 元数据，存储在 `PREFIX_COLL` 下：

```cpp
struct bluestore_cnode_t {
    uint32_t bits;   // PG pgid 有效位数量
    DENC(bluestore_cnode_t, v, p) { ... }
};
```

### 3.3 bluestore_pextent_t

物理 extent，表示磁盘上的连续区间。定义在 `blk/extent_types.h`（Phase 0 重构后从 `bluestore/` 迁移到 `blk/`）。`bluestore_types.h` 中通过别名引用：

```cpp
// blk/extent_types.h
struct pextent_t {
    uint64_t offset = 0;    // 磁盘偏移
    uint32_t length = 0;    // 长度

    pextent_t() = default;
    pextent_t(uint64_t o, uint32_t l) : offset(o), length(l) {}
};
using PExtVector = std::vector<pextent_t>;

// bluestore/bluestore_types.h
using bluestore_pextent_t = pextent_t;  // 别名，保持 API 兼容
```

> cxxlab 简化： 仅保留 offset + length，不做 denc\_lba/denc\_varint\_lowz 变长编码压缩。offset 默认值为 `0`（无 `INVALID_OFFSET` 哨兵值，通过 extent 列表空/非空判断有效性）。

### 3.4 bluestore_blob_t

Blob 是数据持久化的最小描述单元，记录物理 extent 列表和校验和等信息：

```cpp
struct bluestore_blob_t {
    PExtentVector extents;          // 物理 extent 列表（可能不连续）
    uint32_t logical_length;        // 逻辑数据长度

    enum Flags {
        FLAG_CSUM      = (1 << 2),  // 启用校验和
        FLAG_HAS_UNUSED = (1 << 3), // 存在从未写入的区域
    };
    uint32_t flags;

    uint8_t csum_type;              // CSUM_NONE / CSUM_CRC32C / CSUM_XXHASH32
    uint8_t csum_chunk_order;       // 校验和块大小 = 1 << chunk_order
    bufferptr csum_data;            // 校验和值数组

    // 关键方法
    uint64_t get_chunk_size(uint64_t dev_block_size) const;
    int map(uint64_t x_off, uint64_t x_len,
            std::function<int(uint64_t, uint64_t)> f) const;
    void calc_csum(uint64_t b_off, const bufferlist &bl);
    int verify_csum(uint64_t b_off, const bufferlist &bl,
                    uint64_t *bad_off, uint32_t *bad_csum) const;
    void allocated(uint64_t b_off, uint64_t length,
                   const PExtentVector &allocs);
    bool release_extents(bool all, uint64_t logical_offset,
                         PExtentVector *r);
    void split(uint64_t blob_offset, bluestore_blob_t *rb);
};
```

> cxxlab 简化： 移除压缩相关 flag (`FLAG_COMPRESSED`) 和 unused 位图 (`FLAG_HAS_UNUSED` 保留但不实现 unused 追踪)，移除 shared blob flag (`FLAG_SHARED`)。

### 3.5 bluestore_onode_t

Onode 是 object 的元数据，存储在 `PREFIX_OBJ` 下：

```cpp
struct bluestore_onode_t {
    uint64_t nid;                           // 本地唯一数字 ID
    uint64_t size;                          // object 大小
    map<string, bufferptr> attrs;           // 扩展属性

    struct shard_info {
        uint32_t offset;                    // shard 起始逻辑偏移
        uint32_t bytes;                     // 编码后的 shard 大小
    };
    vector<shard_info> extent_map_shards;   // extent map 分片索引

    uint32_t expected_object_size;          // hint: 预期 object 大小
    uint32_t expected_write_size;           // hint: 预期写入大小
    uint32_t alloc_hint_flags;              // hint: 分配提示

    enum Flags {
        FLAG_OMAP       = (1 << 0),          // 有 omap 条目
        FLAG_PGMETA_OMAP = (1 << 1),
        FLAG_PERPOOL_OMAP = (1 << 2),
        FLAG_PERPG_OMAP  = (1 << 3),
    };
    uint8_t flags;

    DENC(bluestore_onode_t, v, p) {
        DENC_START(1, 1, p);
        denc(v.nid, p);
        denc(v.size, p);
        denc(v.attrs, p);
        denc(v.extent_map_shards, p);
        denc(v.expected_object_size, p);
        denc(v.expected_write_size, p);
        denc(v.alloc_hint_flags, p);
        denc(v.flags, p);
        DENC_FINISH(p);
    }
};
```

### 3.6 Extent（逻辑 extent）

Extent 描述 object 中一段连续逻辑数据与 blob 中一段连续物理数据的映射关系：

```cpp
struct Extent {
    uint32_t logical_offset;   // object 内逻辑偏移
    uint32_t blob_offset;      // blob 内偏移
    uint32_t length;           // 长度
    BlobRef blob;              // 指向的 blob
};
```

### 3.7 ExtentMap

ExtentMap 管理一个 object 的所有 extent 和其关联的 blob：

```cpp
class ExtentMap {
    Onode *onode;
    extent_map_t extent_map;               // Extent 的侵入式集合
    blob_map_t spanning_blob_map;          // 跨分片的 blob
    std::vector<Shard> shards;             // 分片索引

    struct Shard {
        bluestore_onode_t::shard_info *shard_info;
        unsigned extents;                  // 本 shard 中的 extent 数
        bool loaded;                       // 是否已从 KV 加载
        bool dirty;                        // 是否已修改需重新编码
    };

    bool needs_reshard() const;
    void fault_range(KeyValueDB *db, uint64_t offset, uint64_t length);
    void dirty_range(uint64_t offset, uint64_t length);
    extent_map_t::iterator seek_lextent(uint64_t offset);
    void punch_hole(Collection *c, uint64_t offset, uint64_t length,
                    old_extent_map_t *old_extents);
    void add(uint64_t lo, uint64_t o, uint64_t l, BlobRef b);
    void rm(extent_map_t::iterator it);
    int compress_extent_map(uint64_t offset, uint64_t length);
};
```

### 3.8 Blob（内存中 Blob）

Blob 是运行时对 `bluestore_blob_t` 的封装：

```cpp
class Blob {
    int16_t id;                             // spanning blob id (>= 0)，否则 -1
    bluestore_blob_t blob;                  // 解码后的 blob 元数据
    bluestore_blob_use_tracker_t used_in_blob;  // 逐 AU 引用追踪

    bool is_spanning() const;
    bool can_split() const;
    void get_ref();
    void put_ref();
    void split(Collection *c, uint64_t offset, Blob *rb);
};
```

> cxxlab 简化： Blob 不关联 SharedBlob，生命周期完全由拥有它的 Collection 管理。`used_in_blob` 使用 `bluestore_blob_use_tracker_t` 追踪哪些 AU 已被写入，用于 `release_extents()` 判断哪些物理 extent 可归还。

### 3.9 Collection

Collection 对应 PG，管理一组 object：

```cpp
class Collection {
    BlueStore *store;
    OpSequencerRef osr;
    bluestore_cnode_t cnode;
    OnodeSpace onode_space;       // 本 collection 的 onode 缓存

    OnodeRef get_onode(const ghobject_t &oid, bool create);
};
```

### 3.10 TransContext（事务上下文）

事务上下文是 BlueStore 中写事务的核心状态机：

```cpp
class TransContext {
    enum state_t {
        STATE_PREPARE,             // 准备 IO
        STATE_AIO_WAIT,            // 等待 AIO 完成
        STATE_IO_DONE,             // IO 完成，准备 KV 提交
        STATE_KV_QUEUED,           // 在 KV 同步线程队列中
        STATE_KV_SUBMITTED,        // 已提交 KV（未 sync）
        STATE_KV_DONE,             // KV 已 sync
        STATE_DEFERRED_QUEUED,     // 在延迟队列中
        STATE_DEFERRED_CLEANUP,    // 清理延迟记录
        STATE_FINISHING,           // 最终化
        STATE_DONE,                // 完成
    };

    CollectionRef ch;
    OpSequencerRef osr;
    uint64_t bytes, ios, cost;
    set<OnodeRef> onodes;
    set<OnodeRef> modified_objects;
    KeyValueDB::Transaction txn;
    interval_set<uint64_t> allocated;
    interval_set<uint64_t> released;
    IOContext ioc;
    bluestore_deferred_transaction_t *deferred_txn;
    uint64_t seq;
};
```

> cxxlab 简化： 移除 statfs delta、BlueStoreThrottle。不实现 deferred write（初始版本全部使用同步 AIO 路径）。

## 4. 关键组件

### 4.1 BlueStore 类

```cpp
class BlueStore {
    // 子系统
    BlueFS *bluefs;
    KeyValueDB *db;
    BlockDevice *bdev;
    FreelistManager *fm;
    Allocator *alloc;

    // 配置
    BlueStoreConfig cfg;

    // ID 分配器
    std::atomic<uint64_t> nid_last, nid_max;
    std::atomic<uint64_t> blobid_last, blobid_max;

    // 线程
    KVThread kv_sync_thread;
    KVThread kv_finalize_thread;
    Finisher finisher;

    // 缓存
    std::vector<OnodeCacheShard*> onode_cache_shards;
    std::vector<BufferCacheShard*> buffer_cache_shards;

    // KV 提交管道队列
    std::mutex kv_lock;
    std::condition_variable kv_cond;
    std::deque<TransContext*> kv_queue;
    std::deque<TransContext*> kv_queue_unsubmitted;
    std::deque<TransContext*> kv_committing_to_finalize;
};
```

### 4.2 BlueStoreConfig

```cpp
struct BlueStoreConfig {
    // 设备
    std::string blk_device_path;

    // 分配
    uint64_t min_alloc_size = 65536;        // 64KB (旋转盘), 16384 (SSD)
    uint64_t max_alloc_size = 0;            // 0 = 不限制
    std::string allocator_type = "bitmap";  // "bitmap" / "avl" / "hybrid"

    // 校验和
    uint8_t csum_type = ChecksumType::CSUM_CRC32C;
    uint8_t csum_order = 16;                // 校验和块 = 64KB
    uint64_t max_blob_size = 524288;        // 512KB

    // 缓存
    uint64_t onode_cache_size = 1024;       // onode 缓存条目数
    uint64_t buffer_cache_size = 0;         // buffer cache 大小 (bytes), 0 = 禁用

    // 线程
    unsigned kv_sync_thread_count = 1;
    unsigned kv_finalize_thread_count = 1;

    // 内部
    uint64_t throttle_bytes = 0;            // 0 = 不限制

    static BlueStoreConfig load_from_file(const std::string &path);
};
```

### 4.3 OpSequencer

每个 Collection 一个 OpSequencer，保证同 PG 内的事务提交顺序：

```cpp
class OpSequencer {
    std::mutex qlock;
    std::deque<TransContext*> q;           // 有序的 TransContext 队列
    uint64_t last_seq;
    std::atomic<int> txc_with_unstable_io; // 有 in-flight AIO 的 txc 数

    void queue_new(TransContext *txc);
    void drain();                           // 等待全部完成
    void flush();                           // 等待 KV 提交
};
```

### 4.4 KV 线程

```cpp
// KV 同步线程：负责提交 KV 事务并 sync WAL
void _kv_sync_thread();

// KV 最终化线程：负责释放分配的空间、回调通知
void _kv_finalize_thread();
```

> cxxlab 简化： 使用单线程模型，kv_sync_thread 和 kv_finalize_thread 各一个线程。移除 BlueStoreThrottle 和复杂的并发控制。

## 5. IO 生命周期

### 5.1 mkfs 路径

生成一个初始化的 BlueStore 实例：

```plaintext
mkfs(cfg)
  │
  ├── 1. 打开/创建块设备
  │     bdev = BlockDevice::create(cfg.blk_device_path)
  │     bdev->open(true)                    // 创建模式
  │     bdev->write(0, bdev_label)          // 写入设备标签
  │
  ├── 2. 计算分配参数
  │     min_alloc_size = cfg.min_alloc_size
  │     max_alloc_size = cfg.max_alloc_size
  │     block_size = bdev->block_size       // 通常是 4KB
  │
  ├── 3. 创建 Allocator（全空闲状态）
  │     alloc = Allocator::create(cfg.allocator_type,
  │                               bdev->size, min_alloc_size)
  │     alloc->init_add_free(reserved_offset, reserved_length)  // 预留 BlueFS 空间
  │     alloc->init_add_free(bluefs_end, bdev->size - bluefs_end) // 剩余全空闲
  │
  ├── 4. 创建并打开 KV 存储
  │     db = KeyValueDB::create("rocksdb", db_path, ...)
  │     db->create_and_open(...)
  │
  ├── 5. 初始化 FreelistManager
  │     fm = FreelistManager::create("bitmap", ...)
  │     txn = db->get_transaction()
  │     fm->create(bdev->size, min_alloc_size, reserved_blocks, txn)
  │     db->submit_transaction_sync(txn)
  │
  ├── 6. 持久化超级块
  │     txn = db->get_transaction()
  │     txn->set(PREFIX_SUPER, "min_alloc_size", min_alloc_size)
  │     txn->set(PREFIX_SUPER, "max_alloc_size", max_alloc_size)
  │     txn->set(PREFIX_SUPER, "nid_max", initial_nid_max)
  │     txn->set(PREFIX_SUPER, "blobid_max", initial_blobid_max)
  │     txn->set(PREFIX_SUPER, "csum_type", cfg.csum_type)
  │     txn->set(PREFIX_SUPER, "csum_order", cfg.csum_order)
  │     txn->set(PREFIX_SUPER, "freelist_type", "bitmap")
  │     txn->set(PREFIX_SUPER, "mkfs_done", "")
  │     db->submit_transaction_sync(txn)
  │
  └── 7. 关闭
        delete alloc
        db->close()
        bdev->close()
```

### 5.2 Mount 路径

从磁盘加载 BlueStore 实例：

```plaintext
mount()
  │
  ├── 1. 打开块设备 + 读取标签
  │     bdev->open(false)                   // 非创建模式
  │     bdev->read(0, &bdev_label)
  │
  ├── 2. 第一次打开 KV（只读，加载超级块）
  │     db = KeyValueDB::create("rocksdb", db_path, ...)
  │     db->open_read_only(...)
  │     _read_super_meta(db)                // 从 PREFIX_SUPER 读取配置
  │
  ├── 3. 初始化 FreelistManager + Allocator
  │     fm = FreelistManager::create("bitmap", ...)
  │     fm->init(db, read_only=false, &cfg_reader)
  │
  ├── 4. 重建 Allocator
  │     alloc = Allocator::create(cfg.allocator_type,
  │                               bdev->size, min_alloc_size)
  │     fm->enumerate_reset()
  │     while (fm->enumerate_next(db, &offset, &length))
  │         alloc->init_add_free(offset, length)
  │     fm->enumerate_reset()
  │
  ├── 5. 关闭只读 KV，重新打开读写
  │     db->close()
  │     db->open(...)                        // 读写模式
  │
  ├── 6. 加载 Collections
  │     Iterator it = db->get_iterator(PREFIX_COLL)
  │     it->lower_bound("")
  │     while (it->valid()) {
  │         // 每个条目对应一个 Collection
  │         coll = new Collection(cnode)
  │         coll_map[pgid] = coll
  │     }
  │
  ├── 7. 启动 KV 线程
  │     kv_sync_thread.start(_kv_sync_thread, this)
  │     kv_finalize_thread.start(_kv_finalize_thread, this)
  │
  ├── 8. 重放 Deferred WAL
  │     // 若存在 PREFIX_DEFERRED 条目，重新执行未完成的写入
  │
  └── 9. 启动缓存管理线程
        mempool_thread.start(...)
```

### 5.3 写入路径

写入入口为 `queue_transactions()`：

```plaintext
queue_transactions(collection_ref, transaction_list)
  │
  ├── 1. 创建 TransContext
  │     txc = new TransContext(STATE_PREPARE)
  │
  ├── 2. 分发事务操作
  │     for each op in transaction_list:
  │       switch op.type:
  │         case OP_WRITE:
  │           _do_write(txc, coll, oid, offset, length, bl, flags)
  │         case OP_ZERO:
  │           _do_zero(txc, coll, oid, offset, length)
  │         case OP_REMOVE:
  │           _do_remove(txc, coll, oid)
  │         case OP_SETATTRS:
  │           _do_setattrs(txc, coll, oid, attrs)
  │         ...
  │
  ├── 3. _do_write() 内部
  │     │
  │     ├── 3a. _choose_write_options()
  │     │     根据 fadvise_flags 设置 buffered 等选项
  │     │
  │     ├── 3b. o->extent_map.fault_range(db, offset, length)
  │     │     按需加载 extent map shard（延迟加载）
  │     │
  │     ├── 3c. 写入策略选择
  │     │     - 对齐检查: offset % min_alloc_size == 0
  │     │       && length <= min_alloc_size 且 fully
  │     │       aligned → _do_write_small()
  │     │     - 否则 → _do_write_big()
  │     │
  │     ├── 3d. _do_write_small()
  │     │     // 数据在一个 min_alloc_size block 内
  │     │     // 可能有部分覆盖（读-改-写）
  │     │     获取或创建 blob
  │     │     若部分覆盖未对齐：
  │     │       bdev->read() 读取未覆盖部分（惩罚读）
  │     │       与新数据拼合后写入
  │     │     若完全覆盖已有 extent：
  │     │       直接覆盖原有 blob 的对应区域
  │     │     更新 blob 引用计数
  │     │
  │     ├── 3e. _do_write_big()
  │     │     // 多 block 对齐写入
  │     │     新建 blob
  │     │     alloc->allocate(need, min_alloc_size, max_alloc_size,
  │     │                     hint, &prealloc)
  │     │     dblob.allocated(b_off, length, extents)
  │     │     dblob.init_csum() / calc_csum()
  │     │     o->extent_map.set_lextent(lo, boff, len, blob)
  │     │
  │     ├── 3f. _wctx_finish()
  │     │     释放旧 extent 空间到 txc->released
  │     │     o->extent_map.dirty_range(offset, length)
  │     │
  │     └── 3g. txc->write_onode(o)
  │           标记 onode 需 KV 写入
  │
  ├── 4. 提交 AIO
  │     for each write_item:
  │       bdev->aio_write(extent.offset, bl, &ioc)
  │     bdev->aio_submit(&ioc)
  │     txc->set_state(STATE_AIO_WAIT)
  │
  ├── 5. 状态机推进
  │     │
  │     ├── STATE_AIO_WAIT → AIO 完成回调:
  │     │     ioc.aio_wait() 或 aio_callback 触发
  │     │     txc->set_state(STATE_IO_DONE)
  │     │
  │     ├── STATE_IO_DONE → _txc_write_nodes():
  │     │     编码 onode → KV txn
  │     │     _txc_finalize_kv():
  │     │       fm->allocate(off, len, txn)   // 持久化分配
  │     │       fm->release(off, len, txn)    // 持久化释放
  │     │     txc->set_state(STATE_KV_QUEUED)
  │     │     入队 kv_queue → 通知 kv_sync_thread
  │     │
  │     ├── STATE_KV_QUEUED → _kv_sync_thread:
  │     │     db->submit_transaction(txc->txn)
  │     │     txc->set_state(STATE_KV_SUBMITTED)
  │     │     db->submit_transaction_sync({}) // sync WAL
  │     │     txc->set_state(STATE_KV_DONE)
  │     │     入队 kv_committing_to_finalize
  │     │     通知 kv_finalize_thread
  │     │
  │     ├── STATE_KV_DONE → _kv_finalize_thread:
  │     │     txc->set_state(STATE_FINISHING)
  │     │     _txc_finish():
  │     │       _txc_release_alloc():
  │     │         alloc->release(txc->released)   // 归还 Allocator
  │     │       osr->q.pop_front()
  │     │       delete txc
  │     │
  │     └── STATE_DONE: 完成
  │
  └── 返回
```

### 5.4 读取路径

```plaintext
read(coll, oid, offset, length, bl, op_flags)
  │
  ├── 1. 获取 Onode
  │     o = coll->get_onode(oid)
  │     if offset >= o->onode.size -> 返回 0 字节
  │
  ├── 2. 加载 extent map
  │     o->extent_map.fault_range(db, offset, length)
  │
  ├── 3. 检查缓存
  │     _read_cache(o, offset, length, &ready_regions, &blobs2read)
  │     // cache hit → ready_regions
  │     // cache miss → blobs2read (需从磁盘读取)
  │
  ├── 4. 构建 AIO 读取
  │     for each (blob, logical_offset, blob_xoffset, length) in blobs2read:
  │       blob.blob.map(blob_xoffset, length, [&](uint64_t poff, uint64_t plen) {
  │         bdev->aio_read(poff, plen, &ioc)
  │       })
  │     bdev->aio_submit(&ioc)
  │     ioc.aio_wait()
  │
  ├── 5. 组装结果
  │     for each blob in blobs2read:
  │       if blob has csum:
  │         blob.blob.verify_csum(...)
  │       if 校验和错误 → 重试
  │     _generate_read_result_bl(o, offset, length, ready_regions,
  │                              blobs2read, bl)
  │
  └── 6. 选择性缓存
        if applicable: buffer_cache.did_read(offset, bl)
```

### 5.5 Collection 列表

```plaintext
collection_list(coll, start, end, max, &ls, &next)
  │
  ├── 1. 计算 OID 范围
  │     get_coll_range(coll, &start, &end)
  │     // 包含 temp 范围和 PG hash 范围
  │
  ├── 2. 创建 KV Iterator
  │     _key_encode_prefix(start) → low_key
  │     _key_encode_prefix(end) → high_key
  │     it = db->make_iterator(IteratorBounds(low_key, high_key + '\xff'))
  │     it->lower_bound(low_key)
  │
  └── 3. 遍历并收集
        while (it->valid() && ls.size() < max) {
          decode ghobject_t from key
          if in range → ls.push_back(oid)
        }
        set next = continuation key
```

### 5.6 释放路径

```plaintext
_txc_release_alloc(txc):
  │
  └── alloc->release(txc->released)
        // 将先前写入的旧 extent 归还到 Allocator
        // 相邻 extent 自动合并
        // HybridAllocator 还会尝试从 bitmap child 回收相邻空间
```

## 6. 线程模型

### 6.1 线程概览

| 线程 | 职责 | 唤醒条件 |
| --- | --- | --- |
| 客户端线程 | 分发事务操作、准备 AIO | 用户调用 `queue_transactions()` |
| AIO 回调 | 标记 IO 完成 | libaio IO 完成事件 |
| `kv_sync_thread` | 提交 KV 事务、sync WAL | `kv_queue` 非空 |
| `kv_finalize_thread` | 释放分配、回调通知 | `kv_committing_to_finalize` 非空 |

### 6.2 状态转移图

```plaintext
               客户端线程
                  │
                  ▼
            STATE_PREPARE
                  │
                  ▼ AIO 提交
           STATE_AIO_WAIT
                  │
       ┌──────────┘
       ▼ AIO 完成回调
       STATE_IO_DONE
                  │
       ┌──────────┘ _txc_write_nodes + _txc_finalize_kv
       ▼
    STATE_KV_QUEUED ──────→ kv_sync_thread
                                  │
                                  ▼
                            STATE_KV_SUBMITTED
                                  │
                                  ▼ sync WAL
                            STATE_KV_DONE ──────→ kv_finalize_thread
                                                       │
                                                       ▼
                                                  STATE_FINISHING
                                                       │
                                                       ▼
                                                    STATE_DONE
```

> cxxlab 简化： 不实现 deferred write 路径。所有写入在 AIO 完成后立即通过 KV 事务持久化，不走 WAL 延迟提交。

### 6.3 OpSequencer 顺序保证

同一 Collection 中的 TransContext 按创建顺序插入 OpSequencer 队列：

```plaintext
OpSequencer.q = [txc0, txc1, txc2, ...]

_txc_finish_io():
  lock osr->qlock
  txc->set_state(STATE_IO_DONE)
  // 顺序保证: 只推进队列头的 txc
  while (osr->q.front()->state == STATE_IO_DONE) {
    osr->q.front() → STATE_KV_QUEUED
    osr->q.pop_front() → kv_queue
  }
  unlock
```

## 7. 启动恢复路径

```plaintext
mount()
  │
  ├── _open_db_and_around()
  │     ├── bdev->open()
  │     ├── db->open_read_only()
  │     ├── _open_super_meta()
  │     ├── _open_fm()
  │     ├── _init_alloc()
  │     │     ├── alloc = _create_alloc()
  │     │     └── fm->enumerate_next() → alloc->init_add_free()
  │     ├── db->close()
  │     └── db->open()   // read-write
  │
  ├── _open_collections()
  │     Iterator on PREFIX_COLL
  │     for each: coll_map[pgid] = new Collection(cnode)
  │
  ├── _kv_start()
  │     start kv_sync_thread, kv_finalize_thread
  │
  ├── _deferred_replay()
  │     // 重放 deferred WAL（如果存在）
  │
  └── _mempool_thread.start()
```

## 8. 简化与决策

### 8.1 设计简化

| Ceph 实现 | cxxlab 处理方式 |
| --- | --- |
| 压缩（zlib/zstd/lz4/snappy） | 暂不实现 |
| Shared Blob 克隆引用计数 | 不实现（无 clone/snapshot） |
| Zoned (SMR) | 不实现 |
| Null FM（BlueFS 文件替代 KV） | 有设计，初始版本暂不启用 |
| Deferred Write（WAL 延迟写入） | 初始版本不实现 |
| PerfCounters 详细统计 | 保留核心统计 |
| AdminSocket 运行时调试 | 移除 |
| mempool 内存池统计 | 移除 |
| BlueStoreThrottle 流量整形 | 移除（`throttle_bytes = 0`） |
| Omap 操作 | 暂不实现 |
| Checksum（CRC32C / XXHASH32） | 支持（CRC32C 为默认） |
| Blob 变长编码（denc_lba / denc_varint_lowz） | 使用简单 DENC |
| BlueFS 内嵌用户态文件系统 | 已实现（见 [bluefs.md](bluefs.md)） |
| BlueRocksEnv RocksDB 文件操作适配 | 已实现（见 [blue-rocks-env.md](blue-rocks-env.md)） |

### 8.2 关键决策

| 决策 | 原因 |
| --- | --- |
| 始终使用 BitmapFreelistManager | 简化实现，不移除已实现的组件 |
| 不使用 deferred write | 减少初始版本复杂度 |
| 不使用 shared blob | 不实现 clone/snapshot 功能 |
| 不使用压缩 | 减少初始版本复杂度 |
| 单线程 KV sync | kv_sync_thread 和 kv_finalize_thread 各一个线程 |
| 配置结构体 | `BlueStoreConfig` 提供默认值 + 文件加载 |

### 8.3 已知待办

- [ ] Deferred write 路径（后续根据性能需求添加）
- [ ] 压缩
- [ ] Null FM 模式启用（设计已完成，见 freelist-manager.md §3.4）
- [ ] Omap 操作
- [ ] Buffer cache 实现
- [ ] 碎片整理/GC

## 9. 参考

- Ceph source: `src/os/bluestore/BlueStore.h` / `.cc`
- Ceph source: `src/os/bluestore/bluestore_types.h`
- Ceph source: `src/os/bluestore/bluestore_kv.h`
- 本项目 [docs/design/overview.md](overview.md): 架构总览
- 本项目 [docs/design/keyvalue-db.md](keyvalue-db.md): KV 抽象层设计
- 本项目 [docs/design/freelist-manager.md](freelist-manager.md): FreelistManager 设计
- 本项目 [docs/design/allocator.md](allocator.md): Allocator 设计
- 本项目 [docs/design/block-device.md](block-device.md): 块设备抽象层设计
- 本项目 [docs/design/bluefs.md](bluefs.md): BlueFS 设计
- 本项目 [docs/design/blue-rocks-env.md](blue-rocks-env.md): BlueRocksEnv 设计
- 本项目 `bluestore/bluestore_types.h`: BlueStore 数据结构定义（Phase 3）
