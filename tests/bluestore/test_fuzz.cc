#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "bluestore/blob.h"
#include "bluestore/bluestore_types.h"
#include "bluestore/extent_map.h"
#include "common/buffer.h"

using namespace TOPNSPC;

namespace {

class FuzzTest : public ::testing::Test {
protected:
    std::vector<Blob *> blobs_;

    void TearDown() override {
        for (auto *b : blobs_) {
            b->put();
        }
        blobs_.clear();
    }

    Blob *make_blob(uint32_t logical_len,
                    const std::vector<std::pair<uint64_t, uint32_t>> &phys) {
        auto *b = new Blob();
        b->get();
        auto &blob = b->dirty_blob();
        blob.add_tail(logical_len);
        blob.dirty_extents().clear();
        for (const auto &p : phys) {
            blob.dirty_extents().emplace_back(p.first, p.second);
        }
        blobs_.push_back(b);
        return b;
    }

    Blob *make_csum_blob(uint8_t csum_order, uint32_t logical_len) {
        auto *b = new Blob();
        b->get();
        auto &blob = b->dirty_blob();
        blob.add_tail(logical_len);
        blob.dirty_extents().clear();
        blob.dirty_extents().emplace_back(0, logical_len);
        blob.init_csum(CSUM_CRC32C, csum_order, logical_len);
        blobs_.push_back(b);
        return b;
    }

    static void coverage(const ExtentMap &em,
                         std::vector<std::pair<uint64_t, uint64_t>> *out) {
        for (const auto &e : em) {
            out->emplace_back(e.logical_offset, e.logical_end());
        }
    }

    static bool has_overlap(const ExtentMap &em) {
        uint64_t prev_end = 0;
        bool first = true;
        for (const auto &e : em) {
            if (!first && e.logical_offset < prev_end) return true;
            prev_end = e.logical_end();
            first = false;
        }
        return false;
    }

    static std::vector<std::pair<uint64_t, uint64_t>> merge_intervals(
        std::vector<std::pair<uint64_t, uint64_t>> intervals) {
        std::sort(intervals.begin(), intervals.end());
        std::vector<std::pair<uint64_t, uint64_t>> merged;
        for (const auto &iv : intervals) {
            if (!merged.empty() && iv.first <= merged.back().second) {
                merged.back().second = std::max(merged.back().second, iv.second);
            } else {
                merged.push_back(iv);
            }
        }
        return merged;
    }

    static bool phys_contiguous(const bluestore_blob_t &blob,
                                uint32_t curr_end_bo, uint32_t next_start_bo) {
        uint64_t curr_phys_end = 0;
        uint64_t next_phys_start = 0;
        uint32_t blob_off = 0;
        for (const auto &e : blob.get_extents()) {
            if (blob_off <= curr_end_bo && curr_end_bo < blob_off + e.length) {
                curr_phys_end = e.offset + (curr_end_bo - blob_off);
            }
            if (blob_off <= next_start_bo && next_start_bo < blob_off + e.length) {
                next_phys_start = e.offset + (next_start_bo - blob_off);
            }
            blob_off += e.length;
        }
        return curr_phys_end == next_phys_start;
    }
};

TEST_F(FuzzTest, CompressExtentMapInvariants) {
    std::mt19937 rng(42);
    for (int iter = 0; iter < 1000; ++iter) {
        ExtentMap em;

        int num_blobs = 1 + rng() % 3;
        std::vector<Blob *> local_blobs;
        for (int i = 0; i < num_blobs; ++i) {
            uint32_t logical_len = 4096 * (1 + rng() % 16);
            std::vector<std::pair<uint64_t, uint32_t>> phys;
            uint32_t remaining = logical_len;
            uint64_t cur_off = rng() % 4096;
            while (remaining > 0) {
                uint32_t chunk =
                    std::min<uint32_t>(4096 * (1 + rng() % 4), remaining);
                phys.emplace_back(cur_off, chunk);
                if (rng() % 2) {
                    cur_off += chunk;
                } else {
                    cur_off += chunk + 4096 * (1 + rng() % 4);
                }
                remaining -= chunk;
            }
            local_blobs.push_back(make_blob(logical_len, phys));
        }

        int num_extents = 1 + rng() % 8;
        std::set<uint32_t> used_offsets;
        uint64_t cur_lo = 0;
        for (int i = 0; i < num_extents; ++i) {
            Blob *b = local_blobs[rng() % local_blobs.size()];
            uint32_t blob_len = b->get_blob().get_logical_length();
            uint32_t ext_len =
                std::min<uint32_t>(4096 * (1 + rng() % 4), blob_len);
            uint32_t bo = (rng() % (blob_len / 4096 + 1)) * 4096;
            if (bo + ext_len > blob_len) bo = blob_len - ext_len;
            uint32_t lo = cur_lo;
            cur_lo += ext_len + (rng() % 2 ? 4096 * (1 + rng() % 3) : 0);
            if (used_offsets.count(lo)) continue;
            used_offsets.insert(lo);
            em.add(lo, bo, ext_len, b);
        }

        if (em.empty()) continue;

        std::vector<std::pair<uint64_t, uint64_t>> before_cov;
        coverage(em, &before_cov);
        before_cov = merge_intervals(before_cov);

        em.compress_extent_map(0, 1ULL << 20);

        ASSERT_FALSE(has_overlap(em))
            << "iter " << iter << " overlap after compress";

        std::vector<std::pair<uint64_t, uint64_t>> after_cov;
        coverage(em, &after_cov);
        after_cov = merge_intervals(after_cov);
        ASSERT_EQ(before_cov, after_cov)
            << "iter " << iter << " coverage changed by compress";

        auto it = em.begin();
        while (it != em.end()) {
            auto next = std::next(it);
            if (next == em.end()) break;
            bool logical_contig = (it->logical_end() == next->logical_offset);
            bool same_blob = (it->blob == next->blob);
            bool blob_contig =
                (it->blob_offset + it->length == next->blob_offset);
            if (logical_contig && same_blob && blob_contig) {
                bool phys = phys_contiguous(it->blob->get_blob(),
                                            it->blob_offset + it->length,
                                            next->blob_offset);
                ASSERT_FALSE(phys)
                    << "iter " << iter
                    << ": physically contiguous extents not merged";
            }
            ++it;
        }

        size_t count1 = em.size();
        em.compress_extent_map(0, 1ULL << 20);
        size_t count2 = em.size();
        ASSERT_EQ(count1, count2)
            << "iter " << iter << " compress not idempotent";
    }
}

TEST_F(FuzzTest, PunchHoleInvariants) {
    std::mt19937 rng(123);
    for (int iter = 0; iter < 1000; ++iter) {
        ExtentMap em;

        int num_blobs = 1 + rng() % 3;
        std::vector<Blob *> local_blobs;
        for (int i = 0; i < num_blobs; ++i) {
            uint32_t logical_len = 4096 * (1 + rng() % 16);
            std::vector<std::pair<uint64_t, uint32_t>> phys;
            phys.emplace_back(rng() % 4096, logical_len);
            local_blobs.push_back(make_blob(logical_len, phys));
        }

        std::vector<std::pair<uint64_t, uint64_t>> before_cov;
        uint64_t cur_lo = 0;
        int num_extents = 1 + rng() % 8;
        for (int i = 0; i < num_extents; ++i) {
            Blob *b = local_blobs[rng() % local_blobs.size()];
            uint32_t blob_len = b->get_blob().get_logical_length();
            uint32_t ext_len =
                std::min<uint32_t>(4096 * (1 + rng() % 4), blob_len);
            uint32_t bo = (rng() % (blob_len / 4096 + 1)) * 4096;
            if (bo + ext_len > blob_len) bo = blob_len - ext_len;
            em.add(cur_lo, bo, ext_len, b);
            before_cov.emplace_back(cur_lo, cur_lo + ext_len);
            cur_lo += ext_len + (rng() % 2 ? 4096 * (1 + rng() % 3) : 0);
        }

        if (em.empty()) continue;

        uint64_t max_lo = cur_lo;
        uint64_t p_off = 4096 * (rng() % (max_lo / 4096 + 1));
        uint64_t p_len = 4096 * (1 + rng() % 8);
        if (p_off >= max_lo) continue;

        std::vector<OldExtent> old_extents;
        em.punch_hole(p_off, p_len, &old_extents);

        for (const auto &e : em) {
            bool intersects = (e.logical_offset < p_off + p_len) &&
                (e.logical_end() > p_off);
            ASSERT_FALSE(intersects)
                << "iter " << iter << " extent remains in hole region";
        }

        ASSERT_FALSE(has_overlap(em))
            << "iter " << iter << " overlap after punch_hole";

        uint64_t before_head = 0, after_head = 0;
        for (const auto &iv : before_cov) {
            if (iv.first < p_off) {
                before_head += std::min(iv.second, p_off) - iv.first;
            }
        }
        for (const auto &e : em) {
            if (e.logical_offset < p_off) {
                after_head +=
                    std::min((uint64_t)e.logical_end(), p_off) - e.logical_offset;
            }
        }
        ASSERT_EQ(before_head, after_head)
            << "iter " << iter << " head coverage changed";

        uint64_t expected_old = 0;
        for (const auto &iv : before_cov) {
            uint64_t s = std::max(iv.first, p_off);
            uint64_t e = std::min(iv.second, p_off + p_len);
            if (s < e) expected_old += e - s;
        }
        uint64_t actual_old = 0;
        for (const auto &oe : old_extents) {
            ASSERT_GE(oe.e.logical_offset, p_off)
                << "iter " << iter << " old_extent starts before hole";
            ASSERT_LE(oe.e.logical_end(), p_off + p_len)
                << "iter " << iter << " old_extent ends after hole";
            actual_old += oe.e.length;
        }
        ASSERT_EQ(expected_old, actual_old)
            << "iter " << iter << " old_extents coverage mismatch";
    }
}

TEST_F(FuzzTest, VerifyCsumOffsetInvariants) {
    std::mt19937 rng(456);
    for (int iter = 0; iter < 1000; ++iter) {
        uint8_t chunk_order = (rng() % 2) ? 12 : 9;
        uint32_t chunk_size = 1u << chunk_order;
        uint64_t dev_block_size = chunk_size;

        uint32_t num_chunks = 2 + rng() % 16;
        uint32_t logical_len = num_chunks * chunk_size;

        Blob *b = make_csum_blob(chunk_order, logical_len);
        auto &blob = b->dirty_blob();

        uint32_t b_off_chunks = rng() % num_chunks;
        uint64_t b_off = b_off_chunks * chunk_size;
        uint32_t data_len = (num_chunks - b_off_chunks) * chunk_size;

        std::string data(data_len, '\0');
        for (auto &c : data) c = static_cast<char>(rng() % 256);
        bufferlist bl;
        bl.append(data);

        blob.calc_csum(b_off, bl, dev_block_size);

        bufferlist clean_bl;
        clean_bl.substr_of(bl, 0, data_len);
        int r = blob.verify_csum(b_off, clean_bl, dev_block_size);
        ASSERT_EQ(r, -1) << "iter " << iter << " clean data failed verify";

        uint32_t avail_chunks = num_chunks - b_off_chunks;
        uint32_t bad_chunk = rng() % avail_chunks;
        std::string corrupted = data;
        corrupted[bad_chunk * chunk_size] ^= 0xFF;
        bufferlist bad_bl;
        bad_bl.append(corrupted);
        r = blob.verify_csum(b_off, bad_bl, dev_block_size);
        ASSERT_EQ(r, static_cast<int>(b_off + bad_chunk * chunk_size))
            << "iter " << iter << " bad chunk " << bad_chunk << " returned " << r;

        uint32_t trim = rng() % (chunk_size - 1) + 1;
        uint32_t partial_len = data_len - trim;
        if (partial_len > 0 && partial_len % chunk_size != 0) {
            bufferlist partial_bl;
            partial_bl.substr_of(bl, 0, partial_len);
            r = blob.verify_csum(b_off, partial_bl, dev_block_size);
            ASSERT_EQ(r, -1)
                << "iter " << iter << " partial clean data failed verify";
        }
    }
}

}  // namespace
