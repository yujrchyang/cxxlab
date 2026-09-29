#include "bluestore/bluestore_types.h"

#include <algorithm>
#include <cstring>

#include "common/crc32.h"

namespace TOPNSPC {

bluestore_blob_use_tracker_t::bluestore_blob_use_tracker_t(
    const bluestore_blob_use_tracker_t &other)
    : au_size(0), num_au(0), alloc_au(0), bytes_per_au(nullptr) {
    if (other.au_size) {
        init(other.au_size * (other.num_au ? other.num_au : 1), other.au_size);
        if (!other.num_au) {
            total_bytes = other.total_bytes;
        } else {
            std::memcpy(bytes_per_au, other.bytes_per_au,
                        other.num_au * sizeof(uint32_t));
        }
    }
}

bluestore_blob_use_tracker_t &bluestore_blob_use_tracker_t::operator=(
    const bluestore_blob_use_tracker_t &rhs) {
    if (this != &rhs) {
        clear();
        if (rhs.au_size) {
            init(rhs.au_size * (rhs.num_au ? rhs.num_au : 1), rhs.au_size);
            if (!rhs.num_au) {
                total_bytes = rhs.total_bytes;
            } else {
                std::memcpy(bytes_per_au, rhs.bytes_per_au,
                            rhs.num_au * sizeof(uint32_t));
            }
        }
    }
    return *this;
}

void bluestore_blob_use_tracker_t::allocate(uint32_t _num_au) {
    alloc_au = _num_au;
    num_au = _num_au;
    bytes_per_au = new uint32_t[_num_au];
    std::memset(bytes_per_au, 0, _num_au * sizeof(uint32_t));
}

void bluestore_blob_use_tracker_t::release(uint32_t _num_au, uint32_t *ptr) {
    if (_num_au && ptr) {
        delete[] ptr;
    }
}

void bluestore_blob_use_tracker_t::init(uint32_t full_length,
                                        uint32_t _au_size) {
    clear();
    au_size = _au_size;
    uint32_t _num_au = full_length / _au_size;
    if (_num_au <= 1) {
        num_au = 0;
        total_bytes = 0;
    } else {
        allocate(_num_au);
    }
}

void bluestore_blob_use_tracker_t::get(uint32_t offset, uint32_t len) {
    if (!num_au) {
        total_bytes += len;
    } else {
        uint32_t end = offset + len;
        while (offset < end) {
            uint32_t phase = offset % au_size;
            size_t pos = offset / au_size;
            uint32_t diff = std::min(au_size - phase, end - offset);
            bytes_per_au[pos] += diff;
            offset += diff;
        }
    }
}

bool bluestore_blob_use_tracker_t::put(uint32_t offset, uint32_t len,
                                       PExtentVector *release) {
    if (release) {
        release->clear();
    }

    bool maybe_empty = true;
    if (!num_au) {
        if (total_bytes <= len) {
            total_bytes = 0;
            return true;
        }
        total_bytes -= len;
        return false;
    }

    uint32_t end = offset + len;
    uint64_t next_offs = 0;

    while (offset < end) {
        uint32_t phase = offset % au_size;
        size_t pos = offset / au_size;
        uint32_t diff = std::min(au_size - phase, end - offset);

        bytes_per_au[pos] -= diff;
        offset += (phase ? au_size - phase : au_size);

        if (bytes_per_au[pos] == 0) {
            if (release) {
                if (release->empty() || next_offs != pos * au_size) {
                    release->emplace_back(pos * au_size, au_size);
                    next_offs = pos * au_size;
                } else {
                    release->back().length += au_size;
                }
                next_offs += au_size;
            }
        } else {
            maybe_empty = false;
        }
    }

    bool empty = maybe_empty ? !is_not_empty() : false;
    if (empty && release) {
        release->clear();
    }

    return empty;
}

bool bluestore_blob_use_tracker_t::can_split() const {
    return num_au > 0;
}

bool bluestore_blob_use_tracker_t::can_split_at(uint32_t blob_offset) const {
    return blob_offset % au_size == 0;
}

void bluestore_blob_use_tracker_t::split(uint32_t blob_offset,
                                         bluestore_blob_use_tracker_t *r) {
    uint32_t split_au = blob_offset / au_size;
    uint32_t right_num_au = num_au - split_au;

    r->au_size = au_size;
    r->allocate(right_num_au);
    std::memcpy(r->bytes_per_au, bytes_per_au + split_au,
                right_num_au * sizeof(uint32_t));

    uint32_t *old_bytes = bytes_per_au;
    uint32_t old_alloc = alloc_au;
    alloc_au = 0;
    num_au = 0;
    bytes_per_au = nullptr;
    allocate(split_au);
    std::memcpy(bytes_per_au, old_bytes, split_au * sizeof(uint32_t));
    release(old_alloc, old_bytes);
}

void bluestore_blob_use_tracker_t::add_tail(uint32_t new_len, uint32_t _au_size) {
    uint32_t full_size = au_size * (num_au ? num_au : 1);
    if (new_len == full_size) {
        return;
    }
    if (!num_au) {
        uint32_t old_total = total_bytes;
        total_bytes = 0;
        init(new_len, _au_size);
        if (num_au) {
            bytes_per_au[0] = old_total;
        }
    } else {
        uint32_t _num_au = new_len / _au_size;
        if (_num_au > num_au) {
            auto old_bytes = bytes_per_au;
            auto old_num_au = num_au;
            auto old_alloc_au = alloc_au;
            alloc_au = num_au = 0;
            bytes_per_au = nullptr;
            allocate(_num_au);
            for (uint32_t i = 0; i < old_num_au; i++) {
                bytes_per_au[i] = old_bytes[i];
            }
            for (uint32_t i = old_num_au; i < num_au; i++) {
                bytes_per_au[i] = 0;
            }
            release(old_alloc_au, old_bytes);
        }
    }
}

void bluestore_blob_t::allocated(uint32_t b_off, uint32_t length,
                                 const PExtentVector &allocs) {
    extents = allocs;
    logical_length = length;
}

void bluestore_blob_t::split(uint32_t blob_offset, bluestore_blob_t &rb) {
    rb.extents.clear();
    rb.logical_length = 0;

    uint32_t offset = 0;
    auto it = extents.begin();
    while (it != extents.end() && offset + it->length <= blob_offset) {
        offset += it->length;
        ++it;
    }

    if (it != extents.end() && offset < blob_offset) {
        uint32_t split_len = blob_offset - offset;
        rb.extents.emplace_back(it->offset + split_len, it->length - split_len);
        it->length = split_len;
        ++it;
    }

    while (it != extents.end()) {
        rb.extents.push_back(*it);
        it = extents.erase(it);
    }

    rb.logical_length = logical_length - blob_offset;
    logical_length = blob_offset;

    if (has_csum()) {
        size_t csum_size = get_csum_value_size();
        uint32_t left_chunks = blob_offset / get_csum_chunk_size();
        uint32_t right_chunks = rb.logical_length / get_csum_chunk_size();

        rb.csum_data = buffer::create(csum_size * right_chunks);
        std::memcpy(rb.csum_data.c_str(),
                    csum_data.c_str() + csum_size * left_chunks,
                    csum_size * right_chunks);

        buffer::ptr new_csum = buffer::create(csum_size * left_chunks);
        std::memcpy(new_csum.c_str(), csum_data.c_str(),
                    csum_size * left_chunks);
        csum_data = new_csum;

        rb.csum_type = csum_type;
        rb.csum_chunk_order = csum_chunk_order;
        rb.flags |= FLAG_CSUM;
    }
}

bool bluestore_blob_t::release_extents(bool all,
                                       const PExtentVector &logical,
                                       PExtentVector *r) {
    if (all) {
        for (auto &e : extents) {
            if (e.is_valid()) {
                r->push_back(e);
            }
        }
        extents.clear();
        logical_length = 0;
        return true;
    }

    PExtentVector new_extents;
    uint32_t new_logical_length = 0;
    uint32_t logical_off = 0;

    for (const auto &e : extents) {
        uint32_t e_end = logical_off + e.length;
        bool fully_released = false;

        for (const auto &l : logical) {
            if (l.offset <= logical_off && l.offset + l.length >= e_end) {
                fully_released = true;
                if (e.is_valid()) {
                    r->push_back(e);
                }
                break;
            }
        }

        if (!fully_released) {
            new_extents.push_back(e);
            new_logical_length += e.length;
        }
        logical_off = e_end;
    }

    extents = std::move(new_extents);
    logical_length = new_logical_length;

    return extents.empty();
}

void bluestore_blob_t::calc_csum(uint64_t b_off, bufferlist &bl,
                                 uint64_t dev_block_size) {
    if (!has_csum() || bl.length() == 0) return;

    uint64_t chunk_size = get_chunk_size(dev_block_size);
    uint32_t blocks = bl.length() / chunk_size;
    size_t vsz = get_csum_value_size();

    for (uint32_t i = 0; i < blocks; ++i) {
        uint32_t crc = calc_crc32(
            reinterpret_cast<const uint8_t *>(bl.c_str()) + i * chunk_size,
            chunk_size);

        uint32_t csum_idx = b_off / chunk_size + i;
        std::memcpy(csum_data.c_str() + csum_idx * vsz, &crc, vsz);
    }
}

int bluestore_blob_t::verify_csum(uint64_t b_off, bufferlist &bl,
                                  uint64_t dev_block_size) const {
    if (!has_csum() || bl.length() == 0) return -1;

    uint64_t chunk_size = get_chunk_size(dev_block_size);
    uint32_t blocks = bl.length() / chunk_size;
    size_t vsz = get_csum_value_size();

    for (uint32_t i = 0; i < blocks; ++i) {
        uint32_t crc = calc_crc32(
            reinterpret_cast<const uint8_t *>(bl.c_str()) + i * chunk_size,
            chunk_size);

        uint32_t csum_idx = b_off / chunk_size + i;
        uint32_t stored_crc;
        std::memcpy(&stored_crc, csum_data.c_str() + csum_idx * vsz, vsz);

        if (crc != stored_crc) {
            return b_off + i * chunk_size;
        }
    }

    return -1;
}

}  // namespace TOPNSPC
