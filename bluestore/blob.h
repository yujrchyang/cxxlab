#pragma once

#include <atomic>
#include <cstdint>

#include "bluestore_types.h"

namespace TOPNSPC {

class Blob {
public:
    Blob() = default;
    ~Blob() = default;

    Blob(const Blob &) = delete;
    Blob &operator=(const Blob &) = delete;

    void get() { ++nref_; }
    bool put() {
        if (--nref_ == 0) {
            delete this;
            return true;
        }
        return false;
    }
    int get_nref() const { return nref_.load(); }

    const bluestore_blob_t &get_blob() const { return blob_; }
    bluestore_blob_t &dirty_blob() { return blob_; }

    const bluestore_blob_use_tracker_t &get_blob_use_tracker() const {
        return used_in_blob_;
    }

    bool is_referenced() const { return used_in_blob_.is_not_empty(); }

    uint32_t get_referenced_bytes() const {
        return used_in_blob_.get_referenced_bytes();
    }

    bool is_spanning() const { return id_ >= 0; }

    int16_t get_id() const { return id_; }
    void set_id(int16_t id) { id_ = id; }

    bool can_split() const {
        return used_in_blob_.can_split() && blob_.can_split();
    }

    bool can_split_at(uint32_t blob_offset) const {
        return used_in_blob_.can_split_at(blob_offset) &&
            blob_.can_split_at(blob_offset);
    }

    void get_ref(uint32_t offset, uint32_t length, uint32_t min_alloc_size);

    bool put_ref(uint32_t offset, uint32_t length, PExtentVector *r);

    void split(uint32_t blob_offset, Blob *r);

    bool can_reuse_blob(uint32_t min_alloc_size, uint32_t target_blob_size,
                        uint32_t b_offset, uint32_t *length0);

private:
    std::atomic_int nref_{0};
    int16_t id_{-1};
    bluestore_blob_t blob_;
    bluestore_blob_use_tracker_t used_in_blob_;
};

using BlobRef = Blob *;

}  // namespace TOPNSPC
