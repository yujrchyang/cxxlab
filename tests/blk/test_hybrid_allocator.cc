#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "blk/allocator.h"
#include "blk/extent_types.h"
#include "blk/hybrid_allocator.h"
#include "common/intarith.h"

using namespace TOPNSPC;

namespace {

class HybridAllocatorTest : public ::testing::Test {
protected:
    static constexpr int64_t BLOCK_SIZE = 4096;
    static constexpr int64_t DEV_SIZE = 1ULL << 30;  // 1GB

    void SetUp() override {
        // Cap at 256 range segs before spilling to bitmap child
        alloc.reset(static_cast<HybridAllocator *>(
            Allocator::create("hybrid", DEV_SIZE, BLOCK_SIZE)));
        ASSERT_NE(alloc, nullptr);
    }

    void init_all_free() {
        alloc->init_add_free(0, DEV_SIZE);
    }

    std::unique_ptr<HybridAllocator, void (*)(HybridAllocator *)> alloc{
        nullptr, [](HybridAllocator *a) { a->shutdown(); delete a; }};
};

TEST_F(HybridAllocatorTest, CreateAndDestroy) {
}

TEST_F(HybridAllocatorTest, InitAddFreeAndGetFree) {
    init_all_free();
    EXPECT_EQ(alloc->get_free(), DEV_SIZE);
}

TEST_F(HybridAllocatorTest, AllocateSingleExtent) {
    init_all_free();
    PExtentVector extents;
    int64_t r = alloc->allocate(BLOCK_SIZE, BLOCK_SIZE, 0, &extents);
    ASSERT_GT(r, 0);
    ASSERT_EQ(extents.size(), 1);
    EXPECT_EQ(extents[0].length, BLOCK_SIZE);
    EXPECT_LT(extents[0].offset, DEV_SIZE);
}

TEST_F(HybridAllocatorTest, AllocateFailsWhenFull) {
    init_all_free();
    PExtentVector extents;
    int64_t r = alloc->allocate(DEV_SIZE, BLOCK_SIZE, 0, &extents);
    ASSERT_GT(r, 0);

    PExtentVector extents2;
    r = alloc->allocate(BLOCK_SIZE, BLOCK_SIZE, 0, &extents2);
    EXPECT_EQ(r, -ENOSPC);
}

TEST_F(HybridAllocatorTest, ReleaseAndReallocate) {
    init_all_free();
    PExtentVector extents;
    int64_t r = alloc->allocate(BLOCK_SIZE * 10, BLOCK_SIZE, 0, &extents);
    ASSERT_GT(r, 0);
    alloc->release(extents);
    EXPECT_EQ(alloc->get_free(), DEV_SIZE);
}

TEST_F(HybridAllocatorTest, PartialAllocWhenInsufficientFree) {
    init_all_free();
    PExtentVector extents;
    alloc->allocate(DEV_SIZE / 2, BLOCK_SIZE, 0, &extents);
    PExtentVector extents2;
    int64_t r = alloc->allocate(DEV_SIZE, BLOCK_SIZE, 0, &extents2);
    ASSERT_GT(r, 0);
    uint64_t total = 0;
    for (auto &e : extents)
        total += e.length;
    for (auto &e : extents2)
        total += e.length;
    EXPECT_EQ(total, DEV_SIZE);
}

TEST_F(HybridAllocatorTest, InitAddFreePartialRange) {
    alloc->init_add_free(BLOCK_SIZE * 10, BLOCK_SIZE * 100);
    EXPECT_EQ(alloc->get_free(), BLOCK_SIZE * 100);
}

TEST_F(HybridAllocatorTest, InitRmFree) {
    init_all_free();
    alloc->init_rm_free(BLOCK_SIZE * 10, BLOCK_SIZE * 100);
    EXPECT_EQ(alloc->get_free(), DEV_SIZE - BLOCK_SIZE * 100);
}

TEST_F(HybridAllocatorTest, ReleaseMergedExtents) {
    init_all_free();
    PExtentVector extents;
    // Multiple small allocs with adjacent extents
    for (int i = 0; i < 5; ++i) {
        PExtentVector e;
        alloc->allocate(BLOCK_SIZE, BLOCK_SIZE, 0, &e);
        extents.insert(extents.end(), e.begin(), e.end());
    }
    alloc->release(extents);
    EXPECT_EQ(alloc->get_free(), DEV_SIZE);
}

TEST_F(HybridAllocatorTest, GetFragmentationZeroWhenEmpty) {
    init_all_free();
    double frag = alloc->get_fragmentation();
    EXPECT_DOUBLE_EQ(frag, 0.0);
}

TEST_F(HybridAllocatorTest, MultipleSmallAllocsRoundtrip) {
    init_all_free();
    std::vector<PExtentVector> allocs;
    for (int i = 0; i < 100; ++i) {
        PExtentVector e;
        int64_t r = alloc->allocate(BLOCK_SIZE, BLOCK_SIZE, 0, &e);
        ASSERT_GT(r, 0);
        allocs.push_back(std::move(e));
    }

    for (auto &e : allocs)
        alloc->release(e);
    EXPECT_EQ(alloc->get_free(), DEV_SIZE);
}

TEST_F(HybridAllocatorTest, AllocMaxSizeLimit) {
    init_all_free();
    PExtentVector extents;
    int64_t r = alloc->allocate(DEV_SIZE / 2, BLOCK_SIZE,
                                BLOCK_SIZE * 2, 0, &extents);
    ASSERT_GT(r, 0);
    for (auto &e : extents)
        EXPECT_LE(e.length, BLOCK_SIZE * 2);
}

TEST_F(HybridAllocatorTest, ShutdownThenInit) {
    alloc->shutdown();
    alloc->init_add_free(0, DEV_SIZE);
    EXPECT_EQ(alloc->get_free(), DEV_SIZE);
}

TEST_F(HybridAllocatorTest, InitAddFreeUnaligned) {
    alloc->init_add_free(100, 5000);
    EXPECT_EQ(alloc->get_free(), 5000);
}

TEST_F(HybridAllocatorTest, AllocWithHint) {
    init_all_free();
    PExtentVector extents;
    int64_t hint = DEV_SIZE / 4;
    int64_t r = alloc->allocate(BLOCK_SIZE, BLOCK_SIZE, hint, &extents);
    ASSERT_GT(r, 0);
    EXPECT_EQ(extents.size(), 1);
}

TEST_F(HybridAllocatorTest, LargeAllocFromSpillover) {
    // Create many small fragments to trigger spillover to bitmap child
    init_all_free();
    PExtentVector first;
    alloc->allocate(DEV_SIZE, BLOCK_SIZE, 0, &first);
    // Now release small scattered extents
    PExtentVector to_release;
    for (uint64_t off = 0; off < DEV_SIZE; off += 100 * BLOCK_SIZE) {
        if (off + BLOCK_SIZE <= DEV_SIZE) {
            to_release.emplace_back(off, BLOCK_SIZE);
        }
    }
    alloc->release(to_release);
    // The AVL tree should have many entries; subsequent large alloc should
    // still succeed via bitmap fallback
    PExtentVector big;
    int64_t r = alloc->allocate(BLOCK_SIZE * 4, BLOCK_SIZE, 0, &big);
    ASSERT_GT(r, 0);
}

TEST_F(HybridAllocatorTest, StressReleaseAlloc) {
    init_all_free();
    std::vector<PExtentVector> allocs;
    for (int i = 0; i < 50; ++i) {
        PExtentVector e;
        alloc->allocate(BLOCK_SIZE * 3, BLOCK_SIZE, 0, &e);
        allocs.push_back(std::move(e));
    }
    for (auto &e : allocs)
        alloc->release(e);
    EXPECT_EQ(alloc->get_free(), DEV_SIZE);
}



TEST_F(HybridAllocatorTest, AllocateMaxAllocSizeZero) {
    init_all_free();
    PExtentVector extents;
    int64_t r = alloc->allocate(BLOCK_SIZE * 4, BLOCK_SIZE, 0, 0, &extents);
    ASSERT_GT(r, 0);
    EXPECT_EQ(r, BLOCK_SIZE * 4);
}

TEST_F(HybridAllocatorTest, ConcurrentAllocateRelease) {
    init_all_free();
    std::atomic<int> success_count{0};
    const int num_threads = 8;
    const int ops_per_thread = 50;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < ops_per_thread; ++j) {
                PExtentVector e;
                if (alloc->allocate(BLOCK_SIZE, BLOCK_SIZE, 0, 0, &e) > 0) {
                    success_count.fetch_add(1);
                    alloc->release(e);
                }
            }
        });
    }
    for (auto &t : threads)
        t.join();

    EXPECT_EQ(success_count.load(), num_threads * ops_per_thread);
    EXPECT_EQ(alloc->get_free(), DEV_SIZE);
}

}  // namespace
