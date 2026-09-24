#include <gtest/gtest.h>

#include <cstdint>

#include "common/interval_set.h"

using namespace TOPNSPC;

namespace {

TEST(IntervalSetTest, EmptySet) {
    interval_set<uint64_t> is;
    EXPECT_TRUE(is.empty());
    EXPECT_EQ(is.size(), 0u);
    EXPECT_EQ(is.begin(), is.end());
}

TEST(IntervalSetTest, BasicInsert) {
    interval_set<uint64_t> is;
    is.insert(10, 5);
    EXPECT_EQ(is.size(), 1u);
    auto it = is.begin();
    EXPECT_EQ(it.get_start(), 10u);
    EXPECT_EQ(it.get_len(), 5u);
}

TEST(IntervalSetTest, InsertZeroLen) {
    interval_set<uint64_t> is;
    is.insert(10, 0);
    EXPECT_TRUE(is.empty());
}

TEST(IntervalSetTest, MergeAdjacent) {
    interval_set<uint64_t> is;
    is.insert(0, 10);
    is.insert(10, 10);
    EXPECT_EQ(is.size(), 1u);
    auto it = is.begin();
    EXPECT_EQ(it.get_start(), 0u);
    EXPECT_EQ(it.get_len(), 20u);
}

TEST(IntervalSetTest, MergeAdjacentReverse) {
    interval_set<uint64_t> is;
    is.insert(10, 10);
    is.insert(0, 10);
    EXPECT_EQ(is.size(), 1u);
    auto it = is.begin();
    EXPECT_EQ(it.get_start(), 0u);
    EXPECT_EQ(it.get_len(), 20u);
}

TEST(IntervalSetTest, MergeOverlapping) {
    interval_set<uint64_t> is;
    is.insert(0, 10);
    is.insert(5, 10);
    EXPECT_EQ(is.size(), 1u);
    auto it = is.begin();
    EXPECT_EQ(it.get_start(), 0u);
    EXPECT_EQ(it.get_len(), 15u);
}

TEST(IntervalSetTest, MergeOverlappingReverse) {
    interval_set<uint64_t> is;
    is.insert(5, 10);
    is.insert(0, 10);
    EXPECT_EQ(is.size(), 1u);
    auto it = is.begin();
    EXPECT_EQ(it.get_start(), 0u);
    EXPECT_EQ(it.get_len(), 15u);
}

TEST(IntervalSetTest, MergeContained) {
    interval_set<uint64_t> is;
    is.insert(0, 20);
    is.insert(5, 5);
    EXPECT_EQ(is.size(), 1u);
    auto it = is.begin();
    EXPECT_EQ(it.get_start(), 0u);
    EXPECT_EQ(it.get_len(), 20u);
}

TEST(IntervalSetTest, MergeExactSame) {
    interval_set<uint64_t> is;
    is.insert(10, 10);
    is.insert(10, 10);
    EXPECT_EQ(is.size(), 1u);
    auto it = is.begin();
    EXPECT_EQ(it.get_start(), 10u);
    EXPECT_EQ(it.get_len(), 10u);
}

TEST(IntervalSetTest, MergeMultiple) {
    interval_set<uint64_t> is;
    is.insert(0, 5);
    is.insert(10, 5);
    is.insert(20, 5);
    is.insert(5, 15);
    EXPECT_EQ(is.size(), 1u);
    auto it = is.begin();
    EXPECT_EQ(it.get_start(), 0u);
    EXPECT_EQ(it.get_len(), 25u);
}

TEST(IntervalSetTest, NoMergeGap) {
    interval_set<uint64_t> is;
    is.insert(0, 5);
    is.insert(10, 5);
    EXPECT_EQ(is.size(), 2u);
}

TEST(IntervalSetTest, EraseComplete) {
    interval_set<uint64_t> is;
    is.insert(10, 10);
    is.erase(10, 10);
    EXPECT_TRUE(is.empty());
}

TEST(IntervalSetTest, EraseZeroLen) {
    interval_set<uint64_t> is;
    is.insert(10, 10);
    is.erase(10, 0);
    EXPECT_EQ(is.size(), 1u);
}

TEST(IntervalSetTest, ErasePartialLeft) {
    interval_set<uint64_t> is;
    is.insert(10, 10);
    is.erase(10, 5);
    EXPECT_EQ(is.size(), 1u);
    auto it = is.begin();
    EXPECT_EQ(it.get_start(), 15u);
    EXPECT_EQ(it.get_len(), 5u);
}

TEST(IntervalSetTest, ErasePartialRight) {
    interval_set<uint64_t> is;
    is.insert(10, 10);
    is.erase(15, 5);
    EXPECT_EQ(is.size(), 1u);
    auto it = is.begin();
    EXPECT_EQ(it.get_start(), 10u);
    EXPECT_EQ(it.get_len(), 5u);
}

TEST(IntervalSetTest, EraseSplit) {
    interval_set<uint64_t> is;
    is.insert(0, 20);
    is.erase(5, 10);
    EXPECT_EQ(is.size(), 2u);
    auto it = is.begin();
    EXPECT_EQ(it.get_start(), 0u);
    EXPECT_EQ(it.get_len(), 5u);
    ++it;
    EXPECT_EQ(it.get_start(), 15u);
    EXPECT_EQ(it.get_len(), 5u);
}

TEST(IntervalSetTest, EraseMultipleIntervals) {
    interval_set<uint64_t> is;
    is.insert(0, 5);
    is.insert(10, 5);
    is.insert(20, 5);
    is.erase(3, 20);
    EXPECT_EQ(is.size(), 2u);
    auto it = is.begin();
    EXPECT_EQ(it.get_start(), 0u);
    EXPECT_EQ(it.get_len(), 3u);
    ++it;
    EXPECT_EQ(it.get_start(), 23u);
    EXPECT_EQ(it.get_len(), 2u);
}

TEST(IntervalSetTest, EraseEmpty) {
    interval_set<uint64_t> is;
    is.erase(10, 10);
    EXPECT_TRUE(is.empty());
}

TEST(IntervalSetTest, EraseNoOverlap) {
    interval_set<uint64_t> is;
    is.insert(0, 10);
    is.insert(20, 10);
    is.erase(50, 10);
    EXPECT_EQ(is.size(), 2u);
}

TEST(IntervalSetTest, RangeStartEndEmpty) {
    interval_set<uint64_t> is;
    EXPECT_EQ(is.range_start(), 0u);
    EXPECT_EQ(is.range_end(), 0u);
}

TEST(IntervalSetTest, RangeStartEnd) {
    interval_set<uint64_t> is;
    is.insert(10, 5);
    is.insert(20, 5);
    EXPECT_EQ(is.range_start(), 10u);
    EXPECT_EQ(is.range_end(), 25u);
}

TEST(IntervalSetTest, InsertSet) {
    interval_set<uint64_t> is1;
    is1.insert(0, 10);
    is1.insert(20, 10);

    interval_set<uint64_t> is2;
    is2.insert(5, 10);
    is2.insert(25, 10);

    is1.insert(is2);
    EXPECT_EQ(is1.size(), 2u);
    auto it = is1.begin();
    EXPECT_EQ(it.get_start(), 0u);
    EXPECT_EQ(it.get_len(), 15u);
    ++it;
    EXPECT_EQ(it.get_start(), 20u);
    EXPECT_EQ(it.get_len(), 15u);
}

TEST(IntervalSetTest, Clear) {
    interval_set<uint64_t> is;
    is.insert(0, 10);
    is.insert(20, 10);
    EXPECT_EQ(is.size(), 2u);
    is.clear();
    EXPECT_TRUE(is.empty());
}

TEST(IntervalSetTest, Swap) {
    interval_set<uint64_t> is1;
    is1.insert(0, 10);

    interval_set<uint64_t> is2;
    is2.insert(20, 10);

    is1.swap(is2);
    EXPECT_EQ(is1.size(), 1u);
    EXPECT_EQ(is1.begin()->first, 20u);
    EXPECT_EQ(is2.size(), 1u);
    EXPECT_EQ(is2.begin()->first, 0u);
}

TEST(IntervalSetTest, IteratorIncrement) {
    interval_set<uint64_t> is;
    is.insert(0, 5);
    is.insert(10, 5);
    is.insert(20, 5);

    uint64_t expected_starts[] = {0, 10, 20};
    int i = 0;
    for (auto it = is.begin(); it != is.end(); ++it, ++i) {
        EXPECT_EQ(it.get_start(), expected_starts[i]);
        EXPECT_EQ(it.get_len(), 5u);
    }
    EXPECT_EQ(i, 3);
}

}  // namespace
