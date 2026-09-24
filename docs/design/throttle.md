# common/throttle — 资源节流基础库

> 实现状态：已实现

## 1. 设计目标

为 cxxlab 各组件提供统一的资源节流机制，防止资源耗尽导致系统崩溃或性能急剧下降。

核心概念：

- 资源计数：维护一个 `count`（当前已占用量）和 `max`（上限），获取时检查是否超限
- FIFO 公平性：采用 per-waiter condition variable 链表模式，保证先到先得
- 超大请求处理：`n > max` 时仅在 `count > max` 才等待，允许单次通过以防止死锁

## 2. 使用指南

### 文件位置

- `common/throttle.h` — 类声明
- `common/throttle.cc` — 类实现
- `tests/common/test_throttle.cc` — 20 个测试用例

### 核心 API

| 方法 | 说明 |
| ---- | ---- |
| `get(n)` | 阻塞获取 n 个资源（FIFO 公平排队） |
| `get_or_fail(n)` | 非阻塞尝试（检查容量 + 队列公平性） |
| `try_get(n, timeout)` | 带超时的尝试获取（cxxlab 扩展） |
| `take(n)` | 无条件获取（强制，count 可超过 max） |
| `put(n)` | 归还 n 个资源，唤醒队首等待者 |
| `reset_max(new_max)` | 动态调整上限 |
| `reset()` | 重置计数为 0，唤醒所有等待者 |
| `get_current()` / `get_max()` / `get_available()` | 查询状态 |
| `past_midpoint()` | count >= max/2 时返回 true |

### 使用示例

```cpp
// 创建节流器：最多 1GB 内存
Throttle mem_throttle(1024ULL * 1024 * 1024);

// 阻塞获取 64MB
mem_throttle.get(64 * 1024 * 1024);

// 非阻塞尝试
if (mem_throttle.get_or_fail(128 * 1024 * 1024)) {
    // 成功获取
} else {
    // 容量不足或有排队等待者
}

// 执行操作...

// 归还资源
mem_throttle.put(64 * 1024 * 1024);
```

## 3. 使用场景

### 3.1 BlueStore

| 节流器                     | 资源       | 上限                        | 说明                             |
| -------------------------- | ---------- | --------------------------- | -------------------------------- |
| `throttle_bytes`           | 内存       | 可配置（默认 512MB）        | 限制未提交事务占用的内存         |
| `throttle_deferred_bytes`  | 延迟写队列 | 可配置（默认 256MB）        | 限制延迟写批次大小               |

作用：

- 防止高并发写入时内存溢出
- 平衡写入吞吐与内存占用
- 触发延迟写刷盘（当 `throttle_deferred_bytes` 达到上限）

### 3.2 BTier

| 节流器                | 资源       | 上限                        | 说明                                   |
| --------------------- | ---------- | --------------------------- | -------------------------------------- |
| `migration_throttle`  | 迁移带宽   | 可配置（默认 100MB/s）      | 限制后台迁移速率，避免影响前台 IO      |

作用：

- 防止迁移风暴占用过多磁盘带宽
- 保证前台 IO 延迟可控

### 3.3 kv/RocksDB（未来）

| 节流器             | 资源       | 上限       | 说明                                           |
| ------------------ | ---------- | ---------- | ---------------------------------------------- |
| `write_throttle`   | 批量写入   | 可配置     | 限制批量写入速率，避免 RocksDB compaction 积压 |

作用：

- 平滑写入速率
- 避免 write stall

## 4. 测试覆盖

测试文件：`tests/common/test_throttle.cc`，共 20 个测试用例。

### 功能测试（12 个）

| 测试用例 | 验证点 |
| -------- | ------ |
| BasicGetPut | `get()`/`put()` 计数正确 |
| GetOrFailSuccess / InsufficientCapacity | 非阻塞尝试成功/失败 |
| TryGetSuccess / Timeout | 超时机制 |
| TakeUnconditional | `take()` 后 count 可超过 max |
| GetBlocksWhenFull | 超过 max 时阻塞 |
| ResetMax / ResetCount / ResetWakesWaiters | 重置行为 |
| PastMidpoint / GetAvailable | 查询正确性 |
| ConcurrentAccess | 多线程无数据竞争 |

### FIFO 公平性测试（4 个）

| 测试用例 | 验证点 |
| -------- | ------ |
| FIFOOrder | 3 个线程按顺序 get，按顺序被唤醒 |
| GetOrFailRespectsQueue | 有等待者时返回 false |
| OversizedRequestNoDeadlock | `n > max` 且 count=0 时立即返回 |
| OversizedRequestBlocksWhenOverMax | `n > max` 且 count>max 时阻塞 |

### 性能测试（3 个）

| 测试用例 | 验证点 |
| -------- | ------ |
| HighConcurrency | 100 线程并发无死锁 |
| ExhaustionAndRecovery | 资源耗尽→释放→恢复 |
| PutWakesBlockedRequest | put 后正确唤醒阻塞请求 |

## 5. 与 Ceph Throttle 的差异

Ceph `src/common/Throttle.h` 提供 5 个独立的 Throttle 变体：

| 变体                    | 用途                       | cxxlab |
| ----------------------- | -------------------------- | ------ |
| `Throttle`              | 基于槽位的阻塞式节流       | 实现   |
| `BackoffThrottle`       | 渐进式延迟注入             | 不实现 |
| `SimpleThrottle`        | 有界并发 + 错误追踪        | 不实现 |
| `OrderedThrottle`       | 有序完成回调               | 不实现 |
| `TokenBucketThrottle`   | 令牌桶速率限制             | 不实现 |

cxxlab `Throttle` 与 Ceph `Throttle` 的具体差异：

| 特性               | Ceph Throttle                                | cxxlab Throttle                              | 说明                       |
| ------------------ | -------------------------------------------- | -------------------------------------------- | -------------------------- |
| FIFO 公平性        | per-waiter CV 链表                           | 同上（复制此模式）                           | 保证先到先得               |
| 超大请求           | `_should_wait` 不对称逻辑                    | 同上（复制此模式）                           | 防止死锁                   |
| `get_or_fail`      | 有，检查队列公平性                           | 同上                                         | 非阻塞尝试                 |
| `take`             | 有，无条件获取                               | 同上                                         | 强制获取                   |
| `try_get(timeout)` | 无                                           | 新增                                         | cxxlab 扩展                |
| `past_midpoint`    | 有                                           | 同上                                         | 容量过半判断               |
| PerfCounters       | 可选（use_perf 参数）                        | 不包含                                       | 简化，按需添加             |
| 依赖               | `std::mutex` / `std::condition_variable`     | 同上                                         | Ceph 已使用 std 标准库     |
| 命名空间           | `ceph::`（无显式命名空间）                   | `TOPNSPC::`                                  | 统一命名空间               |
| `CephContext*`     | 构造函数需要                                 | 不需要                                       | 简化                       |

## 6. 参考

- Ceph 源码：`src/common/Throttle.h` / `Throttle.cc`
- 本项目 `docs/design/bluestore.md` — BlueStore 节流需求
- 本项目 `docs/design/btier.md` — BTier 迁移节流需求
