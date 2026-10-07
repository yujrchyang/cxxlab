#include "bluestore/blob.h"

#include <algorithm>

#include "bluestore/collection.h"

namespace TOPNSPC {

BufferCache *Blob::get_cache() const {
    return coll_ ? coll_->get_cache() : nullptr;
}

void Blob::get_ref(uint32_t offset, uint32_t length, uint32_t min_alloc_size) {
    cxxlab_assert(blob_.get_logical_length() != 0);

    if (used_in_blob_.is_empty()) {
        uint32_t min_release_size = min_alloc_size;
        if (blob_.has_csum()) {
            min_release_size =
                std::max(min_alloc_size, blob_.get_csum_chunk_size());
        }
        uint64_t l = blob_.get_logical_length();
        used_in_blob_.init(l, min_release_size);
    }
    used_in_blob_.get(offset, length);
}

bool Blob::put_ref(uint32_t offset, uint32_t length, PExtentVector *r) {
    PExtentVector logical;

    bool empty = used_in_blob_.put(offset, length, &logical);
    r->clear();

    if (!empty && logical.empty()) {
        return false;
    }

    return blob_.release_extents(empty, logical, r);
}

void Blob::split(uint32_t blob_offset, Blob *r) {
    used_in_blob_.split(blob_offset, &r->used_in_blob_);
    blob_.split(blob_offset, r->blob_);
}

bool Blob::can_reuse_blob(uint32_t min_alloc_size, uint32_t target_blob_size,
                          uint32_t b_offset, uint32_t *length0) {
    if (!blob_.is_mutable()) {
        return false;
    }

    uint32_t length = *length0;
    uint32_t end = b_offset + length;

    if (blob_.has_csum() &&
        ((b_offset % blob_.get_csum_chunk_size()) != 0 ||
         (end % blob_.get_csum_chunk_size()) != 0)) {
        return false;
    }

    auto blen = blob_.get_logical_length();
    uint32_t new_blen = blen;

    target_blob_size = std::max(blen, target_blob_size);

    if (b_offset >= blen) {
        new_blen = end;
    } else {
        new_blen = std::max(blen, end);

        uint32_t overlap = 0;
        if (new_blen > blen) {
            overlap = blen - b_offset;
        } else {
            overlap = length;
        }

        if (!blob_.is_unallocated(b_offset, overlap)) {
            return false;
        }
    }

    if (new_blen > blen) {
        new_blen = (new_blen + min_alloc_size - 1) & ~(min_alloc_size - 1);

        int64_t overflow = int64_t(new_blen) - target_blob_size;
        if (overflow >= length) {
            return false;
        }

        if (blob_.has_unused()) {
            return false;
        }

        if (overflow > 0) {
            new_blen -= overflow;
            length -= overflow;
            *length0 = length;
        }

        if (new_blen > blen) {
            blob_.add_tail(new_blen);
            uint32_t au = min_alloc_size;
            if (blob_.has_csum()) {
                au = std::max(min_alloc_size,
                              blob_.get_csum_chunk_size());
            }
            used_in_blob_.add_tail(new_blen, au);
        }
    }
    return true;
}

}  // namespace TOPNSPC
