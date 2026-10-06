#pragma once

#include <cstdint>
#include <set>
#include <vector>

#include "bluestore/blob.h"

namespace TOPNSPC {

struct Extent {
    uint32_t logical_offset = 0;
    uint32_t blob_offset = 0;
    uint32_t length = 0;
    BlobRef blob = nullptr;

    Extent() = default;
    Extent(uint32_t lo, uint32_t bo, uint32_t l, BlobRef b)
        : logical_offset(lo), blob_offset(bo), length(l), blob(b) {}

    bool operator<(const Extent &other) const {
        return logical_offset < other.logical_offset;
    }

    uint32_t logical_end() const { return logical_offset + length; }

    uint32_t blob_start() const { return logical_offset - blob_offset; }

    uint32_t blob_end() const {
        return blob_start() + blob->get_blob().get_logical_length();
    }

    bool blob_escapes_range(uint32_t o, uint32_t l) const {
        return blob_start() < o || blob_end() > o + l;
    }
};

struct OldExtent {
    Extent e;
    PExtentVector released;
    bool blob_empty = false;

    OldExtent(uint32_t lo, uint32_t bo, uint32_t l, BlobRef b)
        : e(lo, bo, l, b) {}
};

class ExtentMap {
public:
    using extent_map_t = std::set<Extent>;
    using iterator = extent_map_t::iterator;
    using const_iterator = extent_map_t::const_iterator;

    ExtentMap() = default;
    ~ExtentMap() = default;

    ExtentMap(const ExtentMap &) = delete;
    ExtentMap &operator=(const ExtentMap &) = delete;

    iterator seek_lextent(uint64_t offset);
    const_iterator seek_lextent(uint64_t offset) const;

    iterator add(uint32_t lo, uint32_t bo, uint32_t l, BlobRef b);

    void rm(iterator it);

    void punch_hole(uint64_t offset, uint64_t length,
                    std::vector<OldExtent> *old_extents);

    iterator set_lextent(uint32_t logical_offset, uint32_t blob_offset,
                         uint32_t length, BlobRef b,
                         std::vector<OldExtent> *old_extents);

    bool has_any_lextents(uint64_t offset, uint64_t length) const {
        for (const auto &e : extent_map_) {
            if (e.logical_offset >= offset + length) {
                break;
            }
            if (e.logical_end() <= offset) {
                continue;
            }
            return true;
        }
        return false;
    }

    int compress_extent_map(uint64_t offset, uint64_t length);

    bool needs_reshard() const { return false; }

    size_t size() const { return extent_map_.size(); }
    bool empty() const { return extent_map_.empty(); }

    iterator begin() { return extent_map_.begin(); }
    iterator end() { return extent_map_.end(); }
    const_iterator begin() const { return extent_map_.begin(); }
    const_iterator end() const { return extent_map_.end(); }

    void clear() { extent_map_.clear(); }

private:
    extent_map_t extent_map_;
};

}  // namespace TOPNSPC
