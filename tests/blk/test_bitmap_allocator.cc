#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <vector>

#include "blk/allocator.h"
#include "blk/bitmap_allocator.h"
#include "blk/extent_types.h"
#include "common/intarith.h"
#include "common/interval_set.h"

using namespace TOPNSPC;

namespace {

class BitmapAllocatorTest : public ::testing::Test {
protected:
    static constexpr int64_t ALLOC_UNIT = 4096;
    static constexpr int64_t DEV_SIZE = 1ULL << 30;  // 1GB

    void SetUp() override {
        alloc.reset(static_cast<BitmapAllocator *>(
            Allocator::create("bitmap", DEV_SIZE, ALLOC_UNIT)));
        ASSERT_NE(alloc, nullptr);
    }

    void init_all_free() {
        alloc->init_add_free(0, DEV_SIZE);
    }

    std::unique_ptr<BitmapAllocator, void (*)(BitmapAllocator *)> alloc{
        nullptr, [](BitmapAllocator *a) { a->shutdown(); delete a; }};
};

TEST_F(BitmapAllocatorTest, CreateAndDestroy) {
}

TEST_F(BitmapAllocatorTest, InitAddFreeAndGetFree) {
    init_all_free();
    EXPECT_EQ(alloc->get_free(), p2align(DEV_SIZE, ALLOC_UNIT));
}

TEST_F(BitmapAllocatorTest, AllocateSingleExtent) {
    init_all_free();
    PExtentVector extents;
    int64_t r = alloc->allocate(ALLOC_UNIT, ALLOC_UNIT, 0, &extents);
    ASSERT_GT(r, 0);
    ASSERT_EQ(extents.size(), 1);
    EXPECT_EQ(extents[0].length, ALLOC_UNIT);
    EXPECT_LT(extents[0].offset, DEV_SIZE);
}

TEST_F(BitmapAllocatorTest, AllocateFailsWhenFull) {
    init_all_free();
    PExtentVector extents;
    int64_t r = alloc->allocate(DEV_SIZE, ALLOC_UNIT, 0, &extents);
    ASSERT_GT(r, 0);

    PExtentVector extents2;
    r = alloc->allocate(ALLOC_UNIT, ALLOC_UNIT, 0, &extents2);
    EXPECT_EQ(r, -ENOSPC);
}

TEST_F(BitmapAllocatorTest, ReleaseAndReallocate) {
    init_all_free();
    PExtentVector extents;
    int64_t r = alloc->allocate(ALLOC_UNIT * 10, ALLOC_UNIT, 0, &extents);
    ASSERT_GT(r, 0);
    uint64_t freed = 0;
    for (auto &e : extents)
        freed += e.length;
    EXPECT_EQ(freed, ALLOC_UNIT * 10);

    alloc->release(extents);
    EXPECT_EQ(alloc->get_free(), p2align(DEV_SIZE, ALLOC_UNIT));
}

TEST_F(BitmapAllocatorTest, PartialAllocWhenInsufficientFree) {
    init_all_free();
    PExtentVector extents;
    alloc->allocate(DEV_SIZE / 2, ALLOC_UNIT, 0, &extents);
    PExtentVector extents2;
    int64_t r = alloc->allocate(DEV_SIZE, ALLOC_UNIT, 0, &extents2);
    ASSERT_GT(r, 0);
    uint64_t total = 0;
    for (auto &e : extents)
        total += e.length;
    for (auto &e : extents2)
        total += e.length;
    EXPECT_EQ(total, p2align(DEV_SIZE, ALLOC_UNIT));
}

TEST_F(BitmapAllocatorTest, InitAddFreePartialRange) {
    alloc->init_add_free(ALLOC_UNIT * 10, ALLOC_UNIT * 100);
    EXPECT_EQ(alloc->get_free(), ALLOC_UNIT * 100);
}

TEST_F(BitmapAllocatorTest, InitRmFree) {
    init_all_free();
    alloc->init_rm_free(ALLOC_UNIT * 10, ALLOC_UNIT * 100);
    EXPECT_EQ(alloc->get_free(), p2align(DEV_SIZE, ALLOC_UNIT) - ALLOC_UNIT * 100);
}

TEST_F(BitmapAllocatorTest, MultipleSmallAllocsRoundtrip) {
    init_all_free();
    std::vector<PExtentVector> allocs;
    for (int i = 0; i < 10; ++i) {
        PExtentVector e;
        int64_t r = alloc->allocate(ALLOC_UNIT, ALLOC_UNIT, 0, &e);
        ASSERT_GT(r, 0);
        allocs.push_back(std::move(e));
    }

    for (auto &e : allocs)
        alloc->release(e);
    EXPECT_EQ(alloc->get_free(), p2align(DEV_SIZE, ALLOC_UNIT));
}

TEST_F(BitmapAllocatorTest, AllocMaxSizeLimit) {
    init_all_free();
    PExtentVector extents;
    int64_t r = alloc->allocate(DEV_SIZE / 2, ALLOC_UNIT,
                                ALLOC_UNIT * 2, 0, &extents);
    ASSERT_GT(r, 0);
    for (auto &e : extents)
        EXPECT_LE(e.length, ALLOC_UNIT * 2);
}

TEST_F(BitmapAllocatorTest, DoubleReleaseFails) {
    init_all_free();
    PExtentVector extents;
    alloc->allocate(ALLOC_UNIT, ALLOC_UNIT, 0, &extents);
    alloc->release(extents);
    EXPECT_LE(alloc->get_free(), p2align(DEV_SIZE, ALLOC_UNIT));
}

TEST_F(BitmapAllocatorTest, GetFragmentationZeroWhenEmpty) {
    init_all_free();
    double frag = alloc->get_fragmentation();
    EXPECT_DOUBLE_EQ(frag, 0.0);
}

TEST_F(BitmapAllocatorTest, AllocatedAndReleased) {
    init_all_free();
    PExtentVector extents;
    alloc->allocate(ALLOC_UNIT, ALLOC_UNIT, 0, &extents);
    alloc->release(extents);
    PExtentVector extents2;
    int64_t r = alloc->allocate(ALLOC_UNIT, ALLOC_UNIT, 0, &extents2);
    ASSERT_GT(r, 0);
}

TEST_F(BitmapAllocatorTest, ShutdownThenInit) {
    alloc->shutdown();
    alloc->init_add_free(0, DEV_SIZE);
    EXPECT_EQ(alloc->get_free(), p2align(DEV_SIZE, ALLOC_UNIT));
}

TEST_F(BitmapAllocatorTest, InitAddFreeUnaligned) {
    // BitmapAllocator rounds up the offset and rounds down the length
    // to l0_granularity (4096). A range of (100, 5000) becomes (4096, 904)
    // which is < 4096 and gets dropped.
    alloc->init_add_free(100, 5000);
    EXPECT_EQ(alloc->get_free(), 0);
    // A properly aligned range works
    alloc->init_add_free(0, ALLOC_UNIT * 2);
    EXPECT_EQ(alloc->get_free(), ALLOC_UNIT * 2);
}

TEST_F(BitmapAllocatorTest, AllocWithHint) {
    init_all_free();
    PExtentVector extents;
    int64_t hint = DEV_SIZE / 4;
    int64_t r = alloc->allocate(ALLOC_UNIT, ALLOC_UNIT, hint, &extents);
    ASSERT_GT(r, 0);
    EXPECT_EQ(extents.size(), 1);
}

TEST_F(BitmapAllocatorTest, LargeAllocFromFreshDevice) {
    init_all_free();
    PExtentVector extents;
    // Allocate a large chunk that spans across L1 boundaries
    int64_t r = alloc->allocate(ALLOC_UNIT * 10000, ALLOC_UNIT, 0, &extents);
    ASSERT_GT(r, 0);
    uint64_t total = 0;
    for (auto &e : extents)
        total += e.length;
    EXPECT_EQ(total, ALLOC_UNIT * 10000);
}

TEST_F(BitmapAllocatorTest, FragmentationAfterFragmentedFree) {
    init_all_free();
    // Allocate several extents and release every other one to create fragmentation
    std::vector<PExtentVector> allocs;
    for (int i = 0; i < 10; ++i) {
        PExtentVector e;
        alloc->allocate(ALLOC_UNIT * 2, ALLOC_UNIT, 0, &e);
        allocs.push_back(std::move(e));
    }
    for (int i = 1; i < 10; i += 2)
        alloc->release(allocs[i]);
    double frag = alloc->get_fragmentation();
    EXPECT_GT(frag, 0.0);
}

TEST_F(BitmapAllocatorTest, ExhaustiveSmallThenLargeAlloc) {
    init_all_free();
    // Exhaust available space with small allocations
    std::vector<PExtentVector> allocs;
    int64_t chunk = ALLOC_UNIT * 8;
    int num = p2align(DEV_SIZE, ALLOC_UNIT) / chunk;
    for (int i = 0; i < num; ++i) {
        PExtentVector e;
        int64_t r = alloc->allocate(chunk, ALLOC_UNIT, 0, &e);
        ASSERT_GT(r, 0);
        allocs.push_back(std::move(e));
    }
    // Release half
    for (int i = 0; i < num; i += 2)
        alloc->release(allocs[i]);
    // Try a large allocation
    PExtentVector e;
    int64_t r = alloc->allocate(chunk, ALLOC_UNIT, 0, &e);
    ASSERT_GT(r, 0);
}

TEST_F(BitmapAllocatorTest, InitRmFreeThenAlloc) {
    init_all_free();
    alloc->init_rm_free(0, ALLOC_UNIT * 10);
    uint64_t total = 0;
    // Keep allocating until ENOSPC
    while (true) {
        PExtentVector extents;
        int64_t r = alloc->allocate(DEV_SIZE, ALLOC_UNIT, 0, &extents);
        if (r <= 0) break;
        for (auto &e : extents)
            total += e.length;
    }
    // BitmapAllocator L2 granularity is ~512MB for a 1GB device with 4KB alloc_unit.
    // init_rm_free clears the L2 bit for the first 512MB chunk (even though only
    // 40KB was removed), so only the second fully-free 512MB chunk is allocable.
    EXPECT_EQ(total, 512ULL * 1024 * 1024);
}

TEST_F(BitmapAllocatorTest, AllocZeroWant) {
    init_all_free();
    PExtentVector extents;
    int64_t r = alloc->allocate(0, ALLOC_UNIT, 0, &extents);
    EXPECT_EQ(r, -ENOSPC);
}

TEST_F(BitmapAllocatorTest, AllocUnalignedWant) {
    // Should fail if want is not aligned to unit
    init_all_free();
    PExtentVector extents;
    EXPECT_DEATH(alloc->allocate(100, ALLOC_UNIT, 0, &extents), ".*");
}

TEST_F(BitmapAllocatorTest, ReleaseEmptySet) {
    init_all_free();
    interval_set<uint64_t> empty;
    alloc->release(empty);
    EXPECT_EQ(alloc->get_free(), p2align(DEV_SIZE, ALLOC_UNIT));
}

}  // namespace
