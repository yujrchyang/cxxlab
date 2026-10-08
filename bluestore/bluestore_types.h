#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "blk/extent_types.h"
#include "common/buffer.h"
#include "common/cassert.h"
#include "common/denc.h"
#include "common/intarith.h"
#include "common/interval_set.h"
#include "common/uuid.h"

namespace TOPNSPC {

using bluestore_pextent_t = pextent_t;

enum ChecksumType : uint8_t {
    CSUM_NONE = 1,
    CSUM_XXHASH32 = 2,
    CSUM_XXHASH64 = 3,
    CSUM_CRC32C = 4,
    CSUM_CRC32C_16 = 5,
    CSUM_CRC32C_8 = 6,
};

inline size_t csum_value_size(uint8_t type) {
    switch (type) {
    case CSUM_NONE:
        return 0;
    case CSUM_CRC32C:
    case CSUM_XXHASH32:
        return 4;
    case CSUM_CRC32C_16:
        return 2;
    case CSUM_CRC32C_8:
        return 1;
    case CSUM_XXHASH64:
        return 8;
    default:
        return 0;
    }
}

struct bluestore_bdev_label_t {
    uuid_d osd_uuid;
    uint64_t size = 0;
    std::string description;
    std::map<std::string, std::string> meta;

    DENC(bluestore_bdev_label_t, v, p) {
        DENC_START(1, 1, p);
        denc(v.osd_uuid, p);
        denc(v.size, p);
        denc(v.description, p);
        denc(v.meta, p);
        DENC_FINISH(p);
    }
};
WRITE_CLASS_DENC(bluestore_bdev_label_t);

struct bluestore_cnode_t {
    uint32_t bits = 0;

    explicit bluestore_cnode_t(uint32_t b = 0) : bits(b) {}

    DENC(bluestore_cnode_t, v, p) {
        DENC_START(1, 1, p);
        denc(v.bits, p);
        DENC_FINISH(p);
    }
};
WRITE_CLASS_DENC(bluestore_cnode_t);

struct bluestore_blob_use_tracker_t {
    uint32_t au_size = 0;
    uint32_t num_au = 0;
    uint32_t alloc_au = 0;

    union {
        uint32_t *bytes_per_au;
        uint32_t total_bytes;
    };

    bluestore_blob_use_tracker_t()
        : au_size(0), num_au(0), alloc_au(0), bytes_per_au(nullptr) {}

    bluestore_blob_use_tracker_t(const bluestore_blob_use_tracker_t &other);
    bluestore_blob_use_tracker_t &operator=(const bluestore_blob_use_tracker_t &rhs);
    ~bluestore_blob_use_tracker_t() { clear(); }

    void clear() {
        release(alloc_au, bytes_per_au);
        num_au = 0;
        alloc_au = 0;
        bytes_per_au = nullptr;
        au_size = 0;
    }

    uint32_t get_referenced_bytes() const {
        if (!num_au) {
            return total_bytes;
        }
        uint32_t total = 0;
        for (uint32_t i = 0; i < num_au; ++i) {
            total += bytes_per_au[i];
        }
        return total;
    }

    bool is_empty() const {
        if (!num_au) {
            return total_bytes == 0;
        }
        for (uint32_t i = 0; i < num_au; ++i) {
            if (bytes_per_au[i]) {
                return false;
            }
        }
        return true;
    }

    bool is_not_empty() const { return !is_empty(); }

    void init(uint32_t full_length, uint32_t _au_size);
    void get(uint32_t offset, uint32_t len);
    bool put(uint32_t offset, uint32_t len, PExtentVector *release);
    bool can_split() const;
    bool can_split_at(uint32_t blob_offset) const;
    void split(uint32_t blob_offset, bluestore_blob_use_tracker_t *r);
    void add_tail(uint32_t new_len, uint32_t _au_size);

    bool equal(const bluestore_blob_use_tracker_t &other) const;

    void bound_encode(size_t &p) const {
        denc(au_size, p);
        if (au_size) {
            denc(num_au, p);
            if (!num_au) {
                denc(total_bytes, p);
            } else {
                p += sizeof(uint32_t) * num_au;
            }
        }
    }

    void encode(buffer::list::contiguous_appender &p) const {
        denc(au_size, p);
        if (au_size) {
            denc(num_au, p);
            if (!num_au) {
                denc(total_bytes, p);
            } else {
                for (uint32_t i = 0; i < num_au; ++i) {
                    denc(bytes_per_au[i], p);
                }
            }
        }
    }

    void decode(buffer::ptr::const_iterator &p) {
        clear();
        denc(au_size, p);
        if (au_size) {
            uint32_t _num_au;
            denc(_num_au, p);
            if (!_num_au) {
                num_au = 0;
                denc(total_bytes, p);
            } else {
                allocate(_num_au);
                for (uint32_t i = 0; i < _num_au; ++i) {
                    denc(bytes_per_au[i], p);
                }
            }
        }
    }

private:
    void allocate(uint32_t _num_au);
    void release(uint32_t _num_au, uint32_t *ptr);
};
WRITE_CLASS_DENC(bluestore_blob_use_tracker_t);

struct bluestore_blob_t {
private:
    PExtentVector extents;
    uint32_t logical_length = 0;

public:
    enum Flags {
        FLAG_CSUM = 4,
        FLAG_HAS_UNUSED = 8,
    };

    uint32_t flags = 0;
    uint8_t csum_type = CSUM_NONE;
    uint8_t csum_chunk_order = 0;
    buffer::ptr csum_data;

    typedef uint16_t unused_t;
    unused_t unused = 0;

    bluestore_blob_t() = default;
    bluestore_blob_t(const bluestore_blob_t &) = default;
    bluestore_blob_t(bluestore_blob_t &&) = default;
    bluestore_blob_t &operator=(const bluestore_blob_t &) = default;
    bluestore_blob_t &operator=(bluestore_blob_t &&) = default;

    const PExtentVector &get_extents() const { return extents; }
    PExtentVector &dirty_extents() { return extents; }

    bool has_flag(unsigned f) const { return flags & f; }
    void set_flag(unsigned f) { flags |= f; }
    void clear_flag(unsigned f) { flags &= ~f; }

    bool has_csum() const { return has_flag(FLAG_CSUM); }
    bool has_unused() const { return has_flag(FLAG_HAS_UNUSED); }

    uint32_t get_csum_chunk_size() const {
        return 1 << csum_chunk_order;
    }

    uint64_t get_chunk_size(uint64_t dev_block_size) const {
        return has_csum() ? std::max<uint64_t>(dev_block_size, get_csum_chunk_size())
                          : dev_block_size;
    }

    uint32_t get_logical_length() const { return logical_length; }

    uint32_t get_ondisk_length() const {
        uint32_t len = 0;
        for (const auto &e : extents) {
            len += e.length;
        }
        return len;
    }

    size_t get_csum_value_size() const {
        return csum_value_size(csum_type);
    }

    void init_csum(uint8_t type, uint8_t order, uint32_t len) {
        flags |= FLAG_CSUM;
        csum_type = type;
        csum_chunk_order = order;
        csum_data = buffer::create(get_csum_value_size() * len / get_csum_chunk_size());
    }

    bool is_mutable() const {
        return true;
    }

    bool can_split() const {
        return !has_flag(FLAG_HAS_UNUSED);
    }

    bool can_split_at(uint32_t blob_offset) const {
        return !has_csum() || blob_offset % get_csum_chunk_size() == 0;
    }

    bool is_unallocated(uint64_t b_off, uint64_t b_len) const {
        if (b_off + b_len > logical_length) {
            return false;
        }

        uint64_t extent_off = 0;
        for (const auto &e : extents) {
            uint64_t extent_end = extent_off + e.length;

            if (b_off < extent_end && b_off + b_len > extent_off) {
                if (e.offset != bluestore_pextent_t::INVALID_OFFSET) {
                    return false;
                }
            }

            extent_off = extent_end;
            if (extent_off >= b_off + b_len) {
                break;
            }
        }
        return true;
    }

    bool is_allocated(uint64_t b_off, uint64_t b_len) const {
        if (b_off + b_len > logical_length) {
            return false;
        }
        uint64_t extent_off = 0;
        for (const auto &e : extents) {
            uint64_t extent_end = extent_off + e.length;

            if (b_off < extent_end && b_off + b_len > extent_off) {
                if (!e.is_valid()) {
                    return false;
                }
            }

            extent_off = extent_end;
            if (extent_off >= b_off + b_len) {
                break;
            }
        }
        return true;
    }

    bool is_unused(uint64_t offset, uint64_t length) const {
        if (!has_unused()) {
            return false;
        }
        uint64_t blob_len = get_logical_length();
        cxxlab_assert(blob_len % (sizeof(unused_t) * 8) == 0);
        cxxlab_assert(offset + length <= blob_len);
        uint64_t chunk_size = blob_len / (sizeof(unused_t) * 8);
        uint64_t start = offset / chunk_size;
        uint64_t end = round_up_to(offset + length, chunk_size) / chunk_size;
        auto i = start;
        while (i < end && (unused & (1u << i))) {
            i++;
        }
        return i >= end;
    }

    void add_unused(uint64_t offset, uint64_t length) {
        uint64_t blob_len = get_logical_length();
        cxxlab_assert(blob_len % (sizeof(unused_t) * 8) == 0);
        cxxlab_assert(offset + length <= blob_len);
        uint64_t chunk_size = blob_len / (sizeof(unused_t) * 8);
        uint64_t start = round_up_to(offset, chunk_size) / chunk_size;
        uint64_t end = (offset + length) / chunk_size;
        for (auto i = start; i < end; ++i) {
            unused |= (1u << i);
        }
        if (start != end) {
            set_flag(FLAG_HAS_UNUSED);
        }
    }

    void mark_used(uint64_t offset, uint64_t length) {
        if (!has_unused()) {
            return;
        }
        uint64_t blob_len = get_logical_length();
        cxxlab_assert(blob_len % (sizeof(unused_t) * 8) == 0);
        cxxlab_assert(offset + length <= blob_len);
        uint64_t chunk_size = blob_len / (sizeof(unused_t) * 8);
        uint64_t start = offset / chunk_size;
        uint64_t end = round_up_to(offset + length, chunk_size) / chunk_size;
        for (auto i = start; i < end; ++i) {
            unused &= ~(1u << i);
        }
        if (unused == 0) {
            clear_flag(FLAG_HAS_UNUSED);
        }
    }

    template <typename F>
    int map(uint64_t b_off, uint64_t b_len, F &&f) const {
        uint64_t extent_off = 0;
        for (const auto &ex : extents) {
            uint64_t extent_end = extent_off + ex.length;
            if (b_off < extent_end && b_off + b_len > extent_off) {
                uint64_t s = std::max(b_off, extent_off);
                uint64_t e_end = std::min(b_off + b_len, extent_end);
                f(ex.offset + (s - extent_off), e_end - s);
            }
            extent_off = extent_end;
            if (extent_off >= b_off + b_len) {
                break;
            }
        }
        return 0;
    }

    void add_tail(uint32_t new_len) {
        extents.emplace_back(bluestore_pextent_t(bluestore_pextent_t::INVALID_OFFSET,
                                                 new_len - logical_length));
        logical_length = new_len;
        if (has_csum()) {
            buffer::ptr t;
            t.swap(csum_data);
            csum_data = buffer::create(get_csum_value_size() * logical_length / get_csum_chunk_size());
            csum_data.copy_in(0, t.length(), t.c_str());
            csum_data.zero(t.length(), csum_data.length() - t.length());
        }
    }

    void allocated(uint32_t b_off, uint32_t length, const PExtentVector &allocs);
    void split(uint32_t blob_offset, bluestore_blob_t &rb);
    bool release_extents(bool all, const PExtentVector &logical, PExtentVector *r);

    void calc_csum(uint64_t b_off, bufferlist &bl,
                   uint64_t dev_block_size);
    int verify_csum(uint64_t b_off, bufferlist &bl,
                    uint64_t dev_block_size) const;

    DENC_HELPERS
    void bound_encode(size_t &p) const { _denc_friend(*this, p); }
    void encode(buffer::list::contiguous_appender &p) const {
        _denc_friend(*this, p);
    }
    void decode(buffer::ptr::const_iterator &p) {
        _denc_friend(*this, p);
    }

    template <typename T, typename P>
    friend std::enable_if_t<std::is_same_v<bluestore_blob_t,
                                           std::remove_const_t<T>>>
    _denc_friend(T &v, P &p) {
        DENC_START(2, 2, p);
        denc(v.extents, p);
        denc(v.logical_length, p);
        denc(v.flags, p);
        if (v.has_csum()) {
            denc(v.csum_type, p);
            denc(v.csum_chunk_order, p);
            denc(v.csum_data, p);
        }
        if (v.has_unused()) {
            denc(v.unused, p);
        }
        DENC_FINISH(p);
    }
};
WRITE_CLASS_DENC(bluestore_blob_t);

struct bluestore_onode_t {
    uint64_t nid = 0;
    uint64_t size = 0;
    std::map<std::string, buffer::ptr> attrs;

    struct shard_info {
        uint32_t offset = 0;
        uint32_t bytes = 0;

        DENC(shard_info, v, p) {
            DENC_START(1, 1, p);
            denc(v.offset, p);
            denc(v.bytes, p);
            DENC_FINISH(p);
        }
    };

    std::vector<shard_info> extent_map_shards;
    uint32_t expected_object_size = 0;
    uint32_t expected_write_size = 0;
    uint32_t alloc_hint_flags = 0;
    uint8_t flags = 0;

    enum {
        FLAG_OMAP = 1,
        FLAG_PGMETA_OMAP = 2,
        FLAG_PERPOOL_OMAP = 4,
        FLAG_PERPG_OMAP = 8,
    };

    bool has_flag(unsigned f) const { return flags & f; }
    void set_flag(unsigned f) { flags |= f; }
    void clear_flag(unsigned f) { flags &= ~f; }
    bool has_omap() const { return has_flag(FLAG_OMAP); }

    void set_omap_flags(bool pgmeta = false) {
        if (pgmeta) {
            set_flag(FLAG_OMAP | FLAG_PGMETA_OMAP);
        } else {
            set_flag(FLAG_OMAP);
        }
    }

    void clear_omap_flag() {
        clear_flag(FLAG_OMAP | FLAG_PGMETA_OMAP | FLAG_PERPOOL_OMAP | FLAG_PERPG_OMAP);
    }

    bool is_pgmeta_omap() const { return has_flag(FLAG_PGMETA_OMAP); }

    DENC(bluestore_onode_t, v, p) {
        DENC_START(1, 1, p);
        denc(v.nid, p);
        denc(v.size, p);
        denc(v.attrs, p);
        denc(v.extent_map_shards, p);
        denc(v.expected_object_size, p);
        denc(v.expected_write_size, p);
        denc(v.alloc_hint_flags, p);
        denc(v.flags, p);
        DENC_FINISH(p);
    }
};
WRITE_CLASS_DENC(bluestore_onode_t::shard_info);
WRITE_CLASS_DENC(bluestore_onode_t);

struct bluestore_deferred_op_t {
    enum type_t : uint8_t {
        OP_WRITE = 1,
    };

    uint8_t op = 0;
    PExtentVector extents;
    bufferlist data;

    DENC(bluestore_deferred_op_t, v, p) {
        DENC_START(1, 1, p);
        denc(v.op, p);
        denc(v.extents, p);
        denc(v.data, p);
        DENC_FINISH(p);
    }
};
WRITE_CLASS_DENC(bluestore_deferred_op_t);

struct bluestore_deferred_transaction_t {
    uint64_t seq = 0;
    std::vector<bluestore_deferred_op_t> ops;
    interval_set<uint64_t> released;

    bluestore_deferred_transaction_t() = default;

    DENC(bluestore_deferred_transaction_t, v, p) {
        DENC_START(1, 1, p);
        denc(v.seq, p);
        denc(v.ops, p);
        denc(v.released, p);
        DENC_FINISH(p);
    }
};
WRITE_CLASS_DENC(bluestore_deferred_transaction_t);

}  // namespace TOPNSPC
