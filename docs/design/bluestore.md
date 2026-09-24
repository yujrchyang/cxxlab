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
| `BlueFS` | BlueStore 内部日志与元数据文件系统 | `bluefs/bluefs.h` | 已实现 |
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
using PExtentVector = std::vector<pextent_t>;

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

> cxxlab 简化： 移除 statfs delta。BlueStoreThrottle 提取到 `common/Throttle`（Phase 2.5）。Deferred Write 纳入 MVP（小写性能关键路径），状态机保留完整 11 态。

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

> cxxlab 简化： 使用单线程模型，kv_sync_thread 和 kv_finalize_thread 各一个线程。BlueStoreThrottle 提取到 `common/Throttle`（Phase 2.5），简化并发控制。

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
  └── 8. 重放 Deferred WAL
        // 若存在 PREFIX_DEFERRED 条目，重新执行未完成的写入
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

> cxxlab 说明： Deferred Write 纳入 MVP。小写入（≤min_alloc_size）先写入 RocksDB WAL，后台线程批量合并后刷盘，将 HDD 随机小写转为顺序大写。状态机增加 DEFERRED_QUEUED → DEFERRED_CLEANUP → DEFERRED_DONE 三个状态。

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
  └── _deferred_replay()
        // 重放 deferred WAL（如果存在）
```

## 8. 功能裁剪与优先级

BlueStore 功能按 8 大需求类别组织，每个功能点标注优先级：

- MVP: 最小可行产品（104 项，52%）— 核心读写路径、事务语义、空间管理、基本完整性、最小性能、单设备
- P1: 强烈建议（41 项，20%）— OMap、FSCK、Buffer Cache、增强可靠性
- P2: 后续迭代（28 项，14%）— 诊断、高级缓存、错误注入测试
- Deferred: 明确延迟（28 项，14%）— ADR-03（压缩）、ADR-04（SharedBlob/Clone/Snapshot）、多设备操作

### 8.1 需求类别

| # | 需求类别 | 核心目标 |
| --- | --- | --- |
| R1 | 数据持久化 | 对象数据直接写入块设备，元数据写入 KV |
| R2 | 事务语义 | 写操作具备原子性和崩溃一致性 |
| R3 | 空间管理 | 块设备空间的分配、释放、碎片整理 |
| R4 | 数据完整性 | 校验和验证、错误检测与恢复 |
| R5 | 性能优化 | 缓存、延迟写入、压缩、GC |
| R6 | 分布式支撑 | SharedBlob/clone/snapshot 支持 PG 分裂/合并 |
| R7 | 可观测性与运维 | 性能计数、统计、告警、配置热更新 |
| R8 | 多设备管理 | BlueFS 集成、设备迁移/扩展、卷选择器 |

### 8.2 功能点统计

| 需求类别 | 总数 | MVP | P1 | P2 | Deferred |
| --- | --- | --- | --- | --- | --- |
| R1. 数据持久化 | 35 | 24 | 3 | 3 | 5 |
| R2. 事务语义 | 30 | 24 | 3 | 1 | 2 |
| R3. 空间管理 | 30 | 27 | 1 | 2 | 0 |
| R4. 数据完整性 | 20 | 11 | 8 | 1 | 0 |
| R5. 性能优化 | 35 | 9 | 15 | 5 | 6 |
| R6. 分布式支撑 | 13 | 1 | 4 | 0 | 8 |
| R7. 可观测性与运维 | 26 | 3 | 7 | 16 | 0 |
| R8. 多设备管理 | 12 | 5 | 0 | 0 | 7 |
| 合计 | 201 | 104 (52%) | 41 (20%) | 28 (14%) | 28 (14%) |

> 详细功能点清单见 §9。

## 9. 功能点详细清单

### 9.1 R1: 数据持久化（35 items）

| # | 功能点 | 优先级 | 说明 |
| --- | --- | --- | --- |
| 1 | `bluestore_bdev_label_t` | MVP | 设备标签（uuid, size, meta map） |
| 2 | `bluestore_cnode_t` | MVP | Collection 元数据（bits） |
| 3 | `bluestore_pextent_t` | MVP | 已实现（`blk/extent_types.h`） |
| 4 | `bluestore_extent_ref_map_t` | Deferred | extent 引用计数（ADR-04） |
| 5 | `bluestore_blob_use_tracker_t` | MVP | AU 级引用追踪 |
| 6 | `bluestore_blob_t` | MVP | Blob 元数据（去掉压缩/shared） |
| 7 | `bluestore_shared_blob_t` | Deferred | 共享 Blob（ADR-04） |
| 8 | `bluestore_onode_t` | MVP | 对象元数据（去掉 zone_offset_refs） |
| 9 | `bluestore_deferred_op_t` | MVP | 延迟写操作描述 |
| 10 | `bluestore_deferred_transaction_t` | P1 | 延迟事务描述 |
| 11 | `bluestore_compression_header_t` | Deferred | 压缩头（ADR-03） |
| 12 | `Buffer` | P1 | Buffer cache 条目 |
| 13 | `BufferSpace` | P1 | Buffer cache 管理 |
| 14 | `SharedBlob` | Deferred | 共享 Blob 运行时（ADR-04） |
| 15 | `SharedBlobSet` | Deferred | 共享 Blob 集合（ADR-04） |
| 16 | `Blob`（内存中） | MVP | 运行时 Blob 封装 + use_tracker |
| 17 | `Extent` | MVP | 逻辑偏移→blob 映射 |
| 18 | `OldExtent` | MVP | 旧 extent 释放追踪 |
| 19 | `ExtentMap` + shards | MVP | extent 管理 + 分片编码 |
| 20 | `GarbageCollector` | P2 | 压缩场景下的 GC |
| 21 | `Onode`（内存中） | MVP | 运行时对象元数据 |
| 22 | `OnodeSpace` | MVP | Onode 缓存（单 LRU） |
| 23 | `OnodeCacheShard` | P2 | 分片 Onode 缓存 |
| 24 | `BufferCacheShard` | P2 | 分片 Buffer 缓存 |
| 25 | `Collection` | MVP | PG 管理 |
| 26 | `WriteContext` | MVP | 写操作上下文 |
| 27 | `TransContext` | MVP | 事务上下文状态机（11 态） |
| 28 | `OpSequencer` | MVP | 事务排序器 |
| 29 | PREFIX_SUPER（`"S"`） | MVP | 超级块元数据前缀 |
| 30 | PREFIX_COLL（`"C"`） | MVP | Collection 元数据前缀 |
| 31 | PREFIX_OBJ（`"O"`） | MVP | Onode + extent shard 前缀 |
| 32 | PREFIX_DEFERRED（`"L"`） | MVP | 延迟写 WAL 前缀 |
| 33 | PREFIX_ALLOC_BITMAP（`"b"`） | MVP | 已实现（BitmapFM） |
| 34 | Key 编码函数集 | MVP | append_escaped, encode/decode |
| 35 | 生命周期管理 | MVP | mkfs/mount/umount |

### 9.2 R2: 事务语义（30 items）

| # | 功能点 | 优先级 | 说明 |
| --- | --- | --- | --- |
| 1 | `queue_transactions()` | MVP | 事务入口 |
| 2 | `_txc_add_transaction` | MVP | OSD 事务→BlueStore 分发 |
| 3 | `_txc_state_proc` | MVP | 状态机驱动 |
| 4 | `_txc_aio_submit` | MVP | AIO 提交 |
| 5 | `_txc_finish_io` | MVP | IO 保序 |
| 6 | `_txc_write_nodes` | MVP | onode→KV 写入 |
| 7 | `_txc_finalize_kv` | MVP | FM 分配/释放持久化 |
| 8 | `_txc_apply_kv` | MVP | KV 提交 + sync |
| 9 | `_txc_committed_kv` | MVP | KV 提交后回调 |
| 10 | `_txc_finish` | MVP | 事务清理 |
| 11 | `_txc_release_alloc` | MVP | 空间归还分配器 |
| 12 | `_do_write` | MVP | 写入分发入口 |
| 13 | `_do_write_small` | MVP | 小写入（≤1 AU，RMW） |
| 14 | `_do_write_big` | MVP | 大写入（多 AU 对齐） |
| 15 | `_do_alloc_write` | MVP | 空间分配 + 写入 |
| 16 | `_wctx_finish` | MVP | 写上下文完成 |
| 17 | `_do_read` | MVP | 读取主路径 |
| 18 | `_do_zero` | MVP | 零填充 |
| 19 | `_do_remove` | MVP | 对象删除 |
| 20 | `_do_truncate` | MVP | 截断 |
| 21 | `_setattr` / `_setattrs` | MVP | 属性设置 |
| 22 | `_rmattr` / `_rmattrs` | MVP | 属性删除 |
| 23 | `_rename` | MVP | 对象重命名 |
| 24 | `_collection_list` | MVP | 对象列举 |
| 25 | `_do_omap_set` | P1 | OMap 设置 |
| 26 | `_do_omap_get` | P1 | OMap 获取 |
| 27 | `_do_omap_rm` | P1 | OMap 删除 |
| 28 | `_do_gc` | P2 | GC（无压缩，仅整理） |
| 29 | `_do_clone_range` | Deferred | Clone 范围（ADR-04） |
| 30 | `_clone` / `_clone_range` | Deferred | Clone 操作（ADR-04） |

### 9.3 R3: 空间管理（30 items）

| # | 功能点 | 优先级 | 说明 |
| --- | --- | --- | --- |
| 1 | `bluestore_pextent_t` 结构 | MVP | 已实现 |
| 2 | `bluestore_blob_use_tracker_t` | MVP | AU 引用追踪 |
| 3 | `ExtentMap` 结构 | MVP | 逻辑→物理映射 |
| 4 | `extent_map_shards` | MVP | onode 分片索引 |
| 5 | `seek_lextent` | MVP | 查找逻辑 extent |
| 6 | `add` / `rm` extent | MVP | 增删 extent |
| 7 | `punch_hole` | MVP | 数据打孔 |
| 8 | `compress_extent_map` | MVP | 压缩 extent 映射 |
| 9 | `fault_range` | MVP | 按需加载 shard |
| 10 | `dirty_range` | MVP | 脏范围追踪 |
| 11 | `needs_reshard` | MVP | 重分片判断 |
| 12 | `reshard` | MVP | extent map 重分片 |
| 13 | `encode_some` / `decode` | MVP | extent 编解码 |
| 14 | Allocator 集成 | MVP | 分配器调用 |
| 15 | FreelistManager 集成 | MVP | FM 调用 |
| 16 | 空间分配路径 | MVP | alloc + FM mark_alloc |
| 17 | 空间释放路径 | MVP | FM mark_free + alloc release |
| 18 | `_pad_zeros` | MVP | 数据对齐填充 |
| 19 | extent 分裂 | MVP | blob split 操作 |
| 20 | extent 合并 | MVP | 相邻 extent 合并 |
| 21 | `_set_alloc_hint` | P1 | 分配提示 |
| 22 | 碎片整理 | P2 | 在线碎片整理 |
| 23 | 空间统计 | P2 | used/avail 统计 |
| 24 | extent_map_t 容器 | MVP | `std::set` |
| 25 | shard 编码格式 | MVP | onode 内 shard 编码 |
| 26 | shard 加载 | MVP | 从 KV 加载 shard |
| 27 | shard 持久化 | MVP | shard 写入 KV |
| 28 | 批量分配 | MVP | 多 extent 批量分配 |
| 29 | 过度分配处理 | MVP | overclaim 处理 |
| 30 | pending_release 管理 | MVP | 延迟释放队列 |

### 9.4 R4: 数据完整性（20 items）

| # | 功能点 | 优先级 | 说明 |
| --- | --- | --- | --- |
| 1 | `Checksummer` 枚举 | MVP | 仅保留 NONE + CRC32C |
| 2 | CRC32C 校验和计算 | MVP | ISA-L 已支持 |
| 3 | `_verify_csum` | MVP | 读取时校验验证 |
| 4 | `_generate_csum` | MVP | 写入时校验生成 |
| 5 | `bluestore_bdev_label_t` | MVP | 设备标签定义 |
| 6 | `_write_bdev_label` | MVP | 标签写入 |
| 7 | `_read_bdev_label` | MVP | 标签读取 |
| 8 | `_check_or_set_bdev_label` | MVP | 标签校验/创建 |
| 9 | 超级块读写 | MVP | superblock 持久化 |
| 10 | 崩溃恢复 | MVP | mount 时日志重放 |
| 11 | 数据一致性保证 | MVP | 写顺序 + sync |
| 12 | FSCK 主入口 | P1 | `fsck()` 函数 |
| 13 | FSCK onode 检查 | P1 | onode 完整性验证 |
| 14 | FSCK blob 检查 | P1 | blob 完整性验证 |
| 15 | FSCK extent 检查 | P1 | extent map 一致性 |
| 16 | FSCK freelist 检查 | P1 | freelist 一致性 |
| 17 | FSCK collection 检查 | P1 | collection 一致性 |
| 18 | FSCK repair 功能 | P1 | 自动修复 |
| 19 | FSCK cross-ref 验证 | P1 | 元数据↔分配交叉检查 |
| 20 | FSCK 统计报告 | P2 | 错误计数/报告 |

### 9.5 R5: 性能优化（35 items）

| # | 功能点 | 优先级 | 说明 |
| --- | --- | --- | --- |
| 1 | `Buffer` 类 | P1 | 缓存条目 |
| 2 | `BufferSpace` 类 | P1 | 缓存空间 |
| 3 | Buffer 状态管理 | P1 | empty/writing/reading/clean |
| 4 | Buffer LRU 管理 | P1 | 最近最少使用淘汰 |
| 5 | `_read_cache` | P1 | 缓存命中检查 |
| 6 | `_generate_read_result_bl` | MVP | 组装读取结果 |
| 7 | `_prepare_read_ioc` | MVP | 构建 AIO 读取 |
| 8 | `_choose_write_options` | Deferred | 写选项选择（简化版 MVP） |
| 9 | Deferred Write 核心 | MVP | `_do_deferred_write` |
| 10 | `_deferred_submit` | MVP | 延迟写提交 |
| 11 | `DeferredBatch` | P1 | 延迟写批处理 |
| 12 | `DeferredBatch::Op` | P1 | 批处理操作 |
| 13 | `_deferred_try_submit` | P1 | 尝试提交 |
| 14 | `_deferred_finished` | P1 | 完成回调 |
| 15 | `_deferred_aio_finish` | P1 | AIO 完成处理 |
| 16 | `_deferred_replay` | MVP | 崩溃恢复重放 |
| 17 | `BigDeferredWriteContext` | P2 | 大块延迟写 |
| 18 | `BlueStoreThrottle` | P1 | 提取到 common（Phase 2.5） |
| 19 | Throttle 字节限制 | P1 | `throttle_bytes` |
| 20 | Throttle deferred 限制 | P1 | `throttle_deferred_bytes` |
| 21 | Throttle 等待/唤醒 | P1 | 阻塞/唤醒机制 |
| 22 | `_do_gc` | P2 | GC 核心（无压缩） |
| 23 | GC 范围选择 | P2 | GC 候选范围 |
| 24 | GC extent 合并 | P2 | GC 合并操作 |
| 25 | 压缩集成点 | Deferred | 压缩/解压入口（ADR-03） |
| 26 | `_decompress` | Deferred | 解压函数（ADR-03） |
| 27 | 压缩头处理 | Deferred | compression_header（ADR-03） |
| 28 | 压缩提示 | Deferred | compress hint（ADR-03） |
| 29 | 压缩统计 | Deferred | compress stats（ADR-03） |
| 30 | 读取路径优化 | MVP | 直接 IO 读取 |
| 31 | 写入路径优化 | MVP | 直接写入块设备 |
| 32 | 缓存淘汰策略 | P1 | LRU/LFU 策略 |
| 33 | 预读取 | P2 | read-ahead |
| 34 | 写入合并 | MVP | 相邻写合并 |
| 35 | 批量提交 | MVP | KV 批量写入 |

### 9.6 R6: 分布式支撑（13 items）

| # | 功能点 | 优先级 | 说明 |
| --- | --- | --- | --- |
| 1 | `OldExtent` 释放追踪 | MVP | 旧 extent 引用释放 |
| 2 | `bluestore_extent_ref_map_t` | Deferred | 共享引用映射（ADR-04） |
| 3 | `SharedBlob` 内存管理 | Deferred | 共享 Blob 运行时（ADR-04） |
| 4 | `SharedBlobSet` | Deferred | 共享 Blob 集合（ADR-04） |
| 5 | `bluestore_shared_blob_t` | Deferred | 共享 Blob 持久化（ADR-04） |
| 6 | sbid 分配 | Deferred | 共享 Blob ID（ADR-04） |
| 7 | `_clone` | Deferred | 对象克隆（ADR-04） |
| 8 | `_clone_range` | Deferred | 范围克隆（ADR-04） |
| 9 | `_do_clone_range` | Deferred | clone range 操作（ADR-04） |
| 10 | Collection split | P1 | PG 分裂 |
| 11 | Collection merge | P1 | PG 合并 |
| 12 | split/merge 元数据更新 | P1 | cnode bits 更新 |
| 13 | split/merge onode 迁移 | P1 | onode 重分配 |

### 9.7 R7: 可观测性与运维（26 items）

| # | 功能点 | 优先级 | 说明 |
| --- | --- | --- | --- |
| 1 | 基础性能计数器 | P2 | 读写延迟/吞吐 |
| 2 | 扩展性能计数器 | P2 | 细分操作统计 |
| 3 | PREFIX_STAT（`"T"`） | P2 | 统计前缀 |
| 4 | Int64Array merge 统计 | P2 | merge operator 统计 |
| 5 | `volatile_statfs` | P2 | 运行时统计 |
| 6 | FSCK 统计计数 | P1 | 错误/警告计数 |
| 7 | FSCK 详细报告 | P1 | 分类报告输出 |
| 8 | FSCK 日志输出 | P1 | 检查过程日志 |
| 9 | FSCK 进度报告 | P2 | 进度百分比 |
| 10 | Error injection 框架 | P2 | 故障注入入口 |
| 11 | Error injection 写入 | P2 | 写入错误注入 |
| 12 | Error injection 读取 | P2 | 读取错误注入 |
| 13 | Error injection KV | P2 | KV 错误注入 |
| 14 | Error injection 设备 | P2 | 设备错误注入 |
| 15 | BSPerfTracker | P2 | 性能追踪器 |
| 16 | BlueFS perf counters | P2 | BlueFS 统计 |
| 17 | BlueStore 日志 | MVP | 结构化日志 |
| 18 | 事务追踪日志 | MVP | 事务生命周期日志 |
| 19 | IO 追踪日志 | MVP | IO 路径日志 |
| 20 | `md_config_obs_t` | P2 | 配置热更新（静态配置） |
| 21 | `osd_pools_map` | P2 | per-pool 统计 |
| 22 | 设备统计 | P2 | 设备读写统计 |
| 23 | 分配器统计 | P1 | 碎片/利用率 |
| 24 | 缓存统计 | P1 | 命中率/大小 |
| 25 | 事务统计 | P1 | 事务吞吐/延迟 |
| 26 | 空间使用统计 | P1 | used/avail/frag |

### 9.8 R8: 多设备管理（12 items）

| # | 功能点 | 优先级 | 说明 |
| --- | --- | --- | --- |
| 1 | 设备标签管理 | MVP | 多设备标签 |
| 2 | BlueFS 集成 | MVP | BlueFS 存 RocksDB |
| 3 | `RocksDBBlueFSVolumeSelector` | MVP | 卷选择器（已实现） |
| 4 | `_open_bluefs` / `_close_bluefs` | MVP | BlueFS 挂载/卸载 |
| 5 | 设备大小验证 | MVP | 设备容量检查 |
| 6 | 多设备空间分配 | Deferred | 跨设备分配 |
| 7 | 设备回退策略 | Deferred | WAL→DB→slow 回退 |
| 8 | `add_block_device` | Deferred | 添加块设备 |
| 9 | `remove_block_device` | Deferred | 移除块设备 |
| 10 | `set_volume_selector` | Deferred | 设置卷选择器 |
| 11 | Device migration | Deferred | 设备数据迁移 |
| 12 | 多设备统计 | Deferred | 每设备统计 |

## 10. 跨平台适配

cxxlab 需要同时运行在 x86\_64 和 AArch64 上。

### 10.1 平台差异影响

| 差异 | x86\_64 | AArch64 | 对 BlueStore 的影响 |
| --- | --- | --- | --- |
| 字节序 | Little-endian | Little-endian | 无影响 |
| 页大小 | 4KB（固定） | 4KB / 16KB / 64KB | `page.h` 已用 `sysconf(_SC_PAGESIZE)` 动态获取，无影响 |
| libaio | 完整支持 | 完整支持 | 无影响 |
| Direct IO 对齐 | 512B 或 4KB | 512B 或 4KB | 无影响（KernelDevice 已处理） |
| CRC32C 硬件加速 | SSE4.2 (PCLMULQDQ) | CRC 扩展指令 | ISA-L 已屏蔽差异，无影响 |
| 原子操作 | 8 字节原生 | 8 字节原生（LL/SC 或 LSE） | 无影响（std::atomic 已抽象） |

### 10.2 跨平台约束下的架构简化

| Ceph 优化 | cxxlab 处理 |
| --- | --- |
| `mempool` 内存池 | 移除，使用标准 new/delete |
| `ceph::shared_mutex` / `ceph::mutex` | 简化为 `std::mutex` / `std::shared_mutex` |
| `ceph::mono_clock` | 简化为 `std::chrono::steady_clock` |
| `ceph::condition_variable` | 简化为 `std::condition_variable` |
| `PerfCounters` + `BSPerfTracker` | 简化，需要时自行实现简单计数 |
| `Throttle` / `BlueStoreThrottle` | 提取到 common（Phase 2.5） |
| `Finisher` | 简化为 `std::thread` + `std::function` |
| `boost::intrusive` 容器 | 可保留（boost 跨平台），但简化为 `std::list`/`std::set` 优先 |
| `btree::btree_set` | 替换为 `std::set`/`std::map` |
| `WITH_LTTNG` / `WITH_BLKIN` | 移除 |

结论：ARM/x86 双平台对 BlueStore 核心逻辑无影响。主要适配工作是将 Ceph 自定义同步原语替换为 `std::` 标准库，移除 mempool/PerfCounters 等运行时框架，确保序列化使用 `cxxlab_le*` 类型。

## 11. MVP 核心能力（104 items）

MVP 阶段必须实现的功能点，按需求类别分组：

### R1 数据持久化（24 items）

- 核心 on-disk 类型：`bluestore_bdev_label_t`, `bluestore_cnode_t`, `bluestore_pextent_t`, `bluestore_blob_use_tracker_t`, `bluestore_blob_t`, `bluestore_onode_t`, `bluestore_deferred_op_t`
- 核心 in-memory 类型：`Blob`, `Extent`, `OldExtent`, `ExtentMap`, `Onode`, `OnodeSpace`, `Collection`, `WriteContext`, `TransContext`, `OpSequencer`
- KV 前缀：PREFIX_SUPER, PREFIX_COLL, PREFIX_OBJ, PREFIX_DEFERRED, PREFIX_ALLOC_BITMAP
- Key 编码函数集
- 生命周期管理（mkfs/mount/umount）

### R2 事务语义（24 items）

- 事务入口与状态机：`queue_transactions`, `_txc_add_transaction`, `_txc_state_proc`
- IO 管道：`_txc_aio_submit`, `_txc_finish_io`, `_txc_write_nodes`, `_txc_finalize_kv`, `_txc_apply_kv`, `_txc_committed_kv`, `_txc_finish`, `_txc_release_alloc`
- 核心操作：`_do_write`, `_do_write_small`, `_do_write_big`, `_do_alloc_write`, `_wctx_finish`, `_do_read`, `_do_zero`, `_do_remove`, `_do_truncate`, `_setattr`/`_setattrs`, `_rmattr`/`_rmattrs`, `_rename`, `_collection_list`

### R3 空间管理（27 items）

- ExtentMap 全功能：seek/add/rm/punch_hole/compress/fault_range/dirty_range/reshard/encode/decode
- 分配器/FM 集成：分配路径、释放路径、批量分配、过度分配处理
- Extent 操作：分裂、合并、pending_release 管理
- 容器选择：`std::set` 作为 extent_map_t

### R4 数据完整性（11 items）

- Checksummer（CRC32C）、校验和生成/验证
- 设备标签读写、超级块读写
- 崩溃恢复、数据一致性保证

### R5 性能优化（9 items）

- 读取路径：`_generate_read_result_bl`, `_prepare_read_ioc`
- Deferred Write 核心：`_do_deferred_write`, `_deferred_submit`, `_deferred_replay`
- 读写路径优化、写入合并、批量提交

### R6 分布式支撑（1 item）

- `OldExtent` 释放追踪

### R7 可观测性（3 items）

- BlueStore 日志、事务追踪日志、IO 追踪日志

### R8 多设备管理（5 items）

- 设备标签管理、BlueFS 集成、VolumeSelector、BlueFS 挂载/卸载、设备大小验证

## 12. 设计决策

### 12.1 已确认决策

| # | 决策项 | 结论 | 理由 |
| --- | --- | --- | --- |
| Q1 | `hobject_t` 简化程度 | 保留 hash/pool/nspace/key/oid/snap 全字段 | 兼容 Ceph key 格式 |
| Q2 | `extent_map_t` 容器 | `std::set` | 初始简单，后续按需优化 |
| Q3 | OnodeCache 实现 | 单 LRU（`std::list` + `unordered_map`） | 初始版本够用 |
| Q4 | BlueStore 编译产物 | 加入 `libbluestore.so` | 统一在 bluestore 库中 |
| Q5 | ghobject_t 放置位置 | `common/object.h` | BlueStore 核心类型，可能被其他模块引用 |
| Q6 | AIO 回调机制 | 保持现有 `aio_callback_t` | 已有成熟实现 |
| Q7 | Buffer Cache 时机 | P1 阶段实现 | MVP 直接从磁盘读，P1 添加缓存层 |

### 12.2 其他已确认决策

| 决策项 | 结论 |
| --- | --- |
| Throttle 提取 | 提取到 `common/` 作为通用限流组件（Phase 2.5），不绑定 BlueStore |
| Deferred Write | 纳入 MVP，简化实现（保留 WAL + 基本批处理，P1 完善） |
| FSCK | P1 全功能实现（onode/blob/extent/freelist/collection 检查 + repair） |
| OMap | P1 实现（set/get/rm + PREFIX_OMAP 前缀） |
| 压缩 | Deferred（ADR-03），所有压缩相关代码标记为 Deferred |
| SharedBlob/Clone | Deferred（ADR-04），所有 shared 相关代码标记为 Deferred |
| 多设备 | Deferred，MVP 仅支持单设备 |

### 12.3 已知待办

- [ ] Deferred write 路径（MVP）
- [ ] 压缩（Deferred）
- [ ] Null FM 模式启用（设计已完成，见 freelist-manager.md §3.4）
- [ ] Omap 操作（P1）
- [ ] Buffer cache 实现（P1）
- [ ] 碎片整理/GC（P2）

## 13. 参考

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
- 本项目 [docs/design/throttle.md](throttle.md): Throttle 设计
- 本项目 `bluestore/bluestore_types.h`: BlueStore 数据结构定义（Phase 3）
