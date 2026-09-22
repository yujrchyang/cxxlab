# blk — 块设备抽象层

> 实现状态：已实现（KernelDevice + libaio）

## 1. 概述

`blk` 模块为 Linux 上的直接 I/O 提供可移植的块设备抽象。它封装了原始块设备（如 NVMe、SSD），基于 `libaio` 实现了同步和异步读写接口。同时提供内存级空间分配器（Avl/Bitmap/Hybrid Allocator）和共享类型（`pextent_t`、`interval_set`）。

## 2. 需求分析

### 2.1 背景

BlueStore 和 BTier 都需要绕过内核 page cache，直接对裸块设备执行对齐 I/O。`blk` 模块提供统一的块设备抽象层，向上暴露同步/异步读写接口，向下封装 `libaio` 和 Linux `ioctl` 设备探测。

### 2.2 约束条件

| 维度 | 说明 |
| --- | --- |
| 操作系统 | 仅 Linux（x86\_64 + AArch64） |
| I/O 模式 | O\_DIRECT（默认）+ O\_RDWR（缓冲回退） |
| 异步引擎 | libaio（`io_setup` / `io_submit` / `io_getevents`） |
| 对齐要求 | Direct I/O：offset、length、buffer 均须与 `block_size`（通常 4KB）对齐 |
| 缓冲 I/O | 内核 page cache 处理对齐，跳过 `is_valid_io` 检查 |

### 2.3 核心功能

| 功能 | 说明 |
| --- | --- |
| 同步读 | `read(off, len, &bl)` / `read_random(off, len, buf)` |
| 同步写 | `write(off, bl)` — 支持 `pwritev` scatter/gather |
| 异步读 | `aio_read(off, len, &bl, ioc)` — 通过 libaio 提交 |
| 异步写 | `aio_write(off, bl, ioc)` — 通过 libaio 提交 |
| 批量提交 | `aio_submit(ioc)` — 将 pending 队列提交到内核 |
| 刷盘 | `flush()` — `fdatasync`，无写时为空操作 |
| 丢弃 | `discard(off, len)` — `ioctl(BLKDISCARD)` |
| 缓存失效 | `invalidate_cache(off, len)` — `posix_fadvise(DONTNEED)` |

## 3. 架构

```plaintext
┌───────────────────────────────────────────────────────────────┐
│                      BlockDevice (abstract base)              │
│  Sync: read / write / flush / read_random                     │
│  Async: aio_read / aio_write / aio_submit                     │
│  Mgmt: discard / invalidate_cache / collect_metadata          │
└──────────────┬────────────────────────────────────────────────┘
               │ inherit
┌──────────────▼────────────────────────────────────────────────┐
│                      KernelDevice                             │
│  - fd_direct_ (O_DIRECT | O_RDWR)                             │
│  - fd_buffered_ (O_RDWR)                                      │
│  - io_queue_ (aio_queue_t)                                    │
│  - aio_thread_ (completion reaping loop)                      │
└──────────────┬────────────────────────────────────────────────┘
               │ owns/uses
┌──────────────▼───────────┐   ┌────────────────────────────────────┐
│      IOContext           │   │     io_queue_t (abstract)          │
│  pending_aios / running  │   │  submit_batch / get_next_completed │
│  num_pending / num_run   │   │           │                        │
│  aio_wait / try_aio_wake │   │  ┌────────▼────────┐               │
└──────────────┬───────────┘   │  │  aio_queue_t    │               │
               │ include       │  │    (libaio)     │               │
┌──────────────▼───────────┐   │  └─────────────────┘               │
│         aio_t            │   └────────────────────────────────────┘
│  Wrap struct iocb        │
│  iov / bl / fd / priv    │
│  pwritev / preadv        │
│  boost::intrusive hook   │
└──────────────────────────┘
```

## 4. 组件详情

### 4.1 BlockDevice (`block_device.h` / `block_device.cc`)

抽象基类，提供：

- 属性：`size`、`block_size`、`optimal_io_size`、`rotational`、`support_discard`
- 校验：`is_valid_io(off, len)` — 检查与 `block_size` 的对齐及范围是否在 `size` 内
- 工厂：`BlockDevice::create(path, cb, priv)` 在当前平台始终返回 `KernelDevice`
- 回调：`aio_callback_t` 在回调模式下，一批 IO 完成时被调用

### 4.2 KernelDevice (`kernel_device.h` / `kernel_device.cc`)

具体实现，将块设备路径打开两次：

| 描述符 | 标志 | 用途 |
| --- | --- | --- |
| `fd_direct_` | `O_RDWR \| O_DIRECT \| O_CLOEXEC` | 直接 I/O（无缓冲） |
| `fd_buffered_` | `O_RDWR \| O_CLOEXEC` | 带缓冲 I/O 回退 |

打开时通过 `ioctl`（BLKSSZGET、BLKIOOPT、BLKROTATIONAL、BLKDISCARD）探测设备几何参数，并使用自适应 iodepth（`max(16, min(128, size/blocksize/4))`）初始化 `aio_queue_t`。

#### fd 双开策略

```plaintext
read/write (buffered 参数选择 fd)
  │
  ├── buffered=false → fd_direct_ (O_DIRECT)
  │     ├── is_valid_io() 对齐检查
  │     ├── write: bl.rebuild_aligned(block_size)
  │     └── read: buffer::create_aligned(len, block_size)
  │
  └── buffered=true  → fd_buffered_ (O_RDWR)
        └── 跳过 is_valid_io() (kernel page cache 处理对齐)

aio_read  → 始终使用 fd_direct_
aio_write → aio_ && !buffered && dio_ 时走 libaio (fd_direct_)
            否则回退到同步 write() (按 buffered 选 fd)
```

#### 对齐约束

```cpp
bool is_valid_io(uint64_t off, uint64_t len) const {
    return (off % block_size == 0) && (len % block_size == 0)
        && (off + len <= size);
}
```

- Direct I/O (`buffered = false`)：`is_valid_io` 检查 offset 和 length 与 `block_size` 对齐，且不越界
- 缓冲 I/O (`buffered = true`)：跳过 `is_valid_io`，内核 page cache 处理 misalignment
- Direct I/O 写入前调用 `bl.rebuild_aligned(block_size)` 确保 buffer 对齐
- Direct I/O 读取使用 `buffer::create_aligned(len, block_size)` 分配对齐缓冲

#### 内部回退标志

- `dio_`（默认 `true`）：设为 `false` 时回退到带缓冲 I/O
- `aio_`（默认 `true`）：设为 `false` 时回退到同步 `pread`/`pwritev`（而非 libaio）
- `write_hint`：`WRITE_LIFE_*` 枚举已在 `write()` / `aio_write()` 中接受，但目前未转发到内核（`fcntl(F_SET_RW_HINT)`）；此为一个已知缺陷

#### flush 优化

`flush()` 通过 `fdatasync(fd_direct_)` 实现。使用 `std::atomic<bool> io_since_flush_` 跟踪是否有写操作发生——若无写操作则 `flush()` 为空操作（通过 `compare_exchange_strong` 原子检测并跳过），避免不必要的 `fdatasync` 系统调用。`flush_mutex_` 保证同一时刻只有一个 `flush()` 在执行。

#### AIO 完成线程（`_aio_thread`）

```plaintext
_aio_thread (后台轮询循环, 50ms 间隔)
  │
  ├── io_queue_->get_next_completed(50, aios, kMaxReap=256)
  │     ├── io_getevents() 收割完成事件
  │     └── 超时 50ms 无事件 → 继续循环
  │
  └── for each completed aio_t:
        ├── io_since_flush_.store(true)
        ├── res = aio->get_return_value()
        ├── res < 0          → ioc->set_return_value(-EIO)
        ├── res != length    → ioc->set_return_value(-EIO) (部分写入视为错误)
        ├── res == length    → 正常完成
        │
        └── 通知模式:
              ├── 回调模式 (ioc->priv && aio_callback 已设置):
              │     num_running-- 归零时 → aio_callback(aio_callback_priv, ioc->priv)
              └── 等待模式:
                    ioc->try_aio_wake() → num_running-- 归零时 notify
```

### 4.3 IOContext (`io_context.h` / `io_context.cc`)

跟踪每个 IO 上下文中正在进行的操作：

| 状态 | 链表 | 计数器 |
| --- | --- | --- |
| 尚未提交 | `pending_aios` | `num_pending` |
| 已提交/正在进行 | `running_aios` | `num_running` |

- `aio_wait()` — 阻塞调用者（通过 `condition_variable`），直到 `num_running == 0`
- `try_aio_wake()` — 线程安全地减少 `num_running`；归零时通知
- `release_running_aios()` — 清空 `running_aios`（调用者必须保证无 IO 正在进行）

### 4.4 aio_t (`aio.h` / `aio.cc`)

封装单个 `struct iocb`（libaio）及其关联元数据：

- `io_prep_pwritev` / `io_prep_preadv` 构建 iocb
- `iov`：一个 `boost::container::small_vector<struct iovec, 4>` 用于 scatter/gather
- `bl`：持有 `bufferlist` 引用，确保异步写入期间数据存活
- 侵入式链表钩子（`boost::intrusive::list_member_hook<>`）支持 `aio_list_t` — 一种用于零分配批量跟踪的侵入式链表
- `rval` 存储完成结果，由收割循环通过 `reinterpret_cast<aio_t*>(event.obj)` 设置

### 4.5 io_queue_t / aio_queue_t (`aio.h` / `aio.cc`)

抽象接口：

- `init(fds)` — 设置提交队列
- `shutdown()` — 销毁
- `submit_batch(begin, end, priv, retries)` — 提交一组 IO；遇到 `EAGAIN` 时以指数退避重试
- `get_next_completed(timeout_ms, paio, max)` — 收割最多 `max` 个完成项

`aio_queue_t` 通过 `libaio`（`io_setup` / `io_submit` / `io_getevents` / `io_destroy`）实现上述接口。

### 4.6 extent_types.h

blk 模块的共享类型头文件，定义被 BlockDevice、Allocator、BlueStore、BTier 共同使用的物理 extent 和区间集合类型：

```cpp
struct pextent_t {
    uint64_t offset = 0;
    uint32_t length = 0;
};
using PExtentVector = std::vector<pextent_t>;

template <typename T>
class interval_set { /* ... */ };
```

`pextent_t` 表示一个物理设备的 (offset, length) 区间，`PExtentVector` 是 `allocate()` 的返回类型。`interval_set` 基于 `std::map<T, T>`（key=offset, value=length），`insert` 时自动合并相邻和重叠区间，用于 Allocator `release()` 接口和 BlueStore `txc->allocated` / `txc->released`。

## 5. I/O 生命周期

### 5.1 异步 I/O 路径（aio\_read / aio\_write）

```plaintext
aio_read/aio_write(off, len/bl, ioc)
  │
  ├── 1. is_valid_io(off, len) 对齐检查 (Direct I/O)
  │     失败 → 返回 -EINVAL
  │
  ├── 2. 若 aio_ && dio_ (aio_write 还需 !buffered):
  │     ├── 读: buffer::create_aligned(len, block_size) 分配对齐缓冲
  │     ├── 写: bl.rebuild_aligned(block_size) 对齐 bufferlist
  │     ├── 构造 aio_t, io_prep_preadv/pwritev 设置 iocb
  │     ├── 追加到 ioc->pending_aios (侵入式链表)
  │     └── ioc->num_pending++
  │
  └── 3. 否则回退到同步 read()/write()

aio_submit(ioc)
  │
  ├── 1. 若 num_pending == 0 → 直接返回
  ├── 2. pending_aios splice 到 running_aios
  │     num_running += num_pending, num_pending = 0
  ├── 3. io_queue_->submit_batch(begin, end, priv, &retries)
  │     ├── io_submit() 提交到内核 AIO 上下文
  │     └── 遇到 EAGAIN → 指数退避重试 (最多 16 次)
  └── 4. 失败 → ioc->set_return_value(r)

_aio_thread (后台轮询循环, 50ms 间隔)
  │
  ├── io_queue_->get_next_completed(50, aios, 256)
  │     ├── io_getevents() 收割完成事件
  │     └── 超时 50ms 无事件 → 继续循环
  │
  └── for each completed aio_t:
        ├── io_since_flush_ = true
        ├── res = aio->get_return_value()
        ├── res < 0 或 res != length → ioc->set_return_value(-EIO)
        ├── 回调模式: num_running-- 归零时 → aio_callback(priv, ioc->priv)
        └── 等待模式: ioc->try_aio_wake() → num_running-- 归零时 notify
```

### 5.2 同步 I/O 路径（read / write）

```plaintext
read(off, len, &pbl, buffered)
  │
  ├── buffered=false: is_valid_io(off, len) 对齐检查
  ├── 选择 fd: buffered ? fd_buffered_ : fd_direct_
  ├── buffer::create_aligned(len, block_size) 分配对齐缓冲
  └── pread(fd, buf, len, off) → pbl->push_back(buf)

write(off, bl, buffered, write_hint)
  │
  ├── buffered=false: is_valid_io(off, len) + bl.rebuild_aligned(block_size)
  ├── 选择 fd: buffered ? fd_buffered_ : fd_direct_
  ├── bl.prepare_iov(&iov) 构建 scatter/gather
  └── pwritev(fd, iov, off) 循环直到全部写入
        └── io_since_flush_ = true

flush()
  ├── flush_mutex_ 加锁
  ├── io_since_flush_.compare_exchange_strong(true, false)
  │     失败 (原值为 false) → 返回 0 (无写操作, 跳过 fdatasync)
  └── fdatasync(fd_direct_)
```

## 6. 构建

`CMakeLists.txt` 构建共享库 `libblk.so`，链接 `common` 和 `aio`（libaio）：

```cmake
add_library(blk SHARED
    aio.cc allocator.cc avl_allocator.cc bitmap_allocator.cc
    block_device.cc hybrid_allocator.cc io_context.cc kernel_device.cc
)
target_link_libraries(blk PUBLIC common aio)
```

## 7. 线程安全

| 资源 | 保护方式 | 说明 |
| ------ | --------- | ------ |
| `IOContext` 内部状态 | `std::mutex` + `std::condition_variable` | `aio_wait` 阻塞等待 `num_running == 0`；`try_aio_wake` 线程安全递减计数 |
| `aio_queue_t` (libaio) | libaio 内部 | `io_submit` / `io_getevents` 线程安全 |
| `KernelDevice` 设备 fd | 无锁 | `fd_direct_` / `fd_buffered_` 在 `open` 后只读，多线程并发 `aio_read`/`aio_write` 安全 |
| AIO 完成线程 | 单线程 `_aio_thread` | 后台轮询线程，50ms 间隔收割完成事件 |
| `io_since_flush_` | `std::atomic<bool>` + `flush_mutex_` | 原子标志跟踪写操作，`flush_mutex_` 串行化 `flush()` 调用 |

AIO 完成线程的完整流程见 §4.2。

## 8. 已知待办

- [ ] `write_hint`（`WRITE_LIFE_*`）目前未转发到内核（`fcntl(F_SET_RW_HINT)`），这是一个已知缺陷
- [ ] 自适应 iodepth 计算可进一步优化（当前 `max(16, min(128, size/blocksize/4))`）

## 9. 文件

| 文件 | 角色 |
| --- | --- |
| `block_device.h/cc` | 抽象 `BlockDevice` 基类 + 工厂 |
| `kernel_device.h/cc` | `KernelDevice` 实现 |
| `io_context.h/cc` | `IOContext` — 进行中 IO 跟踪器 |
| `aio.h/cc` | `aio_t`（单次操作）+ `aio_queue_t`（libaio 队列） |
| `extent_types.h` | `pextent_t`、`PExtentVector`、`interval_set` 共享类型 |

## 10. 参考

- Ceph source: `src/os/bluestore/BlockDevice.h` / `.cc`
- Ceph source: `src/os/bluestore/KernelDevice.h` / `.cc`
- 本项目 [docs/design/overview.md](overview.md): 架构总览
- 本项目 [docs/design/allocator.md](allocator.md): Allocator 设计
- 本项目 `blk/block_device.h`: BlockDevice 抽象接口
- 本项目 `blk/kernel_device.h`: KernelDevice 实现
- 本项目 `blk/aio.h`: aio_t + aio_queue_t
- 本项目 `blk/io_context.h`: IOContext
- 本项目 `blk/extent_types.h`: pextent_t / PExtentVector / interval_set
