# BlueRocksEnv — RocksDB 文件操作适配层

> 实现状态：已实现（Phase 2.1–2.7）

## 1. 概述

BlueRocksEnv 是连接 BlueFS 和 RocksDB 的桥梁——它实现 `rocksdb::Env` 接口，将 RocksDB 的所有文件操作路由到 BlueFS，而将线程管理、定时等非文件操作委托给 POSIX `Env::Default()`。

### 1.1 约束条件

| 维度 | 说明 |
| --- | --- |
| 操作系统 | 仅 Linux (x86\_64 + AArch64) |
| RocksDB 版本 | v7.10.2 |
| 依赖 | BlueFS（文件操作后端）、POSIX Env（线程/定时委托） |

## 2. 架构

```cpp
class BlueRocksEnv : public rocksdb::EnvWrapper {
public:
    explicit BlueRocksEnv(BlueFS *fs);

    // Override: 文件操作 → BlueFS
    Status NewSequentialFile(...) override;
    Status NewRandomAccessFile(...) override;
    Status NewWritableFile(...) override;
    Status FileExists(...) override;
    Status GetChildren(...) override;
    Status DeleteFile(...) override;
    Status CreateDir(...) override;
    Status DeleteDir(...) override;
    Status GetFileSize(...) override;
    Status RenameFile(...) override;
    // ...

private:
    BlueFS *fs_;  // 指向 BlueFS 实例
};
```

## 3. 路径分发规则

```plaintext
NewSequentialFile("db/000123.sst")
    │
    ├── 首字符是 '/' ?
    │    YES → target()->NewSequentialFile(...)  // 绝对路径 → POSIX Env
    │
    └── NO  → split("db/000123.sst")
                │
                ├── dir = "db"
                ├── file = "000123.sst"
                └── fs_->open_for_read("db", "000123.sst", &h, false)
                      → BlueRocksSequentialFile(fs_, h)
```

关键规则：

- 绝对路径（以 `/` 开头）→ 转发到 POSIX `Env::Default()`，支持 `db_paths`、`wal_dir` 等指向外部目录
- 相对路径 → `split()` 解析为 `(dirname, filename)` 对，调用 BlueFS

`split()` 辅助函数：

```cpp
// "db/000123.sst" → (dir="db", file="000123.sst")
// "db/slow/000456.sst" → (dir="db/slow", file="000456.sst")
// "CURRENT" → (dir="", file="CURRENT")
std::pair<std::string_view, std::string_view> split(const std::string &fn);
```

`err_to_status()` 错误码转换：

```cpp
rocksdb::Status err_to_status(int r) {
    switch (r) {
    case 0:        return Status::OK();
    case -ENOENT:  return Status::NotFound();
    case -EINVAL:  return Status::InvalidArgument();
    default:       return Status::IOError(strerror(-r));
    }
}
```

## 4. 文件句柄类型

BlueRocksEnv 定义了 4 个内部类，将 BlueFS 句柄包装为 RocksDB 的抽象文件接口：

### 4.1 BlueRocksSequentialFile — 顺序读

| RocksDB 方法 | BlueFS 调用 | 说明 |
| --- | --- | --- |
| `Read(n)` | `fs_->read(h, pos, n, scratch)` | 从当前 buffer 位置读取 |
| `Skip(n)` | `h->buf.pos += n` | 推进逻辑偏移，无 IO |
| `InvalidateCache(off, len)` | `h->buf.invalidate()` + `fs_->invalidate_cache()` | 清除缓存 |

### 4.2 BlueRocksRandomAccessFile — 随机读

| RocksDB 方法 | BlueFS 调用 | 说明 |
| --- | --- | --- |
| `Read(offset, n)` | `fs_->read_random(h, offset, n, scratch)` | 直接随机读，不经过缓冲 |
| `GetUniqueId()` | `h->file->fnode.ino` | 返回 inode 编号作为文件唯一 ID |
| `Prefetch(offset, n)` | `fs_->read(h, offset, n)` | 触发 BlueFS 预取缓存 |
| `Hint(pattern)` | 调整 `max_prefetch` | 随机 → 4KB，顺序 → 配置值 |

### 4.3 BlueRocksWritableFile — 顺序写

| RocksDB 方法 | BlueFS 调用 | 说明 |
| --- | --- | --- |
| `Append(data)` | `fs_->append_try_flush(h, data, len)` | 追加到 buffer，超阈值时自动刷写 |
| `Flush()` | `fs_->flush(h)` | 刷写缓冲数据 |
| `Sync()` | `fs_->fsync(h)` | 刷写数据 + sync 元数据日志 |
| `Close()` | `fs_->fsync()` + `fs_->truncate()` | 写入完成，截断未使用的预分配空间 |
| `GetFileSize()` | `fnode->size + buffer.length()` | 包括未刷写的数据 |
| `GetUniqueId()` | `fnode.ino` | inode 编号 |
| `SetWriteLifeTimeHint(hint)` | `h->write_hint = hint` | 传递给 BlueFS 分配 hint |
| `RangeSync(off, n)` | `fs_->flush_range(h, off, n)` | 刷写指定字节范围 |
| `Allocate(off, len)` | `fs_->preallocate(h->file, off, len)` | 预分配磁盘空间 |

### 4.4 BlueRocksDirectory — 目录

```cpp
class BlueRocksDirectory : public rocksdb::Directory {
    Status Fsync() override {
        fs_->sync_metadata(false);  // flush BlueFS log
        return Status::OK();
    }
};
```

## 5. 操作实现

### 5.1 文件创建

| RocksDB 调用 | 实现 |
| --- | --- |
| `NewSequentialFile(fname)` | `split(fname)` → `fs_->open_for_read(dir, file, &h, false)` |
| `NewRandomAccessFile(fname)` | `split(fname)` → `fs_->open_for_read(dir, file, &h, true)` |
| `NewWritableFile(fname)` | `split(fname)` → `fs_->open_for_write(dir, file, &h, false)` |
| `ReuseWritableFile(old, new)` | `fs_->rename(old_dir, old_file, new_dir, new_file)` → `fs_->open_for_write(dir, file, &h, true)` |

### 5.2 目录与文件状态

| RocksDB 调用 | BlueFS 调用 |
| --- | --- |
| `FileExists(fname)` | `fs_->stat(dir, file, nullptr, nullptr)` |
| `GetChildren(dir)` | `fs_->readdir(dir, &result)` |
| `DeleteFile(fname)` | `fs_->unlink(dir, file)` + `fs_->sync_metadata(false)` |
| `CreateDir(dirname)` | `fs_->mkdir(dirname)` |
| `CreateDirIfMissing(dirname)` | `fs_->mkdir(dirname)` (忽略 `-EEXIST`) |
| `DeleteDir(dirname)` | `fs_->rmdir(dirname)` |
| `GetFileSize(fname)` | `fs_->stat(dir, file, &file_size, nullptr)` |
| `RenameFile(src, target)` | `fs_->rename(old_dir, old_file, new_dir, new_file)` + `fs_->sync_metadata(false)` |
| `LinkFile(src, target)` | 不实现（硬链接不支持），返回 `Status::NotSupported()` |
| `LockFile(fname)` | `fs_->lock_file(dir, file, &lock)` |
| `UnlockFile(lock)` | `fs_->unlock_file(lock)` |

### 5.3 日志与目录

| RocksDB 调用 | 实现 |
| --- | --- |
| `NewLogger(fname)` | 忽略文件名，返回 `BlueFSRocksdbLogger` → 日志输出到 stderr |
| `GetTestDirectory()` | 返回唯一名称如 `"temp_1"`、`"temp_2"` |
| `GetAbsolutePath(path)` | 返回 `"/" + path`（BlueFS 路径无根，加 `/` 满足 RocksDB 预期） |

## 6. BlueFSRocksdbLogger

RocksDB 内部日志通过 `NewLogger()` 创建的自定义 Logger 输出。`BlueFSRocksdbLogger` 继承 `rocksdb::Logger`，实现 `Logv()` 将日志输出到 stderr。工厂函数 `CreateRocksdbLogger()` 创建实例。

## 7. 可用性保证 (EnvMirror)

在开发和测试阶段，可以使用 `rocksdb::EnvMirror` 将操作同时发送到 BlueRocksEnv 和 POSIX Env，验证两者行为一致：

```cpp
bool mirror = config.enable_env_mirror;
if (mirror) {
    auto *blue_env = new BlueRocksEnv(bluefs);
    auto *posix_env = rocksdb::Env::Default();
    env = new rocksdb::EnvMirror(posix_env, blue_env, false, true);
} else {
    env = new BlueRocksEnv(bluefs);
}
```

## 8. 总结

| 组件 | 职责 | 文件 |
| --- | --- | --- |
| `BlueRocksEnv` | 实现 `rocksdb::Env`，路由文件操作到 BlueFS | `bluestore/blue_rocks_env.h/cc` |
| `BlueRocksSequentialFile` | 顺序读包装 | `bluestore/blue_rocks_env.cc` (内部类) |
| `BlueRocksRandomAccessFile` | 随机读包装 | `bluestore/blue_rocks_env.cc` (内部类) |
| `BlueRocksWritableFile` | 顺序写包装 | `bluestore/blue_rocks_env.cc` (内部类) |
| `BlueRocksDirectory` | 目录 Fsync 包装 | `bluestore/blue_rocks_env.cc` (内部类) |
| `BlueFSRocksdbLogger` | RocksDB 日志输出到 stderr | `bluestore/blue_rocks_env.cc` |

## 9. 参考

- Ceph source: `src/os/bluestore/BlueRocksEnv.h` / `.cc`
- Ceph source: `src/kv/RocksDBStore.h` / `.cc`（CephRocksdbLogger）
- 本项目 [docs/design/overview.md](overview.md): 架构总览（§9 库边界决策）
- 本项目 [docs/design/bluefs.md](bluefs.md): BlueFS 设计
- 本项目 `bluestore/blue_rocks_env.h`: BlueRocksEnv 实现

## 10. 依赖与封装策略

### 10.1 RocksDB 依赖泄漏

`BlueRocksEnv` 继承 `rocksdb::EnvWrapper`，导致 `blue_rocks_env.h` 必须 `#include "rocksdb/env.h"`。`bluestore` 库因此将 `RocksDB::RocksDB` 设为 PUBLIC 链接——所有链接 `libbluestore.a` 的目标都传递性依赖 RocksDB 头文件。

这与 `kv` 库形成对比：`kv` 库将 RocksDB 设为 PRIVATE（`kv/CMakeLists.txt:13`），因为 `key_value_db.h` 抽象基类不暴露任何 RocksDB 类型。

替代方案（未采用）：

| 方案 | 优势 | 劣势 | 未采用原因 |
| --- | --- | --- | --- |
| PIMPL 模式 | 隐藏 RocksDB 头文件 | 增加间接调用开销；需拆分 `BlueRocksEnv` 接口与实现 | BlueRocksEnv 仅被 BlueStore 使用，无多个消费者 |
| 独立 BlueRocksEnv `.so` | 隔离 RocksDB 依赖 | 增加构建复杂度；需处理符号可见性 | 当前 BlueRocksEnv 与 BlueStore 紧耦合 |

当前决策：保持 PUBLIC 依赖，在 [overview.md](overview.md) §9.2 (ADR-11) 中记录权衡。未来若有第三个消费者（非 BlueStore）需要使用 BlueRocksEnv，应重新评估 PIMPL 或独立库方案。

### 10.2 错误处理边界

`err_to_status()` 将 POSIX errno（BlueFS 返回的负值错误码）转换为 `rocksdb::Status`。未覆盖的错误码统一映射为 `Status::IOError(strerror(-r))`。关键映射：

| BlueFS 返回值 | RocksDB Status | 说明 |
| --- | --- | --- |
| `0` | `Status::OK()` | 成功 |
| `-ENOENT` | `Status::NotFound()` | 文件/目录不存在 |
| `-EINVAL` | `Status::InvalidArgument()` | 参数错误 |
| `-EEXIST` | `Status::OK()` (忽略) | `CreateDirIfMissing` 忽略已存在 |
| 其他负值 | `Status::IOError(strerror(-r))` | 统一 IO 错误 |
