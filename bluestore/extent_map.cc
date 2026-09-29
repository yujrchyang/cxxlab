#include "bluestore/extent_map.h"

#include <algorithm>

namespace TOPNSPC {

ExtentMap::iterator ExtentMap::seek_lextent(uint64_t offset) {
    Extent key;
    key.logical_offset = offset + 1;
    auto it = extent_map_.lower_bound(key);
    if (it == extent_map_.begin()) {
        return extent_map_.end();
    }
    --it;
    if (it->logical_end() > offset) {
        return it;
    }
    return extent_map_.end();
}

ExtentMap::const_iterator ExtentMap::seek_lextent(uint64_t offset) const {
    Extent key;
    key.logical_offset = offset + 1;
    auto it = extent_map_.lower_bound(key);
    if (it == extent_map_.begin()) {
        return extent_map_.end();
    }
    --it;
    if (it->logical_end() > offset) {
        return it;
    }
    return extent_map_.end();
}

ExtentMap::iterator ExtentMap::add(uint32_t lo, uint32_t bo, uint32_t l,
                                   BlobRef b) {
    Extent e(lo, bo, l, b);
    auto result = extent_map_.insert(e);
    return result.second ? result.first : extent_map_.end();
}

void ExtentMap::rm(iterator it) {
    extent_map_.erase(it);
}

void ExtentMap::punch_hole(uint64_t offset, uint64_t length,
                           std::vector<OldExtent> *old_extents) {
    uint64_t end = offset + length;

    auto it = seek_lextent(offset);
    if (it == extent_map_.end()) {
        it = extent_map_.begin();
    }

    while (it != extent_map_.end()) {
        uint32_t e_lo = it->logical_offset;
        uint32_t e_end = it->logical_end();
        uint32_t e_bo = it->blob_offset;
        BlobRef e_blob = it->blob;

        if (e_lo >= end) {
            break;
        }

        if (e_end <= offset) {
            ++it;
            continue;
        }

        old_extents->emplace_back(e_lo, e_bo, it->length, e_blob);

        if (e_lo < offset && e_end > end) {
            uint32_t new_len = offset - e_lo;
            uint32_t new_bo = e_bo;
            extent_map_.erase(it);
            add(e_lo, new_bo, new_len, e_blob);

            uint32_t tail_lo = end;
            uint32_t tail_bo = e_bo + (end - e_lo);
            uint32_t tail_len = e_end - end;
            add(tail_lo, tail_bo, tail_len, e_blob);
            break;
        }

        if (e_lo < offset) {
            uint32_t new_len = offset - e_lo;
            uint32_t new_bo = e_bo;
            extent_map_.erase(it);
            add(e_lo, new_bo, new_len, e_blob);
            it = seek_lextent(offset);
            if (it == extent_map_.end()) {
                it = extent_map_.begin();
            }
            continue;
        }

        if (e_end > end) {
            uint32_t new_lo = end;
            uint32_t new_bo = e_bo + (end - e_lo);
            uint32_t new_len = e_end - end;
            extent_map_.erase(it);
            add(new_lo, new_bo, new_len, e_blob);
            break;
        }

        extent_map_.erase(it);
        it = extent_map_.begin();
    }
}

int ExtentMap::compress_extent_map(uint64_t offset, uint64_t length) {
    int compressed = 0;

    auto it = extent_map_.begin();
    while (it != extent_map_.end()) {
        if (it->logical_offset >= offset + length) {
            break;
        }
        if (it->logical_end() <= offset) {
            ++it;
            continue;
        }

        auto next = std::next(it);
        if (next == extent_map_.end()) {
            break;
        }

        if (it->logical_end() == next->logical_offset &&
            it->blob == next->blob &&
            it->blob_offset + it->length == next->blob_offset) {
            uint32_t new_len = it->length + next->length;
            uint32_t lo = it->logical_offset;
            uint32_t bo = it->blob_offset;
            BlobRef b = it->blob;

            extent_map_.erase(it);
            extent_map_.erase(next);
            add(lo, bo, new_len, b);

            ++compressed;
            it = extent_map_.begin();
        } else {
            ++it;
        }
    }

    return compressed;
}

ExtentMap::iterator ExtentMap::set_lextent(uint32_t logical_offset,
                                           uint32_t blob_offset,
                                           uint32_t length, BlobRef b,
                                           std::vector<OldExtent> *old_extents) {
    if (old_extents) {
        punch_hole(logical_offset, length, old_extents);
    }
    return add(logical_offset, blob_offset, length, b);
}

}  // namespace TOPNSPC
