# Allocator — 内存级空闲空间分配器

> 实现状态：已实现（AvlAllocator + BitmapAllocator + HybridAllocator）

## 1. 需求分析

### 1.1 背景

在 BlueStore 单机引擎架构中，`Allocator` 负责运行时内存中的空间分配决策，
与 `FreelistManager`（持久化分配状态）构成双层空间管理：

| 组件 | 角色 | 存储位置 |
| --- | --- | --- |
| `Allocator` | 运行时的分配决策（返回空闲 extent） | RAM |
| `FreelistManager` | 分配状态的持久化追踪 | KV store (RocksDB) |

两者的关系：

```plaintext
IO 写入路径：
  alloc->allocate(want, unit, max_alloc_size, hint, &extents)  // 内存中标记已分配
  → bdev->write(extents, data)                                 // 写入设备
  → _txc_finalize_kv: fm->allocate(off, len, txn)              // 持久化到 KV

回收路径：
  _txc_release_alloc: alloc->release(txc->released)            // 内存中归还（延迟到所有前置 IO 完成后）
  → _txc_finalize_kv: fm->release(off, len, txn)               // 持久化到 KV

恢复路径：
  fm->enumerate_next() → alloc->init_add_free(offset, length)  // 从 FM 重建
```

### 1.2 约束条件

- 操作系统：仅 Linux（x86\_64 + AArch64）
- 后端存储：依赖 `kv::KeyValueDB` 抽象层（FreelistManager 的持久化后端）
- 最小分配单元 (alloc unit)：与 `FreelistManager::bytes_per_block` 一致（通常 4KB/16KB/64KB）
- 分配器类型：通过配置项选择（`bluestore_allocator`），可选 `"bitmap"` / `"avl"` / `"hybrid"`

### 1.3 核心功能

| 功能 | 说明 |
| --- | --- |
| `allocate(want, unit, max_alloc_size, hint, extents)` | 分配 `want` 字节，返回一个或多个 extent（`PExtentVector`）。`hint` 提示起始搜索位置 |
| `release(release_set)` | 归还空间，相邻 extent 自动合并 |
| `init_add_free(offset, length)` | 启动恢复时添加空闲区间 |
| `init_rm_free(offset, length)` | 启动恢复时移除指定区域（标记已分配） |
| `get_free()` | 查询剩余空闲空间总量 |
| `get_fragmentation()` | 碎片率评估 |
| `foreach(notify)` | 遍历所有空闲区间 |

### 1.4 与 FreelistManager 对比

| 维度 | Allocator | FreelistManager |
| --- | --- | --- |
| 存储位置 | RAM | KV store (RocksDB) |
| 粒度 | alloc unit | block (通常 = alloc unit) |
| 数据结构 | AVL tree / 3-level bitmap | RocksDB KV (XOR merge) |
| 分配/释放 | `allocate()` / `release()` | `allocate()` / `release()` |
| 恢复时 | `init_add_free()` 从 FM 重建 | `enumerate_next()` 扫描空闲区间 |
| 线程安全 | 内部 `std::mutex` 保护 | 无锁 (allocate/release 通过 merge 操作) |

## 2. 架构设计

### 2.1 Allocator 抽象基类

```plaintext
┌───────────────────────────────────────────────┐
│               Allocator (abstract)            │
│  allocate / release / init_add_free /         │
│  init_rm_free / get_free / foreach / dump     │
│  get_name / get_capacity / get_block_size     │
│  get_fragmentation / get_fragmentation_score  │
│  create(type, capacity, block_size)           │
└──────┬──────────────┬──────────────┬──────────┘
       │              │              │
       ▼              ▼              ▼
┌─────────────┐ ┌──────────┐ ┌───────────────┐
│ AvlAllocator│ │ Bitmap   │ │HybridAllocator│
│             │ │Allocator │ │ (Avl + Bitmap)│
│ offset AVL  │ │ 3-level  │ │               │
│ + size AVL  │ │ L0/L1/L2 │ │ spillover     │
│ first/best  │ │ 64bit op │ │ mechanism     │
│ fit policy  │ │ HW accel │ │               │
└─────────────┘ └──────────┘ └───────────────┘
```

工厂函数支持三种类型字符串：

| 类型字符串 | 实现类 | 说明 |
| --- | --- | --- |
| `"avl"` 或 `"stupid"` | `AvlAllocator` | AVL 区间树，默认分配器 |
| `"bitmap"` | `BitmapAllocator` | 三级位图 + 硬件加速 |
| `"hybrid"` | `HybridAllocator` | AvlAllocator + BitmapAllocator 子分配器 |

### 2.2 AvlAllocator 设计

#### 2.2.1 数据结构

两棵 `boost::intrusive::avl_set`：

```plaintext
range_tree (offset 排序):
  [0 ~ 1M] → [2M ~ 5M] → [8M ~ 10M] → ...

range_size_tree (size 排序):
  [8M ~ 10M](2M) → [0 ~ 1M](1M) → [2M ~ 5M](3M)
```

```cpp
struct range_seg_t {
    uint64_t start;
    uint64_t end;
    boost::intrusive::avl_set_member_hook<> offset_hook;
    boost::intrusive::avl_set_member_hook<> size_hook;
};
```

辅助结构：

- `lbas[64]`：按 size 的 highest power-of-2 分桶的 cursor 数组，每个桶记录上次分配位置
- `num_free`：内存中的总空闲字节数

#### 2.2.2 分配策略：First-fit + Best-fit 混合

```plaintext
_allocate(size, unit, &offset, &length):
  1. 获取 max_size = range_size_tree 中的最大区间长度
  2. 若 max_size < size → 降级到 max_size（若 max_size < unit 则返回 ENOSPC）
  3. 决策路径：
     a. force_range_size_alloc 或 max_size < threshold(128K) 或 free_pct < 4%:
        → 直接走 best-fit
     b. 否则：
        → first-fit: _pick_block_after(cursor, size, unit)
           - 从 cursor 位置开始在 range_tree 中顺序搜索
           - 搜索上限：max_search_count(100) / max_search_bytes(16MB)
           - 超过上限或找不到 → 降级到 best-fit
     c. best-fit: _pick_block_fits(size, unit)
        - 在 range_size_tree 中 lower_bound 找到 >= size 的最小区间
        - 若找不到 → size /= 2, 重复直到 size < unit
  4. _remove_from_tree(start, size) → 分裂/删除节点
```

#### 2.2.3 释放策略

```plaintext
_add_to_tree(start, size):
  1. 在 range_tree 中用 upper_bound 找到插入位置 rs_after
  2. 获取前驱节点 rs_before
  3. 尝试合并：
     - rs_before->end == start → 合并到前驱
     - rs_after->start == end → 合并到后继
     - 两边都满足 → 三合一
     - 都不满足 → 新建节点插入
  4. 插入 size_tree
  5. 若插入 size_tree 时达到 range_count_cap 上限：
     → 小于最小节点长度的 segment 被转移到派生类处理 (HybridAllocator::_spillover_range)
```

#### 2.2.4 配置参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `range_size_alloc_threshold` | 128KB | 最大连续空闲 < 该值时强制 best-fit |
| `range_size_alloc_free_pct` | 4% | 空闲率 < 该值时强制 best-fit |
| `max_search_count` | 100 | first-fit 最大搜索次数 |
| `max_search_bytes` | 16MB | first-fit 最大搜索字节数 |
| `range_count_cap` | 取决于 max_mem | AVL 树节点数上限 (0=无限制) |

### 2.3 BitmapAllocator 设计

#### 2.3.1 三级位图结构

BitmapAllocator 使用三级位图管理空闲空间，每一层以不同粒度聚合下一层的状态，实现快速 skip 全满/全空区域：

```plaintext
                          存储设备 (按 alloc unit 划分)
  L0:  ┌─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬ ...
       │1│1│0│1│0│1│1│1│ │1│1│1│1│1│1│1│1│ │0│0│0│0│0│0│0│0│ ...
       └─┴─┴─┴─┴─┴─┴─┴─┘ └─┴─┴─┴─┴─┴─┴─┴─┘ └─┴─┴─┴─┴─┴─┴─┴─┘
         uint64_t slot[0]    uint64_t slot[1]    uint64_t slot[2]   ...
         (64 bit, 每 bit = 1 AU)

        8 个连续 slot 组成 1 个 slotset (64 字节 = 1 cache line)
        └──────── 8 × uint64_t = 512 bits = bits_per_slotset ──────┘

  L1:  ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┬─ ...
       │  11  │  01  │  00  │  01  │  11  │  00  │  01  │  00  │
       └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘
       每 2 bit = 1 个 L1 条目，对应 1 个 L0 slotset (512 AU):
         00 (L1_ENTRY_FULL)    = slotset 内全部已分配
         01 (L1_ENTRY_PARTIAL) = slotset 内部分空闲
         11 (L1_ENTRY_FREE)    = slotset 内全部空闲

       32 个 L1 条目 打包在 1 个 uint64_t slot 中 (32 × 2 bit = 64 bit)

  L2:  ┌──────────────────────┬──────────────────────┬───────────────── ...
       │   0   │  1   │  1   │   0   │  1   │  1   │   1   │  0   │
       └──────────────────────┴──────────────────────┴─────────────────
       每 1 bit = 1 个 L2 条目，对应 32 个 L1 条目 (1 个 L2 slot = 64 个 L1 条目):
         0 = 该区域全部已分配 (无可用空间)
         1 = 该区域有可用空间 (跳过到 L1 进一步检查)
```

粒度与容量关系（以 alloc unit = 4KB 为例）：

| 层级 | 粒度 | 每个 slot 覆盖 | 实现 |
| --- | --- | --- | --- |
| L0 | 1 AU = 4 KB | 64 × 4 KB = 256 KB | `vector<uint64_t>`，每 bit 表示一个 AU (1=空闲，0=已分配) |
| L1 | 512 AU ≈ 2 MB | 32 × 2 MB = 64 MB | `vector<uint64_t>`，每 2 bit 编码一个 L0 slotset 状态 |
| L2 | 32 × 2 MB = 64 MB | 64 × 64 MB = 4 GB | `vector<uint64_t>`，每 1 bit 编码一个 L1 slot 中是否有可用空间 |

对齐优化：每层以 `slots_per_slotset = 8` 个 `uint64_t`（64 字节 = 1 cache line）为单位操作，最大化 cache 利用率。

`available` 计数器：`AllocatorLevel02` 中维护 `uint64_t available`，精确跟踪当前空闲字节数，`allocate()` / `release()` 在锁内原子更新。

#### 2.3.2 分配算法（L2 → L1 → L0 三级调用）

分配从顶层 L2 向下穿透到 L0，找到连续空闲 AU 后向上逐层更新位图：

```plaintext
BitmapAllocator::allocate(want, unit, max_alloc_size, hint, extents)
  │
  └─ AllocatorLevel02::_allocate_l2(want, unit, max_alloc_size, hint, &allocated, extents)
       │
       │  L2 层：两轮扫描 [hint→end) + [0→hint)，跳过全 0 slot
       │
       ├─ 对每个非空 L2 bit:
       │    │
       │    ├─ AllocatorLevel01Loose::_allocate_l1(rem, unit, max_length, l1s, l1e, &allocated, extents)
       │    │    │
       │    │    │  L1 层：遍历 32 个 L1 条目 (1 个 uint64_t slot)
       │    │    │
       │    │    ├─ L1_ENTRY_FULL  (00) → 跳过 (全分配)
       │    │    ├─ L1_ENTRY_FREE  (11) + 剩余 ≥ 整个 slotset → 整块分配，标记全 slotset
       │    │    └─ L1_ENTRY_PARTIAL (01) → 下钻到 L0:
       │    │         │
       │    │         └─ AllocatorLevel01Loose::_allocate_l0(rem, max_length, l0s, l0e, &allocated, extents)
       │    │              │
       │    │              │  L0 层：逐 uint64_t slot 扫描
       │    │              │
       │    │              ├─ slot == 0  (all_slot_clear) → 跳过
       │    │              ├─ slot == ~0 (all_slot_set)   → 取整个 slot (64 AU)
       │    │              └─ 否则 → 逐 bit 扫描 (find_next_set_bit / __builtin_ctzll)
       │    │                       找连续 1 bit 序列 → 确定连续空闲 AU
       │    │
       │    │  找到连续空闲 AU 后：
       │    │    ├─ _fragment_and_emplace(): 合并相邻 extent，按 max_length 切片
       │    │    ├─ _mark_alloc_l0(): L0 对应 bit 清 0
       │    │    └─ 回到 L1 → _mark_l1_on_l0(): 若整个 slotset 变满 → L1 条目改 00
       │    │
       │    └─ 回 L2: 若对应的 L1 slotset 全空 (全部已分配) → L2 对应 bit 清 0
       │
       └─ 分配完成后：available -= allocated
           返回实际分配字节数 (或 -ENOSPC)
```

硬件加速：L0 slot 内连续空闲位扫描使用 `__builtin_ctzll`（find first set）和 `__builtin_popcountll`，L1/L2 的跳过判断直接比较 `== 0` / `== ~0`，无逐位循环。

#### 2.3.3 释放算法（自下而上：L0 → L1 → L2）

释放反向遍历三级，先标记 L0，再向上聚合更新 L1、L2：

```plaintext
BitmapAllocator::release(release_set)
  │
  └─ AllocatorLevel02::_free_l2(release_set)
       │
       for each (offset, length) in release_set:
         │
         ├─ 1. AllocatorLevel01Loose::_free_l1(offset, length)
         │      │
         │      ├─ L0 层：_mark_free_l0(l0_start, l0_end)
         │      │    按 3 段式写入：头部半 slot (bit 操作) → 中间整 slot (整字写入 ~0)
         │      │    → 尾部半 slot (bit 操作)
         │      │
         │      └─ L1 层：_mark_l1_on_l0(l0_start_aligned, l0_end_aligned)
         │            对每个受影响的 L0 slotset:
         │              - agg_and (AND 聚合): 全 1 才保持全空闲 (11)
         │              - agg_or  (OR 聚合):  全 0 才变成全分配 (00)
         │              否则 → 部分空闲 (01)
         │
         ├─ 2. AllocatorLevel02::_mark_l2_free(l2_pos, l2_pos_end)
         │     受影响 L2 条目置 1 (标记该区域有可用空间)
         │
         └─ 3. available += released  (全局空闲计数器递增)
```

提取（claim-free）算法（供 HybridAllocator 回收 bitmap 中空闲块到 AVL）：

```plaintext
AllocatorLevel02::claim_free_to_left(offset):
  从 offset 向左扫描 L0 连续 1 bit，
  逐 bit 清 0 (标记为已分配)，
  更新 L1 聚合，更新 L2 位图，
  返回回收字节数。

claim_free_to_right(offset): 同理向右扫描，
  返回回收字节数。
```

### 2.4 HybridAllocator 设计

HybridAllocator 继承 AvlAllocator，内嵌一个 BitmapAllocator 作为 fallback：

```plaintext
                  AvlAllocator (primary)
                 /              \
          AVL tree          BitmapAllocator (fallback)
         (大块连续)         (小块零散)
```

#### 2.4.1 分配策略

```plaintext
allocate(want, unit, max_alloc_size, hint, extents):
  1. 尝试从 AVL 分配
  2. 若不够 → bitmap 补足
  3. 若 AVL 完全失败 → 释放已分配，回退到 bitmap
```

始终优先尝试 AVL 树，AVL 不能满足时回退到 bitmap。`_add_to_tree()` 认领回收（详见 §2.4.3）是主要的 bitmap → AVL 回流通道。

#### 2.4.2 Spillover 机制

```cpp
// AvlAllocator::_try_insert_range():
// 当 range_size_tree.size() >= range_count_cap 且新区间 < 树中最小节点时
_spillover_range(start, end):
  if (!bmap_alloc)
    bmap_alloc = new BitmapAllocator(capacity, block_size, name + ".fallback")
  bmap_alloc->init_add_free(start, size)  // 溢出到 bitmap
```

#### 2.4.3 合并时的回收机制

`HybridAllocator` 重写了 `_add_to_tree()`，在插入 AVL 树之前尝试从 bitmap child 中"认领"相邻的空闲区间，以增加合并成大块连续区间的概率：

```cpp
void HybridAllocator::_add_to_tree(uint64_t start, uint64_t size) {
    if (child_) {
        uint64_t head = child_->claim_free_to_left(start);
        uint64_t tail = child_->claim_free_to_right(start + size);
        start -= head;
        size += head + tail;
    }
    AvlAllocator::_add_to_tree(start, size);
}
```

### 2.5 hint 参数的使用

`hint` 参数的行为因分配器实现而异：

| 分配器 | hint 处理 | 说明 |
| --- | --- | --- |
| AvlAllocator | 忽略 | 使用 lbas cursor 替代 |
| BitmapAllocator | 使用 | `last_pos = align(hint / l2_granularity, d)`，设置搜索起始 L2 slot；两轮扫描 [hint→end] + [0→hint) |
| HybridAllocator | 透传 | 传递给内部 BitmapAllocator，AVL 路径忽略 |
| BlueStore 写路径 | 始终传 0 | `_do_alloc_write()` 中 `hint = 0` |
| BlueFS | 传末 extent 结尾 | 鼓励连续分配 |

## 3. 关键流程

### 3.1 启动恢复流程

```plaintext
BlueStore::_open_db_and_fm()
  │
  ├── db->open()                          // 打开 KV store
  ├── fm->init(kvdb, ...)                 // 加载 FM 配置
  │
  ├── alloc = Allocator::create(type, bdev->get_size(), min_alloc_size)
  │
  ├── if (!fm->is_null_manager()):
  │     fm->enumerate_reset()
  │     while (fm->enumerate_next(db, &offset, &length))
  │         alloc->init_add_free(offset, length)  // 重建空闲映射
  │     fm->enumerate_reset()
  │
  └── [null_manager 模式]:
        restore_allocator(alloc)           // 从 BlueFS 文件恢复
```

### 3.2 写入 IO 路径

```plaintext
BlueStore::_do_alloc_write()
  │
  ├── alloc->allocate(need, min_alloc_size, need, 0, &prealloc)
  │     └── 内存标记已分配
  │
  ├── for each write item:
  │     ├── 从 prealloc 中取 extent
  │     ├── txc->allocated.insert(off, len)
  │     └── bdev->write(off, data)
  │
  ├── _txc_finalize_kv(txc, txn):
  │     ├── fm->allocate(off, len, txn)     // 持久化
  │     └── fm->release(off, len, txn)
  │
  └── _txc_release_alloc(txc):
        └── alloc->release(txc->released)   // 延迟回收
```

### 3.3 null_manager 模式

当 FM 启用 null_manager 时，allocate/release 不写 KV store。但 Allocator 仍正常在内存中标记分配/释放。关闭时将 allocator 的完整状态写入 BlueFS 文件：

```plaintext
close():
  store_allocator(alloc) → BlueFS file "allocator_ncb"
```

## 4. 设计简化

| Ceph 实现 | cxxlab 处理方式 |
| --- | --- |
| `AdminSocketHook` debug 接口 | 移除（ASok 调试钩子，非核心） |
| `mempool` 内存监控 | 移除（仅统计用途） |
| `cct->_conf` 配置读取 | 改用构造函数参数或全局配置结构体 |
| `bluestore_types.h` 中 `bluestore_pextent_t` | 使用 `blk/extent_types.h` 中的等价类型 |
| ZonedAllocator / StupidAllocator | 不实现 |
| `_fragment_and_emplace` 中的 max_length 切片 | 保留（避免单 extent 过大） |
| `get_fragmentation_score` 算法 | 保留基础实现，用 `intarith.h` 中的 `clz` 替代平台相关内建函数 |

### 4.1 平台相关注意事项

| 特性 | x86\_64 | AArch64 |
| --- | --- | --- |
| `__builtin_ffsll` / `__builtin_popcountll` | 支持 | 支持 |
| cache line size | 64 bytes | 64 bytes (大多数) |
| `__builtin_clz` / `__builtin_ctz` | 支持 | 支持 |

三级位图（BitmapAllocator）中使用的 GCC 内建函数在 x86\_64 和 AArch64 上均有良好支持，无需额外抽象层。

## 5. 线程安全

所有分配器实现自包含 `std::mutex`，外部不额外加锁。关键路径：

| 操作 | 加锁方式 |
| --- | --- |
| `allocate` | 内部 lock，全路径持锁 |
| `release` | 内部 lock |
| `get_free` | 内部 lock（仅读 num_free） |
| `init_add_free` / `init_rm_free` | 内部 lock |
| `foreach` / `dump` | 内部 lock |

## 6. 与 FreelistManager 的关系

```plaintext
mkfs 时：
  alloc → 全空闲状态
  fm->create() → 初始化 KV 位图

挂载时：
  fm->enumerate_next() → alloc->init_add_free()  // 重建

运行时：
  IO 前：alloc->allocate()    // 内存决策
  IO 后：fm->allocate()       // 持久化

回收时：
  IO 完成：alloc->release()   // 内存归还（延迟）
           fm->release()      // 持久化
```

## 7. 依赖关系

Allocator 位于 `blk/` 目录，依赖：

- `common` 库：`bufferlist`、`cxxlab_assert`、`intarith` 工具函数
- `blk/extent_types.h`：`pextent_t`、`PExtentVector`、`interval_set`
- 无 `kv` / `rocksdb` 依赖（只操作内存）

## 8. 文件

| 文件 | 角色 |
| --- | --- |
| `allocator.h/cc` | `Allocator` 抽象基类 + 工厂 + `get_fragmentation_score()` |
| `avl_allocator.h/cc` | `AvlAllocator`（AVL 区间树） |
| `bitmap_allocator.h/cc` | `BitmapAllocator`（三级位图） |
| `hybrid_allocator.h/cc` | `HybridAllocator`（AVL + Bitmap fallback） |

## 9. 参考

- Ceph source: `src/os/bluestore/Allocator.h` / `.cc`
- Ceph source: `src/os/bluestore/AvlAllocator.h` / `.cc`
- Ceph source: `src/os/bluestore/BitmapAllocator.h` / `.cc`
- Ceph source: `src/os/bluestore/HybridAllocator.h` / `.cc`
- Ceph source: `src/os/bluestore/fastbmap_allocator_impl.h`（三级位图核心实现）
- Ceph source: `src/os/bluestore/BlueStore.cc`（`_create_alloc`, `_init_alloc`, `_do_alloc_write`, `_txc_finalize_kv`, `_txc_release_alloc`）
- 本项目 [docs/design/overview.md](overview.md): 架构总览
- 本项目 [docs/design/block-device.md](block-device.md): 块设备抽象层
- 本项目 [docs/design/freelist-manager.md](freelist-manager.md): FreelistManager 设计
- 本项目 `blk/allocator.h`: Allocator 抽象接口
- 本项目 `blk/extent_types.h`: pextent_t / PExtentVector / interval_set
