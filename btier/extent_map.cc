#include "btier/extent_map.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <set>

#include "btier/config.h"
#include "btier/key_map.h"
#include "common/crc32.h"
#include "common/intarith.h"
#include "common/interval_set.h"

namespace TOPNSPC::btier {

// ── Internal helpers ────────────────────────────────────────────

struct ExtentFreeEntry {
    uint32_t free_bytes;
    uint64_t extent_id;
    bool operator<(const ExtentFreeEntry &o) const {
        return free_bytes < o.free_bytes ||
            (free_bytes == o.free_bytes && extent_id < o.extent_id);
    }
};

struct DeferredFreeEntry {
    DiskLocation loc;
    uint64_t added_at_seqno = 0;
};

struct ExtentMap::Impl {
    const BtierConfig &cfg;
    uint64_t block_size;
    uint64_t extent_size;

    std::unordered_map<uint64_t, std::shared_ptr<ExtentEntry>> entries_;
    mutable std::shared_mutex map_lock_;
    uint64_t next_extent_id = 1;

    Allocator *allocators[2] = {nullptr, nullptr};
    BlockDevice *devices[2] = {nullptr, nullptr};
    int64_t device_sizes[2] = {0, 0};

    std::array<std::set<ExtentFreeEntry>, 2> free_lists_;
    mutable std::shared_mutex free_lists_lock_;

    std::vector<DeferredFreeEntry> deferred_free_;
    std::mutex deferred_free_lock_;
    std::atomic<uint64_t> seqno_{0};

    // Dirty extent headers — extents whose used_bytes/live_bytes/generation
    // changed since last flush_dirty_headers().
    std::set<uint64_t> dirty_headers_;
    mutable std::mutex dirty_headers_lock_;

    void mark_dirty(uint64_t extent_id) {
        std::lock_guard lock(dirty_headers_lock_);
        dirty_headers_.insert(extent_id);
    }

    explicit Impl(const BtierConfig &c)
        : cfg(c),
          block_size(c.block_size),
          extent_size(c.extent_size) {}

    std::shared_ptr<ExtentEntry> lookup(uint64_t extent_id) const {
        std::shared_lock lock(map_lock_);
        auto it = entries_.find(extent_id);
        if (it == entries_.end()) return nullptr;
        return it->second;
    }

    void bump_generation(ExtentEntry *entry) {
        uint64_t old = entry->metrics.raw.load(std::memory_order_relaxed);
        uint64_t new_gen = ExtentMetrics::generation(old) + 1;
        uint64_t new_word = ExtentMetrics::pack(
            ExtentMetrics::access_count(old),
            ExtentMetrics::write_count(old),
            ExtentMetrics::randomness(old),
            ExtentMetrics::state(old),
            new_gen);
        entry->metrics.raw.store(new_word, std::memory_order_release);
    }

    void add_deferred_free(const DiskLocation &loc) {
        std::lock_guard lock(deferred_free_lock_);
        deferred_free_.push_back({loc, seqno_.load()});
    }

    uint32_t free_bytes_of(const std::shared_ptr<ExtentEntry> &e) const {
        return e->location.length - ExtentHeader::HEADER_SIZE - e->used_bytes;
    }

    void update_free_list(uint64_t extent_id, Tier tier,
                          uint32_t old_free, uint32_t new_free) {
        std::unique_lock lock(free_lists_lock_);
        auto &list = free_lists_[static_cast<int>(tier)];
        list.erase({old_free, extent_id});
        list.insert({new_free, extent_id});
    }

    void write_extent_header(uint64_t extent_id, const DiskLocation &loc) {
        if (!devices[static_cast<int>(loc.tier)]) return;

        ExtentHeader hdr;
        std::memset(&hdr, 0, sizeof(hdr));
        hdr.magic = ExtentHeader::MAGIC;
        hdr.extent_id = extent_id;
        hdr.length = loc.length;
        hdr.used_bytes = 0;
        hdr.live_bytes = 0;
        hdr.reserved = 0;
        hdr.generation = 0;
        hdr.crc = calc_crc32(reinterpret_cast<const uint8_t *>(&hdr),
                             offsetof(ExtentHeader, crc), 0);

        bufferlist bl;
        bl.append(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
        devices[static_cast<int>(loc.tier)]->write(loc.offset, bl, false);
    }
};

// ── Public API ──────────────────────────────────────────────────

ExtentMap::ExtentMap(const BtierConfig &cfg)
    : impl_(std::make_unique<Impl>(cfg)) {}

ExtentMap::~ExtentMap() = default;

void ExtentMap::add_allocator(Tier tier, Allocator *alloc) {
    impl_->allocators[static_cast<int>(tier)] = alloc;
}

void ExtentMap::add_block_device(Tier tier, BlockDevice *dev) {
    impl_->devices[static_cast<int>(tier)] = dev;
}

void ExtentMap::init_free_space() {
    for (int t = 0; t < 2; t++) {
        if (impl_->allocators[t]) {
            impl_->allocators[t]->init_add_free(0, impl_->allocators[t]->get_capacity());
        }
    }
}

std::optional<DiskLocation> ExtentMap::get_location(uint64_t extent_id) const {
    auto entry = impl_->lookup(extent_id);
    if (!entry) return std::nullopt;
    std::shared_lock lock(entry->struct_lock);
    return entry->location;
}

uint64_t ExtentMap::get_raw_metrics(uint64_t extent_id) const {
    auto entry = impl_->lookup(extent_id);
    if (!entry) return 0;
    return entry->metrics.raw.load(std::memory_order_acquire);
}

void ExtentMap::record_io(uint64_t extent_id, IoOp op, uint32_t now) {
    auto entry = impl_->lookup(extent_id);
    if (!entry) return;

    entry->metrics.last_access_time.store(now, std::memory_order_relaxed);

    uint64_t old_word = entry->metrics.raw.load(std::memory_order_relaxed);
    while (true) {
        uint32_t new_access = std::min<uint32_t>(
            ExtentMetrics::access_count(old_word) + 1, 4095);
        uint32_t new_write = ExtentMetrics::write_count(old_word);
        if (op == IoOp::WRITE)
            new_write = std::min<uint32_t>(new_write + 1, 4095);

        uint64_t new_word = ExtentMetrics::pack(
            new_access,
            new_write,
            ExtentMetrics::randomness(old_word),
            ExtentMetrics::state(old_word),
            ExtentMetrics::generation(old_word));

        if (entry->metrics.raw.compare_exchange_weak(
                old_word, new_word, std::memory_order_relaxed))
            break;
    }
}

void ExtentMap::set_randomness(uint64_t extent_id, uint32_t randomness) {
    auto entry = impl_->lookup(extent_id);
    if (!entry) return;

    uint64_t old_word = entry->metrics.raw.load(std::memory_order_relaxed);
    while (true) {
        uint64_t new_word = ExtentMetrics::pack(
            ExtentMetrics::access_count(old_word),
            ExtentMetrics::write_count(old_word),
            randomness & 0x3F,
            ExtentMetrics::state(old_word),
            ExtentMetrics::generation(old_word));

        if (entry->metrics.raw.compare_exchange_weak(
                old_word, new_word, std::memory_order_relaxed))
            break;
    }
}

void ExtentMap::refresh_randomness(const KeyMap &key_map) {
    auto snap = snapshot();
    for (const auto &s : snap) {
        auto keys = key_map.keys_in_extent(s.extent_id);
        uint32_t randomness = 0;
        for (const auto &key : keys) {
            if (key_map.get_consecutive_sequential(key) == 0) {
                randomness = 63;
                break;
            }
        }
        set_randomness(s.extent_id, randomness);
    }
}

uint64_t ExtentMap::find_extent_with_space(Tier tier,
                                           uint32_t needed_bytes) const {
    std::shared_lock fl(impl_->free_lists_lock_);
    const auto &list = impl_->free_lists_[static_cast<int>(tier)];
    auto it = list.lower_bound({needed_bytes, 0});
    for (; it != list.end(); ++it) {
        auto entry = impl_->lookup(it->extent_id);
        if (!entry) continue;
        uint64_t raw = entry->metrics.raw.load(std::memory_order_acquire);
        if (ExtentMetrics::state(raw) == ACTIVE)
            return it->extent_id;
    }
    return UINT64_MAX;
}

uint32_t ExtentMap::append_slot(uint64_t extent_id, uint32_t size) {
    auto entry = impl_->lookup(extent_id);
    if (!entry) return UINT32_MAX;

    std::unique_lock lock(entry->struct_lock);

    uint64_t raw = entry->metrics.raw.load(std::memory_order_relaxed);
    if (ExtentMetrics::state(raw) != ACTIVE)
        return UINT32_MAX;

    uint32_t capacity = entry->location.length - ExtentHeader::HEADER_SIZE;
    if (entry->used_bytes + size > capacity)
        return UINT32_MAX;

    uint32_t old_free = capacity - entry->used_bytes;

    uint32_t offset = entry->used_bytes;
    entry->used_bytes += size;
    entry->live_bytes += size;

    impl_->bump_generation(entry.get());
    impl_->mark_dirty(extent_id);

    uint32_t new_free = capacity - entry->used_bytes;
    lock.unlock();

    impl_->update_free_list(extent_id, entry->location.tier, old_free, new_free);

    return offset;
}

void ExtentMap::mark_dead_slot(uint64_t extent_id, uint32_t length) {
    auto entry = impl_->lookup(extent_id);
    if (!entry) return;

    std::unique_lock lock(entry->struct_lock);

    uint32_t capacity = entry->location.length - ExtentHeader::HEADER_SIZE;
    uint32_t old_free = capacity - entry->used_bytes;

    if (entry->live_bytes >= length) {
        entry->live_bytes -= length;
    } else {
        entry->live_bytes = 0;
    }

    impl_->bump_generation(entry.get());
    impl_->mark_dirty(extent_id);

    uint32_t new_free = capacity - entry->used_bytes;
    lock.unlock();

    impl_->update_free_list(extent_id, entry->location.tier, old_free, new_free);
}

uint32_t ExtentMap::get_live_bytes(uint64_t extent_id) const {
    auto entry = impl_->lookup(extent_id);
    if (!entry) return 0;
    std::shared_lock lock(entry->struct_lock);
    return entry->live_bytes;
}

uint32_t ExtentMap::get_used_bytes(uint64_t extent_id) const {
    auto entry = impl_->lookup(extent_id);
    if (!entry) return 0;
    std::shared_lock lock(entry->struct_lock);
    return entry->used_bytes;
}

std::optional<ExtentMap::AllocResult>
ExtentMap::allocate_extent(Tier tier, uint64_t size) {
    uint64_t aligned_size = round_up_to(size, impl_->block_size);
    if (aligned_size == 0) aligned_size = impl_->block_size;

    Allocator *alloc = impl_->allocators[static_cast<int>(tier)];
    if (!alloc) return std::nullopt;

    PExtentVector extents;
    int64_t got = alloc->allocate(aligned_size, impl_->block_size,
                                  aligned_size, 0, &extents);
    if (got < (int64_t)aligned_size || extents.empty()) {
        if (tier == Tier::FAST) {
            return allocate_extent(Tier::SLOW, size);
        }
        return std::nullopt;
    }

    uint64_t extent_id = impl_->next_extent_id++;
    DiskLocation loc;
    loc.offset = extents[0].offset;
    loc.length = extents[0].length;
    loc.tier = tier;

    auto entry = std::make_shared<ExtentEntry>(loc);
    {
        std::unique_lock lock(impl_->map_lock_);
        impl_->entries_[extent_id] = entry;
    }

    {
        std::unique_lock fl(impl_->free_lists_lock_);
        uint32_t free_bytes = loc.length - ExtentHeader::HEADER_SIZE;
        impl_->free_lists_[static_cast<int>(tier)].insert({free_bytes, extent_id});
    }

    impl_->write_extent_header(extent_id, loc);

    return AllocResult{extent_id, loc};
}

std::optional<DiskLocation>
ExtentMap::allocate_raw(Tier tier, uint64_t size) {
    uint64_t aligned_size = round_up_to(size, impl_->block_size);
    if (aligned_size == 0) aligned_size = impl_->block_size;

    Allocator *alloc = impl_->allocators[static_cast<int>(tier)];
    if (!alloc) return std::nullopt;

    PExtentVector extents;
    int64_t got = alloc->allocate(aligned_size, impl_->block_size,
                                  aligned_size, 0, &extents);
    if (got < (int64_t)aligned_size || extents.empty()) {
        if (tier == Tier::FAST) {
            return allocate_raw(Tier::SLOW, size);
        }
        return std::nullopt;
    }

    DiskLocation loc;
    loc.offset = extents[0].offset;
    loc.length = extents[0].length;
    loc.tier = tier;
    return loc;
}

std::unique_ptr<ExtentMap::MigrationHandle>
ExtentMap::begin_migration(uint64_t extent_id) {
    auto entry = impl_->lookup(extent_id);
    if (!entry) return nullptr;

    std::unique_lock lock(entry->struct_lock);

    uint64_t raw = entry->metrics.raw.load(std::memory_order_acquire);
    if (ExtentMetrics::state(raw) != ACTIVE)
        return nullptr;

    auto h = std::make_unique<MigrationHandle>();
    h->extent_id = extent_id;
    h->gen_before = ExtentMetrics::generation(raw);
    h->src_loc = entry->location;

    uint64_t new_word = ExtentMetrics::pack(
        ExtentMetrics::access_count(raw),
        ExtentMetrics::write_count(raw),
        ExtentMetrics::randomness(raw),
        MIGRATING,
        h->gen_before);
    entry->metrics.raw.store(new_word, std::memory_order_release);

    return h;
}

bool ExtentMap::commit_migration(MigrationHandle *h,
                                 const DiskLocation &new_loc) {
    if (!h) return false;
    auto entry = impl_->lookup(h->extent_id);
    if (!entry) return false;

    std::unique_lock lock(entry->struct_lock);

    uint64_t raw = entry->metrics.raw.load(std::memory_order_acquire);
    if (ExtentMetrics::generation(raw) != h->gen_before)
        return false;

    DiskLocation old_loc = entry->location;
    entry->location = new_loc;

    uint64_t new_gen = h->gen_before + 1;
    uint64_t new_word = ExtentMetrics::pack(
        ExtentMetrics::access_count(raw),
        ExtentMetrics::write_count(raw),
        ExtentMetrics::randomness(raw),
        ACTIVE,
        new_gen);
    entry->metrics.raw.store(new_word, std::memory_order_release);

    impl_->add_deferred_free(old_loc);
    impl_->mark_dirty(h->extent_id);

    return true;
}

void ExtentMap::abort_migration(MigrationHandle *h) {
    if (!h) return;
    auto entry = impl_->lookup(h->extent_id);
    if (!entry) return;

    std::unique_lock lock(entry->struct_lock);
    uint64_t raw = entry->metrics.raw.load(std::memory_order_acquire);
    if (ExtentMetrics::state(raw) != MIGRATING) return;

    uint64_t new_word = ExtentMetrics::pack(
        ExtentMetrics::access_count(raw),
        ExtentMetrics::write_count(raw),
        ExtentMetrics::randomness(raw),
        ACTIVE,
        ExtentMetrics::generation(raw));
    entry->metrics.raw.store(new_word, std::memory_order_release);
}

bool ExtentMap::check_migration(const MigrationHandle &h) const {
    auto entry = impl_->lookup(h.extent_id);
    if (!entry) return false;
    uint64_t raw = entry->metrics.raw.load(std::memory_order_acquire);
    return ExtentMetrics::generation(raw) == h.gen_before;
}

void ExtentMap::release_source(const DiskLocation &loc) {
    impl_->add_deferred_free(loc);
}

void ExtentMap::free(uint64_t extent_id) {
    std::shared_ptr<ExtentEntry> entry;
    {
        std::unique_lock lock(impl_->map_lock_);
        auto it = impl_->entries_.find(extent_id);
        if (it == impl_->entries_.end()) return;
        entry = it->second;
        impl_->entries_.erase(it);
    }

    {
        std::shared_lock sl(entry->struct_lock);
        uint32_t capacity = entry->location.length - ExtentHeader::HEADER_SIZE;
        uint32_t old_free = capacity - entry->used_bytes;
        {
            std::unique_lock fl(impl_->free_lists_lock_);
            impl_->free_lists_[static_cast<int>(entry->location.tier)]
                .erase({old_free, extent_id});
        }
        impl_->add_deferred_free(entry->location);
    }
}

void ExtentMap::process_deferred_free() {
    std::lock_guard lock(impl_->deferred_free_lock_);
    uint64_t current_seq = impl_->seqno_.fetch_add(1);
    auto it = impl_->deferred_free_.begin();
    while (it != impl_->deferred_free_.end()) {
        if (current_seq - it->added_at_seqno >= 2) {
            Allocator *alloc =
                impl_->allocators[static_cast<int>(it->loc.tier)];
            if (alloc) {
                interval_set<uint64_t> release_set;
                release_set.insert(it->loc.offset, it->loc.length);
                alloc->release(release_set);
            }
            it = impl_->deferred_free_.erase(it);
        } else {
            ++it;
        }
    }
}

void ExtentMap::clear_deferred_free() {
    std::lock_guard lock(impl_->deferred_free_lock_);
    impl_->deferred_free_.clear();
}

size_t ExtentMap::size() const {
    std::shared_lock lock(impl_->map_lock_);
    return impl_->entries_.size();
}

double ExtentMap::fast_watermark() const {
    Allocator *a = impl_->allocators[static_cast<int>(Tier::FAST)];
    if (!a || a->get_capacity() == 0) return 1.0;
    double free = (double)a->get_free() / a->get_capacity();
    return 1.0 - free;
}

std::vector<ExtentMap::SnapshotEntry> ExtentMap::snapshot() const {
    std::vector<SnapshotEntry> result;
    std::shared_lock lock(impl_->map_lock_);
    result.reserve(impl_->entries_.size());
    for (const auto &[id, entry] : impl_->entries_) {
        std::shared_lock sl(entry->struct_lock);
        SnapshotEntry se;
        se.extent_id = id;
        se.location = entry->location;
        se.raw_metrics = entry->metrics.raw.load(std::memory_order_acquire);
        se.last_access_time = entry->metrics.last_access_time.load(
            std::memory_order_acquire);
        se.used_bytes = entry->used_bytes;
        se.live_bytes = entry->live_bytes;
        result.push_back(se);
    }
    return result;
}

void ExtentMap::io_ref_inc(uint64_t extent_id) {
    auto entry = impl_->lookup(extent_id);
    if (entry) entry->io_refs.fetch_add(1, std::memory_order_relaxed);
}

void ExtentMap::io_ref_dec(uint64_t extent_id) {
    auto entry = impl_->lookup(extent_id);
    if (entry) entry->io_refs.fetch_sub(1, std::memory_order_relaxed);
}

void ExtentMap::create_entry_from_journal(uint64_t extent_id,
                                          const DiskLocation &loc) {
    auto entry = std::make_shared<ExtentEntry>(loc);
    {
        std::unique_lock lock(impl_->map_lock_);
        impl_->entries_[extent_id] = entry;
    }
    if (extent_id >= impl_->next_extent_id)
        impl_->next_extent_id = extent_id + 1;

    {
        std::unique_lock fl(impl_->free_lists_lock_);
        uint32_t free_bytes = loc.length - ExtentHeader::HEADER_SIZE;
        impl_->free_lists_[static_cast<int>(loc.tier)]
            .insert({free_bytes, extent_id});
    }
}

void ExtentMap::restore_extent_state(uint64_t extent_id,
                                     uint32_t used_bytes,
                                     uint32_t live_bytes) {
    auto entry = impl_->lookup(extent_id);
    if (!entry) return;
    std::unique_lock lock(entry->struct_lock);

    uint32_t capacity = entry->location.length - ExtentHeader::HEADER_SIZE;
    uint32_t old_free = capacity - entry->used_bytes;

    entry->used_bytes = used_bytes;
    entry->live_bytes = live_bytes;

    uint32_t new_free = capacity - entry->used_bytes;
    lock.unlock();

    impl_->update_free_list(extent_id, entry->location.tier, old_free, new_free);
    impl_->mark_dirty(extent_id);
}

void ExtentMap::flush_dirty_headers() {
    std::set<uint64_t> dirty;
    {
        std::lock_guard lock(impl_->dirty_headers_lock_);
        dirty.swap(impl_->dirty_headers_);
    }

    for (uint64_t extent_id : dirty) {
        auto entry = impl_->lookup(extent_id);
        if (!entry) continue;

        std::shared_lock sl(entry->struct_lock);

        BlockDevice *dev = impl_->devices[static_cast<int>(entry->location.tier)];
        if (!dev) continue;

        ExtentHeader hdr;
        std::memset(&hdr, 0, sizeof(hdr));
        hdr.magic = ExtentHeader::MAGIC;
        hdr.extent_id = extent_id;
        hdr.length = entry->location.length;
        hdr.used_bytes = entry->used_bytes;
        hdr.live_bytes = entry->live_bytes;
        hdr.reserved = 0;
        hdr.generation = ExtentMetrics::generation(
            entry->metrics.raw.load(std::memory_order_acquire));
        hdr.crc = calc_crc32(reinterpret_cast<const uint8_t *>(&hdr),
                             offsetof(ExtentHeader, crc), 0);

        bufferlist bl;
        bl.append(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
        dev->write(entry->location.offset, bl, false);
    }
}

}  // namespace TOPNSPC::btier
