#pragma once

#include <cstdint>
#include <string>

namespace TOPNSPC {

struct BlueFSConfig {
    // 设备路径
    std::string wal_device_path;
    std::string db_device_path;
    std::string slow_device_path;

    // 分配
    uint64_t alloc_size = 1048576;         // 1MB，独占设备分配单元
    uint64_t shared_alloc_size = 1048576;  // 1MB，共享设备分配单元
    uint64_t max_log_runway = 4194304;     // 4MB，日志扩展量
    uint64_t min_log_runway = 1048576;     // 1MB，最小日志余量

    // 写入
    uint64_t min_flush_size = 524288;  // 512KB，触发刷写阈值
    bool buffered_io = false;
    bool sync_write = false;

    // 日志压缩
    uint64_t log_compact_min_size = 16777216;  // 16MB
    double log_compact_min_ratio = 2.0;        // 触发压缩的日志比

    // 读取
    uint64_t max_prefetch = 1048576;  // 1MB

    static BlueFSConfig load_from_file(const std::string &path);
};

}  // namespace TOPNSPC
