#include "btier/migration_engine.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>

#include "blk/block_device.h"
#include "btier/btier_observer.h"
#include "btier/btier_types.h"
#include "btier/extent_map.h"
#include "btier/key_map.h"
#include "btier/scoring_engine.h"
#include "common/buffer.h"

namespace TOPNSPC::btier {

namespace {

struct MigrationTask {
    enum class Type { TIER_MIGRATE,
                      COMPACT } type;
    uint64_t extent_id;
    Tier from;
    Tier to;
    float score;
};

}  // anonymous namespace

struct MigrationEngine::Impl {
    ExtentMap *extent_map;
    KeyMap *key_map;
    ScoringEngine *scoring_engine;
    BlockDevice *fast_dev;
    BlockDevice *slow_dev;
    BtierObserver *observer;
    const BtierConfig &cfg;

    std::atomic<bool> running{false};
    std::thread worker;

    mutable std::mutex queue_lock_;
    std::condition_variable queue_cv_;
    std::queue<MigrationTask> queue_;

    // Stats use atomics for thread-safe access without locking
    std::atomic<uint64_t> promotions_committed{0};
    std::atomic<uint64_t> demotions_committed{0};
    std::atomic<uint64_t> compactions_committed{0};
    std::atomic<uint64_t> interruptions{0};
    std::atomic<uint64_t> io_errors{0};

    Impl(ExtentMap *em, KeyMap *km, ScoringEngine *se,
         BlockDevice *fd, BlockDevice *sd, const BtierConfig &c,
         BtierObserver *obs)
        : extent_map(em),
          key_map(km),
          scoring_engine(se),
          fast_dev(fd),
          slow_dev(sd),
          observer(obs),
          cfg(c) {}

    BlockDevice *device_for(Tier tier) {
        return (tier == Tier::FAST) ? fast_dev : slow_dev;
    }

    // ── Tier migration: move extent data from one tier to another ──
    MigrationResult migrate_tier(uint64_t extent_id, Tier from, Tier to);

    // ── Compaction: copy live data to new extent with compacted offsets ──
    MigrationResult compact(uint64_t extent_id);

    // ── Main loop: 8-step cycle ──
    void main_loop();

    // ── Single cycle (used by both main_loop and run_cycle) ──
    void run_cycle_inline() {
        // ── Step 1: Process deferred-free ──
        extent_map->process_deferred_free();

        // ── Step 2: Randomness refresh ──
        extent_map->refresh_randomness(*key_map);

        // ── Step 3: Adapt weights based on FAST tier usage ──
        double watermark = extent_map->fast_watermark();
        scoring_engine->adapt_weights(watermark);

        // ── Step 4: Score all extents + build migration queue ──
        uint32_t now = (uint32_t)std::time(nullptr);
        auto snapshot = extent_map->snapshot();
        std::vector<std::pair<uint64_t, float>> scored;

        for (const auto &snap : snapshot) {
            float s = scoring_engine->score(snap.raw_metrics,
                                            snap.last_access_time, now);
            scored.push_back({snap.extent_id, s});
        }

        std::sort(scored.begin(), scored.end(),
                  [](const auto &a, const auto &b) {
                      return a.second > b.second;
                  });

        // ── Step 5: Enqueue tier migrations ──
        uint32_t migrated = 0;
        for (const auto &[eid, score] : scored) {
            if (migrated >= cfg.max_migrations_per_cycle) break;

            auto loc = extent_map->get_location(eid);
            if (!loc) continue;

            if (score > cfg.promote_threshold && loc->tier == Tier::SLOW) {
                enqueue({MigrationTask::Type::TIER_MIGRATE, eid,
                         Tier::SLOW, Tier::FAST, score});
                migrated++;
            } else if (score < cfg.demote_threshold && loc->tier == Tier::FAST) {
                enqueue({MigrationTask::Type::TIER_MIGRATE, eid,
                         Tier::FAST, Tier::SLOW, score});
                migrated++;
            }
        }

        // ── Step 6: Enqueue compactions ──
        uint32_t compacted = 0;
        for (const auto &snap : snapshot) {
            if (compacted >= cfg.max_compactions_per_cycle) break;

            uint32_t dead_bytes = snap.used_bytes - snap.live_bytes;
            uint32_t capacity = snap.location.length - ExtentHeader::HEADER_SIZE;

            if (snap.used_bytes > 0 && capacity > 0 &&
                (double)dead_bytes / snap.used_bytes > cfg.compaction_dead_ratio &&
                (double)snap.used_bytes / capacity > cfg.compaction_usage_ratio) {
                enqueue({MigrationTask::Type::COMPACT, snap.extent_id,
                         snap.location.tier, snap.location.tier, 0.0f});
                compacted++;
            }
        }

        // ── Step 7: Process migration queue ──
        process_queue();
    }

    // ── Process migration queue ──
    void process_queue() {
        std::queue<MigrationTask> local;
        {
            std::lock_guard lock(queue_lock_);
            local.swap(queue_);
        }

        while (!local.empty()) {
            auto task = std::move(local.front());
            local.pop();

            if (task.type == MigrationTask::Type::TIER_MIGRATE) {
                migrate_tier(task.extent_id, task.from, task.to);
            } else {
                compact(task.extent_id);
            }
        }
    }

    void enqueue(MigrationTask &&task) {
        std::lock_guard lock(queue_lock_);
        queue_.push(std::move(task));
        queue_cv_.notify_one();
    }
};

// ── Tier migration ──────────────────────────────────────────────

MigrationResult MigrationEngine::Impl::migrate_tier(uint64_t extent_id,
                                                    Tier from, Tier to) {
    auto start = std::chrono::steady_clock::now();
    if (observer) observer->log_migration_start(extent_id, from, to);

    // Step 1: claim the extent (gen_before hidden inside handle)
    auto h = extent_map->begin_migration(extent_id);
    if (!h) return MigrationResult::INTERRUPTED;

    // Read entire extent (header + data) — no locks held
    bufferlist data;
    int r = device_for(from)->read(h->src_loc.offset, h->src_loc.length,
                                   &data, nullptr, true);
    if (r < 0) {
        extent_map->abort_migration(h.get());
        io_errors.fetch_add(1, std::memory_order_relaxed);
        return MigrationResult::FAILED;
    }

    // Allocate destination space on target tier (raw space, no entry)
    auto dst_opt = extent_map->allocate_raw(to, h->src_loc.length);
    if (!dst_opt) {
        extent_map->abort_migration(h.get());
        return MigrationResult::FAILED;
    }
    DiskLocation dst_loc = *dst_opt;

    // Write data to destination
    r = device_for(to)->write(dst_loc.offset, data, true);
    if (r < 0) {
        extent_map->abort_migration(h.get());
        extent_map->release_source(dst_loc);
        io_errors.fetch_add(1, std::memory_order_relaxed);
        return MigrationResult::FAILED;
    }

    // Step 2: commit or abort (gen check inside commit_migration)
    if (extent_map->commit_migration(h.get(), dst_loc)) {
        if (from == Tier::SLOW && to == Tier::FAST)
            promotions_committed.fetch_add(1, std::memory_order_relaxed);
        else if (from == Tier::FAST && to == Tier::SLOW)
            demotions_committed.fetch_add(1, std::memory_order_relaxed);
        if (observer) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
            observer->log_migration_result(extent_id, from, to, "COMMITTED", ms);
        }
        return MigrationResult::COMMITTED;
    } else {
        extent_map->abort_migration(h.get());
        extent_map->release_source(dst_loc);
        interruptions.fetch_add(1, std::memory_order_relaxed);
        if (observer) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
            observer->log_migration_result(extent_id, from, to, "INTERRUPTED", ms);
        }
        return MigrationResult::INTERRUPTED;
    }
}

// ── Compaction ──────────────────────────────────────────────────

MigrationResult MigrationEngine::Impl::compact(uint64_t extent_id) {
    auto start = std::chrono::steady_clock::now();

    // Step 1: claim the extent
    auto h = extent_map->begin_migration(extent_id);
    if (!h) return MigrationResult::INTERRUPTED;

    // Get all live keys in this extent (reverse index)
    auto keys = key_map->keys_in_extent(extent_id);
    if (keys.empty()) {
        // No live data — just free the extent
        extent_map->free(extent_id);
        compactions_committed.fetch_add(1, std::memory_order_relaxed);
        return MigrationResult::COMMITTED;
    }

    BlockDevice *dev = device_for(h->src_loc.tier);

    // Allocate new extent (same tier, same size)
    auto alloc = extent_map->allocate_extent(h->src_loc.tier, h->src_loc.length);
    if (!alloc) {
        extent_map->abort_migration(h.get());
        return MigrationResult::FAILED;
    }
    uint64_t new_extent_id = alloc->extent_id;
    DiskLocation new_loc = alloc->location;

    // Copy live data to new extent with compacted offsets
    std::vector<std::pair<std::string, KeyLocation>> updates;

    for (const auto &key : keys) {
        KeyLocation old_kloc;
        key_map->lookup(key, &old_kloc);

        // Read value from source extent
        bufferlist value;
        uint64_t read_off = h->src_loc.offset +
            ExtentHeader::HEADER_SIZE + old_kloc.offset;
        int r = dev->read(read_off, old_kloc.length, &value, nullptr, true);
        if (r < 0) {
            extent_map->abort_migration(h.get());
            extent_map->free(new_extent_id);
            io_errors.fetch_add(1, std::memory_order_relaxed);
            return MigrationResult::FAILED;
        }

        // Append to new extent
        uint32_t new_offset = extent_map->append_slot(new_extent_id,
                                                      old_kloc.length);
        if (new_offset == UINT32_MAX) {
            extent_map->abort_migration(h.get());
            extent_map->free(new_extent_id);
            return MigrationResult::FAILED;
        }

        // Write value to new extent
        uint64_t write_off = new_loc.offset +
            ExtentHeader::HEADER_SIZE + new_offset;
        r = dev->write(write_off, value, true);
        if (r < 0) {
            extent_map->abort_migration(h.get());
            extent_map->free(new_extent_id);
            io_errors.fetch_add(1, std::memory_order_relaxed);
            return MigrationResult::FAILED;
        }

        updates.push_back({key, KeyLocation{new_extent_id, new_offset, old_kloc.length}});
    }

    // Step 2: verify no concurrent writes occurred during copy
    if (!extent_map->check_migration(*h)) {
        extent_map->abort_migration(h.get());
        extent_map->free(new_extent_id);
        interruptions.fetch_add(1, std::memory_order_relaxed);
        return MigrationResult::INTERRUPTED;
    }

    // Commit: atomically update KeyMap and free old extent
    key_map->batch_update(updates);
    extent_map->free(extent_id);
    compactions_committed.fetch_add(1, std::memory_order_relaxed);
    if (observer) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
        observer->log_compaction_result(extent_id, "COMMITTED", ms);
    }

    return MigrationResult::COMMITTED;
}

// ── Main loop: 8-step cycle ────────────────────────────────────

void MigrationEngine::Impl::main_loop() {
    while (running.load()) {
        run_cycle_inline();

        // Step 8: Sleep until next cycle
        std::this_thread::sleep_for(
            std::chrono::milliseconds(cfg.scan_interval_ms));
    }
}

// ── Public API ──────────────────────────────────────────────────

MigrationEngine::MigrationEngine(ExtentMap *extent_map,
                                 KeyMap *key_map,
                                 ScoringEngine *scoring_engine,
                                 BlockDevice *fast_dev,
                                 BlockDevice *slow_dev,
                                 const BtierConfig &cfg,
                                 BtierObserver *observer)
    : impl_(std::make_unique<Impl>(extent_map, key_map, scoring_engine,
                                   fast_dev, slow_dev, cfg, observer)) {}

MigrationEngine::~MigrationEngine() {
    stop();
}

void MigrationEngine::start() {
    if (impl_->running.load()) return;
    impl_->running.store(true);
    impl_->worker = std::thread([this]() {
        impl_->main_loop();
    });
}

void MigrationEngine::stop() {
    if (!impl_->running.load()) return;
    impl_->running.store(false);
    impl_->queue_cv_.notify_all();
    if (impl_->worker.joinable())
        impl_->worker.join();
}

void MigrationEngine::enqueue_migrate(uint64_t extent_id, Tier from,
                                      Tier to, float score) {
    impl_->enqueue({MigrationTask::Type::TIER_MIGRATE, extent_id,
                    from, to, score});
}

void MigrationEngine::enqueue_compact(uint64_t extent_id) {
    impl_->enqueue({MigrationTask::Type::COMPACT, extent_id,
                    Tier::FAST, Tier::FAST, 0.0f});
}

size_t MigrationEngine::pending() const {
    std::lock_guard lock(impl_->queue_lock_);
    return impl_->queue_.size();
}

MigrationStats MigrationEngine::get_stats() const {
    MigrationStats s;
    s.promotions_committed = impl_->promotions_committed.load(std::memory_order_relaxed);
    s.demotions_committed = impl_->demotions_committed.load(std::memory_order_relaxed);
    s.compactions_committed = impl_->compactions_committed.load(std::memory_order_relaxed);
    s.interruptions = impl_->interruptions.load(std::memory_order_relaxed);
    s.io_errors = impl_->io_errors.load(std::memory_order_relaxed);
    return s;
}

void MigrationEngine::run_cycle() {
    impl_->run_cycle_inline();
}

MigrationResult MigrationEngine::compact_extent(uint64_t extent_id) {
    return impl_->compact(extent_id);
}

MigrationResult MigrationEngine::migrate_extent(uint64_t extent_id,
                                                Tier from, Tier to) {
    return impl_->migrate_tier(extent_id, from, to);
}

}  // namespace TOPNSPC::btier
