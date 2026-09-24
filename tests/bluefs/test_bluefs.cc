#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "blk/block_device.h"
#include "blk/kernel_device.h"
#include "bluefs/bluefs.h"
#include "cxxlab_test.h"

using namespace TOPNSPC;

// Temp file fixture for BlueFS tests
class BlueFSTest : public ::testing::Test {
protected:
    std::string tmp_path_;
    int tmp_fd_ = -1;

    // 8 MB — enough for superblock at offset 4K + log allocation (4MB) + room
    static constexpr uint64_t kFileSize = 8 << 20;

    void SetUp() override {
        auto tmpl = cxxlab_tmp_path("bluefs_test");
        tmp_fd_ = ::mkstemp(tmpl.data());
        ASSERT_GE(tmp_fd_, 0);
        tmp_path_ = tmpl;

        int r = ::fallocate(tmp_fd_, 0, 0, kFileSize);
        if (r < 0) {
            std::vector<char> zeros(4096, 0);
            for (uint64_t off = 0; off < kFileSize; off += zeros.size()) {
                ::pwrite(tmp_fd_, zeros.data(),
                         std::min<uint64_t>(zeros.size(), kFileSize - off),
                         off);
            }
        }
    }

    void TearDown() override {
        if (tmp_fd_ >= 0) {
            ::close(tmp_fd_);
            tmp_fd_ = -1;
        }
        if (!tmp_path_.empty()) {
            ::unlink(tmp_path_.c_str());
            tmp_path_.clear();
        }
    }
};

// ---------------------------------------------------------------------------
// Superblock write/read roundtrip
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, WriteAndReadSuper) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.shared_alloc_size = 65536;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));

    // Initially the superblock is garbage — _open_super should fail
    int r = fs._open_super();
    EXPECT_NE(r, 0);

    // Set UUID before write
    fs.get_mutable_super().uuid.generate();
    // _write_super auto-increments: starting from 0 → written version = 1
    ASSERT_EQ(fs._write_super(BlueFS::BDEV_DB), 0);

    // Now read back via a new BlueFS instance
    BlueFS fs2(cfg);
    ASSERT_NO_FATAL_FAILURE(fs2.add_block_device(BlueFS::BDEV_DB, tmp_path_));

    r = fs2._open_super();
    ASSERT_EQ(r, 0);

    EXPECT_EQ(fs2.get_super().version, 1);
    EXPECT_FALSE(fs2.get_super().uuid.is_zero());
}

// ---------------------------------------------------------------------------
// Superblock update (version bump on each write)
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, SuperVersionIncrement) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;

    // _write_super auto-increments super_.version before writing
    // Starting from 0: first write → version 1, second write → version 2
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        fs.get_mutable_super().uuid.generate();
        ASSERT_EQ(fs._write_super(BlueFS::BDEV_DB), 0);
    }

    // Read back — should be version 1
    BlueFS fs2(cfg);
    ASSERT_NO_FATAL_FAILURE(fs2.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs2._open_super(), 0);
    EXPECT_EQ(fs2.get_super().version, 1);
    EXPECT_FALSE(fs2.get_super().uuid.is_zero());

    // Write again — should bump to 2
    ASSERT_EQ(fs2._write_super(BlueFS::BDEV_DB), 0);

    BlueFS fs3(cfg);
    ASSERT_NO_FATAL_FAILURE(fs3.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs3._open_super(), 0);
    EXPECT_EQ(fs3.get_super().version, 2);
}

// ---------------------------------------------------------------------------
// Corrupted superblock — CRC mismatch
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, CorruptSuperblockFailsCrc) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;

    // Write valid super
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        fs.get_mutable_super().uuid.generate();
        fs.get_mutable_super().version = 42;
        ASSERT_EQ(fs._write_super(BlueFS::BDEV_DB), 0);
    }

    // Corrupt a byte inside the encoded superblock (after 6-byte DENC header,
    // before the CRC trailer) by writing directly to the raw device.
    {
        char buf[4096];
        ::pread(tmp_fd_, buf, sizeof(buf), BlueFS::SUPER_OFFSET);
        // Byte 20 is inside the encoded uuid data (DENC header = 6 bytes,
        // first uuid = 16 bytes → corrupt at offset 6+14 = 20)
        buf[20] ^= 0xff;
        ::pwrite(tmp_fd_, buf, sizeof(buf), BlueFS::SUPER_OFFSET);
    }

    // Reading should fail CRC check
    BlueFS fs2(cfg);
    ASSERT_NO_FATAL_FAILURE(fs2.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    EXPECT_NE(fs2._open_super(), 0);
}

// ---------------------------------------------------------------------------
// get_block_device_size
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, GetBlockDeviceSize) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;

    BlueFS fs(cfg);
    EXPECT_EQ(fs.get_block_device_size(BlueFS::BDEV_DB), 0);
    EXPECT_EQ(fs.get_block_device_size(BlueFS::BDEV_WAL), 0);
    EXPECT_EQ(fs.get_block_device_size(99), 0);

    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    EXPECT_EQ(fs.get_block_device_size(BlueFS::BDEV_DB), kFileSize);
}

// ---------------------------------------------------------------------------
// Shared allocator integration
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, SharedAllocator) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;

    // Create a shared allocator context (simulates BlueStore giving BlueFS
    // a slice of its allocator)
    std::unique_ptr<Allocator> shared_alloc(
        Allocator::create("bitmap", kFileSize, cfg.alloc_size));
    shared_alloc->init_add_free(BlueFS::SUPER_OFFSET + BlueFS::SUPER_LENGTH,
                                kFileSize - BlueFS::SUPER_OFFSET -
                                    BlueFS::SUPER_LENGTH);

    bluefs_shared_alloc_context_t ctx;
    ctx.set(shared_alloc.get(), cfg.alloc_size);

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_,
                                                false, 0, &ctx));

    // After add_block_device with shared_alloc, the allocator slot should be
    // the shared allocator, not a new one
    fs._init_alloc();

    // _init_alloc should skip shared devices, so no extra allocator created
    // Success: _init_alloc did not crash or leak
    SUCCEED();
}

// ---------------------------------------------------------------------------
// mkfs → mount → umount cycle
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, MkfsMountUmount) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.shared_alloc_size = 65536;

    // ---- mkfs ----
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    }

    // ---- mount ----
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);
        // Superblock should be valid
        EXPECT_FALSE(fs.get_super().uuid.is_zero());
        EXPECT_EQ(fs.get_super().version, 1);
        EXPECT_EQ(fs.get_super().block_size, 4096u);
        // Log fnode should have extents
        EXPECT_FALSE(fs.get_super().log_fnode.extents.empty());

        // ---- umount ----
        fs.umount();
    }
}

// ---------------------------------------------------------------------------
// mkdir + dir_exists
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, MkdirAndExists) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);

    // Initially no dirs
    EXPECT_FALSE(fs.dir_exists("foo"));

    // Create a directory
    EXPECT_EQ(fs.mkdir("foo"), 0);
    EXPECT_TRUE(fs.dir_exists("foo"));

    // Duplicate returns -EEXIST
    EXPECT_EQ(fs.mkdir("foo"), -EEXIST);

    // Multiple dirs
    EXPECT_EQ(fs.mkdir("bar"), 0);
    EXPECT_TRUE(fs.dir_exists("bar"));
    EXPECT_EQ(fs.mkdir("baz"), 0);
    EXPECT_TRUE(fs.dir_exists("baz"));

    // Non-existent returns false
    EXPECT_FALSE(fs.dir_exists("nonexistent"));

    fs.umount();
}

// ---------------------------------------------------------------------------
// rmdir (empty directory)
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, Rmdir) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);

    // Remove non-existent dir
    EXPECT_EQ(fs.rmdir("foo"), -ENOENT);

    // Create and remove
    ASSERT_EQ(fs.mkdir("foo"), 0);
    EXPECT_TRUE(fs.dir_exists("foo"));
    EXPECT_EQ(fs.rmdir("foo"), 0);
    EXPECT_FALSE(fs.dir_exists("foo"));

    // Remove again returns -ENOENT
    EXPECT_EQ(fs.rmdir("foo"), -ENOENT);

    fs.umount();
}

// ---------------------------------------------------------------------------
// rmdir — non-empty directory should fail with -ENOTEMPTY
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, RmdirNonEmpty) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);

    ASSERT_EQ(fs.mkdir("mydir"), 0);

    // Create a file inside the directory
    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("mydir", "afile", &w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    // Non-empty rmdir should fail
    EXPECT_EQ(fs.rmdir("mydir"), -ENOTEMPTY);

    // Verify the directory still exists with the file
    EXPECT_TRUE(fs.dir_exists("mydir"));

    fs.umount();
}

// ---------------------------------------------------------------------------
// readdir
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, Readdir) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);

    // List all dirs (empty root)
    std::vector<std::string> ls;
    EXPECT_EQ(fs.readdir("", &ls), 0);
    EXPECT_TRUE(ls.empty());

    // Create some dirs
    ASSERT_EQ(fs.mkdir("alpha"), 0);
    ASSERT_EQ(fs.mkdir("beta"), 0);
    ASSERT_EQ(fs.mkdir("gamma"), 0);

    ls.clear();
    EXPECT_EQ(fs.readdir("", &ls), 0);
    EXPECT_EQ(ls.size(), 3u);
    std::set<std::string> dirs(ls.begin(), ls.end());
    EXPECT_TRUE(dirs.count("alpha"));
    EXPECT_TRUE(dirs.count("beta"));
    EXPECT_TRUE(dirs.count("gamma"));

    // List files in a specific dir (empty since no files created)
    ls.clear();
    EXPECT_EQ(fs.readdir("alpha", &ls), 0);
    EXPECT_TRUE(ls.empty());

    // Non-existent dir returns -ENOENT
    ls.clear();
    EXPECT_EQ(fs.readdir("nonexistent", &ls), -ENOENT);

    // readdir with trailing slash
    ls.clear();
    EXPECT_EQ(fs.readdir("alpha/", &ls), 0);
    EXPECT_TRUE(ls.empty());

    fs.umount();
}

// ---------------------------------------------------------------------------
// readdir — lists files inside a directory, not just subdirs
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, ReaddirWithFiles) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    // Create a file
    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "myfile.txt", &w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    // readdir should list the file
    std::vector<std::string> ls;
    EXPECT_EQ(fs.readdir("d", &ls), 0);
    EXPECT_EQ(ls.size(), 1u);
    EXPECT_EQ(ls[0], "myfile.txt");

    // readdir with trailing slash should also work
    ls.clear();
    EXPECT_EQ(fs.readdir("d/", &ls), 0);
    EXPECT_EQ(ls.size(), 1u);
    EXPECT_EQ(ls[0], "myfile.txt");

    fs.umount();
}

// ---------------------------------------------------------------------------
// Directory persistence — verify dirs survive a remount cycle
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, DirPersistence) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;

    // ---- mkfs + mount ----
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);

        // Create directories
        ASSERT_EQ(fs.mkdir("persist_dir"), 0);
        ASSERT_EQ(fs.mkdir("another_dir"), 0);

        // Sync metadata to disk
        ASSERT_EQ(fs.sync_metadata(), 0);

        // Verify they exist before umount
        EXPECT_TRUE(fs.dir_exists("persist_dir"));
        EXPECT_TRUE(fs.dir_exists("another_dir"));

        fs.umount();
    }

    // ---- remount and verify persistence ----
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);

        EXPECT_TRUE(fs.dir_exists("persist_dir"));
        EXPECT_TRUE(fs.dir_exists("another_dir"));

        fs.umount();
    }
}

// ---------------------------------------------------------------------------
// Full persistence cycle: mkdir → sync → umount → mount → verify
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, DirPersistenceCycle) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;

    // ---- mkfs + mount ----
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);

        ASSERT_EQ(fs.mkdir("surviving_dir"), 0);
        ASSERT_EQ(fs.mkdir("another"), 0);

        // Flush and persist
        ASSERT_EQ(fs.sync_metadata(), 0);

        // Remove one dir before umount
        ASSERT_EQ(fs.rmdir("another"), 0);
        ASSERT_EQ(fs.sync_metadata(), 0);

        fs.umount();
    }

    // ---- mount again, verify state ----
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);

        // surviving_dir should exist
        EXPECT_TRUE(fs.dir_exists("surviving_dir"));

        // another was removed before umount, should not exist
        EXPECT_FALSE(fs.dir_exists("another"));

        // List dirs
        std::vector<std::string> ls;
        EXPECT_EQ(fs.readdir("", &ls), 0);
        EXPECT_EQ(ls.size(), 1u);
        EXPECT_EQ(ls[0], "surviving_dir");

        fs.umount();
    }
}

// ---------------------------------------------------------------------------
// Phase 1.6: File create/open/close
// ---------------------------------------------------------------------------

TEST_F(BlueFSTest, FileCreateAndStat) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);

    ASSERT_EQ(fs.mkdir("mydir"), 0);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("mydir", "testfile.txt", &w), 0);
    ASSERT_NE(w, nullptr);

    uint64_t size = 0;
    uint64_t mtime = 0;
    EXPECT_EQ(fs.stat("mydir", "testfile.txt", &size, &mtime), 0);
    EXPECT_EQ(size, 0ULL);
    EXPECT_EQ(mtime, 0ULL);

    ASSERT_EQ(fs.close_writer(w), 0);

    EXPECT_EQ(fs.stat("mydir", "testfile.txt", &size), 0);
    EXPECT_EQ(size, 0ULL);

    fs.umount();
}

TEST_F(BlueFSTest, FileOpenForReadNotFound) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("mydir"), 0);

    BlueFS::FileReader *r = nullptr;
    EXPECT_EQ(fs.open_for_read("mydir", "nonexistent", &r), -ENOENT);
    EXPECT_EQ(r, nullptr);

    EXPECT_EQ(fs.open_for_read("nonexistent", "file", &r), -ENOENT);

    fs.umount();
}

TEST_F(BlueFSTest, FileOpenForWriteNonExistentDir) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);

    BlueFS::FileWriter *w = nullptr;
    EXPECT_EQ(fs.open_for_write("nonexistent", "file", &w), -ENOENT);
    EXPECT_EQ(w, nullptr);

    fs.umount();
}

TEST_F(BlueFSTest, FileOpenForWriteOverwrite) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("mydir"), 0);

    // Create file via open_for_write
    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("mydir", "f", &w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    // overwrite=true on non-existent should fail
    w = nullptr;
    EXPECT_EQ(fs.open_for_write("mydir", "nonexistent", &w, true), -ENOENT);

    // overwrite=false on existing should succeed (truncate)
    w = nullptr;
    EXPECT_EQ(fs.open_for_write("mydir", "f", &w, false), 0);
    ASSERT_NE(w, nullptr);
    ASSERT_EQ(fs.close_writer(w), 0);

    fs.umount();
}

// ---------------------------------------------------------------------------
// Phase 1.7: File write/read
// ---------------------------------------------------------------------------

TEST_F(BlueFSTest, WriteAndReadBack) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("mydir"), 0);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("mydir", "data.bin", &w), 0);
    ASSERT_NE(w, nullptr);

    const char *data = "Hello BlueFS! This is a test write.";
    size_t data_len = strlen(data);
    ASSERT_EQ(fs.append_try_flush(w, data, data_len), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    BlueFS::FileReader *r = nullptr;
    ASSERT_EQ(fs.open_for_read("mydir", "data.bin", &r), 0);
    ASSERT_NE(r, nullptr);

    // Read from offset 0 — full content
    bufferlist bl;
    int64_t rlen = fs.read(r, 0, data_len, &bl);
    EXPECT_EQ(rlen, (int64_t)data_len);
    std::string read_back = bl.to_str();
    EXPECT_EQ(read_back, std::string(data, data_len));

    fs.close_reader(r);
    fs.umount();
}

TEST_F(BlueFSTest, ReadAtNonZeroOffset) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    const char *data = "0123456789ABCDEFGHIJ";
    size_t data_len = strlen(data);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, data, data_len), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    BlueFS::FileReader *r = nullptr;
    ASSERT_EQ(fs.open_for_read("d", "f", &r), 0);

    // Read substring from offset 5, length 7 → should get "56789AB"
    bufferlist bl;
    int64_t rlen = fs.read(r, 5, 7, &bl);
    EXPECT_EQ(rlen, 7);
    EXPECT_EQ(bl.to_str(), "56789AB");

    // Read from offset 15 to end
    bl.clear();
    rlen = fs.read(r, 15, 10, &bl);
    EXPECT_EQ(rlen, 5);
    EXPECT_EQ(bl.to_str(), "FGHIJ");

    fs.close_reader(r);
    fs.umount();
}

TEST_F(BlueFSTest, WriteFsyncRemountVerify) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    const char *data = "Persistent data after remount!";
    size_t data_len = strlen(data);

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("data"), 0);

        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("data", "persist.bin", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, data, data_len), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);

        uint64_t size = 0;
        EXPECT_EQ(fs.stat("data", "persist.bin", &size), 0);
        EXPECT_EQ(size, data_len);

        fs.umount();
    }

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);

        uint64_t size = 0;
        EXPECT_EQ(fs.stat("data", "persist.bin", &size), 0);
        EXPECT_EQ(size, data_len);

        BlueFS::FileReader *r = nullptr;
        ASSERT_EQ(fs.open_for_read("data", "persist.bin", &r), 0);
        bufferlist bl;
        int64_t rlen = fs.read(r, 0, data_len, &bl);
        EXPECT_EQ(rlen, (int64_t)data_len);
        std::string read_back = bl.to_str();
        EXPECT_EQ(read_back, std::string(data, data_len));

        fs.close_reader(r);
        fs.umount();
    }
}

TEST_F(BlueFSTest, RandomRead) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "file.bin", &w), 0);
    char buf[256];
    for (size_t i = 0; i < sizeof(buf); ++i) buf[i] = (char)i;
    ASSERT_EQ(fs.append_try_flush(w, buf, sizeof(buf)), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    BlueFS::FileReader *r = nullptr;
    ASSERT_EQ(fs.open_for_read("d", "file.bin", &r, true), 0);

    char out[16];
    int64_t rlen = fs.read_random(r, 100, 10, out);
    EXPECT_EQ(rlen, 10);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ((unsigned char)out[i], (unsigned char)(100 + i));
    }

    rlen = fs.read_random(r, 0, 5, out);
    EXPECT_EQ(rlen, 5);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ((unsigned char)out[i], (unsigned char)i);
    }

    rlen = fs.read_random(r, 250, 10, out);
    EXPECT_EQ(rlen, 6);

    fs.close_reader(r);
    fs.umount();
}

TEST_F(BlueFSTest, RenameFile) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("src"), 0);
    ASSERT_EQ(fs.mkdir("dst"), 0);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("src", "f.txt", &w), 0);
    const char *text = "rename test";
    ASSERT_EQ(fs.append_try_flush(w, text, strlen(text)), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    ASSERT_EQ(fs.rename("src", "f.txt", "dst", "f.txt"), 0);

    uint64_t size;
    EXPECT_EQ(fs.stat("src", "f.txt", &size), -ENOENT);
    EXPECT_EQ(fs.stat("dst", "f.txt", &size), 0);
    EXPECT_EQ(size, strlen(text));

    EXPECT_EQ(fs.rename("src", "nonexistent", "dst", "x"), -ENOENT);
    EXPECT_EQ(fs.rename("nonexistent", "f", "dst", "x"), -ENOENT);

    fs.umount();
}

TEST_F(BlueFSTest, MultipleWritesAndReads) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    for (int i = 0; i < 3; ++i) {
        BlueFS::FileWriter *w = nullptr;
        std::string fname = "f" + std::to_string(i);
        ASSERT_EQ(fs.open_for_write("d", fname, &w), 0);
        std::string content = "content_" + std::to_string(i);
        ASSERT_EQ(fs.append_try_flush(w, content.data(), content.size()), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);
    }

    for (int i = 0; i < 3; ++i) {
        std::string fname = "f" + std::to_string(i);
        std::string expected = "content_" + std::to_string(i);

        BlueFS::FileReader *r = nullptr;
        ASSERT_EQ(fs.open_for_read("d", fname, &r), 0);
        bufferlist bl;
        int64_t rlen = fs.read(r, 0, expected.size(), &bl);
        EXPECT_EQ(rlen, (int64_t)expected.size());
        std::string actual = bl.to_str();
        EXPECT_EQ(actual, expected);
        fs.close_reader(r);
    }

    fs.umount();
}

TEST_F(BlueFSTest, OpenForWriteTruncateExisting) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "long content here", 17), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "short", 5), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    BlueFS::FileReader *r = nullptr;
    ASSERT_EQ(fs.open_for_read("d", "f", &r), 0);
    bufferlist bl;
    int64_t rlen = fs.read(r, 0, 10, &bl);
    EXPECT_EQ(rlen, 5);
    std::string actual = bl.to_str();
    EXPECT_EQ(actual, "short");
    fs.close_reader(r);

    fs.umount();
}

// ---------------------------------------------------------------------------
// Phase 1.9: Space allocation — device fallback + shared_alloc tracking
// ---------------------------------------------------------------------------

TEST_F(BlueFSTest, MultiDeviceFallback) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;
    cfg.min_flush_size = 256;  // flush eagerly

    // Create a separate temp file for WAL device (64KB)
    auto wal_tmpl = cxxlab_tmp_path("bluefs_wal");
    int wal_fd = ::mkstemp(wal_tmpl.data());
    ASSERT_GE(wal_fd, 0);
    ::fallocate(wal_fd, 0, 0, 64 << 10);
    std::vector<char> zeros(4096, 0);
    for (uint64_t off = 0; off < (64 << 10); off += zeros.size()) {
        ::pwrite(wal_fd, zeros.data(),
                 std::min<uint64_t>(zeros.size(), (64 << 10) - off), off);
    }

    // mkfs: log file (ino 1) goes to WAL (preferred for log).
    // Data files in dir "d" prefer BDEV_DB via OriginalVolumeSelector.
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(
            fs.add_block_device(BlueFS::BDEV_WAL, wal_tmpl));
        ASSERT_NO_FATAL_FAILURE(
            fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);

        // Create enough small files to exhaust WAL (each allocates 4096).
        // After a few allocations, WAL is full and allocations fall back to
        // DB. Write data to each and verify all readable.
        for (int i = 0; i < 50; ++i) {
            BlueFS::FileWriter *w = nullptr;
            std::string fname = "f" + std::to_string(i);
            ASSERT_EQ(fs.open_for_write("d", fname, &w), 0);
            std::string content = "file_" + std::to_string(i);
            ASSERT_EQ(fs.append_try_flush(w, content.data(), content.size()), 0);
            ASSERT_EQ(fs.fsync(w), 0);
            ASSERT_EQ(fs.close_writer(w), 0);
        }

        // Verify all files
        for (int i = 0; i < 50; ++i) {
            std::string fname = "f" + std::to_string(i);
            std::string expected = "file_" + std::to_string(i);

            uint64_t size = 0;
            ASSERT_EQ(fs.stat("d", fname, &size), 0);
            EXPECT_EQ(size, expected.size());

            BlueFS::FileReader *r = nullptr;
            ASSERT_EQ(fs.open_for_read("d", fname, &r), 0);
            bufferlist bl;
            int64_t rlen = fs.read(r, 0, expected.size(), &bl);
            EXPECT_EQ(rlen, (int64_t)expected.size());
            EXPECT_EQ(bl.to_str(), expected);
            fs.close_reader(r);
        }

        fs.umount();
    }

    ::close(wal_fd);
    ::unlink(wal_tmpl.c_str());
}

TEST_F(BlueFSTest, SharedAllocBlueFSUsed) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;
    cfg.min_flush_size = 256;

    // Create a shared allocator (simulates BlueStore giving BlueFS a slice)
    auto shared_alloc = std::unique_ptr<Allocator>(
        Allocator::create("bitmap", kFileSize, cfg.alloc_size));
    shared_alloc->init_add_free(BlueFS::SUPER_OFFSET + BlueFS::SUPER_LENGTH,
                                kFileSize - BlueFS::SUPER_OFFSET -
                                    BlueFS::SUPER_LENGTH);

    bluefs_shared_alloc_context_t ctx;
    ctx.set(shared_alloc.get(), cfg.alloc_size);

    // Initial bluefs_used should be 0
    EXPECT_EQ(ctx.bluefs_used.load(), 0ULL);

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_,
                                                    false, 0, &ctx));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);

        // mkfs allocates 4MB for the log file via _allocate on the shared
        // device. After mkfs, bluefs_used should reflect the log allocation.
        uint64_t used_after_mkfs = ctx.bluefs_used.load();
        EXPECT_GT(used_after_mkfs, 0ULL);

        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);

        // Write a file — this should increment bluefs_used further
        uint64_t used_before_write = ctx.bluefs_used.load();
        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, "track me", 8), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);

        uint64_t used_after_write = ctx.bluefs_used.load();
        EXPECT_GT(used_after_write, used_before_write);

        fs.umount();
    }
}

// ---------------------------------------------------------------------------
// Phase 1.8: Journal persistence — comprehensive tests
// ---------------------------------------------------------------------------

// An empty file (created but never written to) must survive remount
TEST_F(BlueFSTest, EmptyFilePersistence) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);

        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "empty", &w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);

        uint64_t size = 12345;
        ASSERT_EQ(fs.stat("d", "empty", &size), 0);
        EXPECT_EQ(size, 0ULL);

        ASSERT_EQ(fs.sync_metadata(), 0);
        fs.umount();
    }

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);

        EXPECT_TRUE(fs.dir_exists("d"));
        uint64_t size = 12345;
        ASSERT_EQ(fs.stat("d", "empty", &size), 0);
        EXPECT_EQ(size, 0ULL);

        std::vector<std::string> ls;
        ASSERT_EQ(fs.readdir("d", &ls), 0);
        EXPECT_EQ(ls.size(), 1u);
        EXPECT_EQ(ls[0], "empty");

        fs.umount();
    }
}

// Multiple files with distinct content must all survive remount
TEST_F(BlueFSTest, MultiFilePersistence) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    struct FileSpec {
        const char *name;
        const char *content;
    };
    FileSpec files[] = {
        {"alpha", "Alpha content here"},
        {"beta", "Beta data for persistence test"},
        {"gamma", "Gamma -- the third file"},
    };

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);

        for (auto &f : files) {
            BlueFS::FileWriter *w = nullptr;
            ASSERT_EQ(fs.open_for_write("d", f.name, &w), 0);
            ASSERT_EQ(fs.append_try_flush(w, f.content, strlen(f.content)), 0);
            ASSERT_EQ(fs.fsync(w), 0);
            ASSERT_EQ(fs.close_writer(w), 0);

            uint64_t size = 0;
            ASSERT_EQ(fs.stat("d", f.name, &size), 0);
            EXPECT_EQ(size, strlen(f.content));
        }

        fs.umount();
    }

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);

        for (auto &f : files) {
            uint64_t size = 0;
            ASSERT_EQ(fs.stat("d", f.name, &size), 0);
            EXPECT_EQ(size, strlen(f.content));

            BlueFS::FileReader *r = nullptr;
            ASSERT_EQ(fs.open_for_read("d", f.name, &r), 0);
            bufferlist bl;
            int64_t rlen = fs.read(r, 0, strlen(f.content), &bl);
            EXPECT_EQ(rlen, (int64_t)strlen(f.content));
            EXPECT_EQ(bl.to_str(), std::string(f.content));
            fs.close_reader(r);
        }

        fs.umount();
    }
}

// Truncate + rewrite with shorter content survives remount
TEST_F(BlueFSTest, TruncateAndRewritePersistence) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);

        // Write long content
        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, "This is a long content that will be truncated", 46), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);

        fs.umount();
    }

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);

        // Truncate and write shorter
        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, "Short!", 6), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);

        uint64_t size = 0;
        ASSERT_EQ(fs.stat("d", "f", &size), 0);
        EXPECT_EQ(size, 6ULL);

        fs.umount();
    }

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);

        uint64_t size = 0;
        ASSERT_EQ(fs.stat("d", "f", &size), 0);
        EXPECT_EQ(size, 6ULL);

        // Content should be the shorter version
        BlueFS::FileReader *r = nullptr;
        ASSERT_EQ(fs.open_for_read("d", "f", &r), 0);
        bufferlist bl;
        int64_t rlen = fs.read(r, 0, 10, &bl);
        EXPECT_EQ(rlen, 6);
        EXPECT_EQ(bl.to_str(), "Short!");
        fs.close_reader(r);

        fs.umount();
    }
}

// Multiple appends with fsync in between, then remount and verify
TEST_F(BlueFSTest, AppendAcrossFsyncPersistence) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);

        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);

        // Batch 1: "Hello "
        ASSERT_EQ(fs.append_try_flush(w, "Hello ", 6), 0);
        ASSERT_EQ(fs.fsync(w), 0);

        // Batch 2: "World!"
        ASSERT_EQ(fs.append_try_flush(w, "World!", 6), 0);
        ASSERT_EQ(fs.fsync(w), 0);

        ASSERT_EQ(fs.close_writer(w), 0);

        uint64_t size = 0;
        ASSERT_EQ(fs.stat("d", "f", &size), 0);
        EXPECT_EQ(size, 12ULL);

        fs.umount();
    }

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);

        uint64_t size = 0;
        ASSERT_EQ(fs.stat("d", "f", &size), 0);
        EXPECT_EQ(size, 12ULL);

        BlueFS::FileReader *r = nullptr;
        ASSERT_EQ(fs.open_for_read("d", "f", &r), 0);
        bufferlist bl;
        int64_t rlen = fs.read(r, 0, 12, &bl);
        EXPECT_EQ(rlen, 12);
        EXPECT_EQ(bl.to_str(), "Hello World!");
        fs.close_reader(r);

        fs.umount();
    }
}

// Data spanning multiple extents survives remount
TEST_F(BlueFSTest, MultiExtentPersistence) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    // Write enough data to span multiple allocator units (alloc_size=4096)
    // Use 3x to ensure multiple extents even with extent merging
    std::string data;
    data.reserve(3 * 4096);
    for (int i = 0; i < 3 * 4096; ++i) {
        data.push_back((char)(i & 0xff));
    }

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);

        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "big", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, data.data(), data.size()), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);

        uint64_t size = 0;
        ASSERT_EQ(fs.stat("d", "big", &size), 0);
        EXPECT_EQ(size, data.size());

        fs.umount();
    }

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);

        uint64_t size = 0;
        ASSERT_EQ(fs.stat("d", "big", &size), 0);
        EXPECT_EQ(size, data.size());

        BlueFS::FileReader *r = nullptr;
        ASSERT_EQ(fs.open_for_read("d", "big", &r), 0);

        // Read in full and verify
        bufferlist bl;
        int64_t rlen = fs.read(r, 0, data.size(), &bl);
        EXPECT_EQ(rlen, (int64_t)data.size());
        EXPECT_EQ(bl.to_str(), data);

        // Also verify at a non-zero offset
        bl.clear();
        rlen = fs.read(r, 5000, 1000, &bl);
        EXPECT_EQ(rlen, 1000);
        EXPECT_EQ(bl.to_str(), data.substr(5000, 1000));

        fs.close_reader(r);
        fs.umount();
    }
}

// Rename operation persists across remount
TEST_F(BlueFSTest, RenamePersistence) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    const char *content = "rename persist content";
    size_t content_len = strlen(content);

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("src"), 0);
        ASSERT_EQ(fs.mkdir("dst"), 0);

        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("src", "f", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, content, content_len), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);

        ASSERT_EQ(fs.rename("src", "f", "dst", "g"), 0);
        ASSERT_EQ(fs.sync_metadata(), 0);

        fs.umount();
    }

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);

        // Old name should not exist
        uint64_t size = 0;
        EXPECT_EQ(fs.stat("src", "f", &size), -ENOENT);

        // New name should exist with correct content
        ASSERT_EQ(fs.stat("dst", "g", &size), 0);
        EXPECT_EQ(size, content_len);

        BlueFS::FileReader *r = nullptr;
        ASSERT_EQ(fs.open_for_read("dst", "g", &r), 0);
        bufferlist bl;
        int64_t rlen = fs.read(r, 0, content_len, &bl);
        EXPECT_EQ(rlen, (int64_t)content_len);
        EXPECT_EQ(bl.to_str(), std::string(content, content_len));
        fs.close_reader(r);

        fs.umount();
    }
}

// Multi-cycle: write+remount+append+remount verifies the complete lifecycle
TEST_F(BlueFSTest, MultiCycleAppendRemount) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    // ---- Cycle 1: create file and write batch A ----
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);

        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, "BatchA", 6), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);

        fs.umount();
    }

    // ---- Cycle 2: remount, verify batch A, append batch B ----
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);

        // Verify batch A
        uint64_t size = 0;
        ASSERT_EQ(fs.stat("d", "f", &size), 0);
        EXPECT_EQ(size, 6ULL);

        BlueFS::FileReader *r = nullptr;
        ASSERT_EQ(fs.open_for_read("d", "f", &r), 0);
        bufferlist bl;
        int64_t rlen = fs.read(r, 0, 6, &bl);
        EXPECT_EQ(rlen, 6);
        EXPECT_EQ(bl.to_str(), "BatchA");
        fs.close_reader(r);

        // Truncate and write A+B as one contiguous block (current API
        // opens with truncate by default — overwrite=false truncates)
        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, "BatchABatchB", 12), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);

        ASSERT_EQ(fs.stat("d", "f", &size), 0);
        EXPECT_EQ(size, 12ULL);

        fs.umount();
    }

    // ---- Cycle 3: remount, verify A+B ----
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);

        uint64_t size = 0;
        ASSERT_EQ(fs.stat("d", "f", &size), 0);
        EXPECT_EQ(size, 12ULL);

        BlueFS::FileReader *r = nullptr;
        ASSERT_EQ(fs.open_for_read("d", "f", &r), 0);
        bufferlist bl;
        int64_t rlen = fs.read(r, 0, 12, &bl);
        EXPECT_EQ(rlen, 12);
        EXPECT_EQ(bl.to_str(), "BatchABatchB");
        fs.close_reader(r);

        fs.umount();
    }
}

// ---------------------------------------------------------------------------
// Phase 1.10: Log compaction
// ---------------------------------------------------------------------------

static BlueFSConfig compact_cfg() {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;
    cfg.min_flush_size = 256;
    cfg.log_compact_min_size = 0;
    cfg.log_compact_min_ratio = 1.0;
    return cfg;
}

TEST_F(BlueFSTest, CompactEmpty) {
    auto cfg = compact_cfg();
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);
        ASSERT_EQ(fs.compact_log(), 0);
        EXPECT_TRUE(fs.dir_exists("d"));
        fs.umount();
    }
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);
        EXPECT_TRUE(fs.dir_exists("d"));
        fs.umount();
    }
}

TEST_F(BlueFSTest, CompactSingleFile) {
    auto cfg = compact_cfg();
    const char *data = "compact me!";
    size_t data_len = strlen(data);
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);
        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, data, data_len), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);
        ASSERT_EQ(fs.compact_log(), 0);
        uint64_t size = 0;
        ASSERT_EQ(fs.stat("d", "f", &size), 0);
        EXPECT_EQ(size, data_len);
        BlueFS::FileReader *r = nullptr;
        ASSERT_EQ(fs.open_for_read("d", "f", &r), 0);
        bufferlist bl;
        int64_t rlen = fs.read(r, 0, data_len, &bl);
        EXPECT_EQ(rlen, (int64_t)data_len);
        EXPECT_EQ(bl.to_str(), std::string(data, data_len));
        fs.close_reader(r);
        fs.umount();
    }
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);
        uint64_t size = 0;
        ASSERT_EQ(fs.stat("d", "f", &size), 0);
        EXPECT_EQ(size, data_len);
        BlueFS::FileReader *r = nullptr;
        ASSERT_EQ(fs.open_for_read("d", "f", &r), 0);
        bufferlist bl;
        int64_t rlen = fs.read(r, 0, data_len, &bl);
        EXPECT_EQ(rlen, (int64_t)data_len);
        EXPECT_EQ(bl.to_str(), std::string(data, data_len));
        fs.close_reader(r);
        fs.umount();
    }
}

TEST_F(BlueFSTest, CompactMultiExtentFiles) {
    auto cfg = compact_cfg();
    std::string big_data;
    big_data.reserve(3 * 4096);
    for (int i = 0; i < 3 * 4096; ++i)
        big_data.push_back((char)(i & 0xff));
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);
        for (int i = 0; i < 2; ++i) {
            BlueFS::FileWriter *w = nullptr;
            ASSERT_EQ(fs.open_for_write("d", "f" + std::to_string(i), &w), 0);
            ASSERT_EQ(fs.append_try_flush(w, big_data.data(), big_data.size()), 0);
            ASSERT_EQ(fs.fsync(w), 0);
            ASSERT_EQ(fs.close_writer(w), 0);
        }
        ASSERT_EQ(fs.compact_log(), 0);
        for (int i = 0; i < 2; ++i) {
            uint64_t size = 0;
            ASSERT_EQ(fs.stat("d", "f" + std::to_string(i), &size), 0);
            EXPECT_EQ(size, big_data.size());
            BlueFS::FileReader *r = nullptr;
            ASSERT_EQ(fs.open_for_read("d", "f" + std::to_string(i), &r), 0);
            bufferlist bl;
            int64_t rlen = fs.read(r, 5000, 1000, &bl);
            EXPECT_EQ(rlen, 1000);
            EXPECT_EQ(bl.to_str(), big_data.substr(5000, 1000));
            fs.close_reader(r);
        }
        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f3", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, "post-compact", 12), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);
        fs.umount();
    }
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);
        for (int i = 0; i < 2; ++i) {
            uint64_t size = 0;
            ASSERT_EQ(fs.stat("d", "f" + std::to_string(i), &size), 0);
            EXPECT_EQ(size, big_data.size());
        }
        uint64_t size = 0;
        ASSERT_EQ(fs.stat("d", "f3", &size), 0);
        EXPECT_EQ(size, 12ULL);
        BlueFS::FileReader *r = nullptr;
        ASSERT_EQ(fs.open_for_read("d", "f3", &r), 0);
        bufferlist bl;
        int64_t rlen = fs.read(r, 0, 12, &bl);
        EXPECT_EQ(rlen, 12);
        EXPECT_EQ(bl.to_str(), "post-compact");
        fs.close_reader(r);
        fs.umount();
    }
}

TEST_F(BlueFSTest, CompactViaSyncMetadata) {
    auto cfg = compact_cfg();
    const char *data = "sync compact data";
    size_t data_len = strlen(data);
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);
        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, data, data_len), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);
        ASSERT_EQ(fs.sync_metadata(), 0);
        fs.umount();
    }
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);
        uint64_t size = 0;
        ASSERT_EQ(fs.stat("d", "f", &size), 0);
        EXPECT_EQ(size, data_len);
        BlueFS::FileReader *r = nullptr;
        ASSERT_EQ(fs.open_for_read("d", "f", &r), 0);
        bufferlist bl;
        int64_t rlen = fs.read(r, 0, data_len, &bl);
        EXPECT_EQ(rlen, (int64_t)data_len);
        EXPECT_EQ(bl.to_str(), std::string(data, data_len));
        fs.close_reader(r);
        fs.umount();
    }
}

TEST_F(BlueFSTest, CompactMultipleTimes) {
    auto cfg = compact_cfg();
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);
        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f1", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, "file1", 5), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);
        ASSERT_EQ(fs.compact_log(), 0);
        w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f2", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, "file2", 5), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);
        ASSERT_EQ(fs.compact_log(), 0);
        w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f3", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, "file3", 5), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);
        ASSERT_EQ(fs.compact_log(), 0);
        for (int i = 1; i <= 3; ++i) {
            uint64_t size = 0;
            ASSERT_EQ(fs.stat("d", "f" + std::to_string(i), &size), 0);
            EXPECT_EQ(size, 5ULL);
            BlueFS::FileReader *r = nullptr;
            ASSERT_EQ(fs.open_for_read("d", "f" + std::to_string(i), &r), 0);
            bufferlist bl;
            int64_t rlen = fs.read(r, 0, 5, &bl);
            EXPECT_EQ(rlen, 5);
            EXPECT_EQ(bl.to_str(), "file" + std::to_string(i));
            fs.close_reader(r);
        }
        fs.umount();
    }
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);
        for (int i = 1; i <= 3; ++i) {
            uint64_t size = 0;
            ASSERT_EQ(fs.stat("d", "f" + std::to_string(i), &size), 0);
            EXPECT_EQ(size, 5ULL);
            BlueFS::FileReader *r = nullptr;
            ASSERT_EQ(fs.open_for_read("d", "f" + std::to_string(i), &r), 0);
            bufferlist bl;
            int64_t rlen = fs.read(r, 0, 5, &bl);
            EXPECT_EQ(rlen, 5);
            EXPECT_EQ(bl.to_str(), "file" + std::to_string(i));
            fs.close_reader(r);
        }
        fs.umount();
    }
}

TEST_F(BlueFSTest, CompactPreservesDirsAndFiles) {
    auto cfg = compact_cfg();
    struct Entry {
        const char *dir;
        const char *file;
        const char *content;
    };
    Entry entries[] = {
        {"dir1", "a", "content-a"},
        {"dir1", "b", "content-b"},
        {"dir2", "c", "content-c"},
        {"dir2", "d", "content-d"},
        {"dir2", "e", "content-e"},
    };
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        std::set<std::string> created_dirs;
        for (auto &e : entries) {
            if (!created_dirs.count(e.dir)) {
                ASSERT_EQ(fs.mkdir(e.dir), 0);
                created_dirs.insert(e.dir);
            }
            BlueFS::FileWriter *w = nullptr;
            ASSERT_EQ(fs.open_for_write(e.dir, e.file, &w), 0);
            ASSERT_EQ(fs.append_try_flush(w, e.content, strlen(e.content)), 0);
            ASSERT_EQ(fs.fsync(w), 0);
            ASSERT_EQ(fs.close_writer(w), 0);
        }
        ASSERT_EQ(fs.compact_log(), 0);
        EXPECT_TRUE(fs.dir_exists("dir1"));
        EXPECT_TRUE(fs.dir_exists("dir2"));
        for (auto &e : entries) {
            uint64_t size = 0;
            ASSERT_EQ(fs.stat(e.dir, e.file, &size), 0);
            EXPECT_EQ(size, strlen(e.content));
            BlueFS::FileReader *r = nullptr;
            ASSERT_EQ(fs.open_for_read(e.dir, e.file, &r), 0);
            bufferlist bl;
            int64_t rlen = fs.read(r, 0, strlen(e.content), &bl);
            EXPECT_EQ(rlen, (int64_t)strlen(e.content));
            EXPECT_EQ(bl.to_str(), std::string(e.content));
            fs.close_reader(r);
        }
        fs.umount();
    }
    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);
        EXPECT_TRUE(fs.dir_exists("dir1"));
        EXPECT_TRUE(fs.dir_exists("dir2"));
        for (auto &e : entries) {
            uint64_t size = 0;
            ASSERT_EQ(fs.stat(e.dir, e.file, &size), 0);
            EXPECT_EQ(size, strlen(e.content));
            BlueFS::FileReader *r = nullptr;
            ASSERT_EQ(fs.open_for_read(e.dir, e.file, &r), 0);
            bufferlist bl;
            int64_t rlen = fs.read(r, 0, strlen(e.content), &bl);
            EXPECT_EQ(rlen, (int64_t)strlen(e.content));
            EXPECT_EQ(bl.to_str(), std::string(e.content));
            fs.close_reader(r);
        }
        fs.umount();
    }
}

TEST_F(BlueFSTest, CompactLogSizeReasonable) {
    auto cfg = compact_cfg();
    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);
    for (int i = 0; i < 100; ++i) {
        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f" + std::to_string(i), &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, "dataX", 5), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);
    }

    ASSERT_EQ(fs.compact_log(), 0);

    // After compaction the log_fnode is written to superblock.
    // The compacted metadata for 100 files + 1 dir should fit in
    // well under 64KB.
    EXPECT_GT(fs.get_super().log_fnode.size, 0ULL);
    EXPECT_LT(fs.get_super().log_fnode.size, 65536ULL);

    fs.umount();
}

TEST_F(BlueFSTest, CompactIdempotent) {
    auto cfg = compact_cfg();
    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);
    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "data", 4), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);
    ASSERT_EQ(fs.compact_log(), 0);
    ASSERT_EQ(fs.compact_log(), 0);
    ASSERT_EQ(fs.compact_log(), 0);
    uint64_t size = 0;
    ASSERT_EQ(fs.stat("d", "f", &size), 0);
    EXPECT_EQ(size, 4ULL);
    fs.umount();
}

// ---------------------------------------------------------------------------
// Phase 1.11: unlink
// ---------------------------------------------------------------------------

TEST_F(BlueFSTest, UnlinkBasic) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "data", 4), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    ASSERT_EQ(fs.unlink("d", "f"), 0);

    uint64_t size = 0;
    EXPECT_EQ(fs.stat("d", "f", &size), -ENOENT);

    std::vector<std::string> ls;
    ASSERT_EQ(fs.readdir("d", &ls), 0);
    EXPECT_TRUE(ls.empty());

    fs.umount();
}

TEST_F(BlueFSTest, UnlinkNonExistent) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    EXPECT_EQ(fs.unlink("d", "nonexistent"), -ENOENT);
    EXPECT_EQ(fs.unlink("nonexistent", "f"), -ENOENT);

    fs.umount();
}

TEST_F(BlueFSTest, UnlinkPreservesOtherFiles) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f1", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "keep", 4), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f2", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "remove", 6), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    ASSERT_EQ(fs.unlink("d", "f2"), 0);

    uint64_t size = 0;
    EXPECT_EQ(fs.stat("d", "f1", &size), 0);
    EXPECT_EQ(size, 4ULL);
    EXPECT_EQ(fs.stat("d", "f2", &size), -ENOENT);

    fs.umount();
}

TEST_F(BlueFSTest, UnlinkPersistence) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);

        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, "data", 4), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);

        ASSERT_EQ(fs.unlink("d", "f"), 0);
        ASSERT_EQ(fs.sync_metadata(), 0);

        fs.umount();
    }

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);

        uint64_t size = 0;
        EXPECT_EQ(fs.stat("d", "f", &size), -ENOENT);

        std::vector<std::string> ls;
        ASSERT_EQ(fs.readdir("d", &ls), 0);
        EXPECT_TRUE(ls.empty());

        fs.umount();
    }
}

// ---------------------------------------------------------------------------
// Phase 1.11: truncate
// ---------------------------------------------------------------------------

TEST_F(BlueFSTest, TruncateToSmaller) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "long content here", 17), 0);
    ASSERT_EQ(fs.fsync(w), 0);

    ASSERT_EQ(fs.truncate(w, 4), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    uint64_t size = 99;
    ASSERT_EQ(fs.stat("d", "f", &size), 0);
    EXPECT_EQ(size, 4ULL);

    BlueFS::FileReader *r = nullptr;
    ASSERT_EQ(fs.open_for_read("d", "f", &r), 0);
    bufferlist bl;
    int64_t rlen = fs.read(r, 0, 10, &bl);
    EXPECT_EQ(rlen, 4);
    EXPECT_EQ(bl.to_str(), "long");
    fs.close_reader(r);

    fs.umount();
}

TEST_F(BlueFSTest, TruncateToZero) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "something", 9), 0);
    ASSERT_EQ(fs.fsync(w), 0);

    ASSERT_EQ(fs.truncate(w, 0), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    uint64_t size = 99;
    ASSERT_EQ(fs.stat("d", "f", &size), 0);
    EXPECT_EQ(size, 0ULL);

    BlueFS::FileReader *r = nullptr;
    ASSERT_EQ(fs.open_for_read("d", "f", &r), 0);
    bufferlist bl;
    int64_t rlen = fs.read(r, 0, 10, &bl);
    EXPECT_EQ(rlen, 0);
    fs.close_reader(r);

    fs.umount();
}

TEST_F(BlueFSTest, TruncateNoop) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "data", 4), 0);
    ASSERT_EQ(fs.fsync(w), 0);

    // Truncate to same size is a no-op
    ASSERT_EQ(fs.truncate(w, 4), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    uint64_t size = 0;
    ASSERT_EQ(fs.stat("d", "f", &size), 0);
    EXPECT_EQ(size, 4ULL);

    fs.umount();
}

TEST_F(BlueFSTest, TruncateGrowFails) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "short", 5), 0);
    ASSERT_EQ(fs.fsync(w), 0);

    EXPECT_LT(fs.truncate(w, 100), 0);

    ASSERT_EQ(fs.close_writer(w), 0);

    fs.umount();
}

TEST_F(BlueFSTest, TruncatePersistence) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    const char *long_data = "This is a somewhat longer piece of content";
    size_t long_len = strlen(long_data);

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);

        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, long_data, long_len), 0);
        ASSERT_EQ(fs.fsync(w), 0);

        ASSERT_EQ(fs.truncate(w, 7), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);

        fs.umount();
    }

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
        ASSERT_EQ(fs.mount(), 0);

        uint64_t size = 0;
        ASSERT_EQ(fs.stat("d", "f", &size), 0);
        EXPECT_EQ(size, 7ULL);

        BlueFS::FileReader *r = nullptr;
        ASSERT_EQ(fs.open_for_read("d", "f", &r), 0);
        bufferlist bl;
        int64_t rlen = fs.read(r, 0, long_len, &bl);
        EXPECT_EQ(rlen, 7);
        EXPECT_EQ(bl.to_str(), "This is");
        fs.close_reader(r);

        fs.umount();
    }
}

// ---------------------------------------------------------------------------
// Phase 1.11: O_DIRECT block-aligned writes
// ---------------------------------------------------------------------------

TEST_F(BlueFSTest, DirectWriteAligned) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = false;  // O_DIRECT

    // Use a separate temp file with data for direct IO
    auto direct_tmpl = cxxlab_tmp_path("bluefs_direct");
    int direct_fd = ::mkstemp(direct_tmpl.data());
    ASSERT_GE(direct_fd, 0);
    ::fallocate(direct_fd, 0, 0, kFileSize);
    std::vector<char> zeros(4096, 0);
    for (uint64_t off = 0; off < kFileSize; off += zeros.size()) {
        ::pwrite(direct_fd, zeros.data(),
                 std::min<uint64_t>(zeros.size(), kFileSize - off), off);
    }
    ::close(direct_fd);

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB,
                                                    direct_tmpl));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);

        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);

        // Write 4096 bytes (one full block, aligned)
        std::string block_data(4096, 'X');
        ASSERT_EQ(fs.append_try_flush(w, block_data.data(), block_data.size()),
                  0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);

        uint64_t size = 0;
        ASSERT_EQ(fs.stat("d", "f", &size), 0);
        EXPECT_EQ(size, 4096ULL);

        BlueFS::FileReader *r = nullptr;
        ASSERT_EQ(fs.open_for_read("d", "f", &r), 0);
        bufferlist bl;
        int64_t rlen = fs.read(r, 0, 4096, &bl);
        EXPECT_EQ(rlen, 4096);
        EXPECT_EQ(bl.to_str(), block_data);
        fs.close_reader(r);

        fs.umount();
    }

    ::unlink(direct_tmpl.c_str());
}

TEST_F(BlueFSTest, DirectWriteMultipleBlocks) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = false;

    auto direct_tmpl = cxxlab_tmp_path("bluefs_direct_multi");
    int direct_fd = ::mkstemp(direct_tmpl.data());
    ASSERT_GE(direct_fd, 0);
    ::fallocate(direct_fd, 0, 0, kFileSize);
    std::vector<char> zeros(4096, 0);
    for (uint64_t off = 0; off < kFileSize; off += zeros.size()) {
        ::pwrite(direct_fd, zeros.data(),
                 std::min<uint64_t>(zeros.size(), kFileSize - off), off);
    }
    ::close(direct_fd);

    {
        BlueFS fs(cfg);
        ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB,
                                                    direct_tmpl));
        ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
        ASSERT_EQ(fs.mount(), 0);
        ASSERT_EQ(fs.mkdir("d"), 0);

        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);

        // Write 8192 bytes (2 blocks)
        std::string block_data(8192, 'Y');
        ASSERT_EQ(fs.append_try_flush(w, block_data.data(), block_data.size()),
                  0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);

        uint64_t size = 0;
        ASSERT_EQ(fs.stat("d", "f", &size), 0);
        EXPECT_EQ(size, 8192ULL);

        BlueFS::FileReader *r = nullptr;
        ASSERT_EQ(fs.open_for_read("d", "f", &r), 0);
        bufferlist bl;
        int64_t rlen = fs.read(r, 4096, 4096, &bl);
        EXPECT_EQ(rlen, 4096);
        EXPECT_EQ(bl.to_str(), std::string(4096, 'Y'));
        fs.close_reader(r);

        fs.umount();
    }

    ::unlink(direct_tmpl.c_str());
}

// ---------------------------------------------------------------------------
// Phase 1.11: File operations edge cases
// ---------------------------------------------------------------------------

TEST_F(BlueFSTest, UnlinkWithOpenWriter) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "data", 4), 0);

    // Unlink while a writer is still open
    ASSERT_EQ(fs.unlink("d", "f"), 0);

    // The file should be gone from the directory
    uint64_t size = 0;
    EXPECT_EQ(fs.stat("d", "f", &size), -ENOENT);

    // But the writer handle still works (deleted flag protects against
    // new IO, but existing operations on the handle are safe)
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    fs.umount();
}

TEST_F(BlueFSTest, TruncateDeletedFile) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "data", 4), 0);

    ASSERT_EQ(fs.unlink("d", "f"), 0);

    // Truncate on deleted file should be a no-op
    EXPECT_EQ(fs.truncate(w, 0), 0);

    ASSERT_EQ(fs.close_writer(w), 0);

    fs.umount();
}

TEST_F(BlueFSTest, ReaddirAfterUnlink) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    for (int i = 0; i < 5; ++i) {
        BlueFS::FileWriter *w = nullptr;
        ASSERT_EQ(fs.open_for_write("d", "f" + std::to_string(i), &w), 0);
        ASSERT_EQ(fs.append_try_flush(w, "x", 1), 0);
        ASSERT_EQ(fs.fsync(w), 0);
        ASSERT_EQ(fs.close_writer(w), 0);
    }

    ASSERT_EQ(fs.unlink("d", "f2"), 0);
    ASSERT_EQ(fs.unlink("d", "f4"), 0);

    std::vector<std::string> ls;
    ASSERT_EQ(fs.readdir("d", &ls), 0);
    EXPECT_EQ(ls.size(), 3u);

    std::set<std::string> remaining(ls.begin(), ls.end());
    EXPECT_TRUE(remaining.count("f0"));
    EXPECT_TRUE(remaining.count("f1"));
    EXPECT_TRUE(remaining.count("f3"));

    fs.umount();
}

// ---------------------------------------------------------------------------
// lock_file / unlock_file lifecycle
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, LockFileLifecycle) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    BlueFS::FileLock *lock = nullptr;

    // Lock a non-existent file should create it
    ASSERT_EQ(fs.lock_file("d", "lockfile", &lock), 0);
    ASSERT_NE(lock, nullptr);

    // Re-lock on same file should fail with -ENOLCK
    BlueFS::FileLock *lock2 = nullptr;
    EXPECT_EQ(fs.lock_file("d", "lockfile", &lock2), -ENOLCK);
    EXPECT_EQ(lock2, nullptr);

    // File should be visible via stat
    uint64_t size = 12345;
    EXPECT_EQ(fs.stat("d", "lockfile", &size), 0);
    EXPECT_EQ(size, 0ULL);

    // Unlock should succeed
    EXPECT_EQ(fs.unlock_file(lock), 0);

    // After unlock, re-lock should succeed
    ASSERT_EQ(fs.lock_file("d", "lockfile", &lock2), 0);
    ASSERT_NE(lock2, nullptr);
    EXPECT_EQ(fs.unlock_file(lock2), 0);

    // Lock on non-existent dir should fail
    EXPECT_EQ(fs.lock_file("nonexistent", "f", &lock), -ENOENT);

    fs.umount();
}

TEST_F(BlueFSTest, LockFileNonExistentDir) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);

    BlueFS::FileLock *lock = nullptr;
    EXPECT_EQ(fs.lock_file("nonexistent", "f", &lock), -ENOENT);
    EXPECT_EQ(lock, nullptr);

    fs.umount();
}

// ---------------------------------------------------------------------------
// preallocate
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, PreallocateBasic) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    // Create a file and write a small amount
    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "data", 4), 0);
    ASSERT_EQ(fs.fsync(w), 0);

    // Preallocate beyond current end
    int r = fs.preallocate(w->file, 0, 8192);
    EXPECT_EQ(r, 0);

    // Write more data (should be within preallocated space)
    ASSERT_EQ(fs.append_try_flush(w, " more data", 10), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    // Verify content
    BlueFS::FileReader *r2 = nullptr;
    ASSERT_EQ(fs.open_for_read("d", "f", &r2), 0);
    bufferlist bl;
    int64_t rlen = fs.read(r2, 0, 20, &bl);
    EXPECT_EQ(rlen, 14);
    EXPECT_EQ(bl.to_str(), "data more data");
    fs.close_reader(r2);

    fs.umount();
}

TEST_F(BlueFSTest, PreallocateDeletedFile) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);

    // Unlink before preallocate
    ASSERT_EQ(fs.unlink("d", "f"), 0);

    // Preallocate on deleted file should be no-op (return 0)
    EXPECT_EQ(fs.preallocate(w->file, 0, 4096), 0);

    ASSERT_EQ(fs.close_writer(w), 0);
    fs.umount();
}

// ---------------------------------------------------------------------------
// get_used
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, GetUsedBasic) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));

    // Before mkfs, no allocator — get_used returns 0
    EXPECT_EQ(fs.get_used(), 0ULL);

    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);

    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    // After mount, space is in use (log file)
    EXPECT_GT(fs.get_used(), 0ULL);

    // Per-device query matches total
    EXPECT_EQ(fs.get_used(BlueFS::BDEV_DB), fs.get_used());

    // Non-existent device returns 0
    EXPECT_EQ(fs.get_used(99), 0ULL);

    // Write a file — total should be >= before (at least non-decreasing)
    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "data", 4), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    EXPECT_GE(fs.get_used(), fs.get_used(BlueFS::BDEV_DB));

    fs.umount();
}

// ---------------------------------------------------------------------------
// rename overwrite existing target
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, RenameOverwriteExisting) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    // Create file a
    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "a", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "file_a", 6), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    // Create file b
    w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "b", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, "file_b", 6), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    // Rename a -> b (b already exists)
    ASSERT_EQ(fs.rename("d", "a", "d", "b"), 0);

    // Only b should survive, with content of a
    uint64_t size = 0;
    EXPECT_EQ(fs.stat("d", "a", &size), -ENOENT);
    ASSERT_EQ(fs.stat("d", "b", &size), 0);
    EXPECT_EQ(size, 6ULL);

    BlueFS::FileReader *r = nullptr;
    ASSERT_EQ(fs.open_for_read("d", "b", &r), 0);
    bufferlist bl;
    EXPECT_EQ(fs.read(r, 0, 6, &bl), 6);
    EXPECT_EQ(bl.to_str(), "file_a");
    fs.close_reader(r);

    fs.umount();
}

// ---------------------------------------------------------------------------
// read with char* output
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, ReadToCharBuffer) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    const char *data = "Hello from char* read!";
    size_t data_len = strlen(data);

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "f", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, data, data_len), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    BlueFS::FileReader *r = nullptr;
    ASSERT_EQ(fs.open_for_read("d", "f", &r), 0);

    // Read into char buffer
    char buf[64] = {};
    int64_t rlen = fs.read(r, 0, data_len, nullptr, buf);
    EXPECT_EQ(rlen, (int64_t)data_len);
    EXPECT_EQ(std::string(buf, data_len), std::string(data, data_len));

    // Partial read at offset
    memset(buf, 0, sizeof(buf));
    rlen = fs.read(r, 6, 10, nullptr, buf);
    EXPECT_EQ(rlen, 10);
    EXPECT_EQ(std::string(buf, 10), "from char*");

    fs.close_reader(r);
    fs.umount();
}

// ---------------------------------------------------------------------------
// large file spanning many extents
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, LargeFileManyExtents) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    // Write 256KB — enough to force multiple extents (alloc_size=4096)
    std::string data;
    data.reserve(256 << 10);
    for (size_t i = 0; i < data.capacity(); ++i) {
        data.push_back((char)(i & 0xff));
    }

    BlueFS::FileWriter *w = nullptr;
    ASSERT_EQ(fs.open_for_write("d", "big", &w), 0);
    ASSERT_EQ(fs.append_try_flush(w, data.data(), data.size()), 0);
    ASSERT_EQ(fs.fsync(w), 0);
    ASSERT_EQ(fs.close_writer(w), 0);

    // Read back and verify
    BlueFS::FileReader *r = nullptr;
    ASSERT_EQ(fs.open_for_read("d", "big", &r), 0);

    bufferlist bl;
    int64_t rlen = fs.read(r, 0, data.size(), &bl);
    EXPECT_EQ(rlen, (int64_t)data.size());
    EXPECT_EQ(bl.to_str(), data);

    // Read at mid offset
    bl.clear();
    rlen = fs.read(r, 100000, 5000, &bl);
    EXPECT_EQ(rlen, 5000);
    EXPECT_EQ(bl.to_str(), data.substr(100000, 5000));

    fs.close_reader(r);
    fs.umount();
}

// ---------------------------------------------------------------------------
// Concurrent writes to different files (basic multithread safety)
// ---------------------------------------------------------------------------
TEST_F(BlueFSTest, ConcurrentWritesToDifferentFiles) {
    BlueFSConfig cfg;
    cfg.alloc_size = 4096;
    cfg.buffered_io = true;

    BlueFS fs(cfg);
    ASSERT_NO_FATAL_FAILURE(fs.add_block_device(BlueFS::BDEV_DB, tmp_path_));
    ASSERT_EQ(fs.mkfs(cfg.alloc_size), 0);
    ASSERT_EQ(fs.mount(), 0);
    ASSERT_EQ(fs.mkdir("d"), 0);

    constexpr int kNumThreads = 4;
    constexpr int kNumWrites = 20;
    std::vector<std::thread> threads;
    std::atomic<int> fail_count{0};

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kNumWrites; ++i) {
                std::string fname = "t" + std::to_string(t) + "_f" +
                    std::to_string(i);
                std::string content = "content_" + std::to_string(t) + "_" +
                    std::to_string(i);

                BlueFS::FileWriter *w = nullptr;
                if (fs.open_for_write("d", fname, &w) != 0) {
                    fail_count++;
                    continue;
                }
                if (fs.append_try_flush(w, content.data(), content.size()) !=
                    0) {
                    fail_count++;
                }
                if (fs.fsync(w) != 0) {
                    fail_count++;
                }
                if (fs.close_writer(w) != 0) {
                    fail_count++;
                }

                // Read back
                BlueFS::FileReader *r = nullptr;
                if (fs.open_for_read("d", fname, &r) != 0) {
                    fail_count++;
                    continue;
                }
                bufferlist bl;
                if (fs.read(r, 0, content.size(), &bl) !=
                    (int64_t)content.size()) {
                    fail_count++;
                }
                if (bl.to_str() != content) {
                    fail_count++;
                }
                fs.close_reader(r);
            }
        });
    }

    for (auto &th : threads) {
        th.join();
    }

    EXPECT_EQ(fail_count.load(), 0);

    // All files should exist and be readable
    std::vector<std::string> ls;
    ASSERT_EQ(fs.readdir("d", &ls), 0);
    EXPECT_EQ((int)ls.size(), kNumThreads * kNumWrites);

    fs.umount();
}
