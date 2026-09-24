#include "bluefs/bluefs_types.h"

#include <algorithm>

#include "common/denc.h"

namespace TOPNSPC {

std::ostream &operator<<(std::ostream &out, const bluefs_extent_t &e) {
    return out << "extent(bdev=" << (int)e.bdev << " offset=0x" << std::hex
               << e.offset << std::dec << " length=0x" << std::hex << e.length
               << std::dec << ")";
}

std::ostream &operator<<(std::ostream &out, const bluefs_fnode_delta_t &d) {
    out << "delta(ino=" << d.ino << " size=" << d.size << " mtime=" << d.mtime
        << " offset=" << d.offset << " extents=[";
    for (size_t i = 0; i < d.extents.size(); ++i) {
        if (i) out << ", ";
        out << d.extents[i];
    }
    return out << "])";
}

std::ostream &operator<<(std::ostream &out, const bluefs_fnode_t &f) {
    out << "fnode(ino=" << f.ino << " size=" << f.size
        << " allocated=" << f.allocated << " extents=[";
    for (size_t i = 0; i < f.extents.size(); ++i) {
        if (i) out << ", ";
        out << f.extents[i];
    }
    return out << "])";
}

std::ostream &operator<<(std::ostream &out, const bluefs_super_t &s) {
    return out << "super(version=" << s.version
               << " block_size=" << s.block_size << ")";
}

std::ostream &operator<<(std::ostream &out, const bluefs_transaction_t &t) {
    return out << "txn(seq=" << t.seq << " ops_len=" << t.op_bl.length()
               << ")";
}

std::vector<bluefs_extent_t>::iterator bluefs_fnode_t::seek(
    uint64_t off, uint64_t *x_off) {
    if (extents.empty()) return extents.end();
    if (off >= allocated) return extents.end();

    auto it = std::upper_bound(extents_index.begin(), extents_index.end(),
                               off);
    if (it == extents_index.begin()) return extents.end();
    --it;

    size_t idx = it - extents_index.begin();
    if (x_off) *x_off = off - extents_index[idx];
    return extents.begin() + idx;
}

bluefs_fnode_delta_t bluefs_fnode_t::make_delta() const {
    bluefs_fnode_delta_t delta;
    delta.ino = ino;
    delta.size = size;
    delta.mtime = mtime;
    delta.offset = allocated_committed;

    uint64_t pos = 0;
    for (auto &e : extents) {
        if (pos + e.length <= allocated_committed) {
            pos += e.length;
            continue;
        }
        uint64_t skip = (pos < allocated_committed)
            ? (allocated_committed - pos)
            : 0;
        if (skip == 0) {
            delta.extents.push_back(e);
        } else {
            delta.extents.push_back(
                bluefs_extent_t(e.bdev, e.offset + skip, e.length - skip));
        }
        pos += e.length;
    }
    return delta;
}

}  // namespace TOPNSPC
