# BTier — 块级自适应分层存储引擎

> 实现状态：已实现（Phase 4 A–C）

## 1. 需求分析

### 1.1 背景

BTier 是一个块级自适应分层存储引擎，在 FAST（如 NVMe）和 SLOW（如 HDD）两个设备之间自动迁移数据。热数据放置在 FAST 设备上以获得低延迟，冷数据降级到 SLOW 设备以释放 FAST 空间。

### 1.2 约束条件

| 维度 | 说明 |
| --- | --- |
| 操作系统 | 仅 Linux (x86\_64 + AArch64) |
| 设备 | 两个块设备：FAST + SLOW |
| 依赖 | `blk/`（BlockDevice + Allocator）、`common/`（bufferlist, denc, crc32, intarith） |
| 不依赖 | bluestore、kv、RocksDB — BTier 是独立的存储引擎 |

### 1.3 功能需求

| 功能 | 说明 |
| --- | --- |
| `put(key, value)` | 写入键值对，小值打包到共享 extent，大值独占 extent |
| `get(key)` | 读取值，通过 KeyMap 定位 extent + offset |
| `del(key)` | 删除键，标记 slot 为 dead |
| `sync()` | 持久化屏障：flush journal + fsync 设备 |
| 后台迁移 | 根据评分自动 promote（SLOW→FAST）/ demote（FAST→SLOW） |
| 后台压缩 | 回收 extent 内的 dead space |
| 崩溃恢复 | 从 WAL journal 重放恢复 KeyMap 和 ExtentMap |

## 2. 架构设计

### 2.1 整体架构

```plaintext
┌───────────────────────────────────────────────────────────────┐
│                         BtierEngine                           │
│  (orchestrator: init, put, get, del, sync, admin, recovery)   │
├──────────┬─────────────┬───────────────┬──────────┬───────────┤
│          │             │               │          │           │
│  ┌───────▼──────┐ ┌────▼─────────┐ ┌───▼──────────▼┐  ┌───────▼───────┐
│  │   KeyMap     │ │  ExtentMap   │ │ScoringEngine  │  │ MigrationEng. │
│  │  (key→       │ │  (extent→    │ │ (scoring +    │  │ (demote/      │
│  │   extent     │ │   location   │ │  weight       │  │  promote +    │
│  │  +offset)    │ │  + metrics   │ │  adaptation)  │  │  compact)     │
│  │  + reverse   │ │  + free_list │ │               │  │  + try_reloc  │
│  └───────┬──────┘ └──────┬───────┘ └──────┬────────┘  └───────┬───────┘
│          │               │                │                   │
│  ┌───────▼───────────────▼───────┐        │           ┌───────▼────────┐
│  │         Journal (WAL)         │◄───────┘           │  BlockDevice   │
│  │  (txn commit + checkpoint)    │                    │  (blk/)        │
│  └───────────────────────────────┘                    └────────────────┘
│
│  ┌──────────────────────────────────────────────────┐
│  │  Existing: blk/ (BlockDevice + Allocator) +      │
│  │  common/ (bufferlist, denc, crc32, intarith)     │
│  └──────────────────────────────────────────────────┘
└──────────────────────────────────────────────────────────────────────────┘
```

### 2.2 模块概览

| 模块 | 文件 | 深度 | 隐藏 |
| --- | --- | --- | --- |
| `BtierEngine` | `btier.h/cc` | 中 | 设备初始化、模块组装、I/O 分发、恢复编排 |
| `KeyMap` | `key_map.h/cc` | 深 | key→extent 映射、反向索引、per-key stride 追踪、持久化 |
| `ExtentMap` | `extent_map.h/cc` | 深 | extent→(location, metrics)、双层分配、MigrationHandle 状态转换、多键打包 |
| `ScoringEngine` | `scoring_engine.h/cc` | 深 | 4 维评分公式、权重自适应、watermark 逻辑 |
| `MigrationEngine` | `migration_engine.h/cc` | 深 | 异步迁移线程、3 步协议 (MigrationHandle)、压缩 |
| `Journal` | `journal.h/cc` | 中 | WAL 事务追加、checkpoint、恢复扫描 |
| `BtierConfig` | `config.h/cc` | 浅 | 权重默认值、extent 大小、阈值 |

### 2.3 构建组织

```plaintext
btier/
├── CMakeLists.txt
├── btier.h / btier.cc                  # BtierEngine (公共 API)
├── btier_types.h                       # DiskLocation, ExtentMetrics, ExtentHeader 等
├── config.h / config.cc                # BtierConfig, WeightSet
├── extent_map.h / extent_map.cc        # ExtentMap
├── key_map.h / key_map.cc              # KeyMap
├── scoring_engine.h / scoring_engine.cc    # ScoringEngine
├── migration_engine.h / migration_engine.cc  # MigrationEngine
├── journal.h / journal.cc              # WAL journal
└── btier_observer.h / btier_observer.cc  # 可观测性
```

构建为 `libbtier.so`，链接 `common`（PUBLIC）和 `blk`（PUBLIC）。不依赖 bluestore / kv / RocksDB。

## 3. 核心数据结构

### 3.1 btier_types.h

```cpp
namespace TOPNSPC::btier {

enum class Tier : uint8_t { FAST = 0, SLOW = 1 };

struct DiskLocation {
    uint64_t offset = 0;
    uint32_t length = 0;
    Tier     tier = Tier::FAST;
};

// 2 个操作状态：接受追加 / 正在迁移
enum ExtentState : uint32_t { ACTIVE = 0, MIGRATING = 1 };

// 16 字节 extent 指标，打包到 64 位原子字中：
//   bit 0-11: access_count | bit 12-23: write_count
//   bit 24-29: randomness  | bit 30-31: state
//   bit 32-63: generation (32 位，无实际 wrap-around 风险)
struct ExtentMetrics {
    std::atomic<uint32_t> last_access_time{0};
    std::atomic<uint64_t> raw{0};
    // ... pack/accessor 静态方法 ...
};

// 4KB 磁盘 extent 头：magic + CRC + generation
struct ExtentHeader {
    static constexpr uint64_t MAGIC = 0x4254494552535445ULL;  // "BTIERSTE"
    static constexpr uint32_t HEADER_SIZE = 4096;
    uint64_t magic;
    uint64_t extent_id;
    uint32_t length, used_bytes, live_bytes, reserved;
    uint64_t generation;
    uint32_t crc;         // CRC32C of first 40 bytes
    uint32_t pad[1013];   // pad to 4KB
};
static_assert(sizeof(ExtentHeader) == 4096);

struct KeyLocation {
    uint64_t extent_id = 0;
    uint32_t offset = 0;     // extent 数据区内的偏移
    uint32_t length = 0;
};

enum class IoOp { READ, WRITE };

}  // namespace TOPNSPC::btier
```

状态转换：

```plaintext
              create ──► ACTIVE ◄───────────────────────┐
                           │ migration starts           │
                           ▼                            │
                       MIGRATING ── commit ───────────►─┘
                           │
                           │ commit fail (gen changed)
                           ▼
                       ACTIVE (migration aborted, retry)

              live_bytes == 0 ──► FREE (removed, space deferred-free)
```

### 3.2 KeyMap

映射 `key → (extent_id, offset, length)`，维护反向索引（extent_id → keys）用于压缩时更新键位置。per-key stride 追踪用于随机性检测。

```cpp
class KeyMap {
public:
    bool lookup(const std::string &key, KeyLocation *loc) const;
    void put(const std::string &key, const KeyLocation &loc, uint64_t lba);
    void erase(const std::string &key);

    // 反向索引：返回 extent 中的所有键（压缩时使用）
    std::unordered_set<std::string> keys_in_extent(uint64_t extent_id) const;

    // 批量更新（压缩 commit 时原子更新所有键位置）
    void batch_update(const std::vector<std::pair<std::string, KeyLocation>> &updates);

    // per-key stride：0 = 随机，>0 = 顺序
    uint32_t get_consecutive_sequential(const std::string &key) const;

    void persist(Journal *journal);
    void recover(Journal *journal);
};
```

stride 追踪逻辑：每次 `put()` 记录 `last_lba`，若 `delta <= 64KB`（顺序阈值）则 `consecutive_sequential++`，否则归零。多键 extent 中不同键可有不同 stride 模式，per-key 追踪正确识别随机键。

### 3.3 ExtentMap

映射 `extent_id → ExtentEntry`，提供多键打包（`append_slot`）、无锁指标（atomic CAS）、基于 generation 的迁移中断协议（封装在 `MigrationHandle` 中）。

```cpp
class ExtentMap {
public:
    // 位置查询 (shared_lock)
    std::optional<DiskLocation> get_location(uint64_t extent_id) const;

    // 无锁指标更新 (CAS，不 bump generation)
    void record_io(uint64_t extent_id, IoOp op, uint32_t now);
    void set_randomness(uint64_t extent_id, uint32_t randomness);
    void refresh_randomness(const KeyMap &key_map);

    // 多键打包
    uint64_t find_extent_with_space(Tier tier, uint32_t needed_bytes) const;
    uint32_t append_slot(uint64_t extent_id, uint32_t size);  // bump gen
    void mark_dead_slot(uint64_t extent_id, uint32_t length); // bump gen

    // 分配 (FAST→SLOW fallback)
    std::optional<AllocResult> allocate_extent(Tier tier, uint64_t size);
    std::optional<DiskLocation> allocate_raw(Tier tier, uint64_t size);

    // 迁移协议 (generation 隐藏在 MigrationHandle 中)
    struct MigrationHandle {
        uint64_t extent_id;
        DiskLocation src_loc;
    private:
        friend class ExtentMap;
        uint64_t gen_before;  // 调用者不可访问
    };

    std::unique_ptr<MigrationHandle> begin_migration(uint64_t extent_id);
    bool commit_migration(MigrationHandle *h, const DiskLocation &new_loc);
    void abort_migration(MigrationHandle *h);
    bool check_migration(const MigrationHandle &h) const;

    // 生命周期
    void free(uint64_t extent_id);
    void release_source(const DiskLocation &loc);
    void process_deferred_free();  // 2-cycle grace period

    // 快照
    std::vector<SnapshotEntry> snapshot() const;
    double fast_watermark() const;
};
```

迁移协议关键点：

- `begin_migration`: 设状态为 MIGRATING，快照 `gen_before` + source location
- 数据复制期间不持有锁，I/O 路径并发运行
- `append_slot` / `mark_dead_slot` 在 `struct_lock` 下 bump generation
- `commit_migration`: 若 gen 未变 → 更新 location + bump gen + source 进入 deferred-free；若 gen 已变 → 返回 false（INTERRUPTED）
- `MigrationHandle::gen_before` 是 private 字段，调用者（MigrationEngine）永不直接接触 generation

### 3.4 ScoringEngine

4 维评分公式，权重自适应：

```cpp
class ScoringEngine {
public:
    float score(uint64_t raw_metrics, uint32_t current_time) const;
    void  adapt_weights(double fast_watermark);
    WeightSet current_weights() const;
};
```

评分公式：

```plaintext
Score = w1 * norm(recency) + w2 * norm(frequency)
      + w3 * norm(randomness) - w4 * norm(write_count)
```

| 权重 | 默认值 | 理由 |
| --- | --- | --- |
| `w_recency` | 0.35 | 最近访问是近期访问的最强预测因子 |
| `w_frequency` | 0.30 | 频率提供中期信号 |
| `w_randomness` | 0.25 | 随机 I/O 从 FAST 层获益最大 |
| `w_write_penalty` | 0.10 | 写惩罚（写密集 extent 倾向降级），权重低以避免抖动 |

权重自适应：

| watermark 区域 | 调整 |
| --- | --- |
| `< low_watermark (30%)` | 提升 recency (×1.2) 和 frequency (×1.1) → 更多 promote |
| `> high_watermark (80%)` | 放大 write_penalty (×pressure×2) 和 randomness (×pressure×1.5)，降低 recency → 加速 demote |
| 中间 | 使用基础权重 |

### 3.5 MigrationEngine

后台线程执行 3 类操作：promote（SLOW→FAST）、demote（FAST→SLOW）、compact（回收 dead space）。

```cpp
class MigrationEngine {
public:
    void start();
    void stop();
    void enqueue_migrate(uint64_t extent_id, Tier from, Tier to, float score);
    void enqueue_compact(uint64_t extent_id);
    struct Stats { /* promotions, demotions, compactions, interruptions, ... */ };
    Stats get_stats() const;
};
```

### 3.6 Journal

单设备 WAL，事务性崩溃恢复。KeyMap 和 allocator 状态从 FAST 设备上的 journal 恢复。

```cpp
class Journal {
public:
    uint64_t begin_txn();
    int append(uint64_t txn_id, const JournalRecord &rec);
    int commit_txn(uint64_t txn_id);  // CRC + 4K pad + 单次 write + fsync
    int checkpoint(const std::vector<JournalRecord> &full_state);
    std::vector<JournalRecord> recover();
    void sync();
    void trim();
    static constexpr uint64_t kJournalSize = 64 * 1024 * 1024;  // 64MB
};
```

事务格式：`BEGIN → records... → COMMIT(CRC)`，整体 4K 对齐，单次 `write()` 调用保证原子性。未 commit 的事务在恢复时丢弃。

循环缓冲区管理：64MB 循环缓冲区，<80% 正常，≥80% 异步 checkpoint+trim，≥95% 阻塞写入。

### 3.7 BtierConfig

```cpp
struct WeightSet {
    float w_recency = 0.35f;
    float w_frequency = 0.30f;
    float w_randomness = 0.25f;
    float w_write_penalty = 0.10f;
};

struct BtierConfig {
    std::string fast_dev_path;
    std::string slow_dev_path;
    uint64_t extent_size = 4 * 1024 * 1024;       // 4MB
    uint64_t block_size = 4096;
    uint64_t large_value_threshold = 2 * 1024 * 1024;  // 2MB
    WeightSet base_weights;
    double low_watermark = 0.30;
    double high_watermark = 0.80;
    uint32_t scan_interval_ms = 1000;
    float promote_threshold = 0.7;
    float demote_threshold = 0.3;
    double compaction_dead_ratio = 0.50;
    double compaction_usage_ratio = 0.80;

    static BtierConfig load(const std::string &path);  // JSON
    int save(const std::string &path) const;
};
```

## 4. 关键流程

### 4.1 多键 Extent 打包

小值（< `large_value_threshold`）打包到共享 extent，消除写放大：

```plaintext
put(key, value):
  if value.size() >= large_value_threshold:
    → 独占 extent: allocate_extent(FAST, size + HEADER_SIZE)
    → 写 ExtentHeader + value
  else:
    → find_extent_with_space(FAST, size)
    → 若找到: append_slot(extent_id, size) → offset
    → 若没找到: allocate_extent(FAST, extent_size) → append_slot → offset
    → 写 value 到 extent 数据区

  record_io(extent_id, WRITE, now)

  if key 已存在:
    → mark_dead_slot(old_extent_id, old_length)
    → if old extent live_bytes == 0: free(old_extent_id)

  → Journal 事务 (OP_EXTENT_NEW + OP_KEY_PUT + OP_MARK_DEAD)
  → KeyMap::put(key, {extent_id, offset, size}, lba)
```

写放大的改善：4KB 值仅写 4KB（追加到已有 extent）或 8KB（新 extent 含 4KB header），相比 always-COW 的 4MB/次，减少约 500x。

### 4.2 Per-Key Stride 随机性检测

每次写入键 K 时，在 KeyMap 中更新 `last_lba` 和 `consecutive_sequential` 计数器。评分前，`ExtentMap::refresh_randomness()` 遍历所有 extent，从 KeyMap 的 per-key stride 计算 per-extent randomness，通过 `set_randomness()` 写入 ExtentMetrics（CAS，不 bump generation）。

多键正确性：Key A（顺序，consecutive=10）和 Key B（随机，consecutive=0）共享 extent E。per-extent stride 会平均为"部分顺序" → 对 Key B 的假阴性。per-key stride 正确识别 Key B 为随机 → extent E 标记为随机 → promote 到 FAST。

### 4.3 迁移中断协议

```plaintext
MigrationEngine:                       Concurrent Append (Put):
  Step 1: begin_migration()            append_slot() on extent
   → struct_lock (exclusive)           → struct_lock (exclusive)
   → set state=MIGRATING               → state is MIGRATING → return UINT32_MAX
   → snapshot gen_before + src_loc     → (append redirected to another extent)
   → release struct_lock
                                       
  Step 2: Copy data to destination     → Reads still work (MIGRATING
   (no locks held — only I/O)            doesn't block reads)
                                       
  Step 3a: commit_migration(handle, new_loc)
   → struct_lock (exclusive)
   → check gen == handle.gen_before?
   → if yes: COMMITTED (update loc, bump gen, source→deferred-free)
   → if no: INTERRUPTED (abort, release dest)
```

为什么中断协议有意义：若迁移期间发生 `append_slot` 或 `mark_dead_slot`，generation 改变，`commit_migration` 检测到后中止。迁移副本缺少追加的数据——中止是正确的。无 generation 检查则会导致数据丢失。

### 4.4 Extent 压缩

触发条件：`dead_bytes / used_bytes > 0.5` 且 `used_bytes / capacity > 0.8`。

协议：

1. `begin_migration(extent_id)` → MigrationHandle
2. `keys_in_extent(extent_id)` → 获取所有活跃键（反向索引）
3. 读取源 extent 中的活跃值
4. 分配新 extent（同层）
5. 对每个活跃键：`append_slot(new_extent_id, size)` → 写值到新 extent
6. `check_migration(handle)` → 若 gen 变 → INTERRUPTED（abort + free new）
7. `batch_update` KeyMap: 所有键 → (new_extent_id, new_offset, length)
8. `free(old_extent_id)` → source space 进入 deferred-free

为什么压缩需要更新 KeyMap（而迁移不需要）：迁移复制整个 extent（相同数据、相同 offset、相同 extent_id），仅物理位置改变（存在 ExtentMap 中，对 KeyMap 透明）。压缩改变 offset（dead slot 移除，活跃数据紧凑排列），可能改变 extent_id — KeyMap 必须更新。

### 4.5 Deferred-Free 协议

`commit_migration` 成功后，源 location 不再是 extent 的 location，但可能仍有 in-flight 读。源空间不立即归还 allocator，而是进入 `deferred_free_` 列表，标记 seqno。`process_deferred_free()` 在每个迁移周期开始时调用，仅释放 aged ≥ 2 周期的条目（默认 ≥ 2 秒），确保所有 in-flight 读已完成。

### 4.6 MigrationEngine 主循环

每个周期（默认 1 秒）执行固定序列：

1. `process_deferred_free()` — 释放 aged 条目，为新分配腾出空间
2. Randomness refresh — 从 KeyMap per-key stride 计算 per-extent randomness
3. `adapt_weights(fast_watermark)` — 自适应权重
4. Score 所有 extent + 构建迁移队列
5. Enqueue tier migrations（score > promote_threshold → promote；score < demote_threshold → demote）
6. Enqueue compactions（dead_ratio + usage_ratio 满足条件）
7. `process_queue()` — 顺序执行迁移和压缩（每个迁移数据复制期间无锁）
8. Sleep `scan_interval_ms`

## 5. 并发模型

### 5.1 锁策略

| 资源 | 保护方式 | 说明 |
| --- | --- | --- |
| KeyMap map\_ + reverse\_index\_ | `shared_mutex` | 结构变更用 exclusive，lookup 用 shared |
| ExtentMap entries\_ | `shared_mutex` | 插入/删除用 exclusive，lookup 拷贝 shared_ptr 后释放 |
| Per-extent location/used/live | Per-entry `struct_lock` (shared\_mutex) | 读用 shared，append/migrate/compact 用 exclusive |
| Per-extent metrics (raw word) | `atomic<uint64_t>` | 无锁 CAS，I/O 读路径无锁竞争 |
| Free-space lists | `shared_mutex` | allocate/append/mark\_dead/free 时更新 |
| Allocators | Per-allocator `mutex` | AvlAllocator 内部锁，竞争罕见 |
| ScoringEngine weights | `shared_mutex` | adapt\_weights (exclusive, 罕见)，score (shared, 后台线程) |
| Journal | `mutex` | 串行追加 |
| Deferred-free list | `mutex` | commit\_migration/free 更新，process\_deferred\_free 排空 |

### 5.2 读路径 (Get)

```plaintext
get(key):
  1. KeyMap::lookup(key) → KeyLocation  (shared_lock, copy, release)
  2. ExtentMap::get_location(extent_id) → DiskLocation  (shared_lock, copy, release)
  3. ExtentMap::record_io(extent_id, READ, now)  (atomic CAS, no lock)
  4. ExtentMap::io_ref_inc(extent_id)  (atomic increment)
  5. BlockDevice::read(location.offset + HEADER_SIZE + offset, length, &value)
  6. ExtentMap::io_ref_dec(extent_id)
```

读路径不阻塞迁移：`get_location()` 返回当前 location（无论状态）。若状态为 MIGRATING，location 仍指向有效数据（源空间在 deferred-free 释放前有效）。

### 5.3 写路径 (Put)

```plaintext
put(key, value):
  Phase 1: 处理旧 key (if exists) — KeyMap::lookup
  Phase 2: 分配 + 写入新数据
    → 大值: allocate_extent(FAST, size+HEADER) → write header+value
    → 小值: find_extent_with_space → append_slot → write value
           (或 allocate_extent → append_slot → write)
  Phase 3: record_io(extent_id, WRITE, now)  (CAS)
  Phase 4: Journal 事务 (begin → OP_MARK_DEAD + OP_EXTENT_NEW + OP_KEY_PUT → commit+fsync)
  Phase 5: 提交内存状态 (mark_dead_slot + KeyMap::put)
```

始终追加（never in-place overwrite）：覆盖写分配新 slot 并标记旧 slot 为 dead，避免 read-modify-write 和 torn write。Dead slot 由压缩回收。

## 6. 持久化与恢复

### 6.1 持久化语义

- `put()` 返回前 journal 事务已 commit（fsync）。崩溃后 journal 重放恢复 key 映射。
- `sync()` 写入脏 ExtentHeader + fsync。非正确性必需（journal 足够），但加速恢复。
- `shutdown()` 调用 `sync()` → checkpoint journal → 关闭设备。

### 6.2 崩溃场景

| 崩溃点 | 恢复时状态 | 结果 |
| --- | --- | --- |
| `OP_TXN_COMMIT` 写入前 | journal 中部分事务 | 丢弃，无数据丢失 |
| COMMIT 写入后，数据写入前 | key 映射存在，extent 数据缺失 | ExtentHeader CRC 校验失败 → 标记 corrupt，移除 key |
| 数据写入后，header 更新前 | key 映射 + 数据存在，header 陈旧 | header used\_bytes 从 journal 重放重建 |
| `sync()` 后 | 全部持久 | 正常恢复 |

### 6.3 恢复流程

```plaintext
BtierEngine::init(config):
  1. 打开块设备 (fast + slow)
  2. 创建 allocators (FAST 标记 journal 区为 used)
  3. 初始化 ExtentMap (init_free_space)
  4. 打开 Journal
  5. Journal::recover() → 重放已 commit 的事务:
     → OP_EXTENT_NEW → 创建 ExtentEntry
     → OP_KEY_PUT → 填充 KeyMap
     → OP_KEY_DEL → 移除 KeyMap 条目
     → OP_MARK_DEAD → 更新 live_bytes
     → OP_EXTENT_FREE → 移除 ExtentEntry
  6. 标记 allocator: 对每个 extent → init_rm_free(offset, length)
  7. 验证 ExtentHeader CRC
  8. 启动 MigrationEngine
```

## 7. 简化与决策

### 7.1 关键决策

| 决策 | 原因 |
| --- | --- |
| 单设备 journal（无镜像） | 镜像增加复杂度但不保证原子性；单设备 + 事务 framing 更简单且正确 |
| 始终追加（never in-place） | 避免 read-modify-write 和 torn write；dead slot 由压缩回收 |
| 32 位 generation | 8 位有理论 wrap-around 风险；32 位需要 40 亿次迁移，实际不可能 |
| MigrationHandle 封装 generation | 调用者永不接触 gen\_before，减少误用 |
| Per-key stride（非 per-extent） | 多键 extent 中不同键有不同访问模式，per-extent 会产生假阴性 |
| 2-cycle deferred-free | 保证 in-flight 读完成（默认 ≥ 2 秒 grace period） |

### 7.2 已知待办

- [ ] 异步压缩线程池（当前在迁移线程中执行）
- [ ] 设备热替换

## 8. 文件

| 文件 | 角色 |
| --- | --- |
| `btier/btier.h` / `btier.cc` | `BtierEngine` 公共 API |
| `btier/btier_types.h` | `DiskLocation`、`ExtentMetrics`、`ExtentHeader`、`KeyLocation` 等数据结构 |
| `btier/config.h` / `config.cc` | `BtierConfig`、`WeightSet` |
| `btier/extent_map.h` / `extent_map.cc` | `ExtentMap` + `MigrationHandle` |
| `btier/key_map.h` / `key_map.cc` | `KeyMap` |
| `btier/scoring_engine.h` / `scoring_engine.cc` | `ScoringEngine` |
| `btier/migration_engine.h` / `migration_engine.cc` | `MigrationEngine` |
| `btier/journal.h` / `journal.cc` | `Journal` (WAL) |
| `btier/btier_observer.h` / `btier_observer.cc` | 可观测性 |

## 9. 参考

- Ceph source: `src/os/bluestore/BlueStore.cc`（deferred-free 释放协议参考，§4.5）
- 本项目 [docs/design/overview.md](overview.md): 架构总览
- 本项目 [docs/design/block-device.md](block-device.md): 块设备抽象层
- 本项目 [docs/design/allocator.md](allocator.md): Allocator 设计
- 本项目 `btier/btier.h`: BtierEngine 公共 API
- 本项目 `btier/btier_types.h`: 数据结构定义
- 本项目 [docs/dev-plan.md](../dev-plan.md): 开发计划（Phase 4: BTier）

## 10. 开发计划

BTier 的开发计划见 [dev-plan.md](../dev-plan.md) Phase 4，按接口边界分为 4 个阶段：

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| A (A1–A7) | 核心 I/O 路径（types → config → extent\_map → key\_map → journal → btier engine） | 已完成 |
| B (B1–B3) | 双层分配 + 评分引擎 | 已完成 |
| C1 (C1.1–C1.4) | 迁移 + 后台线程 + 可观测性 | 已完成 |
| C2 (C2.1–C2.2) | 压缩 + 端到端集成测试 | 已完成 |
