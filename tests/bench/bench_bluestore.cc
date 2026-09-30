// Copyright 2024 cxxlab authors
// SPDX-License-Identifier: Apache-2.0

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "bluestore/bluestore.h"
#include "bluestore/bluestore_config.h"
#include "bluestore/trans_context.h"
#include "common/formatter.h"
#include "cxxlab_test.h"

using namespace TOPNSPC;
using clock_type = std::chrono::steady_clock;

namespace {

struct BenchConfig {
    uint64_t device_size = 64 * 1024 * 1024;
    uint64_t min_alloc = 65536;
    uint64_t cache_size = 0;
    uint64_t small_write_size = 4096;
    int small_write_count = 200;
    uint64_t big_write_size = 1 << 20;
    int big_write_count = 20;
    int read_count = 200;
    int mixed_ops = 200;
};

struct BenchResult {
    double p50_ns = 0;
    double p99_ns = 0;
    double throughput_bps = 0;
    uint64_t total_ops = 0;
    uint64_t total_bytes = 0;
};

std::string make_tmp_dir(const char *name) {
    auto tmpl = cxxlab_tmp_path(name);
    char *dir = ::mkdtemp(const_cast<char *>(tmpl.c_str()));
    if (!dir) {
        std::fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        std::exit(1);
    }
    return std::string(dir);
}

void create_block_file(const std::string &path, uint64_t size) {
    int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "open failed: %s\n", strerror(errno));
        std::exit(1);
    }
    int r = ::fallocate(fd, 0, 0, size);
    if (r < 0) {
        std::vector<char> zeros(65536, 0);
        for (uint64_t off = 0; off < size; off += zeros.size()) {
            ::pwrite(fd, zeros.data(),
                     std::min<uint64_t>(zeros.size(), size - off), off);
        }
    }
    ::close(fd);
}

BlueStoreConfig make_config(const std::string &store_path,
                            const BenchConfig &bcfg) {
    BlueStoreConfig cfg;
    cfg.path = store_path;
    cfg.db_path = store_path + "/db";
    cfg.bdev_path = store_path + "/block";
    cfg.min_alloc_size = bcfg.min_alloc;
    cfg.block_size = 4096;
    cfg.max_blob_size = 262144;
    cfg.allocator_type = "bitmap";
    cfg.buffer_cache_size = bcfg.cache_size;
    return cfg;
}

std::unique_ptr<BlueStore> setup_store(const std::string &store_path,
                                       const BenchConfig &bcfg) {
    ::mkdir((store_path + "/db").c_str(), 0755);
    create_block_file(store_path + "/block", bcfg.device_size);

    auto cfg = make_config(store_path, bcfg);
    auto store = std::make_unique<BlueStore>();
    if (store->mkfs(cfg) != 0 || store->mount(cfg) != 0) {
        std::fprintf(stderr, "store setup failed\n");
        std::exit(1);
    }
    return store;
}

void teardown_store(std::unique_ptr<BlueStore> &store,
                    const std::string &store_path) {
    if (store) {
        store->umount();
        store.reset();
    }
    std::filesystem::remove_all(store_path);
}

void write_op(BlueStore &store, CollectionRef coll, const ghobject_t &oid,
              uint64_t offset, uint64_t length, const std::string &data) {
    bufferlist bl;
    bl.append(data.data(), length);
    std::vector<BlueStoreTransaction> tls;
    BlueStoreTransaction bt;
    bt.write(oid, offset, length, bl);
    tls.push_back(std::move(bt));
    store.queue_transactions(coll, tls);
    coll->get_osr()->drain();
}

BenchResult compute_result(std::vector<uint64_t> &latencies_ns,
                           uint64_t total_bytes) {
    BenchResult r;
    r.total_ops = latencies_ns.size();
    r.total_bytes = total_bytes;

    if (latencies_ns.empty()) return r;

    std::sort(latencies_ns.begin(), latencies_ns.end());
    r.p50_ns = static_cast<double>(latencies_ns[latencies_ns.size() / 2]);
    r.p99_ns = static_cast<double>(latencies_ns[latencies_ns.size() * 99 / 100]);

    uint64_t total_ns = 0;
    for (auto ns : latencies_ns) total_ns += ns;
    if (total_ns > 0) {
        r.throughput_bps =
            static_cast<double>(total_bytes) * 1e9 / static_cast<double>(total_ns);
    }
    return r;
}

void print_result(const char *name, const BenchResult &r) {
    std::printf("[%s] %llu ops, %llu bytes\n", name,
                static_cast<unsigned long long>(r.total_ops),
                static_cast<unsigned long long>(r.total_bytes));
    std::printf("  latency   p50=%.1fus  p99=%.1fus\n",
                r.p50_ns / 1000.0, r.p99_ns / 1000.0);
    if (r.throughput_bps > 0) {
        std::printf("  throughput  %.1f MB/s\n", r.throughput_bps / (1024 * 1024));
    }
    std::printf("\n");
}

BenchResult bench_small_write(BlueStore &store, CollectionRef coll,
                              const BenchConfig &bcfg) {
    ghobject_t oid(1, 0, "", "bench_small", "", 0, 0);
    std::string data(bcfg.small_write_size, 'A');

    std::vector<uint64_t> latencies;
    latencies.reserve(bcfg.small_write_count);

    for (int i = 0; i < bcfg.small_write_count; ++i) {
        auto t0 = clock_type::now();
        write_op(store, coll, oid, i * bcfg.small_write_size,
                 bcfg.small_write_size, data);
        auto t1 = clock_type::now();
        latencies.push_back(
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count()));
    }

    return compute_result(latencies,
                          static_cast<uint64_t>(bcfg.small_write_count) *
                              bcfg.small_write_size);
}

BenchResult bench_big_write(BlueStore &store, CollectionRef coll,
                            const BenchConfig &bcfg) {
    ghobject_t oid(1, 0, "", "bench_big", "", 0, 0);
    std::string data(bcfg.big_write_size, 'B');

    std::vector<uint64_t> latencies;
    latencies.reserve(bcfg.big_write_count);

    for (int i = 0; i < bcfg.big_write_count; ++i) {
        auto t0 = clock_type::now();
        write_op(store, coll, oid, i * bcfg.big_write_size,
                 bcfg.big_write_size, data);
        auto t1 = clock_type::now();
        latencies.push_back(
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count()));
    }

    return compute_result(latencies,
                          static_cast<uint64_t>(bcfg.big_write_count) *
                              bcfg.big_write_size);
}

BenchResult bench_read_random(BlueStore &store, CollectionRef coll,
                              const BenchConfig &bcfg) {
    ghobject_t oid(1, 0, "", "bench_read", "", 0, 0);
    uint64_t obj_size = 4 * 1024 * 1024;
    std::string data(4096, 'C');

    uint64_t written = 0;
    while (written < obj_size) {
        uint64_t chunk = std::min<uint64_t>(4096, obj_size - written);
        write_op(store, coll, oid, written, chunk, data);
        written += chunk;
    }

    std::mt19937 rng(42);
    std::uniform_int_distribution<uint64_t> dist(0, obj_size - 4096);

    std::vector<uint64_t> latencies;
    latencies.reserve(bcfg.read_count);

    for (int i = 0; i < bcfg.read_count; ++i) {
        uint64_t off = dist(rng);
        bufferlist read_bl;

        auto t0 = clock_type::now();
        store.read(coll, oid, off, 4096, read_bl);
        auto t1 = clock_type::now();
        latencies.push_back(
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count()));
    }

    return compute_result(latencies,
                          static_cast<uint64_t>(bcfg.read_count) * 4096);
}

BenchResult bench_mixed(BlueStore &store, CollectionRef coll,
                        const BenchConfig &bcfg) {
    ghobject_t oid(1, 0, "", "bench_mixed", "", 0, 0);
    std::string write_data(4096, 'D');

    write_op(store, coll, oid, 0, 4096, write_data);

    std::mt19937 rng(123);
    std::vector<uint64_t> latencies;
    latencies.reserve(bcfg.mixed_ops);
    uint64_t total_bytes = 0;

    for (int i = 0; i < bcfg.mixed_ops; ++i) {
        if (i % 2 == 0) {
            auto t0 = clock_type::now();
            write_op(store, coll, oid, (i * 4096) % (256 * 1024), 4096,
                     write_data);
            auto t1 = clock_type::now();
            latencies.push_back(
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                        .count()));
            total_bytes += 4096;
        } else {
            bufferlist read_bl;
            auto t0 = clock_type::now();
            store.read(coll, oid, (i * 4096) % (256 * 1024), 4096, read_bl);
            auto t1 = clock_type::now();
            latencies.push_back(
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                        .count()));
            total_bytes += 4096;
        }
    }

    return compute_result(latencies, total_bytes);
}

void dump_perf(BlueStore &store) {
    JSONFormatter f;
    store.dump_perf_counters(&f);
    std::ostringstream os;
    f.flush(os);
    std::printf("%s\n", os.str().c_str());
}

void print_header(const BenchConfig &bcfg) {
    std::printf("=== BlueStore Benchmark ===\n");
    std::printf("Device: %lluMB  min_alloc: %lluKB  cache: %s\n\n",
                static_cast<unsigned long long>(bcfg.device_size / (1024 * 1024)),
                static_cast<unsigned long long>(bcfg.min_alloc / 1024),
                bcfg.cache_size > 0 ? "on" : "off");
}

}  // namespace

int main(int argc, char *argv[]) {
    BenchConfig bcfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto parse_int = [&](int &val) {
            if (++i < argc) val = std::atoi(argv[i]);
        };
        if (arg == "--small-count")
            parse_int(bcfg.small_write_count);
        else if (arg == "--big-count")
            parse_int(bcfg.big_write_count);
        else if (arg == "--read-count")
            parse_int(bcfg.read_count);
        else if (arg == "--mixed-ops")
            parse_int(bcfg.mixed_ops);
        else if (arg == "--help") {
            std::printf(
                "Usage: bench_bluestore [options]\n"
                "  --small-count N   small write op count (default 200)\n"
                "  --big-count N     big write op count (default 20)\n"
                "  --read-count N    random read op count (default 200)\n"
                "  --mixed-ops N     mixed op count (default 200)\n");
            return 0;
        }
    }

    std::string store_path = make_tmp_dir("bench_bluestore");
    auto store = setup_store(store_path, bcfg);

    auto coll = store->create_collection(1, 0);
    if (!coll) {
        std::fprintf(stderr, "create_collection failed\n");
        return 1;
    }

    print_header(bcfg);

    auto r1 = bench_small_write(*store, coll, bcfg);
    print_result("small_write", r1);

    auto r2 = bench_big_write(*store, coll, bcfg);
    print_result("big_write", r2);

    auto r3 = bench_read_random(*store, coll, bcfg);
    print_result("read_random", r3);

    auto r4 = bench_mixed(*store, coll, bcfg);
    print_result("mixed", r4);

    std::printf("=== Perf Counters ===\n");
    dump_perf(*store);

    teardown_store(store, store_path);
    return 0;
}
