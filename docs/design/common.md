# common — 基础库

> 实现状态：已实现（所有模块的基础依赖）

## 1. 概述

`common` 模块是整个 cxxlab 的基石库，提供序列化框架、缓冲区管理、校验和、UUID、断言和数学工具等基础能力。所有其他模块（`blk`、`kv`、`bluestore`、`btier`）均 PUBLIC 链接 `common`。

## 2. 架构

```plaintext
┌─────────────────────────────────────────────────────────────┐
│                      common (libcommon.so)                  │
│                                                             │
│  buffer_fwd.h ─── buffer.h / buffer.cc (bufferlist)         │
│       │                buffer_error.h                       │
│       │                deleter.h                            │
│       │                inline_memory.h                      │
│       │                page.h                               │
│       │                spinlock.h / spinlock.cc             │
│       │                crc32.h / crc32.cc                   │
│       └───────> denc.h (DENC serialization)                 │
│                  byteorder.h (little-endian types)          │
│                  uuid.h                                     │
│                  cassert.h / cassert.cc                     │
│                  intarith.h                                 │
│                  likely.h / scope_guard.h                   │
│                  safe_io.h / safe_io.cc                     │
│                  error.h / error.cc                         │
│                  formatter.h / formatter.cc                 │
│                  logger.h / logger.cc                       │
│                  cpu.h / cpu.cc                             │
│                  armor.h / armor.cc                         │
│                  common_fwd.h (TOPNSPC macro)               │
└─────────────────────────────────────────────────────────────┘
```

### 编译依赖

| 库 | 依赖 (PUBLIC) | 依赖 (PRIVATE) | 构建产物 |
| --- | --- | --- | --- |
| `common` | Boost::boost, isal | spdlog, fmt, pthread | libcommon.so |

> `HAVE_ISA_L=1` 编译宏为 PUBLIC，供 CRC32C 使用 Intel ISA-L 加速库。

## 3. 核心组件

### 3.1 TOPNSPC 宏与前置声明

`common_fwd.h`:

```cpp
#define TOPNSPC cxxlab
```

所有模块统一在 `cxxlab` 命名空间下。`common_fwd.h` 是最底层的头文件，被所有其他头文件包含。

`buffer_fwd.h` — bufferlist 前置声明与别名：

```cpp
namespace TOPNSPC::buffer { class ptr; class list; class hash; }
namespace TOPNSPC {
    using bufferptr = buffer::ptr;
    using bufferlist = buffer::list;
    using bufferhash = buffer::hash;
}
```

`buffer_fwd.h` 允许其他模块的头文件引用 `bufferlist` 类型名而不引入 `buffer.h` 的完整定义（减少编译依赖）。

### 3.2 bufferlist (`buffer.h` / `buffer.cc`)

零拷贝 scatter-gather 缓冲区管理器。三层结构：

```plaintext
raw  (底层内存块, 引用计数, CRC 缓存)
 │
 └─ ptr  (指向 raw 的 [offset, offset+len) 区间, 零拷贝切片)
     │
     └─ list  (ptr 的侵入式链表, 追加/遍历/CRC/IO)
```

#### 3.2.1 raw — 底层内存持有者

`buffer::raw` (`buffer.h`) 持有原始内存块：

- `data` + `len` — 内存指针和长度
- `nref` (`std::atomic<unsigned>`) — 引用计数，线程安全
- `last_crc_offset` / `last_crc_val` — CRC 缓存（避免重复计算）
- `crc_spinlock` — 保护 CRC 缓存的 spinlock
- `bptr_storage` — 嵌入式 `ptr_node` 存储（hypercombine 优化占位）

子类（不同内存后端）：

| 子类 | 内存来源 | 析构 | 用途 |
| --- | --- | --- | --- |
| `raw_malloc` | `malloc()` | `free()` | 通用堆分配 |
| `raw_posix_aligned` | `posix_memalign()` | `free()` | 对齐分配（Direct I/O） |
| `raw_combined` | `posix_memalign()` 一次分配 data + raw 对象 | `free(data)` | 高效分配，减少一次 malloc |
| `raw_char` | `new char[]` | `delete[]` | C++ 堆分配 |
| `raw_claimed_char` | 外部缓冲，不释放 | 空操作 | 包装外部内存 |
| `raw_static` | 静态缓冲，不释放 | 空操作 | 包装静态数据 |
| `raw_claim_buffer` | 外部缓冲 + 自定义 deleter | 调用 deleter | 释放回调 |

> `raw_combined` 是关键优化：将 `raw` 对象本身和数据区放在同一个 `posix_memalign` 调用中（data 在前，raw 对象在后），减少一次内存分配和一次指针跳转。

工厂函数：

```cpp
unique_leakable_ptr<raw> create(unsigned len);            // 默认 sizeof(void*) 对齐
unique_leakable_ptr<raw> create_aligned(unsigned len, unsigned align);
unique_leakable_ptr<raw> create_page_aligned(unsigned len);
unique_leakable_ptr<raw> create_small_page_aligned(unsigned len);  // <1 page 用 4K 对齐
unique_leakable_ptr<raw> copy(const char *c, unsigned len);       // 拷贝
unique_leakable_ptr<raw> create_static(unsigned len, char *buf);  // 静态
unique_leakable_ptr<raw> claim_char(unsigned len, char *buf);     // 不释放
unique_leakable_ptr<raw> claim_malloc(unsigned len, char *buf);   // malloc 缓冲
unique_leakable_ptr<raw> claim_buffer(unsigned len, char *buf, deleter del);  // 自定义释放
```

#### 3.2.2 ptr — 区间引用

`buffer::ptr` (`buffer.h`) 指向 `raw` 的一个 `[offset, offset+len)` 区间：

- `_raw` + `_off` + `_len` — 指向的 raw、起始偏移、长度
- 构造时 `nref++`，析构时 `nref--`，为 0 时 `delete raw`
- `clone()` — 深拷贝（分配新 raw 并复制数据）
- `iterator_impl<is_const>` — 支持 deep（拷贝）和 shallow（引用）两种迭代模式

#### 3.2.3 ptr_node 与 buffers_t — 侵入式链表

`ptr_node` (`buffer.h`) 继承 `ptr_hook` + `ptr`，作为侵入式链表节点：

- `ptr_hook` 有 `next` 指针，形成侵入式单向链表
- `cloner` — 拷贝构造新节点
- `disposer` — 析构节点（检查 hypercombine 优化，当前总是 `delete`）

`buffers_t` (`buffer.h`) 是自定义侵入式链表（非 `std::list`）：

- `_root` + `_tail` — 哨兵节点和尾指针
- `push_back` / `push_front` / `insert_after` / `erase_after` — O(1) 操作
- `splice_back` — O(1) 链表拼接
- `clone_from` — 深拷贝整个链表
- `clear_and_dispose` — 逐节点析构

> 使用侵入式链表而非 `std::list` 的原因：节点内存由 `ptr_node` 自身持有，避免额外的节点分配开销，且 `ptr_node` 可以在 `raw` 的 `bptr_storage` 中原地构造（hypercombine 优化占位）。

#### 3.2.4 list — bufferlist 主体

`buffer::list` (`buffer.h`)：

- `buffers_t _buffers` — ptr_node 侵入式链表
- `_carriage` — 指向最后一个 ptr_node，用于追加优化
- `_len` / `_num` — 总数据长度和 buffer 数量

追加优化 — `_carriage` 机制：

`append(data, len)` 优先写入 `_carriage` 的 `unused_tail`（同一个 raw 的尾部空闲空间），不足时 `refill_append_space()` 分配新 raw：

- 容量倍增策略： `2 x 前一个 raw 长度`，上限 `BUFFER_ALLOC_UNIT_MAX` (256KB)
- 新分配使用 `raw_combined`（data + raw 对象一次分配）
- `_carriage != &_buffers.back()` 时插入新的 ptr_node（复用同一个 raw 的不同区间）

三个 appender/filler 类：

| 类 | 用途 | 特点 |
| --- | --- | --- |
| `contiguous_appender` | DENC encode 使用 | `obtain_contiguous_space(len)` 预留连续空间，`get_pos_add(n)` 直接在连续内存上写入。支持 deep（拷贝）和 shallow（引用其他 bufferlist）模式 |
| `page_aligned_appender` | BlueFS/AIO 使用 | 按 `min_alloc` 页数分配，`_refill()` 分配新页对齐 buffer |
| `contiguous_filler` | 轻量填充器 | 仅持有 `char *pos`，`sizeof == sizeof(char*)`，`advance(n)` 和 `copy_in(len, src)` |

CRC 缓存三路逻辑 (`buffer.cc`)：

```cpp
uint32_t buffer::list::crc32c(uint32_t crc) {
    for (each node) {
        if (raw 有缓存 && CRC 匹配) → 直接用缓存值 (cache_hit)
        else if (raw 有缓存但 CRC 不同) → 增量计算:
            crc = cached_crc2 ^ calc_crc32(NULL, len, cached_crc1 ^ crc) (cache_adjust)
        else → 全量计算 + 缓存结果 (cache_miss)
    }
}
```

I/O 支持：

- `prepare_iov<VectorT>(&iov)` — 转换为 `struct iovec[]` 供 `writev`/`readv`
- `prepare_iovs()` — 返回 `iov_vec_t`，处理超过 `IOV_MAX` 的分批
- `write_fd(fd)` / `write_fd(fd, offset)` — 用 `writev`/`pwritev` 写入
- `read_fd(fd, len)` / `recv_fd(fd, len)` — 读取到新 buffer
- `pread_file(fn, off, len)` / `read_file(fn)` / `write_file(fn)` — 文件 I/O

其他操作：

- `rebuild()` / `rebuild_aligned(align)` / `rebuild_page_aligned()` — 重建为单 buffer 或对齐
- `reserve(prealloc)` — 预分配空间
- `append_hole(len)` — 追加未初始化空间（返回 `contiguous_filler`）
- `append_zero(len)` / `prepend_zero(len)` — 追加零
- `splice(off, len, claim_by)` — 分割/提取区间
- `substr_of(other, off, len)` — 子串
- `claim_append(other)` — O(1) 拼接（链表 splice）
- `share(other)` — 浅拷贝
- `static_from_mem/cstring/string` — 零拷贝静态包装
- `encode_base64` / `decode_base64` — Base64 编解码

```cpp
bufferlist bl;
bl.append("hello");                           // 追加数据
bl.append((const char*)&val, sizeof(val));   // 追加 POD
bl.rebuild_aligned(4096);                     // 对齐到 4KB（Direct I/O 用）

// 遍历
for (auto &node : bl.buffers()) {
    process(node.c_str(), node.length());
}

// scatter/gather I/O
std::vector<iovec> iov;
bl.prepare_iov(&iov);
::writev(fd, iov.data(), iov.size());
```

### 3.3 DENC 序列化框架 (`denc.h`)

基于模板 traits 的编译期序列化框架。通过 `denc(o, p)` 入口统一调度 encode/decode。

#### 设计原理

```plaintext
denc_traits<T>  defines:
  supported       — type has DENC support?
  bounded         — can compute encoded size at compile time?
  featured        — needs features parameter (versioned)?
  need_contiguous — decode requires contiguous memory?
       │
       ▼
denc(o, p)  top-level dispatch:
  denc(const T&, size_t&)                    → bound_encode (compute size)
  denc(const T&, contiguous_appender&)      → encode (write)
  denc(T&, const_iterator&)                  → decode (read)
       │
       ▼
is_const_iterator_v<It> determines encode vs decode:
  contiguous_appender (non-const) → encode
  ptr::const_iterator (const)     → decode
```

#### 内置支持类型

| 类别 | 类型 | 编码格式 |
| --- | --- | --- |
| 原始定长 | `le64/le32/le16`, `uint8_t`, `int8_t` | 直接写入 `sizeof(T)` 字节 |
| 整数 | `int16/32/64_t`, `uint16/32/64_t`, `bool` | 通过 `ExtType` 映射到 le 类型 |
| 字符串 | `std::string` | `uint32_t len` + 原始数据 |
| buffer | `buffer::ptr`, `buffer::list` | `uint32_t len` + 数据 |
| pair | `std::pair<A, B>` | 递归 denc first + second |
| tuple | `std::tuple<Ts...>` | 递归 denc 每个元素 |
| array | `std::array<T, N>` | 递归 denc 每个元素（无 count 前缀） |
| 容器 | `std::vector/list/set/map` | `uint32_t count` + 元素递归 |
| Boost 容器 | `flat_map/flat_set/small_vector` | 同上 |
| optional | `std::optional<T>`, `boost::optional<T>` | `bool has_value` + 值 |

#### 变长编码

DENC 框架内置四种变长编码，用于压缩小整数和地址：

| 编码 | 格式 | 用途 |
| --- | --- | --- |
| `denc_varint` | 每字节 7 bit 数据 + 1 bit continuation | 通用变长整数 |
| `denc_signed_varint` | 最低 1 bit 符号 + varint | 带符号变长 |
| `denc_varint_lowz` | 2 bit 低位零 nibble 数 + varint | 压缩末尾零（如 0x1000 → 3 字节） |
| `denc_lba` | 3 bit 低位零 nibble + 28-30 bit + continuation | LBA 地址优化（4K 对齐高频场景） |

#### traits 属性

| 属性 | 含义 | 影响 |
| --- | --- | --- |
| `supported` | 类型是否有 DENC 支持 | false 时 `denc()` 编译失败 |
| `bounded` | 定长类型，可编译期计算编码大小 | 容器 `bound_encode` 可用 `elem_size x count` |
| `featured` | 需要 features 参数（版本化特性） | 调用 `encode(o, p, features)` 而非 `encode(o, p)` |
| `need_contiguous` | decode 需要连续内存 | `decode()` 先 `copy_shallow` 到单 ptr 再解码 |

#### 自定义类型序列化

方式 A: DENC 宏（推荐，结构体内联）：

```cpp
struct Extent {
    uint64_t offset;
    uint32_t length;
    DENC(Extent, v, p) {
        DENC_START(1, 1, p);    // struct_v=1, compat_v=1
        denc(v.offset, p);
        denc(v.length, p);
        DENC_FINISH(p);
    }
};
```

`DENC_START(struct_v, compat_v, p)` 写入：`struct_v(1B)` + `compat_v(1B)` + `length(4B)`。
`DENC_FINISH(p)` 校验：`pos > end` 抛 `malformed_input`，`pos < end` 跳过剩余（向前兼容）。
长度前缀允许 decode 跳过未知字段（新版加字段时旧版仍可解码）。

方式 B: WRITE_CLASS_DENC（traits 特化）：

```cpp
struct Blob {
    uint64_t id;
    void encode(buffer::list::contiguous_appender &p) const { ... }
    void decode(buffer::ptr::const_iterator &p) { ... }
    void bound_encode(size_t &n) const { ... }
};
WRITE_CLASS_DENC(Blob);  // 在 TOPNSPC 命名空间内
```

方式 C: WRITE_CLASS_DENC_BOUNDED — 同上，但 `bounded=true`，表示定长类型。

#### 辅助函数

- `encode(v, bl)` — 计算 size + contiguous_appender + encode
- `decode(v, iter)` — 根据 `need_contiguous` 选择直接解码或先 copy_shallow
- `encode_nohead(v, bl)` / `decode_nohead(num, v, iter)` — 跳过 count 前缀（已知元素数量时）
- `encoded_sizeof(v)` / `encoded_sizeof_bounded<T>()` — 计算编码大小

#### has\_legacy\_denc 回退

`_denc::has_legacy_denc<T>` 检测类型是否有 `decode(buffer::list::const_iterator&)` 成员函数。若有，则 `denc(T&, buffer::list::const_iterator&)` 回退到调用 `T::decode(p)`，允许旧式解码方法与新 traits 共存。

#### 使用规则

- 简单 POD 字段（uint64/int32 等用于 FreelistManager meta）：直接 `bl.append((const char*)&v, sizeof(v))` / `p.copy(...)`，不需要 DENC
- 简单容器/vector/string：直接 `#include "common/denc.h"`，模板已内置支持
- 复合结构体（onode\_t、extent\_map 等含多成员 + map + vector）：用 `DENC` 宏或 `WRITE_CLASS_DENC`

### 3.4 CRC32C (`crc32.h` / `crc32.cc`)

```cpp
uint32_t calc_crc32(const uint8_t *data, size_t length,
                     uint32_t previous_crc = 0);
```

CRC-32C (Castagnoli 多项式) 校验：

- 多项式： `0x82F63B78`（Castagnoli，即 CRC-32C 的反转形式）
- 编译期查表： `constexpr CRC32Table` 在编译时生成 256 项查找表
- ISA-L 加速： `#ifdef HAVE_ISA_L` 时调用 `crc32_iscsi()`（基于 SSE4.2/PCLMUL 硬件指令）
- 软件回退：标准查表法（逐字节 `crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF]`）
- 增量计算： `previous_crc` 参数支持流式校验（分段计算）
- 空缓冲： `data == nullptr` 时计算全零缓冲的 CRC（用于空洞校验）

> 与 Ceph 的差异：Ceph 用 `ceph_crc32c_init()` 运行时检测 CPU 指令支持，cxxlab 直接用 ISA-L 库或软件回退。

用于 BlueFS 超级块校验、BlueStore blob 校验和、BTier ExtentHeader CRC。

### 3.5 uuid\_d (`uuid.h`)

```cpp
struct uuid_d {
    std::array<uint8_t, 16> uuid{};
    void generate();         // /dev/urandom + RFC 4122 v4
    bool is_zero() const;
    bool operator==(const uuid_d &o) const;
    bool operator!=(const uuid_d &o) const;
    bool operator<(const uuid_d &o) const;
    // DENC 序列化
};
```

RFC 4122 version 4 UUID:

- `generate()` 从 `/dev/urandom` 读取 16 字节
- 设置版本位： `uuid[6] = (uuid[6] & 0x0f) | 0x40`（version 4）
- 设置变体位： `uuid[8] = (uuid[8] & 0x3f) | 0x80`（variant 10）

用于 BlueFS 超级块（`uuid` + `osd_uuid`）和 BlueStore 设备标签。

### 3.6 断言 (`cassert.h` / `cassert.cc`)

```cpp
#define cxxlab_assert(expr)          // 致命断言 → abort
#define cxxlab_assertf(expr, ...)    // 带格式化消息的断言
#define cxxlab_abort(...)            // 主动终止 + 格式化消息
#define cxxlab_abort_msg(msg)        // 主动终止 + 固定消息
#define cxxlab_assert_warn(expr)     // 警告但不终止
```

- `cxxlab_assert` 是 `common_assert` 的别名，断言失败时调用 `__common_assert_fail` 打印文件/行号/函数名后 `abort()`
- `cxxlab_assertf` 支持类似 `printf` 的格式化消息
- `cxxlab_assert_warn` 仅打印警告，不终止程序
- 宏采用表达式形式（非 `do { } while(0)`），允许在 GTest 参数中使用
- `__PRETTY_FUNCTION__` / `__func__` 根据编译器支持自动选择

### 3.7 数学工具 (`intarith.h`)

模板化的编译期数学函数，全部 `constexpr inline`。使用 C++20 `<bit>` 头实现位计数函数。

基本舍入函数：

| 函数 | 说明 |
| --- | --- |
| `div_round_up(n, d)` | 向上取整除法 |
| `round_up_to(n, d)` | 向上对齐到 `d` 的倍数 |
| `round_down_to(n, d)` | 向下对齐到 `d` 的倍数 |
| `shift_round_up(x, y)` | `(x + (1<<y) - 1) >> y` |

2 的幂对齐函数（位运算，要求 `align` 为 2 的幂）：

| 函数 | 说明 |
| --- | --- |
| `isp2(x)` | 判断是否为 2 的幂 |
| `p2align(x, align)` | 2 的幂对齐 |
| `p2phase(x, align)` | 2 的幂取模 |
| `p2nphase(x, align)` | 负取模（`(-x) & (align-1)`） |
| `p2roundup(x, align)` | 2 的幂向上对齐 |

位计数函数（C++20 `<bit>`）：

| 函数 | C++20 实现 | 说明 |
| --- | --- | --- |
| `ctz(v)` | `std::countr_zero` | 尾零位数（trailing zeros） |
| `clz(v)` | `std::countl_zero` | 首零位数（leading zeros） |
| `cbits(v)` | `std::bit_width` | 有效位数 |
| `popcount(v)` | `std::popcount` | 置 1 位数 |

> `p2align` 等函数要求 `align` 为 2 的幂，使用位运算而非取模，性能更优。非 2 的幂对齐使用 `round_up_to` / `round_down_to`。

### 3.8 interval\_set (`common/interval_set.h`)

> `interval_set` 最初定义在 `blk/extent_types.h` 中，后提取为独立模块 `common/interval_set.h`，成为独立可测试的基础数据结构。提取过程中发现并修复了 `insert()` 中的合并 bug：当前驱区间完全包含新插入区间时（如 `[0, 20)` 包含 `[5, 10)`），原实现错误地将前驱截断而非保持完整。

```cpp
template <typename T>
class interval_set {
    void insert(T off, T len);   // 插入区间，自动合并相邻
    void erase(T off, T len);    // 移除区间
    bool empty() const;
    T range_start() const;
    T range_end() const;
    void insert(const interval_set &other);  // 合并两个集合
};
```

基于 `std::map<T, T>`（key=offset, value=length），`insert` 时自动合并相邻和重叠区间。用于 Allocator `release()` 接口和 BlueStore `txc->allocated` / `txc->released`。独立的 25 个单元测试覆盖了所有边界条件（相邻合并、重叠合并、包含合并、分割删除等）。

### 3.9 其他工具

#### safe\_io (`safe_io.h` / `safe_io.cc`)

EINTR 重试的 POSIX I/O 包装：

```cpp
ssize_t safe_read(int fd, void *buf, size_t count);
ssize_t safe_write(int fd, const void *buf, size_t count);
ssize_t safe_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t safe_pwrite(int fd, const void *buf, size_t count, off_t offset);
ssize_t safe_read_exact(int fd, void *buf, size_t count);    // 读不够返回 -EDOM
ssize_t safe_pread_exact(int fd, void *buf, size_t count, off_t offset);
int safe_write_file(const char *base, const char *file, ...);  // tmp+rename+fsync 原子写入
int safe_read_file(const char *base, const char *file, ...);
```

全部循环重试直到完成或非 EINTR 错误。`safe_write_file` 使用临时文件 + `fsync` + `rename` + 目录 `fsync` 的原子写入模式。

#### spinlock (`spinlock.h` / `spinlock.cc`)

C++20 `atomic_flag` + `wait()`/`notify_all()` 实现：

```cpp
void lock() {
    while (lock_.test_and_set(std::memory_order_acquire))
        lock_.wait(true, std::memory_order_relaxed);  // futex 等待，非忙等
}
void unlock() {
    lock_.clear(std::memory_order_release);
    lock_.notify_all();  // 唤醒等待者
}
```

非传统自旋锁——竞争时通过 `wait()` 进入系统级等待（futex），不烧 CPU。比 `std::mutex` 更轻量，用于 `buffer::raw` 的 CRC 缓存保护。

#### byteorder (`byteorder.h`)

小端序类型包装：

```cpp
template <typename T>
struct cxxlab_le {
    // native_to_little on assign, little_to_native on read
    // packed, direct byte-comparable
};
using cxxlab_le64 = cxxlab_le<__u64>;
using cxxlab_le32 = cxxlab_le<__u32>;
using cxxlab_le16 = cxxlab_le<__u16>;
```

`__attribute__((packed))` 保证无填充，可直接 `memcpy` 到 buffer。`operator==` 比较小端字节存储（等效于解码后比较）。DENC 框架对 `le64/le32/le16` 类型有直接特化（直接写入，无需转换）。

#### deleter (`deleter.h`)

引用计数删除器链，用于 `buffer::claim_buffer` 的自定义释放：

- `deleter::impl` — 引用计数 + 链式 `next`，析构时递归释放
- `raw_object_tag` — 裸指针低位标记优化（`ptr | 1`），避免分配 `impl` 对象
- `make_deleter(next, lambda)` — lambda 删除器
- `make_free_deleter(ptr)` — `free()` 删除器
- `make_object_deleter(obj)` — 对象持有删除器（析构时释放对象）
- `share()` — 引用计数 +1，返回共享副本
- `append(d)` — 链式追加（多删除器按序执行）

#### inline\_memory (`inline_memory.h`)

优化的内存操作函数：

- `maybe_inline_memcpy(dest, src, len, inline_len)` — `always_inline`，按长度选择 `__builtin_memcpy` 特化（1/2/3/4/8 字节）或 8 字节循环
- `mem_is_zero(data, len)` — x86\_64 使用 128-bit 宽度比较（`__attribute__((mode(TI)))`），非 x86\_64 使用 64-bit

`maybe_inline_memcpy` 是 bufferlist 追加路径的性能关键——小数据（1-8 字节）走编译期特化，避免 `memcpy` 函数调用开销。

#### 其他头文件

| 组件 | 文件 | 说明 |
| --- | --- | --- |
| `likely.h` | `likely.h` | `likely(x)`/`unlikely(x)`/`expect(x, hint)` 分支预测提示 (`__builtin_expect`) |
| `scope_guard.h` | `scope_guard.h` | RAII 守卫，析构时执行 lambda。支持 `dismiss()` 取消。CTAD 自动推导类型 |
| `page.h` | `page.h` | `page()` 返回 `page_info{size, mask, shift}`，运行时通过 `sysconf(_SC_PAGESIZE)` 获取，一次性初始化 |
| `buffer_error.h` | `buffer_error.h` | `buffer::error`/`end_of_buffer`/`malformed_input` 异常（继承 `std::runtime_error`） |
| `error.h` / `error.cc` | `error.h` / `error.cc` | `cpp_strerror(int err)` — errno 到字符串 |
| `formatter.h` / `formatter.cc` | `formatter.h` / `formatter.cc` | JSON 格式化输出 |
| `logger.h` / `logger.cc` | `logger.h` / `logger.cc` | spdlog 日志封装 |
| `cpu.h` / `cpu.cc` | `cpu.h` / `cpu.cc` | CPU 特性检测 |
| `armor.h` / `armor.cc` | `armor.h` / `armor.cc` | Base64 编解码（`armor`/`unarmor`） |
| `valgrind.h` | `valgrind.h` | Valgrind 注解宏（`ANNOTATE_HAPPENS_BEFORE` 等） |

## 4. 构建

`common` 编译为 `libcommon.so`（SHARED），是所有模块的基础依赖：

```cmake
add_library(common SHARED
    buffer.cc cassert.cc crc32.cc cpu.cc error.cc
    formatter.cc logger.cc safe_io.cc spinlock.cc armor.cc
)
target_link_libraries(common PUBLIC Boost::boost isal)
target_link_libraries(common PRIVATE spdlog fmt pthread)
target_compile_definitions(common PUBLIC HAVE_ISA_L=1)
```

## 5. 参考

- Ceph source: `src/common/buffer.h` / `buffer.cc`
- Ceph source: `src/common/denc.h`
- Ceph source: `src/common/crc32c.h` / `crc32c.cc`
- Ceph source: `src/common/intarith.h`
- Ceph source: `src/common/spinlock.h` / `spinlock.cc`
- Ceph source: `src/common/safe_io.h` / `safe_io.cc`
- 本项目 [docs/design/overview.md](overview.md): 架构总览
- 本项目 `common/buffer.h`: bufferlist 定义
- 本项目 `common/denc.h`: DENC 序列化框架
- 本项目 `common/intarith.h`: 数学工具函数
- 本项目 `blk/extent_types.h`: `pextent_t` / `PExtentVector` 定义
- 本项目 `common/interval_set.h`: `interval_set` 定义
