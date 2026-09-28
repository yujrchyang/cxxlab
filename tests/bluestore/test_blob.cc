#include <gtest/gtest.h>

#include "bluestore/blob.h"

using namespace TOPNSPC;

TEST(BlobTest, DefaultConstruction) {
    Blob b;
    EXPECT_EQ(b.get_nref(), 0);
    EXPECT_EQ(b.get_id(), -1);
    EXPECT_FALSE(b.is_spanning());
    EXPECT_FALSE(b.is_referenced());
    EXPECT_EQ(b.get_referenced_bytes(), 0u);
}

TEST(BlobTest, ReferenceCounting) {
    Blob *b = new Blob();
    EXPECT_EQ(b->get_nref(), 0);

    b->get();
    EXPECT_EQ(b->get_nref(), 1);

    b->get();
    EXPECT_EQ(b->get_nref(), 2);

    EXPECT_FALSE(b->put());
    EXPECT_EQ(b->get_nref(), 1);

    EXPECT_TRUE(b->put());
}

TEST(BlobTest, SpanningBlob) {
    Blob b;
    EXPECT_FALSE(b.is_spanning());

    b.set_id(5);
    EXPECT_TRUE(b.is_spanning());
    EXPECT_EQ(b.get_id(), 5);
}

TEST(BlobTest, GetRefSimple) {
    Blob b;
    bluestore_blob_t &blob = b.dirty_blob();

    PExtentVector extents;
    extents.emplace_back(0x1000, 4096);
    blob.allocated(0, 4096, extents);

    b.get_ref(0, 4096, 4096);
    EXPECT_TRUE(b.is_referenced());
    EXPECT_EQ(b.get_referenced_bytes(), 4096u);
}

TEST(BlobTest, GetRefMultiple) {
    Blob b;
    bluestore_blob_t &blob = b.dirty_blob();

    PExtentVector extents;
    extents.emplace_back(0x1000, 4096);
    blob.allocated(0, 4096, extents);

    b.get_ref(0, 2048, 4096);
    EXPECT_EQ(b.get_referenced_bytes(), 2048u);

    b.get_ref(2048, 2048, 4096);
    EXPECT_EQ(b.get_referenced_bytes(), 4096u);
}

TEST(BlobTest, PutRefSimple) {
    Blob b;
    bluestore_blob_t &blob = b.dirty_blob();

    PExtentVector extents;
    extents.emplace_back(0x1000, 4096);
    blob.allocated(0, 4096, extents);

    b.get_ref(0, 4096, 4096);
    EXPECT_TRUE(b.is_referenced());

    PExtentVector released;
    EXPECT_TRUE(b.put_ref(0, 4096, &released));
    EXPECT_FALSE(b.is_referenced());
    EXPECT_EQ(released.size(), 1u);
    EXPECT_EQ(released[0].offset, 0x1000u);
    EXPECT_EQ(released[0].length, 4096u);
}

TEST(BlobTest, PutRefPartial) {
    Blob b;
    bluestore_blob_t &blob = b.dirty_blob();

    PExtentVector extents;
    extents.emplace_back(0x1000, 4096);
    extents.emplace_back(0x2000, 4096);
    blob.allocated(0, 8192, extents);

    b.get_ref(0, 8192, 4096);

    PExtentVector released;
    EXPECT_FALSE(b.put_ref(0, 4096, &released));
    EXPECT_TRUE(b.is_referenced());
    EXPECT_EQ(released.size(), 1u);
    EXPECT_EQ(released[0].offset, 0x1000u);
}

TEST(BlobTest, CanSplit) {
    Blob b;
    bluestore_blob_t &blob = b.dirty_blob();

    PExtentVector extents;
    extents.emplace_back(0x1000, 8192);
    blob.allocated(0, 8192, extents);

    b.get_ref(0, 8192, 4096);

    EXPECT_TRUE(b.can_split());
    EXPECT_TRUE(b.can_split_at(4096));
}

TEST(BlobTest, CanSplitWithCsum) {
    Blob b;
    bluestore_blob_t &blob = b.dirty_blob();

    PExtentVector extents;
    extents.emplace_back(0x1000, 8192);
    blob.allocated(0, 8192, extents);
    blob.init_csum(CSUM_CRC32C, 12, 8192);

    b.get_ref(0, 8192, 4096);

    EXPECT_TRUE(b.can_split());
    EXPECT_TRUE(b.can_split_at(4096));
    EXPECT_FALSE(b.can_split_at(2048));
}

TEST(BlobTest, SplitSimple) {
    Blob b;
    bluestore_blob_t &blob = b.dirty_blob();

    PExtentVector extents;
    extents.emplace_back(0x1000, 8192);
    blob.allocated(0, 8192, extents);

    b.get_ref(0, 8192, 4096);

    Blob r;
    b.split(4096, &r);

    EXPECT_EQ(b.get_blob().get_logical_length(), 4096u);
    EXPECT_EQ(r.get_blob().get_logical_length(), 4096u);
    EXPECT_EQ(b.get_blob().get_extents().size(), 1u);
    EXPECT_EQ(r.get_blob().get_extents().size(), 1u);
}

TEST(BlobTest, SplitMultipleExtents) {
    Blob b;
    bluestore_blob_t &blob = b.dirty_blob();

    PExtentVector extents;
    extents.emplace_back(0x1000, 4096);
    extents.emplace_back(0x2000, 4096);
    blob.allocated(0, 8192, extents);

    b.get_ref(0, 8192, 4096);

    Blob r;
    b.split(4096, &r);

    EXPECT_EQ(b.get_blob().get_logical_length(), 4096u);
    EXPECT_EQ(r.get_blob().get_logical_length(), 4096u);
    EXPECT_EQ(b.get_blob().get_extents().size(), 1u);
    EXPECT_EQ(r.get_blob().get_extents().size(), 1u);
    EXPECT_EQ(b.get_blob().get_extents()[0].offset, 0x1000u);
    EXPECT_EQ(r.get_blob().get_extents()[0].offset, 0x2000u);
}

TEST(BlobTest, SplitMidExtent) {
    Blob b;
    bluestore_blob_t &blob = b.dirty_blob();

    PExtentVector extents;
    extents.emplace_back(0x1000, 8192);
    blob.allocated(0, 8192, extents);

    b.get_ref(0, 8192, 4096);

    Blob r;
    b.split(2048, &r);

    EXPECT_EQ(b.get_blob().get_logical_length(), 2048u);
    EXPECT_EQ(r.get_blob().get_logical_length(), 6144u);
    EXPECT_EQ(b.get_blob().get_extents().size(), 1u);
    EXPECT_EQ(r.get_blob().get_extents().size(), 1u);
    EXPECT_EQ(b.get_blob().get_extents()[0].offset, 0x1000u);
    EXPECT_EQ(b.get_blob().get_extents()[0].length, 2048u);
    EXPECT_EQ(r.get_blob().get_extents()[0].offset, 0x1800u);
    EXPECT_EQ(r.get_blob().get_extents()[0].length, 6144u);
}

TEST(BlobTest, CanReuseBlob) {
    Blob b;
    bluestore_blob_t &blob = b.dirty_blob();

    PExtentVector extents;
    extents.emplace_back(0x1000, 4096);
    blob.allocated(0, 4096, extents);

    uint32_t length = 4096;
    EXPECT_TRUE(b.can_reuse_blob(4096, 8192, 4096, &length));
    EXPECT_EQ(length, 4096u);
    EXPECT_EQ(b.get_blob().get_logical_length(), 8192u);
}

TEST(BlobTest, CanReuseBlobOverlapUnallocated) {
    Blob b;
    bluestore_blob_t &blob = b.dirty_blob();

    PExtentVector extents;
    extents.emplace_back(0x1000, 4096);
    extents.emplace_back(bluestore_pextent_t::INVALID_OFFSET, 4096);
    blob.allocated(0, 8192, extents);

    uint32_t length = 4096;
    EXPECT_TRUE(b.can_reuse_blob(4096, 8192, 4096, &length));
}

TEST(BlobTest, CanReuseBlobOverlapAllocated) {
    Blob b;
    bluestore_blob_t &blob = b.dirty_blob();

    PExtentVector extents;
    extents.emplace_back(0x1000, 4096);
    extents.emplace_back(0x2000, 4096);
    blob.allocated(0, 8192, extents);

    uint32_t length = 4096;
    EXPECT_FALSE(b.can_reuse_blob(4096, 8192, 4096, &length));
}

TEST(BlobTest, CanReuseBlobImmutable) {
    Blob b;
    bluestore_blob_t &blob = b.dirty_blob();

    PExtentVector extents;
    extents.emplace_back(0x1000, 4096);
    blob.allocated(0, 4096, extents);
    blob.set_flag(bluestore_blob_t::FLAG_HAS_UNUSED);

    uint32_t length = 4096;
    EXPECT_FALSE(b.can_reuse_blob(4096, 8192, 4096, &length));
}
