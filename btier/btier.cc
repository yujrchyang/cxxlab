#include "btier/btier.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>

#include "blk/allocator.h"
#include "blk/block_device.h"
#include "btier/btier_observer.h"
#include "btier/extent_map.h"
#include "btier/journal.h"
#include "btier/key_map.h"
#include "btier/migration_engine.h"
#include "btier/scoring_engine.h"
#include "common/buffer.h"
#include "common/crc32.h"
#include "common/intarith.h"

namespace TOPNSPC::btier {

struct BtierEngine::Impl {
    BtierConfig cfg;

    std::unique_ptr<BlockDevice> fast_dev;
    std::unique_ptr<BlockDevice> slow_dev;
    uint64_t fast_dev_size = 0;
    uint64_t slow_dev_size = 0;

    std::unique_ptr<Allocator> fast_alloc;
    std::unique_ptr<Allocator> slow_alloc;

    std::unique_ptr<ExtentMap> extent_map;
    std::unique_ptr<KeyMap> key_map;
    std::unique_ptr<Journal> journal;
    std::unique_ptr<ScoringEngine> scoring_engine;
    std::unique_ptr<MigrationEngine> migration_engine;
    std::unique_ptr<BtierObserver> observer;

    bool initialized = false;

    int recover_internal(const std::vector<JournalRecord> &records);
    std::vector<JournalRecord> build_checkpoint_state();

    // put() helpers
    struct PutWriteResult {
        uint64_t target_extent_id;
        uint32_t offset;
        uint32_t size;
        DiskLocation extent_loc;
        bool new_extent_created;
        KeyLocation old_kloc;
        bool has_old;
        uint32_t now;
    };

    int put_pre_transaction_extent(uint64_t extent_id, const DiskLocation &loc);
    int put_allocate_new_extent(Tier tier, uint64_t size,
                                uint64_t &extent_id, DiskLocation &loc);
    int put_write_data(const std::string &key, const bufferlist &value,
                       PutWriteResult &ctx);
    int put_journal_transaction(const std::string &key, const PutWriteResult &ctx);
    void put_cleanup_on_failure(const PutWriteResult &ctx);
    void put_commit_memory_state(const std::string &key, const PutWriteResult &ctx);
};

BtierEngine::BtierEngine() : impl_(std::make_unique<Impl>()) {}
BtierEngine::~BtierEngine() { shutdown(); }

int BtierEngine::init(const BtierConfig &config) {
    impl_->cfg = config;

    // ── Step 1: Open block devices ──
    impl_->fast_dev = BlockDevice::create(config.fast_dev_path, nullptr, nullptr);
    int r = impl_->fast_dev->open(config.fast_dev_path);
    if (r < 0) return r;

    impl_->slow_dev = BlockDevice::create(config.slow_dev_path, nullptr, nullptr);
    r = impl_->slow_dev->open(config.slow_dev_path);
    if (r < 0) {
        impl_->fast_dev->close();
        return r;
    }

    impl_->fast_dev_size = impl_->fast_dev->get_size();
    impl_->slow_dev_size = impl_->slow_dev->get_size();

    // ── Step 2: Create allocators ──
    // FAST allocator covers the full device; journal region is removed
    // so allocator returns real device offsets (journal at [0, journal_size)).
    uint64_t journal_size = config.journal_size;
    if (journal_size > impl_->fast_dev_size)
        journal_size = impl_->fast_dev_size / 2;

    impl_->fast_alloc.reset(
        Allocator::create("avl", impl_->fast_dev_size,
                          config.block_size, "btier-fast"));
    impl_->slow_alloc.reset(
        Allocator::create("avl", impl_->slow_dev_size,
                          config.block_size, "btier-slow"));

    // ── Step 3: Initialize ExtentMap ──
    impl_->extent_map = std::make_unique<ExtentMap>(config);
    impl_->extent_map->add_allocator(Tier::FAST, impl_->fast_alloc.get());
    impl_->extent_map->add_allocator(Tier::SLOW, impl_->slow_alloc.get());
    impl_->extent_map->add_block_device(Tier::FAST, impl_->fast_dev.get());
    impl_->extent_map->add_block_device(Tier::SLOW, impl_->slow_dev.get());
    impl_->extent_map->init_free_space();

    // Mark journal region as used on FAST device (must be AFTER init_free_space)
    impl_->fast_alloc->init_rm_free(0, journal_size);

    // ── Step 4: Initialize KeyMap ──
    impl_->key_map = std::make_unique<KeyMap>();

    // ── Step 5: Open journal ──
    impl_->journal = std::make_unique<Journal>(
        impl_->fast_dev.get(), journal_size);

    // ── Step 6: Recover from journal ──
    auto records = impl_->journal->recover();
    if (!records.empty()) {
        r = impl_->recover_internal(records);
        if (r < 0) return r;
    } else {
        // First init — write empty checkpoint
        impl_->journal->checkpoint({});
    }

    // ── Step 7: Initialize ScoringEngine ──
    impl_->scoring_engine = std::make_unique<ScoringEngine>(impl_->cfg);

    // ── Step 8: Initialize observer ──
    impl_->observer = std::make_unique<BtierObserver>();

    // ── Step 9: Start MigrationEngine ──
    impl_->migration_engine = std::make_unique<MigrationEngine>(
        impl_->extent_map.get(),
        impl_->key_map.get(),
        impl_->scoring_engine.get(),
        impl_->fast_dev.get(),
        impl_->slow_dev.get(),
        impl_->cfg,
        impl_->observer.get());
    impl_->migration_engine->start();

    impl_->initialized = true;
    return 0;
}

int BtierEngine::recover() {
    if (!impl_->journal) return -EINVAL;
    auto records = impl_->journal->recover();
    return impl_->recover_internal(records);
}

int BtierEngine::Impl::recover_internal(
    const std::vector<JournalRecord> &records) {
    // Single-pass replay. Records are in write order, so OP_EXTENT_NEW
    // always precedes its associated OP_KEY_PUT for the same extent.
    for (const auto &rec : records) {
        switch (rec.op) {
        case OP_EXTENT_NEW:
            extent_map->create_entry_from_journal(rec.extent_id,
                                                  rec.extent_loc);
            break;
        case OP_KEY_PUT:
            key_map->put(rec.key, rec.key_loc, 0);
            break;
        case OP_KEY_DEL:
            key_map->erase(rec.key);
            break;
        case OP_MARK_DEAD:
            extent_map->mark_dead_slot(rec.extent_id, rec.dead_length);
            break;
        case OP_EXTENT_FREE:
            // Still replayed for backward compatibility with old journals
            extent_map->free(rec.extent_id);
            break;
        default:
            break;
        }
    }

    // Recalculate used_bytes/live_bytes for each extent from KeyMap
    // (create_entry_from_journal sets them to 0; OP_KEY_PUT only updates
    // KeyMap, not ExtentMap. We need to restore them for correct operation.)
    // Also free extents with no live keys (OP_EXTENT_FREE is no longer
    // journaled in put()/del() — recovery detects empties here.)
    auto snaps = extent_map->snapshot();
    for (const auto &snap : snaps) {
        auto keys = key_map->keys_in_extent(snap.extent_id);
        uint32_t total_live = 0;
        for (const auto &key : keys) {
            KeyLocation kloc;
            key_map->lookup(key, &kloc);
            total_live += kloc.length;
        }
        if (total_live == 0) {
            // No live keys — free the extent
            extent_map->free(snap.extent_id);
        } else {
            extent_map->restore_extent_state(snap.extent_id,
                                             total_live, total_live);
        }
    }

    // Mark allocated regions as used in allocators
    for (const auto &snap : extent_map->snapshot()) {
        Allocator *alloc = (snap.location.tier == Tier::FAST)
            ? fast_alloc.get()
            : slow_alloc.get();
        alloc->init_rm_free(snap.location.offset, snap.location.length);
    }

    // Verify ExtentHeader CRC for each extent
    for (const auto &snap : extent_map->snapshot()) {
        BlockDevice *dev = (snap.location.tier == Tier::FAST)
            ? fast_dev.get()
            : slow_dev.get();
        if (!dev) continue;

        bufferlist hdr_bl;
        int r = dev->read(snap.location.offset, ExtentHeader::HEADER_SIZE,
                          &hdr_bl, nullptr, true);
        if (r < 0 || hdr_bl.length() < sizeof(ExtentHeader)) continue;

        ExtentHeader hdr;
        std::memcpy(&hdr, hdr_bl.c_str(), sizeof(hdr));
        if (hdr.magic != ExtentHeader::MAGIC) continue;

        uint32_t expected_crc = calc_crc32(
            reinterpret_cast<const uint8_t *>(&hdr),
            offsetof(ExtentHeader, crc), 0);
        if (hdr.crc != expected_crc) {
            // CRC mismatch — extent is corrupt, remove its keys
            auto keys = key_map->keys_in_extent(snap.extent_id);
            for (const auto &key : keys) {
                key_map->erase(key);
            }
            extent_map->free(snap.extent_id);
        }
    }

    // Clear deferred-free list — entries added during recovery (from free()
    // calls) are for extents whose space is already marked as free by
    // init_add_free. process_deferred_free() would double-release them.
    extent_map->clear_deferred_free();

    return 0;
}

std::vector<JournalRecord> BtierEngine::Impl::build_checkpoint_state() {
    std::vector<JournalRecord> state;
    auto snaps = extent_map->snapshot();
    for (const auto &snap : snaps) {
        JournalRecord ext_rec;
        ext_rec.op = OP_EXTENT_NEW;
        ext_rec.extent_id = snap.extent_id;
        ext_rec.extent_loc = snap.location;
        state.push_back(ext_rec);

        auto keys = key_map->keys_in_extent(snap.extent_id);
        for (const auto &key : keys) {
            KeyLocation kloc;
            key_map->lookup(key, &kloc);
            JournalRecord key_rec;
            key_rec.op = OP_KEY_PUT;
            key_rec.key = key;
            key_rec.key_loc = kloc;
            state.push_back(key_rec);
        }
    }
    return state;
}

void BtierEngine::shutdown() {
    if (!impl_->initialized) return;

    // Stop migrator first (waits for in-flight migrations to complete)
    if (impl_->migration_engine) {
        impl_->migration_engine->stop();
        impl_->migration_engine.reset();
    }

    sync();

    // Checkpoint journal (writes full state snapshot, then trims)
    if (impl_->journal) {
        auto state = impl_->build_checkpoint_state();
        impl_->journal->checkpoint(state);
    }

    if (impl_->journal) {
        impl_->journal->close();
        impl_->journal.reset();
    }

    if (impl_->extent_map) {
        impl_->extent_map->process_deferred_free();
        impl_->extent_map.reset();
    }

    impl_->key_map.reset();
    impl_->scoring_engine.reset();
    impl_->observer.reset();
    impl_->fast_alloc.reset();
    impl_->slow_alloc.reset();

    if (impl_->fast_dev) {
        impl_->fast_dev->close();
        impl_->fast_dev.reset();
    }
    if (impl_->slow_dev) {
        impl_->slow_dev->close();
        impl_->slow_dev.reset();
    }

    impl_->initialized = false;
}

int BtierEngine::sync() {
    if (!impl_->initialized) return -EINVAL;
    // Flush dirty ExtentHeaders to devices
    if (impl_->extent_map) impl_->extent_map->flush_dirty_headers();
    // Fsync journal (FAST device)
    if (impl_->journal) impl_->journal->sync();
    // Fsync SLOW device (ExtentHeaders may have been written there)
    if (impl_->slow_dev) impl_->slow_dev->flush();
    return 0;
}

int BtierEngine::Impl::put_pre_transaction_extent(uint64_t extent_id,
                                                  const DiskLocation &loc) {
    uint64_t txn = journal->begin_txn();
    JournalRecord rec;
    rec.op = OP_EXTENT_NEW;
    rec.extent_id = extent_id;
    rec.extent_loc = loc;
    journal->append(txn, rec);
    int r = journal->commit_txn(txn);
    if (r < 0) {
        extent_map->free(extent_id);
        return r;
    }
    return 0;
}

int BtierEngine::Impl::put_allocate_new_extent(Tier tier, uint64_t size,
                                               uint64_t &extent_id,
                                               DiskLocation &loc) {
    auto alloc = extent_map->allocate_extent(tier, size);
    if (!alloc) return -ENOSPC;
    extent_id = alloc->extent_id;
    loc = alloc->location;
    return put_pre_transaction_extent(extent_id, loc);
}

int BtierEngine::Impl::put_write_data(const std::string &key,
                                      const bufferlist &value,
                                      PutWriteResult &ctx) {
    BlockDevice *dev;

    if (ctx.size >= cfg.large_value_threshold) {
        // Large value → dedicated extent
        int r = put_allocate_new_extent(Tier::FAST,
                                        ctx.size + ExtentHeader::HEADER_SIZE,
                                        ctx.target_extent_id, ctx.extent_loc);
        if (r < 0) return r;
        ctx.new_extent_created = true;

        ctx.offset = extent_map->append_slot(ctx.target_extent_id,
                                             (uint32_t)ctx.size);
        if (ctx.offset == UINT32_MAX) {
            extent_map->free(ctx.target_extent_id);
            return -EIO;
        }

        dev = (ctx.extent_loc.tier == Tier::FAST)
            ? fast_dev.get()
            : slow_dev.get();
        r = dev->write(ctx.extent_loc.offset + ExtentHeader::HEADER_SIZE + ctx.offset,
                       const_cast<bufferlist &>(value), true);
        if (r < 0) {
            extent_map->free(ctx.target_extent_id);
            return r;
        }
    } else {
        // Small value → pack into existing extent or create new
        ctx.target_extent_id = extent_map->find_extent_with_space(
            Tier::FAST, (uint32_t)ctx.size);
        ctx.new_extent_created = false;

        if (ctx.target_extent_id == UINT64_MAX) {
            int r = put_allocate_new_extent(Tier::FAST, cfg.extent_size,
                                            ctx.target_extent_id, ctx.extent_loc);
            if (r < 0) return r;
            ctx.new_extent_created = true;
        } else {
            auto loc = extent_map->get_location(ctx.target_extent_id);
            if (!loc) return -EIO;
            ctx.extent_loc = *loc;
        }

        // Reserve slot (bumps generation)
        ctx.offset = extent_map->append_slot(ctx.target_extent_id,
                                             (uint32_t)ctx.size);
        if (ctx.offset == UINT32_MAX) {
            if (ctx.new_extent_created) {
                extent_map->free(ctx.target_extent_id);
            }
            // Retry once with a fresh extent
            int r = put_allocate_new_extent(Tier::FAST, cfg.extent_size,
                                            ctx.target_extent_id, ctx.extent_loc);
            if (r < 0) return r;
            ctx.new_extent_created = true;

            ctx.offset = extent_map->append_slot(ctx.target_extent_id,
                                                 (uint32_t)ctx.size);
            if (ctx.offset == UINT32_MAX) {
                extent_map->free(ctx.target_extent_id);
                return -EIO;
            }
        }

        dev = (ctx.extent_loc.tier == Tier::FAST)
            ? fast_dev.get()
            : slow_dev.get();
        int r = dev->write(ctx.extent_loc.offset + ExtentHeader::HEADER_SIZE + ctx.offset,
                           const_cast<bufferlist &>(value), true);
        if (r < 0) {
            if (ctx.new_extent_created) {
                extent_map->free(ctx.target_extent_id);
            } else {
                extent_map->mark_dead_slot(ctx.target_extent_id,
                                           (uint32_t)ctx.size);
            }
            return r;
        }
    }

    return 0;
}

int BtierEngine::Impl::put_journal_transaction(const std::string &key,
                                               const PutWriteResult &ctx) {
    uint64_t txn_id = journal->begin_txn();

    if (ctx.has_old) {
        JournalRecord dead_rec;
        dead_rec.op = OP_MARK_DEAD;
        dead_rec.extent_id = ctx.old_kloc.extent_id;
        dead_rec.dead_length = ctx.old_kloc.length;
        journal->append(txn_id, dead_rec);
    }

    JournalRecord put_rec;
    put_rec.op = OP_KEY_PUT;
    put_rec.key = key;
    put_rec.key_loc = {ctx.target_extent_id, ctx.offset, ctx.size};
    journal->append(txn_id, put_rec);

    return journal->commit_txn(txn_id);
}

void BtierEngine::Impl::put_cleanup_on_failure(const PutWriteResult &ctx) {
    if (ctx.new_extent_created) {
        extent_map->free(ctx.target_extent_id);
    } else {
        extent_map->mark_dead_slot(ctx.target_extent_id, ctx.size);
    }
}

void BtierEngine::Impl::put_commit_memory_state(const std::string &key,
                                                const PutWriteResult &ctx) {
    KeyLocation new_kloc;
    new_kloc.extent_id = ctx.target_extent_id;
    new_kloc.offset = ctx.offset;
    new_kloc.length = ctx.size;
    uint64_t lba = ctx.extent_loc.offset + ExtentHeader::HEADER_SIZE + ctx.offset;
    key_map->put(key, new_kloc, lba);

    if (ctx.has_old) {
        extent_map->mark_dead_slot(ctx.old_kloc.extent_id, ctx.old_kloc.length);
        if (extent_map->get_live_bytes(ctx.old_kloc.extent_id) == 0) {
            extent_map->free(ctx.old_kloc.extent_id);
        }
    }
}

int BtierEngine::put(const std::string &key, const bufferlist &value) {
    if (!impl_->initialized) return -EINVAL;

    // Journal backpressure: if usage >= 95%, checkpoint before proceeding
    if (impl_->journal && impl_->journal->is_near_full()) {
        auto state = impl_->build_checkpoint_state();
        impl_->journal->checkpoint(state);
    }

    Impl::PutWriteResult ctx;
    ctx.size = value.length();
    ctx.now = (uint32_t)std::time(nullptr);
    ctx.has_old = impl_->key_map->lookup(key, &ctx.old_kloc);

    // Phase 1: Allocate space and write data
    int r = impl_->put_write_data(key, value, ctx);
    if (r < 0) return r;

    // Phase 2: Update metrics
    impl_->extent_map->record_io(ctx.target_extent_id, IoOp::WRITE, ctx.now);

    // Phase 3: Journal transaction (key mapping)
    r = impl_->put_journal_transaction(key, ctx);
    if (r < 0) {
        impl_->put_cleanup_on_failure(ctx);
        return r;
    }

    // Phase 4: Trigger async checkpoint if journal usage >= 80%
    if (impl_->journal && impl_->journal->needs_checkpoint()) {
        auto state = impl_->build_checkpoint_state();
        impl_->journal->checkpoint(state);
    }

    // Phase 5: Commit in-memory state
    impl_->put_commit_memory_state(key, ctx);

    return 0;
}

int BtierEngine::get(const std::string &key, bufferlist &value) {
    if (!impl_->initialized) return -EINVAL;

    KeyLocation kloc;
    if (!impl_->key_map->lookup(key, &kloc))
        return -ENOENT;

    auto loc = impl_->extent_map->get_location(kloc.extent_id);
    if (!loc) return -ENOENT;

    uint32_t now = (uint32_t)std::time(nullptr);
    impl_->extent_map->record_io(kloc.extent_id, IoOp::READ, now);

    impl_->extent_map->io_ref_inc(kloc.extent_id);

    BlockDevice *dev = (loc->tier == Tier::FAST)
        ? impl_->fast_dev.get()
        : impl_->slow_dev.get();
    int r = dev->read(loc->offset + ExtentHeader::HEADER_SIZE + kloc.offset,
                      kloc.length, &value, nullptr, true);

    impl_->extent_map->io_ref_dec(kloc.extent_id);

    return (r < 0) ? r : 0;
}

int BtierEngine::del(const std::string &key) {
    if (!impl_->initialized) return -EINVAL;

    KeyLocation kloc;
    if (!impl_->key_map->lookup(key, &kloc))
        return -ENOENT;

    // Begin journal transaction
    uint64_t txn_id = impl_->journal->begin_txn();

    JournalRecord del_rec;
    del_rec.op = OP_KEY_DEL;
    del_rec.key = key;
    impl_->journal->append(txn_id, del_rec);

    JournalRecord dead_rec;
    dead_rec.op = OP_MARK_DEAD;
    dead_rec.extent_id = kloc.extent_id;
    dead_rec.dead_length = kloc.length;
    impl_->journal->append(txn_id, dead_rec);

    // Note: OP_EXTENT_FREE is NOT journaled here. The extent is freed
    // in-memory after mark_dead_slot if live_bytes reaches 0. Recovery
    // detects empty extents during restore_extent_state and frees them.

    int r = impl_->journal->commit_txn(txn_id);
    if (r < 0) return r;

    // Commit in-memory state — re-check live_bytes AFTER mark_dead_slot
    impl_->key_map->erase(key);
    impl_->extent_map->mark_dead_slot(kloc.extent_id, kloc.length);
    if (impl_->extent_map->get_live_bytes(kloc.extent_id) == 0) {
        impl_->extent_map->free(kloc.extent_id);
    }

    return 0;
}

BtierEngine::Stats BtierEngine::get_stats() const {
    Stats s;
    if (!impl_->initialized) return s;

    s.num_keys = impl_->key_map->size();
    auto snaps = impl_->extent_map->snapshot();
    s.num_extents = snaps.size();
    for (const auto &snap : snaps) {
        if (snap.location.tier == Tier::FAST)
            s.fast_extents++;
        else
            s.slow_extents++;
    }
    s.fast_watermark = impl_->extent_map->fast_watermark();
    if (impl_->journal) {
        s.journal_bytes = impl_->journal->get_used_bytes();
    }

    if (impl_->migration_engine) {
        s.migrations_pending = impl_->migration_engine->pending();
        s.migration = impl_->migration_engine->get_stats();
    }

    return s;
}

void BtierEngine::set_weights(const WeightSet &w) {
    impl_->cfg.base_weights = w;
}

void BtierEngine::set_watermarks(double low, double high) {
    impl_->cfg.low_watermark = low;
    impl_->cfg.high_watermark = high;
}

void BtierEngine::set_scan_interval(uint32_t ms) {
    impl_->cfg.scan_interval_ms = ms;
}

std::vector<ScoredExtent> BtierEngine::run_scoring_pass() {
    std::vector<ScoredExtent> result;
    if (!impl_->initialized || !impl_->scoring_engine)
        return result;

    uint32_t now = (uint32_t)std::time(nullptr);

    // ── Step 1: Randomness refresh ──
    impl_->extent_map->refresh_randomness(*impl_->key_map);

    // ── Step 2: Adapt weights based on FAST tier usage ──
    double watermark = impl_->extent_map->fast_watermark();
    impl_->scoring_engine->adapt_weights(watermark);

    // ── Step 3: Score all extents ──
    auto snapshot = impl_->extent_map->snapshot();
    for (const auto &snap : snapshot) {
        float s = impl_->scoring_engine->score(
            snap.raw_metrics, snap.last_access_time, now);
        result.push_back({snap.extent_id, s, snap.location.tier});
    }

    // ── Step 4: Sort by score descending ──
    std::sort(result.begin(), result.end(),
              [](const ScoredExtent &a, const ScoredExtent &b) {
                  return a.score > b.score;
              });

    return result;
}

bool BtierEngine::compact_extent(uint64_t extent_id) {
    if (!impl_->initialized || !impl_->migration_engine)
        return false;
    auto result = impl_->migration_engine->compact_extent(extent_id);
    return result == MigrationResult::COMMITTED;
}

void BtierEngine::run_migration_cycle() {
    if (!impl_->initialized || !impl_->migration_engine)
        return;
    impl_->migration_engine->run_cycle();
}

MigrationStats BtierEngine::get_migration_stats() const {
    if (!impl_->initialized || !impl_->migration_engine)
        return {};
    return impl_->migration_engine->get_stats();
}

}  // namespace TOPNSPC::btier
