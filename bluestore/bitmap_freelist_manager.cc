#include "bluestore/bitmap_freelist_manager.h"

#include <cstring>
#include <string>

#include "common/cassert.h"
#include "kv/merge_op/xor_merge_op.h"

namespace TOPNSPC {

namespace {

// Big-endian uint64_t encoding for memcmp-compatible key ordering.
void key_encode_u64(uint64_t val, std::string *out) {
    out->push_back(static_cast<char>(val >> 56));
    out->push_back(static_cast<char>(val >> 48));
    out->push_back(static_cast<char>(val >> 40));
    out->push_back(static_cast<char>(val >> 32));
    out->push_back(static_cast<char>(val >> 24));
    out->push_back(static_cast<char>(val >> 16));
    out->push_back(static_cast<char>(val >> 8));
    out->push_back(static_cast<char>(val));
}

uint64_t key_decode_u64(const char *p) {
    uint64_t val = 0;
    val |= static_cast<uint64_t>(static_cast<unsigned char>(p[0])) << 56;
    val |= static_cast<uint64_t>(static_cast<unsigned char>(p[1])) << 48;
    val |= static_cast<uint64_t>(static_cast<unsigned char>(p[2])) << 40;
    val |= static_cast<uint64_t>(static_cast<unsigned char>(p[3])) << 32;
    val |= static_cast<uint64_t>(static_cast<unsigned char>(p[4])) << 24;
    val |= static_cast<uint64_t>(static_cast<unsigned char>(p[5])) << 16;
    val |= static_cast<uint64_t>(static_cast<unsigned char>(p[6])) << 8;
    val |= static_cast<uint64_t>(static_cast<unsigned char>(p[7]));
    return val;
}

int get_next_clear_bit(bufferlist &bl, int start) {
    const char *p = bl.c_str();
    int bits = static_cast<int>(bl.length()) << 3;
    while (start < bits) {
        unsigned char byte_mask = 1u << (start & 7);
        if ((p[start >> 3] & byte_mask) == 0)
            return start;
        ++start;
    }
    return -1;
}

int get_next_set_bit(bufferlist &bl, int start) {
    const char *p = bl.c_str();
    int bits = static_cast<int>(bl.length()) << 3;
    while (start < bits) {
        unsigned char byte_mask = 1u << (start & 7);
        if (p[start >> 3] & byte_mask)
            return start;
        ++start;
    }
    return -1;
}

bool is_power_of_2(uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

}  // anonymous namespace

BitmapFreelistManager::BitmapFreelistManager(
    std::string meta_prefix, std::string bitmap_prefix)
    : meta_prefix_(std::move(meta_prefix)),
      bitmap_prefix_(std::move(bitmap_prefix)) {}

int BitmapFreelistManager::create(
    uint64_t new_size, uint64_t granularity,
    Transaction txn) {
    cxxlab_assert(is_power_of_2(granularity));
    bytes_per_block_ = granularity;
    size_ = new_size & ~(bytes_per_block_ - 1);
    blocks_per_key_ = 128;

    _init_misc();

    blocks_ = _size_2_block_count(size_);

    if (blocks_ * bytes_per_block_ > size_) {
        _xor(size_, blocks_ * bytes_per_block_ - size_, txn);
    }

    _xor(0, bytes_per_block_, txn);

    {
        bufferlist bl;
        bl.append(reinterpret_cast<const char *>(&bytes_per_block_),
                  sizeof(bytes_per_block_));
        txn->set(meta_prefix_, "bytes_per_block", bl);
    }
    {
        bufferlist bl;
        bl.append(reinterpret_cast<const char *>(&blocks_per_key_),
                  sizeof(blocks_per_key_));
        txn->set(meta_prefix_, "blocks_per_key", bl);
    }
    {
        bufferlist bl;
        bl.append(reinterpret_cast<const char *>(&blocks_),
                  sizeof(blocks_));
        txn->set(meta_prefix_, "blocks", bl);
    }
    {
        bufferlist bl;
        bl.append(reinterpret_cast<const char *>(&size_),
                  sizeof(size_));
        txn->set(meta_prefix_, "size", bl);
    }
    return 0;
}

int BitmapFreelistManager::init(
    KeyValueDB *kvdb, bool db_in_read_only,
    std::function<int(const std::string &, std::string *)> cfg_reader) {
    int r = _read_cfg(cfg_reader);
    if (r != 0) {
        _load_from_db(kvdb);
    }
    (void)db_in_read_only;
    _init_misc();
    return 0;
}

void BitmapFreelistManager::shutdown() {}

void BitmapFreelistManager::enumerate_reset() {
    std::lock_guard l(lock_);
    enumerate_offset_ = 0;
    enumerate_bl_pos_ = 0;
    enumerate_bl_.clear();
    enumerate_p_.reset();
}

bool BitmapFreelistManager::enumerate_next(
    KeyValueDB *kvdb, uint64_t *offset, uint64_t *length) {
    std::lock_guard l(lock_);

    if (enumerate_offset_ == 0 && enumerate_bl_pos_ == 0) {
        enumerate_p_ = kvdb->get_iterator(bitmap_prefix_);
        enumerate_p_->lower_bound(std::string());
        if (!enumerate_p_->valid()) {
            // Freelist is empty
            return false;
        }
        std::string k = enumerate_p_->key();
        enumerate_offset_ = key_decode_u64(k.data());
        enumerate_bl_ = enumerate_p_->value();
        cxxlab_assert(enumerate_offset_ == 0);
    }

    if (enumerate_offset_ >= size_) {
        return false;
    }

    while (true) {
        enumerate_bl_pos_ = get_next_clear_bit(enumerate_bl_, enumerate_bl_pos_);
        if (enumerate_bl_pos_ >= 0) {
            *offset = _get_offset(enumerate_offset_, enumerate_bl_pos_);
            break;
        }
        enumerate_p_->next();
        enumerate_bl_.clear();
        if (!enumerate_p_->valid()) {
            enumerate_offset_ += bytes_per_key_;
            enumerate_bl_pos_ = 0;
            *offset = _get_offset(enumerate_offset_, enumerate_bl_pos_);
            break;
        }
        std::string k = enumerate_p_->key();
        uint64_t next = enumerate_offset_ + bytes_per_key_;
        enumerate_offset_ = key_decode_u64(k.data());
        enumerate_bl_ = enumerate_p_->value();
        enumerate_bl_pos_ = 0;
        if (enumerate_offset_ > next) {
            *offset = next;
            break;
        }
    }

    uint64_t end = 0;
    if (enumerate_p_->valid()) {
        while (true) {
            enumerate_bl_pos_ = get_next_set_bit(enumerate_bl_, enumerate_bl_pos_);
            if (enumerate_bl_pos_ >= 0) {
                end = _get_offset(enumerate_offset_, enumerate_bl_pos_);
                end = std::min(get_alloc_units() * bytes_per_block_, end);
                *length = end - *offset;
                return true;
            }
            enumerate_p_->next();
            enumerate_bl_.clear();
            enumerate_bl_pos_ = 0;
            if (!enumerate_p_->valid())
                break;
            std::string k = enumerate_p_->key();
            enumerate_offset_ = key_decode_u64(k.data());
            enumerate_bl_ = enumerate_p_->value();
        }
    }

    if (enumerate_offset_ < size_) {
        end = get_alloc_units() * bytes_per_block_;
        *length = end - *offset;
        enumerate_offset_ = size_;
        enumerate_bl_pos_ = static_cast<int>(blocks_per_key_);
        return true;
    }

    return false;
}

void BitmapFreelistManager::allocate(
    uint64_t offset, uint64_t length,
    Transaction txn) {
    if (!is_null_manager())
        _xor(offset, length, txn);
}

void BitmapFreelistManager::release(
    uint64_t offset, uint64_t length,
    Transaction txn) {
    if (!is_null_manager())
        _xor(offset, length, txn);
}

void BitmapFreelistManager::_xor(
    uint64_t offset, uint64_t length,
    Transaction txn) {
    cxxlab_assert((offset & block_mask_) == offset);
    cxxlab_assert((length & block_mask_) == length);

    uint64_t first_key = offset & key_mask_;
    uint64_t last_key = (offset + length - 1) & key_mask_;

    if (first_key == last_key) {
        bufferptr p(blocks_per_key_ >> 3);
        p.zero();
        unsigned s = static_cast<unsigned>(
            (offset & ~key_mask_) / bytes_per_block_);
        unsigned e = static_cast<unsigned>(
            ((offset + length - 1) & ~key_mask_) / bytes_per_block_);
        for (unsigned i = s; i <= e; ++i) {
            p[i >> 3] ^= 1ull << (i & 7);
        }
        std::string k;
        key_encode_u64(first_key, &k);
        bufferlist bl;
        bl.append(p);
        txn->merge(bitmap_prefix_, k, bl);
    } else {
        {
            bufferptr p(blocks_per_key_ >> 3);
            p.zero();
            unsigned s = static_cast<unsigned>(
                (offset & ~key_mask_) / bytes_per_block_);
            unsigned e = blocks_per_key_;
            for (unsigned i = s; i < e; ++i) {
                p[i >> 3] ^= 1ull << (i & 7);
            }
            std::string k;
            key_encode_u64(first_key, &k);
            bufferlist bl;
            bl.append(p);
            txn->merge(bitmap_prefix_, k, bl);
            first_key += bytes_per_key_;
        }

        while (first_key < last_key) {
            std::string k;
            key_encode_u64(first_key, &k);
            txn->merge(bitmap_prefix_, k, all_set_bl_);
            first_key += bytes_per_key_;
        }

        cxxlab_assert(first_key == last_key);
        {
            bufferptr p(blocks_per_key_ >> 3);
            p.zero();
            unsigned e = static_cast<unsigned>(
                ((offset + length - 1) & ~key_mask_) / bytes_per_block_);
            for (unsigned i = 0; i <= e; ++i) {
                p[i >> 3] ^= 1ull << (i & 7);
            }
            std::string k;
            key_encode_u64(first_key, &k);
            bufferlist bl;
            bl.append(p);
            txn->merge(bitmap_prefix_, k, bl);
        }
    }
}

void BitmapFreelistManager::_init_misc() {
    bufferptr z(blocks_per_key_ >> 3);
    memset(z.c_str(), 0xff, z.length());
    all_set_bl_.clear();
    all_set_bl_.append(z);

    block_mask_ = ~(bytes_per_block_ - 1);
    bytes_per_key_ = bytes_per_block_ * blocks_per_key_;
    key_mask_ = ~(bytes_per_key_ - 1);
}

int BitmapFreelistManager::_read_cfg(
    std::function<int(const std::string &, std::string *)> cfg_reader) {
    if (!cfg_reader)
        return -1;

    struct CfgEntry {
        const char *key;
        uint64_t *val;
    };
    CfgEntry entries[] = {
        {"bfm_size", &size_},
        {"bfm_blocks", &blocks_},
        {"bfm_bytes_per_block", &bytes_per_block_},
        {"bfm_blocks_per_key", &blocks_per_key_},
    };

    for (auto &e : entries) {
        std::string val;
        int r = cfg_reader(e.key, &val);
        if (r != 0)
            return r;
        size_t pos;
        *e.val = std::stoull(val, &pos);
    }
    return 0;
}

void BitmapFreelistManager::_load_from_db(KeyValueDB *kvdb) {
    auto it = kvdb->get_iterator(meta_prefix_);
    it->lower_bound(std::string());

    while (it->valid()) {
        std::string k = it->key();
        bufferlist bl = it->value();
        if (k == "bytes_per_block") {
            cxxlab_assert(bl.length() == sizeof(bytes_per_block_));
            std::memcpy(&bytes_per_block_, bl.c_str(), sizeof(bytes_per_block_));
        } else if (k == "blocks") {
            cxxlab_assert(bl.length() == sizeof(blocks_));
            std::memcpy(&blocks_, bl.c_str(), sizeof(blocks_));
        } else if (k == "size") {
            cxxlab_assert(bl.length() == sizeof(size_));
            std::memcpy(&size_, bl.c_str(), sizeof(size_));
        } else if (k == "blocks_per_key") {
            cxxlab_assert(bl.length() == sizeof(blocks_per_key_));
            std::memcpy(&blocks_per_key_, bl.c_str(), sizeof(blocks_per_key_));
        }
        it->next();
    }
}

uint64_t BitmapFreelistManager::_size_2_block_count(
    uint64_t target_size) const {
    auto target_blocks = target_size / bytes_per_block_;
    if (target_blocks / blocks_per_key_ * blocks_per_key_ != target_blocks) {
        target_blocks = (target_blocks / blocks_per_key_ + 1) * blocks_per_key_;
    }
    return target_blocks;
}

void BitmapFreelistManager::get_meta(
    uint64_t target_size,
    std::vector<std::pair<std::string, std::string>> *res) const {
    if (target_size == 0) {
        res->emplace_back("bfm_blocks", std::to_string(blocks_));
        res->emplace_back("bfm_size", std::to_string(size_));
    } else {
        target_size = target_size & ~(bytes_per_block_ - 1);
        auto target_blocks = _size_2_block_count(target_size);
        res->emplace_back("bfm_blocks", std::to_string(target_blocks));
        res->emplace_back("bfm_size", std::to_string(target_size));
    }
    res->emplace_back("bfm_bytes_per_block",
                      std::to_string(bytes_per_block_));
    res->emplace_back("bfm_blocks_per_key",
                      std::to_string(blocks_per_key_));
}

}  // namespace TOPNSPC
