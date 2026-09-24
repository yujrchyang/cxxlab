#pragma once

#include <atomic>
#include <cstdint>
#include <ostream>
#include <string_view>
#include <vector>

#include "blk/extent_types.h"
#include "common/denc.h"
#include "common/uuid.h"

namespace TOPNSPC {

class Allocator;

struct bluefs_shared_alloc_context_t {
    Allocator *a = nullptr;
    uint64_t alloc_unit = 0;
    std::atomic<uint64_t> bluefs_used = 0;

    void set(Allocator *_a, uint64_t _au) {
        a = _a;
        alloc_unit = _au;
        bluefs_used = 0;
    }
    void reset() {
        a = nullptr;
        alloc_unit = 0;
    }
};

// =========================================================================
// bluefs_extent_t — physical extent on a block device
// =========================================================================

struct bluefs_extent_t {
    uint64_t offset = 0;
    uint32_t length = 0;
    uint8_t bdev = 0;

    bluefs_extent_t() = default;
    bluefs_extent_t(uint8_t b, uint64_t o, uint32_t l)
        : offset(o), length(l), bdev(b) {}

    uint64_t end() const { return offset + length; }

    DENC(bluefs_extent_t, v, p) {
        DENC_START(1, 1, p);
        denc(v.offset, p);
        denc(v.length, p);
        denc(v.bdev, p);
        DENC_FINISH(p);
    }
};

std::ostream &operator<<(std::ostream &out, const bluefs_extent_t &e);
WRITE_CLASS_DENC(bluefs_extent_t);

// =========================================================================
// bluefs_fnode_delta_t — incremental fnode update (for journal)
// =========================================================================

struct bluefs_fnode_delta_t {
    uint64_t ino = 0;
    uint64_t size = 0;
    uint64_t mtime = 0;
    uint64_t offset = 0;
    std::vector<bluefs_extent_t> extents;

    DENC(bluefs_fnode_delta_t, v, p) {
        DENC_START(1, 1, p);
        denc(v.ino, p);
        denc(v.size, p);
        denc(v.mtime, p);
        denc(v.offset, p);
        denc(v.extents, p);
        DENC_FINISH(p);
    }
};

std::ostream &operator<<(std::ostream &out, const bluefs_fnode_delta_t &d);
WRITE_CLASS_DENC(bluefs_fnode_delta_t);

// =========================================================================
// bluefs_fnode_t — file inode
// =========================================================================

struct bluefs_fnode_t {
    uint64_t ino = 0;
    uint64_t size = 0;
    uint64_t mtime = 0;
    uint8_t unused_ = 0;
    std::vector<bluefs_extent_t> extents;
    std::vector<uint64_t> extents_index;

    uint64_t allocated = 0;
    uint64_t allocated_committed = 0;

    bluefs_fnode_t() = default;
    bluefs_fnode_t(uint64_t _ino, uint64_t _size, uint64_t _mtime)
        : ino(_ino), size(_size), mtime(_mtime) {}
    bluefs_fnode_t(const bluefs_fnode_t &other)
        : ino(other.ino),
          size(other.size),
          mtime(other.mtime),
          allocated(other.allocated),
          allocated_committed(other.allocated_committed) {
        clone_extents(other);
    }

    uint64_t get_allocated() const { return allocated; }

    void recalc_allocated() {
        allocated = 0;
        extents_index.clear();
        extents_index.reserve(extents.size());
        for (auto &e : extents) {
            extents_index.push_back(allocated);
            allocated += e.length;
        }
        allocated_committed = allocated;
    }

    void reset_delta() { allocated_committed = allocated; }

    void clone_extents(const bluefs_fnode_t &fnode) {
        for (const auto &e : fnode.extents) {
            append_extent(e);
        }
    }

    void claim_extents(std::vector<bluefs_extent_t> &e) {
        for (const auto &x : e) append_extent(x);
        e.clear();
    }

    void append_extent(const bluefs_extent_t &ext) {
        if (!extents.empty() && extents.back().end() == ext.offset &&
            extents.back().bdev == ext.bdev &&
            (uint64_t)extents.back().length + (uint64_t)ext.length < 0xffffffff) {
            // Merge with last extent. extents_index for the last extent is
            // already set — it stores the start offset of the extent, which
            // does not change on merge. Merely extending the last extent is
            // safe because extents_index[i] always gives the correct start
            // offset for extent[i], and seek() uses it only for binary search.
            extents.back().length += ext.length;
        } else {
            extents_index.push_back(allocated);
            extents.push_back(ext);
        }
        allocated += ext.length;
    }

    void pop_front_extent() {
        auto it = extents.begin();
        allocated -= it->length;
        extents_index.erase(extents_index.begin());
        for (auto &i : extents_index) {
            i -= it->length;
        }
        extents.erase(it);
    }

    void swap_extents(bluefs_fnode_t &other) {
        other.extents.swap(extents);
        other.extents_index.swap(extents_index);
        std::swap(allocated, other.allocated);
        std::swap(allocated_committed, other.allocated_committed);
    }

    void swap(bluefs_fnode_t &other) {
        std::swap(ino, other.ino);
        std::swap(size, other.size);
        std::swap(mtime, other.mtime);
        swap_extents(other);
    }

    void clear_extents() {
        extents_index.clear();
        extents.clear();
        allocated = 0;
        allocated_committed = 0;
    }

    std::vector<bluefs_extent_t>::iterator seek(uint64_t off,
                                                uint64_t *x_off);
    bluefs_fnode_delta_t make_delta() const;

    DENC_HELPERS
    void bound_encode(size_t &p) const { _denc_friend(*this, p); }
    void encode(buffer::list::contiguous_appender &p) const {
        _denc_friend(*this, p);
    }
    void decode(buffer::ptr::const_iterator &p) {
        _denc_friend(*this, p);
        recalc_allocated();
    }
    template <typename T, typename P>
    friend std::enable_if_t<std::is_same_v<bluefs_fnode_t,
                                           std::remove_const_t<T>>>
    _denc_friend(T &v, P &p) {
        DENC_START(1, 1, p);
        denc(v.ino, p);
        denc(v.size, p);
        denc(v.mtime, p);
        denc(v.unused_, p);
        denc(v.extents, p);
        DENC_FINISH(p);
    }
};

std::ostream &operator<<(std::ostream &out, const bluefs_fnode_t &f);
WRITE_CLASS_DENC(bluefs_fnode_t);

// =========================================================================
// bluefs_super_t — superblock
// =========================================================================

struct bluefs_super_t {
    uuid_d uuid;
    uuid_d osd_uuid;
    uint64_t version = 0;
    uint32_t block_size = 4096;
    bluefs_fnode_t log_fnode;

    uint64_t block_mask() const {
        return ~((uint64_t)block_size - 1);
    }

    DENC(bluefs_super_t, v, p) {
        DENC_START(1, 1, p);
        denc(v.uuid, p);
        denc(v.osd_uuid, p);
        denc(v.version, p);
        denc(v.block_size, p);
        denc(v.log_fnode, p);
        DENC_FINISH(p);
    }
};

std::ostream &operator<<(std::ostream &out, const bluefs_super_t &s);
WRITE_CLASS_DENC(bluefs_super_t);

// =========================================================================
// bluefs_transaction_t — journal transaction
// =========================================================================

struct bluefs_transaction_t {
    enum op_t : uint8_t {
        OP_NONE = 0,
        OP_INIT = 2,
        OP_DIR_LINK = 5,
        OP_DIR_UNLINK = 6,
        OP_DIR_CREATE = 7,
        OP_DIR_REMOVE = 8,
        OP_FILE_UPDATE = 9,
        OP_FILE_REMOVE = 10,
        OP_JUMP = 11,
        OP_JUMP_SEQ = 12,
        OP_FILE_UPDATE_INC = 13,
    };

    uuid_d uuid;
    uint64_t seq = 0;
    bufferlist op_bl;

    void clear() { *this = bluefs_transaction_t(); }
    bool empty() const { return op_bl.length() == 0; }

    void op_init() {
        auto a = op_bl.get_contiguous_appender(1);
        denc(uint8_t(OP_INIT), a);
    }
    void op_dir_create(std::string_view dir) {
        auto a = op_bl.get_contiguous_appender(1 + sizeof(uint32_t) + dir.size());
        denc(uint8_t(OP_DIR_CREATE), a);
        denc(std::string(dir), a);
    }
    void op_dir_remove(std::string_view dir) {
        auto a = op_bl.get_contiguous_appender(1 + sizeof(uint32_t) + dir.size());
        denc(uint8_t(OP_DIR_REMOVE), a);
        denc(std::string(dir), a);
    }
    void op_dir_link(std::string_view dir, std::string_view file,
                     uint64_t ino) {
        auto a = op_bl.get_contiguous_appender(
            1 + sizeof(uint32_t) + dir.size() + sizeof(uint32_t) + file.size() +
            sizeof(uint64_t));
        denc(uint8_t(OP_DIR_LINK), a);
        denc(std::string(dir), a);
        denc(std::string(file), a);
        denc(ino, a);
    }
    void op_dir_unlink(std::string_view dir, std::string_view file) {
        auto a = op_bl.get_contiguous_appender(
            1 + sizeof(uint32_t) + dir.size() + sizeof(uint32_t) + file.size());
        denc(uint8_t(OP_DIR_UNLINK), a);
        denc(std::string(dir), a);
        denc(std::string(file), a);
    }
    void op_file_update(bluefs_fnode_t &file) {
        size_t bound = 0;
        file.bound_encode(bound);
        auto a = op_bl.get_contiguous_appender(1 + bound);
        denc(uint8_t(OP_FILE_UPDATE), a);
        denc(file, a);
        file.reset_delta();
    }
    void op_file_update_inc(bluefs_fnode_t &file) {
        auto delta = file.make_delta();
        size_t bound = 0;
        denc(delta, bound);
        auto a = op_bl.get_contiguous_appender(1 + bound);
        denc(uint8_t(OP_FILE_UPDATE_INC), a);
        denc(delta, a);
        file.reset_delta();
    }
    void op_file_remove(uint64_t ino) {
        auto a = op_bl.get_contiguous_appender(1 + sizeof(uint64_t));
        denc(uint8_t(OP_FILE_REMOVE), a);
        denc(ino, a);
    }
    void op_jump(uint64_t next_seq, uint64_t offset) {
        auto a = op_bl.get_contiguous_appender(1 + sizeof(uint64_t) + sizeof(uint64_t));
        denc(uint8_t(OP_JUMP), a);
        denc(next_seq, a);
        denc(offset, a);
    }
    void op_jump_seq(uint64_t next_seq) {
        auto a = op_bl.get_contiguous_appender(1 + sizeof(uint64_t));
        denc(uint8_t(OP_JUMP_SEQ), a);
        denc(next_seq, a);
    }
    void claim_ops(bluefs_transaction_t &from) {
        op_bl.claim_append(from.op_bl);
    }

    DENC_HELPERS
    void bound_encode(size_t &p) const { _denc_friend(*this, p); }
    void encode(buffer::list::contiguous_appender &p) const {
        _denc_friend(*this, p);
    }
    void decode(buffer::ptr::const_iterator &p) {
        _denc_friend(*this, p);
    }
    template <typename T, typename P>
    friend std::enable_if_t<std::is_same_v<bluefs_transaction_t,
                                           std::remove_const_t<T>>>
    _denc_friend(T &v, P &p) {
        DENC_START(1, 1, p);
        denc(v.uuid, p);
        denc(v.seq, p);
        denc(v.op_bl, p);
        DENC_FINISH(p);
    }
};

std::ostream &operator<<(std::ostream &out, const bluefs_transaction_t &t);
WRITE_CLASS_DENC(bluefs_transaction_t);

}  // namespace TOPNSPC
