#include <gtest/gtest.h>

#include "bluestore/extent_map.h"

using namespace TOPNSPC;

class ExtentMapTest : public ::testing::Test {
protected:
    void SetUp() override {}

    BlobRef create_blob(uint32_t length) {
        Blob *b = new Blob();
        bluestore_blob_t &blob = b->dirty_blob();
        PExtentVector extents;
        extents.emplace_back(0x1000, length);
        blob.allocated(0, length, extents);
        return b;
    }

    void release_blob(BlobRef b) {
        while (b->get_nref() > 0) {
            b->put();
        }
        delete b;
    }
};

TEST_F(ExtentMapTest, EmptyMap) {
    ExtentMap em;
    EXPECT_TRUE(em.empty());
    EXPECT_EQ(em.size(), 0u);
}

TEST_F(ExtentMapTest, AddSingleExtent) {
    ExtentMap em;
    BlobRef b = create_blob(4096);

    auto it = em.add(0, 0, 4096, b);
    EXPECT_NE(it, em.end());
    EXPECT_EQ(em.size(), 1u);

    release_blob(b);
}

TEST_F(ExtentMapTest, AddMultipleExtents) {
    ExtentMap em;
    BlobRef b1 = create_blob(4096);
    BlobRef b2 = create_blob(4096);

    em.add(0, 0, 4096, b1);
    em.add(4096, 0, 4096, b2);

    EXPECT_EQ(em.size(), 2u);

    release_blob(b1);
    release_blob(b2);
}

TEST_F(ExtentMapTest, SeekLextentFound) {
    ExtentMap em;
    BlobRef b = create_blob(4096);

    em.add(0, 0, 4096, b);

    auto it = em.seek_lextent(0);
    EXPECT_NE(it, em.end());
    EXPECT_EQ(it->logical_offset, 0u);

    it = em.seek_lextent(2048);
    EXPECT_NE(it, em.end());
    EXPECT_EQ(it->logical_offset, 0u);

    it = em.seek_lextent(4095);
    EXPECT_NE(it, em.end());
    EXPECT_EQ(it->logical_offset, 0u);

    release_blob(b);
}

TEST_F(ExtentMapTest, SeekLextentNotFound) {
    ExtentMap em;
    BlobRef b = create_blob(4096);

    em.add(0, 0, 4096, b);

    auto it = em.seek_lextent(4096);
    EXPECT_EQ(it, em.end());

    it = em.seek_lextent(8192);
    EXPECT_EQ(it, em.end());

    release_blob(b);
}

TEST_F(ExtentMapTest, SeekLextentMultipleExtents) {
    ExtentMap em;
    BlobRef b1 = create_blob(4096);
    BlobRef b2 = create_blob(4096);
    BlobRef b3 = create_blob(4096);

    em.add(0, 0, 4096, b1);
    em.add(8192, 0, 4096, b2);
    em.add(16384, 0, 4096, b3);

    auto it = em.seek_lextent(2048);
    EXPECT_NE(it, em.end());
    EXPECT_EQ(it->logical_offset, 0u);

    it = em.seek_lextent(6144);
    EXPECT_EQ(it, em.end());

    it = em.seek_lextent(10000);
    EXPECT_NE(it, em.end());
    EXPECT_EQ(it->logical_offset, 8192u);

    release_blob(b1);
    release_blob(b2);
    release_blob(b3);
}

TEST_F(ExtentMapTest, RemoveExtent) {
    ExtentMap em;
    BlobRef b = create_blob(4096);

    auto it = em.add(0, 0, 4096, b);
    EXPECT_EQ(em.size(), 1u);

    em.rm(it);
    EXPECT_EQ(em.size(), 0u);
    EXPECT_TRUE(em.empty());

    release_blob(b);
}

TEST_F(ExtentMapTest, PunchHoleComplete) {
    ExtentMap em;
    BlobRef b = create_blob(4096);

    em.add(0, 0, 4096, b);

    std::vector<OldExtent> old_extents;
    em.punch_hole(0, 4096, &old_extents);

    EXPECT_EQ(em.size(), 0u);
    EXPECT_EQ(old_extents.size(), 1u);
    EXPECT_EQ(old_extents[0].e.logical_offset, 0u);
    EXPECT_EQ(old_extents[0].e.length, 4096u);

    release_blob(b);
}

TEST_F(ExtentMapTest, PunchHoleMiddle) {
    ExtentMap em;
    BlobRef b = create_blob(8192);

    em.add(0, 0, 8192, b);

    std::vector<OldExtent> old_extents;
    em.punch_hole(2048, 4096, &old_extents);

    EXPECT_EQ(em.size(), 2u);
    EXPECT_EQ(old_extents.size(), 1u);

    auto it = em.seek_lextent(0);
    EXPECT_NE(it, em.end());
    EXPECT_EQ(it->logical_offset, 0u);
    EXPECT_EQ(it->length, 2048u);

    it = em.seek_lextent(6144);
    EXPECT_NE(it, em.end());
    EXPECT_EQ(it->logical_offset, 6144u);
    EXPECT_EQ(it->length, 2048u);

    release_blob(b);
}

TEST_F(ExtentMapTest, PunchHoleStart) {
    ExtentMap em;
    BlobRef b = create_blob(8192);

    em.add(0, 0, 8192, b);

    std::vector<OldExtent> old_extents;
    em.punch_hole(0, 4096, &old_extents);

    EXPECT_EQ(em.size(), 1u);
    EXPECT_EQ(old_extents.size(), 1u);

    auto it = em.seek_lextent(0);
    EXPECT_EQ(it, em.end());

    it = em.seek_lextent(4096);
    EXPECT_NE(it, em.end());
    EXPECT_EQ(it->logical_offset, 4096u);
    EXPECT_EQ(it->length, 4096u);

    release_blob(b);
}

TEST_F(ExtentMapTest, PunchHoleEnd) {
    ExtentMap em;
    BlobRef b = create_blob(8192);

    em.add(0, 0, 8192, b);

    std::vector<OldExtent> old_extents;
    em.punch_hole(4096, 4096, &old_extents);

    EXPECT_EQ(em.size(), 1u);
    EXPECT_EQ(old_extents.size(), 1u);

    auto it = em.seek_lextent(0);
    EXPECT_NE(it, em.end());
    EXPECT_EQ(it->logical_offset, 0u);
    EXPECT_EQ(it->length, 4096u);

    it = em.seek_lextent(4096);
    EXPECT_EQ(it, em.end());

    release_blob(b);
}

TEST_F(ExtentMapTest, PunchHoleMultipleExtents) {
    ExtentMap em;
    BlobRef b1 = create_blob(4096);
    BlobRef b2 = create_blob(4096);
    BlobRef b3 = create_blob(4096);

    em.add(0, 0, 4096, b1);
    em.add(4096, 0, 4096, b2);
    em.add(8192, 0, 4096, b3);

    std::vector<OldExtent> old_extents;
    em.punch_hole(0, 12288, &old_extents);

    EXPECT_EQ(em.size(), 0u);
    EXPECT_EQ(old_extents.size(), 3u);

    release_blob(b1);
    release_blob(b2);
    release_blob(b3);
}

TEST_F(ExtentMapTest, PunchHolePartialOverlap) {
    ExtentMap em;
    BlobRef b1 = create_blob(4096);
    BlobRef b2 = create_blob(4096);

    em.add(0, 0, 4096, b1);
    em.add(4096, 0, 4096, b2);

    std::vector<OldExtent> old_extents;
    em.punch_hole(2048, 4096, &old_extents);

    EXPECT_EQ(em.size(), 2u);
    EXPECT_EQ(old_extents.size(), 2u);

    auto it = em.seek_lextent(0);
    EXPECT_NE(it, em.end());
    EXPECT_EQ(it->logical_offset, 0u);
    EXPECT_EQ(it->length, 2048u);

    it = em.seek_lextent(6144);
    EXPECT_NE(it, em.end());
    EXPECT_EQ(it->logical_offset, 6144u);
    EXPECT_EQ(it->length, 2048u);

    release_blob(b1);
    release_blob(b2);
}

TEST_F(ExtentMapTest, CompressExtentMapAdjacent) {
    ExtentMap em;
    BlobRef b = create_blob(8192);

    em.add(0, 0, 4096, b);
    em.add(4096, 4096, 4096, b);

    int compressed = em.compress_extent_map(0, 8192);
    EXPECT_EQ(compressed, 1);
    EXPECT_EQ(em.size(), 1u);

    auto it = em.seek_lextent(0);
    EXPECT_NE(it, em.end());
    EXPECT_EQ(it->logical_offset, 0u);
    EXPECT_EQ(it->blob_offset, 0u);
    EXPECT_EQ(it->length, 8192u);

    release_blob(b);
}

TEST_F(ExtentMapTest, CompressExtentMapNonAdjacent) {
    ExtentMap em;
    BlobRef b1 = create_blob(4096);
    BlobRef b2 = create_blob(4096);

    em.add(0, 0, 4096, b1);
    em.add(4096, 0, 4096, b2);

    int compressed = em.compress_extent_map(0, 8192);
    EXPECT_EQ(compressed, 0);
    EXPECT_EQ(em.size(), 2u);

    release_blob(b1);
    release_blob(b2);
}

TEST_F(ExtentMapTest, CompressExtentMapGap) {
    ExtentMap em;
    BlobRef b = create_blob(8192);

    em.add(0, 0, 4096, b);
    em.add(8192, 8192, 4096, b);

    int compressed = em.compress_extent_map(0, 12288);
    EXPECT_EQ(compressed, 0);
    EXPECT_EQ(em.size(), 2u);

    release_blob(b);
}

TEST_F(ExtentMapTest, NeedsReshard) {
    ExtentMap em;
    EXPECT_FALSE(em.needs_reshard());
}

TEST_F(ExtentMapTest, Clear) {
    ExtentMap em;
    BlobRef b1 = create_blob(4096);
    BlobRef b2 = create_blob(4096);

    em.add(0, 0, 4096, b1);
    em.add(4096, 0, 4096, b2);

    EXPECT_EQ(em.size(), 2u);

    em.clear();
    EXPECT_EQ(em.size(), 0u);
    EXPECT_TRUE(em.empty());

    release_blob(b1);
    release_blob(b2);
}

TEST_F(ExtentMapTest, Iterator) {
    ExtentMap em;
    BlobRef b1 = create_blob(4096);
    BlobRef b2 = create_blob(4096);
    BlobRef b3 = create_blob(4096);

    em.add(0, 0, 4096, b1);
    em.add(4096, 0, 4096, b2);
    em.add(8192, 0, 4096, b3);

    int count = 0;
    uint32_t expected_offsets[] = {0, 4096, 8192};
    for (auto it = em.begin(); it != em.end(); ++it) {
        EXPECT_EQ(it->logical_offset, expected_offsets[count]);
        ++count;
    }
    EXPECT_EQ(count, 3);

    release_blob(b1);
    release_blob(b2);
    release_blob(b3);
}
