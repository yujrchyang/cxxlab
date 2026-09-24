#include "bluestore/bluefs.h"

#include <cerrno>

#include "blk/block_device.h"
#include "common/buffer_error.h"
#include "common/denc.h"

namespace TOPNSPC {

BlueFS::BlueFS(const BlueFSConfig &cfg)
    : cfg_(cfg),
      bdev_(MAX_BDEV),
      ioc_(MAX_BDEV),
      block_reserved_(MAX_BDEV),
      alloc_(MAX_BDEV),
      alloc_size_(MAX_BDEV, 0) {
    dirty_.pending_release.resize(MAX_BDEV);
}

BlueFS::~BlueFS() {
    umount(true);
    for (auto p : ioc_) {
        if (p) p->aio_wait();  // 等待 I/O 完成
    }
    for (auto p : bdev_) {
        if (p) {
            p->close();
            delete p;  // BlueFS 拥有，负责删除
        }
    }
    for (auto p : ioc_) {
        delete p;  // BlueFS 拥有，负责删除
    }
    // 注意：alloc_ 已在 umount() -> _stop_alloc() 中清理
}

// =====================================================================
// 设备管理
// =====================================================================

int BlueFS::add_block_device(unsigned id, const std::string &path, bool trim,
                             uint64_t reserved,
                             bluefs_shared_alloc_context_t *shared_alloc) {
    if (id >= bdev_.size() || bdev_[id] != nullptr) return -EBUSY;
    if (shared_alloc && shared_alloc_) return -EBUSY;

    auto b = BlockDevice::create(path, nullptr, nullptr);
    if (!b) return -ENOMEM;

    int r = b->open(path);
    if (r < 0) return r;

    if (reserved > b->get_size()) return -EINVAL;

    block_reserved_[id] = reserved;

    if (trim) {
        b->discard(0, b->get_size());
    }

    bdev_[id] = b.release();           // 转移所有权到 BlueFS
    ioc_[id] = new IOContext(nullptr); // BlueFS 拥有

    if (shared_alloc) {
        shared_alloc_ = shared_alloc;
        alloc_[id] = shared_alloc->a;  // 非拥有：指向外部分配器，BlueFS 不删除
        shared_alloc_id_ = id;
    }

    return 0;
}

uint64_t BlueFS::get_block_device_size(unsigned id) const {
    if (id < bdev_.size() && bdev_[id]) {
        return bdev_[id]->get_size();
    }
    return 0;
}

uint64_t BlueFS::_get_total(unsigned id) const {
    if (id >= bdev_.size() || !bdev_[id]) return 0;
    uint64_t sz = get_block_device_size(id);
    if (block_reserved_[id] >= sz) return 0;
    return sz - block_reserved_[id];
}

bool BlueFS::_file_exists(uint64_t ino) {
    std::lock_guard l(nodes_.lock);
    return nodes_.file_map.find(ino) != nodes_.file_map.end();
}

uint64_t BlueFS::get_total(unsigned id) const {
    return _get_total(id);
}

uint64_t BlueFS::get_free(unsigned id) {
    if (id >= bdev_.size() || !bdev_[id]) return 0;
    if (is_shared_alloc(id)) {
        uint64_t total = _get_total(id);
        uint64_t used = shared_alloc_ ? shared_alloc_->bluefs_used.load() : 0;
        return total > used ? total - used : 0;
    }
    if (alloc_[id]) {
        return alloc_[id]->get_free();
    }
    return 0;
}

// =====================================================================
// 分配器初始化
// =====================================================================

void BlueFS::_init_alloc() {
    dirty_.pending_release.resize(MAX_BDEV);
    for (unsigned id = 0; id < bdev_.size(); ++id) {
        if (!bdev_[id]) continue;
        if (is_shared_alloc(id)) continue;

        if (alloc_[id]) {
            alloc_[id]->shutdown();
            delete alloc_[id];  // 仅删除 BlueFS 拥有的分配器
            alloc_[id] = nullptr;
        }

        alloc_[id] = Allocator::create("avl", bdev_[id]->get_size(),
                                       cfg_.alloc_size);  // BlueFS 拥有
        uint64_t total = _get_total(id);
        uint64_t skip = block_reserved_[id];
        if (id == BDEV_DB) {
            skip = std::max(skip, uint64_t(SUPER_OFFSET + SUPER_LENGTH));
        }
        if (total > skip) {
            alloc_[id]->init_add_free(skip, total - skip);
        }
    }
}

void BlueFS::_stop_alloc() {
    for (auto &a : alloc_) {
        if (a) {
            if (shared_alloc_ && a == shared_alloc_->a) {
                continue;  // 跳过：非拥有，由外部管理
            }
            a->shutdown();
            delete a;  // 仅删除 BlueFS 拥有的分配器
            a = nullptr;
        }
    }
}

// =====================================================================
// 超级块
// =====================================================================

int BlueFS::_write_super(int dev) {
    if (dev >= bdev_.size() || !bdev_[dev]) return -ENODEV;

    bluefs_super_t tmp = super_;
    ++tmp.version;

    bufferlist bl;
    encode(tmp, bl);
    uint32_t crc = bl.crc32c(-1);
    encode(crc, bl);

    if (bl.length() > SUPER_LENGTH) return -EIO;
    bl.append_zero(SUPER_LENGTH - bl.length());

    int r = bdev_[dev]->write(SUPER_OFFSET, bl, false, WRITE_LIFE_SHORT);
    if (r < 0) return r;

    super_.version = tmp.version;
    return 0;
}

int BlueFS::_open_super() {
    if (!bdev_[BDEV_DB]) return -ENODEV;
    if (!ioc_[BDEV_DB]) return -ENODEV;

    bufferlist bl;
    int r = bdev_[BDEV_DB]->read(SUPER_OFFSET, SUPER_LENGTH, &bl,
                                 ioc_[BDEV_DB], false);
    if (r < 0) return r;

    try {
        auto p = bl.cbegin();
        decode(super_, p);

        bufferlist t;
        t.substr_of(bl, 0, p.get_off());
        uint32_t actual_crc = t.crc32c(-1);

        uint32_t expected_crc;
        decode(expected_crc, p);

        if (actual_crc != expected_crc) return -EIO;
    } catch (const buffer::malformed_input &) {
        return -EIO;
    }

    return 0;
}

// =====================================================================
// 内部辅助
// =====================================================================

BlueFS::FileRef BlueFS::_get_file(uint64_t ino) {
    std::lock_guard l(nodes_.lock);
    auto it = nodes_.file_map.find(ino);
    if (it != nodes_.file_map.end()) {
        return it->second;
    }
    auto f = std::make_shared<File>(ino);
    nodes_.file_map[ino] = f;
    return f;
}

// =====================================================================
// 空间分配
// =====================================================================

int BlueFS::_allocate(uint8_t prefer_bdev, uint64_t len, uint64_t alloc_unit,
                      bluefs_fnode_t *node, uint64_t *hint) {
    // 注意：prefer_bdev 可能是共享分配器对应的设备
    // is_shared_alloc(prefer_bdev) 为 true 时，alloc_[prefer_bdev] 指向外部分配器
    // 此方法只是使用分配器，不涉及所有权转移
    
    if (alloc_unit == 0) {
        alloc_unit = alloc_size_[prefer_bdev];
        if (alloc_unit == 0) alloc_unit = cfg_.alloc_size;
    }

    uint64_t need = p2roundup(len, alloc_unit);

    for (unsigned id = prefer_bdev; id < bdev_.size(); ++id) {
        if (!bdev_[id]) continue;
        if (is_shared_alloc(id) && (!shared_alloc_ || !shared_alloc_->a))
            continue;

        int64_t hint_val = 0;
        if (hint && id == prefer_bdev) hint_val = *hint;

        PExtentVector extents;
        int64_t alloc_len = alloc_[id]->allocate(need, alloc_unit, 0,
                                                 hint_val, &extents);

        if (alloc_len <= 0) continue;

        if (is_shared_alloc(id) && shared_alloc_) {
            shared_alloc_->bluefs_used += alloc_len;
        }

        for (auto &e : extents) {
            bluefs_extent_t ext;
            ext.bdev = id;
            ext.offset = e.offset;
            ext.length = e.length;
            node->append_extent(ext);
        }

        if (hint) {
            auto &last = extents.back();
            *hint = last.offset + last.length;
        }

        return 0;
    }

    return -ENOSPC;
}

// =====================================================================
// 写入器管理
// =====================================================================

BlueFS::FileWriter *BlueFS::_create_writer(FileRef f) {
    auto w = new FileWriter(f);
    for (unsigned i = 0; i < MAX_BDEV; ++i) {
        if (bdev_[i]) {
            w->iocv[i] = new IOContext(nullptr);
        }
    }
    w->pos = f->fnode.size;
    return w;
}

void BlueFS::_drain_writer(FileWriter *h) {
    for (unsigned i = 0; i < MAX_BDEV; ++i) {
        if (bdev_[i] && h->iocv[i]) {
            h->iocv[i]->aio_wait();
            delete h->iocv[i];
            h->iocv[i] = nullptr;
        }
    }
}

void BlueFS::_close_writer(FileWriter *h) {
    if (!h) return;
    _drain_writer(h);
    delete h;
}

int BlueFS::close_writer(FileWriter *h) {
    if (!h) return -EIO;
    int r = _flush_F(h, true);
    if (r < 0) return r;
    _flush_bdev(h);
    _close_writer(h);
    return 0;
}

void BlueFS::close_reader(FileReader *h) {
    delete h;
}

void BlueFS::_flush_bdev(FileWriter *h) {
    for (unsigned i = 0; i < MAX_BDEV; ++i) {
        if (h->dirty_devs[i] && h->iocv[i]) {
            h->iocv[i]->aio_wait();
            h->dirty_devs[i] = false;
            if (bdev_[i]) {
                bdev_[i]->flush();
            }
        }
    }
}

void BlueFS::_flush_special(FileWriter *h) {
    for (unsigned i = 0; i < MAX_BDEV; ++i) {
        if (h->dirty_devs[i] && h->iocv[i]) {
            h->iocv[i]->aio_wait();
            h->dirty_devs[i] = false;
        }
    }
}

bufferlist BlueFS::FileWriter::flush_buffer(uint64_t block_size,
                                            uint64_t block_mask) {
    bufferlist bl;

    // Prepend any tail_block from a previous partial write
    if (tail_block.length()) {
        tail_block.splice(0, tail_block.length(), &bl);
    }

    // Move the current buffer content
    buffer.splice(0, buffer.length(), &bl);

    // Save unaligned tail for next write (ensures O_DIRECT alignment)
    if (bl.length() & ~block_mask) {
        uint64_t tail = bl.length() & ~block_mask;
        bl.splice(bl.length() - tail, tail, &tail_block);
    } else {
        tail_block.clear();
    }

    return bl;
}

int BlueFS::_flush_data(FileWriter *h, uint64_t offset, uint64_t length,
                        bool buffered) {
    auto &fnode = h->file->fnode;

    if (length == 0) {
        return 0;
    }

    bufferlist &bl = h->buffer;

    uint64_t x_off = 0;
    auto it = fnode.seek(offset, &x_off);
    if (it == fnode.extents.end()) {
        return -ENOENT;
    }

    uint64_t bloff = 0;
    uint64_t remaining = bl.length();

    while (remaining > 0 && it != fnode.extents.end()) {
        uint64_t x_len = std::min<uint64_t>(it->length - x_off, remaining);

        bufferlist t;
        t.substr_of(bl, bloff, x_len);

        int r = bdev_[it->bdev]->write(it->offset + x_off, t, buffered,
                                       WRITE_LIFE_NOT_SET);
        if (r < 0) return r;
        h->dirty_devs[it->bdev] = true;

        bloff += x_len;
        remaining -= x_len;
        x_off = 0;
        ++it;
    }

    h->pos += length;
    h->file->fnode.size = std::max(h->file->fnode.size, h->pos);
    return 0;
}

int BlueFS::_flush_F(FileWriter *h, bool force) {
    std::lock_guard l(h->lock);
    uint64_t length = h->get_buffer_length();
    if (length == 0) {
        return 0;
    }
    if (!force && length < cfg_.min_flush_size) {
        return 0;
    }

    int r = _flush_range_F(h, h->pos, length);
    if (r == 0) {
        h->buffer.clear();
    }
    return r;
}

int BlueFS::_flush_range_F(FileWriter *h, uint64_t offset, uint64_t length) {
    if (h->file->deleted) return 0;
    if (offset + length <= h->pos) {
        return 0;
    }
    if (offset < h->pos) {
        length -= h->pos - offset;
        offset = h->pos;
    }

    uint64_t allocated = h->file->fnode.get_allocated();
    if (allocated < offset + length) {
        int r = _allocate(
            vselector_->select_prefer_bdev(h->file->vselector_hint),
            offset + length - allocated, 0, &h->file->fnode);
        if (r < 0) {
            return r;
        }
        h->file->is_dirty = true;
    }

    if (h->file->fnode.size < offset + length) {
        if (vselector_ && h->file->vselector_hint) {
            vselector_->add_usage(h->file->vselector_hint,
                                  offset + length - h->file->fnode.size,
                                  false);
        }
        h->file->fnode.size = offset + length;
        h->file->is_dirty = true;
    }

    bool buffered = cfg_.buffered_io;
    int r = _flush_data(h, offset, length, buffered);
    return r;
}

// =====================================================================
// 日志数据写入
// =====================================================================

int BlueFS::_flush_log_data(bufferlist &bl) {
    auto &fnode = log_.writer->file->fnode;
    uint64_t pos = log_.writer->pos;
    uint64_t x_off = 0;
    auto it = fnode.seek(pos, &x_off);
    uint64_t bloff = 0;
    uint64_t remaining = bl.length();
    while (remaining > 0 && it != fnode.extents.end()) {
        uint64_t x_len = std::min<uint64_t>(it->length - x_off, remaining);
        bufferlist t;
        t.substr_of(bl, bloff, x_len);
        int r = bdev_[it->bdev]->write(it->offset + x_off, t, false,
                                       WRITE_LIFE_NOT_SET);
        if (r < 0) return r;
        log_.writer->dirty_devs[it->bdev] = true;
        bloff += x_len;
        remaining -= x_len;
        x_off = 0;
        ++it;
    }
    log_.writer->pos += bl.length();
    if (log_.writer->pos > fnode.size) {
        fnode.size = log_.writer->pos;
    }
    return 0;
}

// =====================================================================
// 脏文件跟踪 & 日志刷新
// =====================================================================

void BlueFS::_signal_dirty_to_log(FileWriter *h) {
    std::lock_guard l(dirty_.lock);
    auto &f = h->file;
    if (f->dirty_seq <= dirty_.seq_stable || f->dirty_seq == 0) {
        f->dirty_seq = dirty_.seq_live;
        dirty_.files[dirty_.seq_live].push_back(f);
    }
    f->is_dirty = false;
}

int BlueFS::_consume_dirty(uint64_t seq) {
    std::lock_guard l(dirty_.lock);
    auto it = dirty_.files.find(seq);
    if (it == dirty_.files.end()) return 0;

    for (auto &f : it->second) {
        log_.t.op_file_update_inc(f->fnode);
    }

    return 0;
}

int BlueFS::_flush_and_sync_log(uint64_t want_seq) {
    if (want_seq && want_seq <= dirty_.seq_stable) return 0;
    if (want_seq) {
        cxxlab_assert(want_seq <= dirty_.seq_live);
    }

    std::unique_lock log_l(log_.lock);

    uint64_t seq = log_.seq_live;
    ++log_.seq_live;

    {
        std::lock_guard d(dirty_.lock);
        dirty_.seq_live = log_.seq_live;
    }

    int r = _consume_dirty(seq);
    if (r < 0) return r;

    r = _maybe_extend_log();
    if (r < 0) return r;

    bluefs_transaction_t t;
    t.seq = seq;
    t.uuid = super_.uuid;
    t.op_bl.claim_append(log_.t.op_bl);
    log_.t.op_bl.clear();
    log_.t.seq = log_.seq_live;

    bufferlist bl;
    encode(t, bl);

    uint32_t block_size = super_.block_size;
    uint64_t padding = p2roundup(bl.length(), block_size) - bl.length();
    if (padding > 0) {
        bl.append_zero(padding);
    }

    r = _flush_log_data(bl);
    if (r < 0) {
        log_l.unlock();
        return r;
    }
    _flush_bdev(log_.writer);
    log_.writer->buffer.clear();
    log_l.unlock();

    {
        std::lock_guard d(dirty_.lock);
        dirty_.seq_stable = seq;
        auto fit = dirty_.files.find(seq);
        if (fit != dirty_.files.end()) {
            for (auto &f : fit->second) {
                f->dirty_seq = 0;
            }
            dirty_.files.erase(fit);
        }
    }

    for (unsigned i = 0; i < dirty_.pending_release.size(); ++i) {
        if (dirty_.pending_release[i].empty()) continue;
        uint64_t released = 0;
        PExtentVector rv;
        for (auto &[off, len] : dirty_.pending_release[i]) {
            rv.emplace_back(off, len);
            released += len;
        }
        if (is_shared_alloc(i)) {
            if (shared_alloc_) {
                shared_alloc_->bluefs_used -= released;
            }
        } else if (alloc_[i]) {
            alloc_[i]->release(rv);
        }
        dirty_.pending_release[i].clear();
    }

    return 0;
}

int BlueFS::_maybe_extend_log() {
    auto &fnode = log_.writer->file->fnode;
    uint64_t pos = log_.writer->pos;
    if (pos + cfg_.min_log_runway < fnode.allocated) {
        return 0;
    }

    uint64_t alloc_len = p2roundup(
        std::max(cfg_.min_log_runway, cfg_.max_log_runway), cfg_.alloc_size);
    uint8_t prefer = vselector_->select_prefer_bdev(
        vselector_->get_hint_for_log());
    uint64_t hint = 0;
    int r = _allocate(prefer, alloc_len, cfg_.alloc_size, &fnode, &hint);
    if (r < 0) return r;

    log_.t.op_file_update_inc(log_.writer->file->fnode);
    return 0;
}

// =====================================================================
// _replay — 日志重放
// =====================================================================

int BlueFS::_replay(bool no_stdout) {
    auto log_file = _get_file(1);
    auto &fnode = log_file->fnode;

    if (fnode.extents.empty()) {
        return 0;
    }

    uint32_t block_size = super_.block_size;
    uint64_t log_seq = 0;
    uint64_t pos = 0;
    bool seen_recs = false;

    while (pos < fnode.size) {
        uint64_t read_len = std::min<uint64_t>(block_size, fnode.size - pos);

        uint64_t x_off = 0;
        auto it = fnode.seek(pos, &x_off);
        if (it == fnode.extents.end()) break;

        uint64_t dev_off = it->offset + x_off;
        uint64_t dev_len = std::min<uint64_t>(it->length - x_off, read_len);

        bufferlist bl;
        int r = bdev_[it->bdev]->read(dev_off, dev_len, &bl,
                                      ioc_[it->bdev], false);
        if (r < 0) return r;
        if (bl.length() == 0) break;

        // Peek DENC header to determine total transaction size
        uint64_t raw_tx_size;
        {
            auto peek = bl.cbegin();
            uint8_t sv, sc;
            uint32_t sl;
            try {
                denc(sv, peek);
                denc(sc, peek);
                denc(sl, peek);
            } catch (const buffer::malformed_input &) {
                if (!seen_recs) return -EIO;
                break;
            }
            raw_tx_size = (uint64_t)sl + 6;
        }

        // Read additional blocks if transaction spans multiple blocks
        uint64_t bs = block_size;
        uint64_t need = p2roundup(raw_tx_size, bs);
        if (need > bl.length()) {
            uint64_t more = need - bl.length();
            while (more > 0) {
                uint64_t r_off = pos + bl.length();
                uint64_t r_len = std::min<uint64_t>(
                    bs, fnode.size - r_off);

                uint64_t rx_off = 0;
                auto rit = fnode.seek(r_off, &rx_off);
                if (rit == fnode.extents.end()) break;

                uint64_t rd_off = rit->offset + rx_off;
                uint64_t rd_len =
                    std::min<uint64_t>(rit->length - rx_off, r_len);

                bufferlist more_bl;
                r = bdev_[rit->bdev]->read(rd_off, rd_len, &more_bl,
                                           ioc_[rit->bdev], false);
                if (r < 0) return r;
                bl.claim_append(more_bl);
                more -= p2roundup((uint64_t)r_len, bs);
            }
        }

        // Decode transaction
        auto p = bl.cbegin();
        bluefs_transaction_t t;
        try {
            decode(t, p);
        } catch (const buffer::malformed_input &) {
            if (!seen_recs) return -EIO;
            break;
        }

        // Validate UUID
        if (t.uuid != super_.uuid) {
            if (!seen_recs) return -EIO;
            break;
        }

        // Validate sequence
        if (t.seq != log_seq + 1) {
            if (t.seq == log_seq) break;  // duplicate (partial write)
            return -EIO;
        }

        seen_recs = true;

        // Process ops
        auto op_p = t.op_bl.cbegin();
        while (!op_p.end()) {
            uint8_t op;
            try {
                decode(op, op_p);
            } catch (const buffer::end_of_buffer &) {
                break;
            }

            switch (op) {
            case bluefs_transaction_t::OP_INIT:
                break;

            case bluefs_transaction_t::OP_JUMP: {
                uint64_t next_seq, offset;
                decode(next_seq, op_p);
                decode(offset, op_p);
                if (next_seq <= log_seq) return -EIO;
                if (offset >= fnode.size) return -EIO;
                log_seq = next_seq - 1;
                pos = offset;
                goto next_block;
            }

            case bluefs_transaction_t::OP_JUMP_SEQ: {
                uint64_t next_seq;
                decode(next_seq, op_p);
                if (next_seq <= log_seq) return -EIO;
                log_seq = next_seq - 1;
                break;
            }

            case bluefs_transaction_t::OP_DIR_CREATE: {
                std::string dirname;
                decode(dirname, op_p);
                auto dir = std::make_shared<Dir>();
                std::lock_guard l(nodes_.lock);
                nodes_.dir_map[dirname] = dir;
                break;
            }

            case bluefs_transaction_t::OP_DIR_REMOVE: {
                std::string dirname;
                decode(dirname, op_p);
                std::lock_guard l(nodes_.lock);
                nodes_.dir_map.erase(dirname);
                break;
            }

            case bluefs_transaction_t::OP_DIR_LINK: {
                std::string dirname, filename;
                uint64_t ino;
                decode(dirname, op_p);
                decode(filename, op_p);
                decode(ino, op_p);
                auto file = _get_file(ino);
                if (vselector_) {
                    vselector_->sub_usage(file->vselector_hint, file->fnode);
                    file->vselector_hint =
                        vselector_->get_hint_by_dir(dirname);
                    vselector_->add_usage(file->vselector_hint, file->fnode);
                } else {
                    file->vselector_hint =
                        vselector_->get_hint_by_dir(dirname);
                }
                std::lock_guard l(nodes_.lock);
                auto dit = nodes_.dir_map.find(dirname);
                if (dit != nodes_.dir_map.end()) {
                    dit->second->file_map[filename] = file;
                }
                ++file->refs;
                if (ino > ino_last_) ino_last_ = ino;
                break;
            }

            case bluefs_transaction_t::OP_DIR_UNLINK: {
                std::string dirname, filename;
                decode(dirname, op_p);
                decode(filename, op_p);
                std::lock_guard l(nodes_.lock);
                auto dit = nodes_.dir_map.find(dirname);
                if (dit != nodes_.dir_map.end()) {
                    auto fit = dit->second->file_map.find(filename);
                    if (fit != dit->second->file_map.end()) {
                        cxxlab_assert(fit->second->refs > 0);
                        --fit->second->refs;
                        dit->second->file_map.erase(fit);
                    }
                }
                break;
            }

            case bluefs_transaction_t::OP_FILE_UPDATE: {
                bluefs_fnode_t fnode;
                decode(fnode, op_p);
                auto file = _get_file(fnode.ino);
                if (vselector_ && file->vselector_hint) {
                    vselector_->sub_usage(file->vselector_hint,
                                          file->fnode);
                }
                file->fnode = fnode;
                if (vselector_ && file->vselector_hint) {
                    vselector_->add_usage(file->vselector_hint,
                                          file->fnode);
                }
                if (fnode.ino > ino_last_) ino_last_ = fnode.ino;
                break;
            }

            case bluefs_transaction_t::OP_FILE_UPDATE_INC: {
                bluefs_fnode_delta_t delta;
                decode(delta, op_p);
                auto file = _get_file(delta.ino);
                cxxlab_assert(delta.offset == file->fnode.allocated);
                file->fnode.mtime = delta.mtime;
                file->fnode.size = delta.size;
                file->fnode.claim_extents(delta.extents);
                if (delta.ino > ino_last_) ino_last_ = delta.ino;
                break;
            }

            case bluefs_transaction_t::OP_FILE_REMOVE: {
                uint64_t ino;
                decode(ino, op_p);
                std::lock_guard l(nodes_.lock);
                auto fit = nodes_.file_map.find(ino);
                if (fit != nodes_.file_map.end()) {
                    if (fit->second->refs > 0) {
                        fit->second->deleted = true;
                    } else {
                        nodes_.file_map.erase(fit);
                    }
                }
                break;
            }

            default:
                return -EIO;
            }
        }

        ++log_seq;
        pos += p2roundup(raw_tx_size, bs);
        continue;

next_block:
        continue;
    }

    log_.seq_live = log_seq + 1;
    dirty_.seq_live = log_seq + 1;
    dirty_.seq_stable = log_seq;

    // Update log file size to reflect actual replayed data
    log_file->fnode.size = pos;

    // Post-replay: remove orphaned files (refs==0) from file_map.
    // These files were deleted but the OP_FILE_REMOVE was already
    // processed; keeping them would cause mount's init_rm_free loop
    // to wastefully reserve their extents.
    {
        std::lock_guard nl(nodes_.lock);
        auto it = nodes_.file_map.begin();
        while (it != nodes_.file_map.end()) {
            if (it->first > 1 && it->second->refs <= 0) {
                it = nodes_.file_map.erase(it);
            } else {
                ++it;
            }
        }
    }

    return 0;
}

// =====================================================================
// mkfs
// =====================================================================

int BlueFS::mkfs(uint64_t bluefs_alloc_size) {
    if (!vselector_) {
        auto vs = std::make_unique<OriginalVolumeSelector>(
            bdev_[BDEV_WAL] ? _get_total(BDEV_WAL) : 0,
            bdev_[BDEV_DB] ? _get_total(BDEV_DB) : 0,
            bdev_[BDEV_SLOW] ? _get_total(BDEV_SLOW) : 0);
        vselector_ = std::move(vs);
    }

    _init_alloc();

    super_.uuid.generate();
    super_.block_size = 4096;
    super_.osd_uuid = uuid_d{};

    auto log_file = std::make_shared<File>(uint64_t(1));
    log_file->vselector_hint = vselector_->get_hint_for_log();

    uint64_t hint = 0;
    uint8_t prefer = vselector_->select_prefer_bdev(log_file->vselector_hint);
    int r = _allocate(prefer, BLUEFS_LOG_INITIAL, bluefs_alloc_size,
                      &log_file->fnode, &hint);
    if (r < 0) return r;

    vselector_->add_usage(log_file->vselector_hint, log_file->fnode);

    log_.writer = _create_writer(log_file);
    log_.t.uuid = super_.uuid;
    log_.t.op_init();
    log_.t.seq = 1;

    bufferlist bl;
    encode(log_.t, bl);
    uint32_t block_size = super_.block_size;
    uint64_t padding = p2roundup(bl.length(), block_size) - bl.length();
    if (padding > 0) {
        bl.append_zero(padding);
    }

    _flush_log_data(bl);
    _flush_bdev(log_.writer);
    log_.writer->buffer.clear();

    super_.log_fnode = log_file->fnode;
    r = _write_super(BDEV_DB);
    if (r < 0) return r;

    if (bdev_[BDEV_DB]) {
        bdev_[BDEV_DB]->flush();
    }

    _close_writer(log_.writer);
    log_.writer = nullptr;

    return 0;
}

// =====================================================================
// mount
// =====================================================================

int BlueFS::mount() {
    int r = _open_super();
    if (r < 0) return r;

    if (!vselector_) {
        auto vs = std::make_unique<OriginalVolumeSelector>(
            bdev_[BDEV_WAL] ? _get_total(BDEV_WAL) : 0,
            bdev_[BDEV_DB] ? _get_total(BDEV_DB) : 0,
            bdev_[BDEV_SLOW] ? _get_total(BDEV_SLOW) : 0);
        vselector_ = std::move(vs);
    }

    _init_alloc();

    auto log_file = std::make_shared<File>(uint64_t(1));
    log_file->fnode = super_.log_fnode;
    log_file->vselector_hint = vselector_->get_hint_for_log();
    {
        std::lock_guard l(nodes_.lock);
        nodes_.file_map[1] = log_file;
    }

    ino_last_ = 1;

    r = _replay(false);
    if (r < 0) return r;

    // Remove allocated space from allocators and track vselector usage
    for (auto &[ino, f] : nodes_.file_map) {
        for (auto &e : f->fnode.extents) {
            if (is_shared_alloc(e.bdev)) {
                if (shared_alloc_) {
                    shared_alloc_->bluefs_used += e.length;
                }
            } else if (alloc_[e.bdev]) {
                alloc_[e.bdev]->init_rm_free(e.offset, e.length);
            }
        }
        if (vselector_) {
            vselector_->add_usage(f->vselector_hint, f->fnode);
        }
    }

    log_.writer = _create_writer(_get_file(1));
    log_.writer->pos = log_file->fnode.size;
    log_file->fnode.reset_delta();

    return 0;
}

// =====================================================================
// umount
// =====================================================================

void BlueFS::umount(bool avoid_compact) {
    if (log_.writer) {
        _flush_and_sync_log();
        _flush_bdev(log_.writer);
        super_.log_fnode = _get_file(1)->fnode;
        int r = _write_super(BDEV_DB);
        if (r < 0) {
            // Log failure but continue cleanup
        }
        _close_writer(log_.writer);
        log_.writer = nullptr;
    }

    vselector_.reset();
    _stop_alloc();

    {
        std::lock_guard l(nodes_.lock);
        nodes_.file_map.clear();
        nodes_.dir_map.clear();
    }

    {
        std::lock_guard l(dirty_.lock);
        dirty_.files.clear();
        dirty_.seq_stable = 0;
        dirty_.seq_live = 1;
    }

    log_.t.clear();
    super_ = bluefs_super_t{};
    log_.seq_live = 1;
}

// =====================================================================
// 文件读写
// =====================================================================

int BlueFS::append_try_flush(FileWriter *h, const char *buf, size_t len) {
    h->append(buf, len);
    if (h->get_buffer_length() >= cfg_.min_flush_size) {
        return _flush_F(h, false);
    }
    return 0;
}

int BlueFS::flush(FileWriter *h, bool force) {
    return _flush_F(h, force);
}

int BlueFS::fsync(FileWriter *h) {
    int r = _flush_F(h, true);
    if (r < 0) return r;
    _flush_bdev(h);
    if (h->file->is_dirty) {
        _signal_dirty_to_log(h);
        h->file->is_dirty = false;
    }
    uint64_t old_dirty_seq = 0;
    {
        std::lock_guard l(dirty_.lock);
        old_dirty_seq = h->file->dirty_seq;
    }
    if (dirty_.seq_stable < old_dirty_seq) {
        r = _flush_and_sync_log(old_dirty_seq);
        if (r < 0) return r;
    }
    return 0;
}

// =====================================================================
// 文件读取
// =====================================================================

int64_t BlueFS::_read(FileReader *h, uint64_t off, size_t len,
                      bufferlist *outbl, char *out) {
    auto *buf = &h->buf;
    bool prefetch = !outbl && !out;

    if (!h->ignore_eof && off + len > h->file->fnode.size) {
        if (off > h->file->fnode.size)
            len = 0;
        else
            len = h->file->fnode.size - off;
    }

    if (outbl) outbl->clear();

    int64_t ret = 0;
    while (len > 0) {
        // Cache miss: read from disk
        if (off < buf->bl_off || off >= buf->get_buf_end()) {
            buf->bl.clear();
            buf->bl_off = off & super_.block_mask();

            uint64_t x_off = 0;
            auto p = h->file->fnode.seek(buf->bl_off, &x_off);
            if (p == h->file->fnode.extents.end()) break;

            uint64_t bs = super_.block_size;
            uint64_t off_align = off & ~super_.block_mask();
            uint64_t want = std::max<uint64_t>(
                p2roundup((uint64_t)(len + off_align), bs),
                buf->max_prefetch);
            uint64_t l = std::min<uint64_t>(p->length - x_off, want);
            l = std::min<uint64_t>(l, uint64_t(1) << 30);

            uint64_t eof_offset = p2roundup(h->file->fnode.size, bs);
            if (!h->ignore_eof && buf->bl_off + l > eof_offset) {
                l = eof_offset - buf->bl_off;
            }

            bool use_buffered = (h->file->fnode.ino == 1)
                ? false
                : cfg_.buffered_io;
            int r = bdev_[p->bdev]->read(p->offset + x_off, l, &buf->bl,
                                         ioc_[p->bdev], use_buffered);
            if (r < 0) return r;

            continue;
        }

        // Cache hit: copy from buffer
        size_t left = buf->get_buf_remaining(off);
        int64_t r = std::min<int64_t>(len, left);
        if (outbl) {
            bufferlist t;
            t.substr_of(buf->bl, off - buf->bl_off, r);
            outbl->claim_append(t);
        }
        if (out) {
            auto p = buf->bl.cbegin();
            p.seek(off - buf->bl_off);
            p.copy(r, out);
            out += r;
        }
        off += r;
        len -= r;
        ret += r;
        buf->pos += r;
    }
    return ret;
}

int64_t BlueFS::_read_random(FileReader *h, uint64_t off, uint64_t len,
                             char *out) {
    if (!h->ignore_eof && off + len > h->file->fnode.size) {
        if (off > h->file->fnode.size)
            len = 0;
        else
            len = h->file->fnode.size - off;
    }

    int64_t ret = 0;
    while (len > 0) {
        uint64_t x_off = 0;
        auto p = h->file->fnode.seek(off, &x_off);
        if (p == h->file->fnode.extents.end()) break;

        uint64_t l = std::min<uint64_t>(p->length - x_off, len);
        l = std::min<uint64_t>(l, uint64_t(1) << 30);

        bool use_buffered = (h->file->fnode.ino == 1)
            ? false
            : cfg_.buffered_io;
        int r = bdev_[p->bdev]->read_random(p->offset + x_off, l, out,
                                            use_buffered);
        if (r < 0) return r;

        off += l;
        len -= l;
        ret += l;
        out += l;
    }
    return ret;
}

int64_t BlueFS::read(FileReader *h, uint64_t off, size_t len,
                     bufferlist *outbl, char *out) {
    return _read(h, off, len, outbl, out);
}

int64_t BlueFS::read_random(FileReader *h, uint64_t off, uint64_t len,
                            char *out) {
    return _read_random(h, off, len, out);
}

// =====================================================================
// 文件操作
// =====================================================================

int BlueFS::stat(std::string_view dirname, std::string_view filename,
                 uint64_t *size, uint64_t *mtime) {
    std::lock_guard nl(nodes_.lock);
    auto dit = nodes_.dir_map.find(std::string(dirname));
    if (dit == nodes_.dir_map.end()) return -ENOENT;
    auto fit = dit->second->file_map.find(std::string(filename));
    if (fit == dit->second->file_map.end()) return -ENOENT;
    FileRef file = fit->second;
    if (size) *size = file->fnode.size;
    if (mtime) *mtime = file->fnode.mtime;
    return 0;
}

void BlueFS::_drop_link(FileRef file) {
    --file->refs;
    if (file->refs == 0) {
        if (vselector_) {
            vselector_->sub_usage(file->vselector_hint, file->fnode);
        }
        log_.t.op_file_remove(file->fnode.ino);
        nodes_.file_map.erase(file->fnode.ino);
        file->deleted = true;
        {
            std::lock_guard dl(dirty_.lock);
            for (auto &e : file->fnode.extents) {
                dirty_.pending_release[e.bdev].insert({e.offset, e.length});
            }
            // Retract from dirty set: no need to log metadata for a deleted file
            if (file->dirty_seq > dirty_.seq_stable) {
                auto dit = dirty_.files.find(file->dirty_seq);
                if (dit != dirty_.files.end()) {
                    auto &v = dit->second;
                    v.erase(std::remove(v.begin(), v.end(), file), v.end());
                    if (v.empty()) dirty_.files.erase(dit);
                }
                file->dirty_seq = dirty_.seq_stable;
            }
        }
    }
}

int BlueFS::unlink(std::string_view dirname, std::string_view filename) {
    std::lock_guard ll(log_.lock);
    std::lock_guard nl(nodes_.lock);

    auto dit = nodes_.dir_map.find(std::string(dirname));
    if (dit == nodes_.dir_map.end()) return -ENOENT;
    auto fit = dit->second->file_map.find(std::string(filename));
    if (fit == dit->second->file_map.end()) return -ENOENT;

    FileRef file = fit->second;
    dit->second->file_map.erase(std::string(filename));
    log_.t.op_dir_unlink(dirname, filename);
    _drop_link(file);
    return 0;
}

int BlueFS::truncate(FileWriter *h, uint64_t offset) {
    if (h->file->deleted) return 0;
    if (h->file->fnode.ino <= 1) return -EINVAL;
    if (offset > h->file->fnode.size) return -EINVAL;
    if (offset == h->file->fnode.size) return 0;

    if (h->get_buffer_length()) {
        int r = _flush_F(h, true);
        if (r < 0) return r;
    }
    _flush_bdev(h);

    std::lock_guard ll(log_.lock);

    if (vselector_) {
        vselector_->sub_usage(h->file->vselector_hint, h->file->fnode);
    }

    std::vector<bluefs_extent_t> old_extents;
    old_extents.swap(h->file->fnode.extents);
    h->file->fnode.extents_index.clear();
    h->file->fnode.allocated = 0;

    std::vector<bluefs_extent_t> to_free;
    uint64_t pos = 0;
    for (auto &e : old_extents) {
        if (pos < offset) {
            uint64_t keep = std::min<uint64_t>(e.length, offset - pos);
            h->file->fnode.append_extent(
                bluefs_extent_t(e.bdev, e.offset, keep));
            if (keep < e.length) {
                to_free.emplace_back(e.bdev, e.offset + keep,
                                     e.length - keep);
            }
        } else {
            to_free.emplace_back(e.bdev, e.offset, e.length);
        }
        pos += e.length;
    }

    h->file->fnode.size = offset;
    h->file->is_dirty = true;

    if (vselector_) {
        vselector_->add_usage(h->file->vselector_hint, h->file->fnode);
    }

    {
        std::lock_guard dl(dirty_.lock);
        for (auto &e : to_free) {
            dirty_.pending_release[e.bdev].insert({e.offset, e.length});
        }
    }

    log_.t.op_file_update(h->file->fnode);
    return 0;
}

int BlueFS::rename(std::string_view old_dirname,
                   std::string_view old_filename,
                   std::string_view new_dirname,
                   std::string_view new_filename) {
    std::lock_guard ll(log_.lock);
    std::lock_guard nl(nodes_.lock);

    auto odit = nodes_.dir_map.find(std::string(old_dirname));
    if (odit == nodes_.dir_map.end()) return -ENOENT;
    auto ofit = odit->second->file_map.find(std::string(old_filename));
    if (ofit == odit->second->file_map.end()) return -ENOENT;
    FileRef file = ofit->second;

    auto ndit = nodes_.dir_map.find(std::string(new_dirname));
    if (ndit == nodes_.dir_map.end()) return -ENOENT;

    // If target exists and is different, unlink it
    auto nfit = ndit->second->file_map.find(std::string(new_filename));
    if (nfit != ndit->second->file_map.end() && nfit->second != file) {
        log_.t.op_dir_unlink(new_dirname, new_filename);
    }

    ndit->second->file_map[std::string(new_filename)] = file;
    odit->second->file_map.erase(std::string(old_filename));

    log_.t.op_dir_link(new_dirname, new_filename, file->fnode.ino);
    log_.t.op_dir_unlink(old_dirname, old_filename);
    return 0;
}

// =====================================================================
// 文件创建/关闭
// =====================================================================

int BlueFS::open_for_write(std::string_view dirname,
                           std::string_view filename, FileWriter **h,
                           bool overwrite) {
    FileRef file;
    bool create = false;
    std::vector<bluefs_extent_t> pending_release_extents;

    {
        std::lock_guard ll(log_.lock);
        std::lock_guard nl(nodes_.lock);

        auto dit = nodes_.dir_map.find(std::string(dirname));
        if (dit == nodes_.dir_map.end()) return -ENOENT;
        DirRef dir = dit->second;

        auto fit = dir->file_map.find(std::string(filename));
        if (fit == dir->file_map.end()) {
            if (overwrite) return -ENOENT;
            file = std::make_shared<File>();
            file->fnode.ino = ++ino_last_;
            file->vselector_hint = vselector_->get_hint_by_dir(
                std::string(dirname));
            nodes_.file_map[ino_last_] = file;
            dir->file_map[std::string(filename)] = file;
            ++file->refs;
            create = true;
        } else {
            file = fit->second;
            if (!overwrite) {
                // Truncate: save old extents for release
                vselector_->sub_usage(file->vselector_hint, file->fnode);
                file->fnode.size = 0;
                pending_release_extents.swap(file->fnode.extents);
                file->fnode.clear_extents();
            }
            // else: overwrite in place, keep existing extents
        }

        file->fnode.mtime = 0;  // simplified: use 0 (no clock dependency)
        log_.t.op_file_update(file->fnode);
        if (create) {
            log_.t.op_dir_link(dirname, filename, file->fnode.ino);
        }

        // Release old extents via pending_release
        {
            std::lock_guard dl(dirty_.lock);
            for (auto &e : pending_release_extents) {
                dirty_.pending_release[e.bdev].insert(
                    {e.offset, e.length});
            }
        }
    }

    *h = _create_writer(file);
    return 0;
}

int BlueFS::open_for_read(std::string_view dirname,
                          std::string_view filename, FileReader **h,
                          bool random) {
    std::lock_guard nl(nodes_.lock);
    auto dit = nodes_.dir_map.find(std::string(dirname));
    if (dit == nodes_.dir_map.end()) return -ENOENT;
    DirRef dir = dit->second;

    auto fit = dir->file_map.find(std::string(filename));
    if (fit == dir->file_map.end()) return -ENOENT;
    FileRef file = fit->second;

    uint64_t prefetch = random ? 4096 : cfg_.max_prefetch;
    *h = new FileReader(file, prefetch, random, false);
    return 0;
}

// =====================================================================
// 目录操作
// =====================================================================

int BlueFS::mkdir(std::string_view dirname) {
    std::lock_guard ll(log_.lock);
    std::lock_guard nl(nodes_.lock);
    auto sd = std::string(dirname);
    auto p = nodes_.dir_map.find(sd);
    if (p != nodes_.dir_map.end()) return -EEXIST;
    nodes_.dir_map[sd] = std::make_shared<Dir>();
    log_.t.op_dir_create(dirname);
    return 0;
}

int BlueFS::rmdir(std::string_view dirname) {
    std::lock_guard ll(log_.lock);
    std::lock_guard nl(nodes_.lock);
    auto sd = std::string(dirname);
    auto p = nodes_.dir_map.find(sd);
    if (p == nodes_.dir_map.end()) return -ENOENT;
    DirRef dir = p->second;
    if (!dir->file_map.empty()) return -ENOTEMPTY;
    nodes_.dir_map.erase(sd);
    log_.t.op_dir_remove(dirname);
    return 0;
}

bool BlueFS::dir_exists(std::string_view dirname) {
    std::lock_guard nl(nodes_.lock);
    return nodes_.dir_map.find(std::string(dirname)) != nodes_.dir_map.end();
}

int BlueFS::readdir(std::string_view dirname, std::vector<std::string> *ls) {
    while (!dirname.empty() && dirname.back() == '/') {
        dirname.remove_suffix(1);
    }
    std::lock_guard nl(nodes_.lock);
    if (dirname.empty()) {
        ls->reserve(nodes_.dir_map.size());
        for (auto &q : nodes_.dir_map) {
            ls->push_back(q.first);
        }
    } else {
        auto p = nodes_.dir_map.find(std::string(dirname));
        if (p == nodes_.dir_map.end()) return -ENOENT;
        DirRef dir = p->second;
        ls->reserve(dir->file_map.size());
        for (auto &q : dir->file_map) {
            ls->push_back(q.first);
        }
    }
    return 0;
}

// =====================================================================
// 元数据同步
// =====================================================================

int BlueFS::sync_metadata(bool avoid_compact) {
    int r = _flush_and_sync_log();
    if (r < 0) return r;

    super_.log_fnode = _get_file(1)->fnode;
    r = _write_super(BDEV_DB);
    if (r < 0) return r;
    if (bdev_[BDEV_DB]) {
        bdev_[BDEV_DB]->flush();
    }
    if (!avoid_compact) {
        _maybe_compact_log();
    }
    return 0;
}

// =====================================================================
// Log compaction
// =====================================================================

uint64_t BlueFS::_estimate_log_size() {
    std::lock_guard nl(nodes_.lock);
    int avg_dir_size = 40;
    int avg_file_size = 12;
    uint64_t size = 4096 * 2;
    size += nodes_.file_map.size() * (1 + sizeof(bluefs_fnode_t) + sizeof(bluefs_fnode_delta_t));
    size += nodes_.dir_map.size() * (1 + avg_dir_size);
    size += nodes_.file_map.size() * (1 + avg_dir_size + avg_file_size);
    size += 24;  // OP_JUMP_SEQ
    size = p2roundup<uint64_t>(size, super_.block_size);
    return size;
}

bool BlueFS::_should_compact() {
    if (log_is_compacting_.load()) return false;

    uint64_t current = _get_file(1)->fnode.size;
    uint64_t expected = _estimate_log_size();

    if (current < cfg_.log_compact_min_size) return false;
    if ((double)current / (double)expected < cfg_.log_compact_min_ratio)
        return false;
    return true;
}

void BlueFS::_compact_log_dump_metadata(bluefs_transaction_t *t) {
    t->uuid = super_.uuid;
    std::lock_guard nl(nodes_.lock);

    for (auto &[dirname, dir] : nodes_.dir_map) {
        t->op_dir_create(dirname);
        for (auto &[fname, file] : dir->file_map) {
            if (file->fnode.ino == 1) continue;
            t->op_dir_link(dirname, fname, file->fnode.ino);
        }
    }

    for (auto &[ino, file] : nodes_.file_map) {
        if (ino == 1) continue;
        t->op_file_update(file->fnode);
    }
}

int BlueFS::_compact_log_async() {
    bool was_compacting = log_is_compacting_.exchange(true);
    if (was_compacting) {
        return 0;
    }

    int ret = 0;
    uint64_t cur_seq;
    {
        std::lock_guard ll(log_.lock);
        cur_seq = log_.seq_live;
    }

    if (!_should_compact()) {
        log_is_compacting_ = false;
        return 0;
    }

    auto log_file = _get_file(1);
    uint8_t prefer = vselector_->select_prefer_bdev(
        vselector_->get_hint_for_log());
    void *log_hint = log_file->vselector_hint;

    {
        std::lock_guard ll(log_.lock);

        // Save old fnode for vselector update and pending release
        bluefs_fnode_t old_fnode = log_file->fnode;

        // Dump current metadata into a compacted transaction
        bluefs_transaction_t compacted_t;
        compacted_t.seq = 1;
        compacted_t.uuid = super_.uuid;
        _compact_log_dump_metadata(&compacted_t);

        // The compacted transaction ends with OP_JUMP_SEQ so that on
        // replay the expected sequence continues at cur_seq.
        compacted_t.op_jump_seq(cur_seq);

        // Encode the compacted transaction (padded to block_size)
        bufferlist compacted_bl;
        encode(compacted_t, compacted_bl);
        uint64_t bs = super_.block_size;
        uint64_t padding = p2roundup<uint64_t>(compacted_bl.length(), bs) -
            compacted_bl.length();
        if (padding > 0) {
            compacted_bl.append_zero(padding);
        }

        // Update vselector: remove old log extents, will add new ones
        if (vselector_ && log_hint) {
            vselector_->sub_usage(log_hint, old_fnode);
            vselector_->sub_usage(log_hint, old_fnode.size, true);
        }

        // Allocate new extents for the compacted log
        log_file->fnode.clear_extents();
        uint64_t hint = 0;
        ret = _allocate(prefer, compacted_bl.length(), cfg_.alloc_size,
                        &log_file->fnode, &hint);
        if (ret < 0) {
            // Restore old fnode on allocation failure
            log_file->fnode = old_fnode;
            log_is_compacting_ = false;
            return ret;
        }

        // Update vselector for new extents
        if (vselector_ && log_hint) {
            vselector_->add_usage(log_hint, log_file->fnode);
            vselector_->add_usage(log_hint, log_file->fnode.size, true);
        }

        // Close old log writer, create new one at position 0
        _close_writer(log_.writer);
        log_.writer = _create_writer(log_file);
        log_.writer->pos = 0;
        log_file->fnode.size = 0;

        // Write compacted data to new log extents
        log_.writer->buffer.clear();
        log_.writer->buffer.append(compacted_bl);
        ret = _flush_data(log_.writer, 0, compacted_bl.length(), false);
        if (ret < 0) {
            log_.writer->buffer.clear();
            log_is_compacting_ = false;
            return ret;
        }
        log_.writer->buffer.clear();
        _flush_bdev(log_.writer);

        // Update superblock with new log fnode
        super_.log_fnode = log_file->fnode;
        ret = _write_super(BDEV_DB);
        if (ret < 0) {
            log_is_compacting_ = false;
            return ret;
        }
        if (bdev_[BDEV_DB]) {
            bdev_[BDEV_DB]->flush();
        }

        // Release old log extents
        {
            std::lock_guard dl(dirty_.lock);
            for (auto &e : old_fnode.extents) {
                dirty_.pending_release[e.bdev].insert({e.offset, e.length});
            }
        }

        // Reset log transaction state
        log_.t.clear();
        log_.t.uuid = super_.uuid;
        log_.t.seq = log_.seq_live;
    }

    log_is_compacting_ = false;
    return 0;
}

void BlueFS::_maybe_compact_log() {
    if (!_should_compact()) return;
    _compact_log_async();
}

int BlueFS::compact_log() {
    return _compact_log_async();
}

// =====================================================================
// BlueRocksEnv 接口
// =====================================================================

int BlueFS::lock_file(std::string_view dirname, std::string_view filename,
                      FileLock **plock) {
    std::lock_guard l(nodes_.lock);
    auto dit = nodes_.dir_map.find(std::string{dirname});
    if (dit == nodes_.dir_map.end()) {
        return -ENOENT;
    }
    auto &dir = dit->second;
    std::string fname(filename);
    auto fit = dir->file_map.find(fname);
    FileRef file;
    if (fit == dir->file_map.end()) {
        file = std::make_shared<File>();
        file->fnode.ino = ++ino_last_;
        nodes_.file_map[ino_last_] = file;
        dir->file_map[fname] = file;
        ++file->refs;
    } else {
        file = fit->second;
        if (file->locked) {
            return -ENOLCK;
        }
    }
    file->locked = true;
    *plock = new FileLock(file);
    return 0;
}

int BlueFS::unlock_file(FileLock *l) {
    std::lock_guard nl(nodes_.lock);
    cxxlab_assert(l->file->locked);
    l->file->locked = false;
    delete l;
    return 0;
}

void BlueFS::invalidate_cache(FileRef f, uint64_t offset, uint64_t len) {
    if (offset & ~super_.block_mask()) {
        uint64_t delta = offset & ~super_.block_mask();
        len += delta;
        offset &= super_.block_mask();
        len = round_up_to(len, super_.block_size);
    }
    uint64_t x_off = 0;
    auto p = f->fnode.seek(offset, &x_off);
    while (len > 0 && p != f->fnode.extents.end()) {
        uint64_t x_len = std::min(p->length - x_off, len);
        bdev_[p->bdev]->invalidate_cache(p->offset + x_off, x_len);
        offset += x_len;
        len -= x_len;
        x_off = 0;
        ++p;
    }
}

void BlueFS::flush_range(FileWriter *h, uint64_t offset, uint64_t length) {
    std::lock_guard l(h->lock);
    _flush_range_F(h, offset, length);
}

int BlueFS::preallocate(FileRef f, uint64_t off, uint64_t len) {
    std::lock_guard ll(log_.lock);
    if (f->deleted) return 0;
    cxxlab_assert(f->fnode.ino > 1);
    uint64_t allocated = f->fnode.get_allocated();
    if (off + len > allocated) {
        uint64_t want = off + len - allocated;
        int r = _allocate(
            vselector_->select_prefer_bdev(f->vselector_hint),
            want, 0, &f->fnode);
        if (r < 0) return r;
        log_.t.op_file_update_inc(f->fnode);
    }
    return 0;
}

uint64_t BlueFS::get_used() {
    uint64_t used = 0;
    for (unsigned id = 0; id < MAX_BDEV; ++id) {
        if (!bdev_[id]) continue;
        used += get_used(id);
    }
    return used;
}

uint64_t BlueFS::get_used(unsigned id) {
    if (id >= bdev_.size() || !bdev_[id]) return 0;
    if (is_shared_alloc(id)) {
        return shared_alloc_ ? shared_alloc_->bluefs_used.load() : 0;
    }
    if (alloc_[id]) {
        return _get_total(id) - alloc_[id]->get_free();
    }
    return 0;
}

}  // namespace TOPNSPC
