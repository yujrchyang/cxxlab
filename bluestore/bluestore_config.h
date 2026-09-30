#pragma once

#include <cstdint>
#include <string>

#include "common/common_fwd.h"

namespace TOPNSPC {

struct BlueStoreConfig {
    std::string path;
    std::string db_path;
    std::string bdev_path;

    uint64_t min_alloc_size = 65536;
    uint64_t block_size = 4096;
    uint64_t max_blob_size = 262144;        // 256KB
    uint64_t prefer_deferred_size = 65536;  // 64KB, writes smaller than this use deferred path
    uint8_t csum_type = 1;                  // CSUM_CRC32C

    std::string freelist_type = "bitmap";
    std::string allocator_type = "bitmap";

    uint64_t buffer_cache_size = 64 * 1024 * 1024;  // 64 MiB
    size_t onode_cache_size = 1024;

    bool create = false;
    bool force = false;

    double inject_read_err_rate = 0;
    double inject_write_err_rate = 0;
    double inject_kv_err_rate = 0;

    static BlueStoreConfig make_default(const std::string &path);
};

inline BlueStoreConfig BlueStoreConfig::make_default(const std::string &p) {
    BlueStoreConfig cfg;
    cfg.path = p;
    cfg.db_path = p + "/db";
    cfg.bdev_path = p + "/block";
    return cfg;
}

}  // namespace TOPNSPC
