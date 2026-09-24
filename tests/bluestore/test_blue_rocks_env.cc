#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "blk/kernel_device.h"
#include "bluestore/blue_rocks_env.h"
#include "bluefs/bluefs.h"
#include "common/cassert.h"
#include "cxxlab_test.h"
#include "rocksdb/env.h"

using namespace TOPNSPC;

class BlueRocksEnvTest : public ::testing::Test {
protected:
    std::string tmp_path_;
    int tmp_fd_ = -1;
    static constexpr uint64_t kFileSize = 16 << 20;  // 16 MB

    std::unique_ptr<BlueFS> fs_;
    std::unique_ptr<BlueRocksEnv> env_;

    void SetUp() override {
        auto tmpl = cxxlab_tmp_path("bluere_test");
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

        BlueFSConfig cfg;
        cfg.alloc_size = 4096;
        cfg.shared_alloc_size = 65536;
        cfg.buffered_io = true;

        fs_ = std::make_unique<BlueFS>(cfg);
        ASSERT_NO_FATAL_FAILURE(
            fs_->add_block_device(BlueFS::BDEV_DB, tmp_path_));

        fs_->get_mutable_super().uuid.generate();
        ASSERT_EQ(fs_->mkfs(4096), 0);
        ASSERT_EQ(fs_->mount(), 0);

        // Create a "db" directory (RocksDB convention)
        ASSERT_EQ(fs_->mkdir("db"), 0);

        env_ = std::make_unique<BlueRocksEnv>(fs_.get());
    }

    void TearDown() override {
        env_.reset();
        if (fs_) {
            fs_->umount(true);
            fs_.reset();
        }
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

// =====================================================================
// 2.1 + 2.2: SequentialFile (write via BlueFS, read via Env)
// =====================================================================
TEST_F(BlueRocksEnvTest, SequentialFileReadWrite) {
    const std::string fname = "db/test_seq.dat";
    const std::string data = "Hello BlueRocks SequentialFile!";

    // Write via BlueFS directly
    BlueFS::FileWriter *w;
    ASSERT_EQ(fs_->open_for_write("db", "test_seq.dat", &w, false), 0);
    fs_->append_try_flush(w, data.data(), data.size());
    ASSERT_EQ(fs_->fsync(w), 0);
    fs_->close_writer(w);

    // Read via Env
    std::unique_ptr<rocksdb::SequentialFile> f;
    auto s = env_->NewSequentialFile(fname, &f, rocksdb::EnvOptions());
    ASSERT_TRUE(s.ok()) << s.ToString();

    char scratch[256];
    rocksdb::Slice result;
    s = f->Read(sizeof(scratch), &result, scratch);
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(result.size(), data.size());
    ASSERT_EQ(std::string(result.data(), result.size()), data);
}

TEST_F(BlueRocksEnvTest, SequentialFileSkip) {
    const std::string fname = "db/test_skip.dat";
    const std::string data = "0123456789ABCDEF";

    BlueFS::FileWriter *w;
    ASSERT_EQ(fs_->open_for_write("db", "test_skip.dat", &w, false), 0);
    fs_->append_try_flush(w, data.data(), data.size());
    ASSERT_EQ(fs_->fsync(w), 0);
    fs_->close_writer(w);

    std::unique_ptr<rocksdb::SequentialFile> f;
    ASSERT_TRUE(env_->NewSequentialFile(fname, &f, rocksdb::EnvOptions()).ok());

    // Skip first 5 bytes, then read
    ASSERT_TRUE(f->Skip(5).ok());
    char scratch[16];
    rocksdb::Slice result;
    ASSERT_TRUE(f->Read(sizeof(scratch), &result, scratch).ok());
    ASSERT_EQ(result.size(), 11);
    ASSERT_EQ(std::string(result.data(), result.size()), "56789ABCDEF");
}

TEST_F(BlueRocksEnvTest, InvalidateCacheWithRandomAccessFile) {
    const std::string fname = "db/test_inval_rand.dat";
    const std::string data = "Cache invalidation test for random-access file";

    BlueFS::FileWriter *w;
    ASSERT_EQ(fs_->open_for_write("db", "test_inval_rand.dat", &w, false), 0);
    fs_->append_try_flush(w, data.data(), data.size());
    ASSERT_EQ(fs_->fsync(w), 0);
    fs_->close_writer(w);

    std::unique_ptr<rocksdb::RandomAccessFile> f;
    ASSERT_TRUE(
        env_->NewRandomAccessFile(fname, &f, rocksdb::EnvOptions()).ok());

    // Read before invalidation
    char scratch[128];
    rocksdb::Slice result;
    ASSERT_TRUE(f->Read(0, data.size(), &result, scratch).ok());
    ASSERT_EQ(std::string(result.data(), result.size()), data);

    // Invalidate cache for the whole file
    ASSERT_TRUE(f->InvalidateCache(0, data.size()).ok());

    // Read again after invalidation — should still get correct data
    rocksdb::Slice result2;
    ASSERT_TRUE(f->Read(0, data.size(), &result2, scratch).ok());
    ASSERT_EQ(std::string(result2.data(), result2.size()), data);
}

TEST_F(BlueRocksEnvTest, SequentialFileNotFound) {
    std::unique_ptr<rocksdb::SequentialFile> f;
    auto s = env_->NewSequentialFile("db/nonexistent.dat", &f,
                                     rocksdb::EnvOptions());
    ASSERT_TRUE(s.IsNotFound());
}

// =====================================================================
// 2.3: RandomAccessFile
// =====================================================================
TEST_F(BlueRocksEnvTest, RandomAccessFileRead) {
    const std::string fname = "db/test_rand.dat";
    // Write structured data: 4 blocks of 16 bytes each
    std::string data;
    for (int i = 0; i < 4; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "BLOCK_%02d------", i);
        data.append(buf, 16);
    }

    BlueFS::FileWriter *w;
    ASSERT_EQ(fs_->open_for_write("db", "test_rand.dat", &w, false), 0);
    fs_->append_try_flush(w, data.data(), data.size());
    ASSERT_EQ(fs_->fsync(w), 0);
    fs_->close_writer(w);

    std::unique_ptr<rocksdb::RandomAccessFile> f;
    ASSERT_TRUE(
        env_->NewRandomAccessFile(fname, &f, rocksdb::EnvOptions()).ok());

    // Read block 2 (offset 32, 16 bytes)
    char scratch[32];
    rocksdb::Slice result;
    ASSERT_TRUE(f->Read(32, 16, &result, scratch).ok());
    ASSERT_EQ(result.size(), 16);
    ASSERT_EQ(std::string(result.data(), 8), "BLOCK_02");

    // Read block 0
    ASSERT_TRUE(f->Read(0, 8, &result, scratch).ok());
    ASSERT_EQ(result.size(), 8);
    ASSERT_EQ(std::string(result.data(), 8), "BLOCK_00");
}

TEST_F(BlueRocksEnvTest, RandomAccessFileGetUniqueId) {
    const std::string fname = "db/test_unique.dat";

    BlueFS::FileWriter *w;
    ASSERT_EQ(fs_->open_for_write("db", "test_unique.dat", &w, false), 0);
    fs_->append_try_flush(w, "data", 4);
    ASSERT_EQ(fs_->fsync(w), 0);
    fs_->close_writer(w);

    std::unique_ptr<rocksdb::RandomAccessFile> f;
    ASSERT_TRUE(
        env_->NewRandomAccessFile(fname, &f, rocksdb::EnvOptions()).ok());

    char id[32];
    size_t len = f->GetUniqueId(id, sizeof(id));
    ASSERT_GT(len, 0);
    ASSERT_LT(len, sizeof(id));
}

TEST_F(BlueRocksEnvTest, RandomAccessFilePrefetch) {
    const std::string fname = "db/test_prefetch.dat";
    std::string data(4096, 'A');

    BlueFS::FileWriter *w;
    ASSERT_EQ(fs_->open_for_write("db", "test_prefetch.dat", &w, false), 0);
    fs_->append_try_flush(w, data.data(), data.size());
    ASSERT_EQ(fs_->fsync(w), 0);
    fs_->close_writer(w);

    std::unique_ptr<rocksdb::RandomAccessFile> f;
    ASSERT_TRUE(
        env_->NewRandomAccessFile(fname, &f, rocksdb::EnvOptions()).ok());

    ASSERT_TRUE(f->Prefetch(0, 1024).ok());
    char scratch[64];
    rocksdb::Slice result;
    ASSERT_TRUE(f->Read(0, 4, &result, scratch).ok());
    ASSERT_EQ(std::string(result.data(), result.size()), "AAAA");
}

// =====================================================================
// 2.4: WritableFile
// =====================================================================
TEST_F(BlueRocksEnvTest, WritableFileAppendSync) {
    const std::string fname = "db/test_write.dat";
    const std::string data = "Hello WritableFile!";

    std::unique_ptr<rocksdb::WritableFile> f;
    ASSERT_TRUE(env_->NewWritableFile(fname, &f, rocksdb::EnvOptions()).ok());

    ASSERT_TRUE(f->Append(rocksdb::Slice(data)).ok());
    ASSERT_TRUE(f->Sync().ok());

    // Verify file size via Env
    uint64_t file_size = 0;
    ASSERT_TRUE(env_->GetFileSize(fname, &file_size).ok());
    ASSERT_EQ(file_size, data.size());

    // Close and verify by reading back
    ASSERT_TRUE(f->Close().ok());
    f.reset();

    // Read back via BlueFS
    BlueFS::FileReader *r;
    ASSERT_EQ(fs_->open_for_read("db", "test_write.dat", &r, false), 0);
    char buf[128];
    int64_t nread = fs_->read(r, 0, sizeof(buf), nullptr, buf);
    ASSERT_EQ(nread, static_cast<int64_t>(data.size()));
    ASSERT_EQ(std::string(buf, nread), data);
    fs_->close_reader(r);
}

TEST_F(BlueRocksEnvTest, WritableFileAppendMultiple) {
    const std::string fname = "db/test_append.dat";

    std::unique_ptr<rocksdb::WritableFile> f;
    ASSERT_TRUE(env_->NewWritableFile(fname, &f, rocksdb::EnvOptions()).ok());

    ASSERT_TRUE(f->Append(rocksdb::Slice("Hello ")).ok());
    ASSERT_TRUE(f->Append(rocksdb::Slice("World")).ok());
    ASSERT_TRUE(f->Append(rocksdb::Slice("!")).ok());
    ASSERT_EQ(f->GetFileSize(), 12);

    // Flush (doesn't sync to disk but flushes BlueFS buffer)
    ASSERT_TRUE(f->Flush().ok());
    ASSERT_TRUE(f->Sync().ok());

    uint64_t file_size = 0;
    ASSERT_TRUE(env_->GetFileSize(fname, &file_size).ok());
    ASSERT_EQ(file_size, 12);

    ASSERT_TRUE(f->Close().ok());
    f.reset();

    // Verify
    BlueFS::FileReader *r;
    ASSERT_EQ(fs_->open_for_read("db", "test_append.dat", &r, false), 0);
    char buf[32];
    int64_t nread = fs_->read(r, 0, sizeof(buf), nullptr, buf);
    ASSERT_EQ(std::string(buf, nread), "Hello World!");
    fs_->close_reader(r);
}

TEST_F(BlueRocksEnvTest, WritableFileTruncateOnClose) {
    const std::string fname = "db/test_trunc.dat";

    std::unique_ptr<rocksdb::WritableFile> f;
    ASSERT_TRUE(env_->NewWritableFile(fname, &f, rocksdb::EnvOptions()).ok());

    // Preallocate 64KB so there's allocated space beyond our data.
    // After Close() the file should be truncated to actual written size.
    ASSERT_TRUE(f->Allocate(0, 65536).ok());
    ASSERT_TRUE(f->Append(rocksdb::Slice("0123456789")).ok());
    ASSERT_EQ(f->GetFileSize(), 10);

    // Close will fsync then truncate from 65536 → 10
    ASSERT_TRUE(f->Close().ok());
    f.reset();

    uint64_t file_size = 0;
    ASSERT_TRUE(env_->GetFileSize(fname, &file_size).ok());
    ASSERT_EQ(file_size, 10);
}

TEST_F(BlueRocksEnvTest, WritableFileGetUniqueId) {
    const std::string fname = "db/test_wuid.dat";

    std::unique_ptr<rocksdb::WritableFile> f;
    ASSERT_TRUE(env_->NewWritableFile(fname, &f, rocksdb::EnvOptions()).ok());

    ASSERT_TRUE(f->Append(rocksdb::Slice("data")).ok());

    char id[32];
    size_t len = f->GetUniqueId(id, sizeof(id));
    ASSERT_GT(len, 0);
    ASSERT_LT(len, sizeof(id));

    ASSERT_TRUE(f->Close().ok());
}

TEST_F(BlueRocksEnvTest, WritableFileRangeSync) {
    const std::string fname = "db/test_rangesync.dat";

    std::unique_ptr<rocksdb::WritableFile> f;
    ASSERT_TRUE(env_->NewWritableFile(fname, &f, rocksdb::EnvOptions()).ok());

    ASSERT_TRUE(f->Append(rocksdb::Slice(std::string(8192, 'B'))).ok());
    ASSERT_TRUE(f->RangeSync(0, 4096).ok());
    ASSERT_TRUE(f->Sync().ok());
    ASSERT_TRUE(f->Close().ok());
}

// =====================================================================
// 2.4: ReuseWritableFile (rename + open for write)
// =====================================================================
TEST_F(BlueRocksEnvTest, ReuseWritableFile) {
    const std::string old_name = "db/test_old.dat";
    const std::string new_name = "db/test_new.dat";

    // Create original file
    std::unique_ptr<rocksdb::WritableFile> f;
    ASSERT_TRUE(env_->NewWritableFile(old_name, &f, rocksdb::EnvOptions()).ok());
    ASSERT_TRUE(f->Append(rocksdb::Slice("original")).ok());
    ASSERT_TRUE(f->Close().ok());
    f.reset();

    // Reuse: renames old to new and opens for write
    ASSERT_TRUE(
        env_->ReuseWritableFile(new_name, old_name, &f, rocksdb::EnvOptions())
            .ok());
    ASSERT_TRUE(f->Append(rocksdb::Slice("reused")).ok());
    ASSERT_TRUE(f->Close().ok());
    f.reset();

    // Verify new file content (old_name should be gone)
    EXPECT_TRUE(env_->FileExists(old_name).IsNotFound());

    // overwrite=true preserves existing extents; _create_writer
    // initializes pos = fnode.size, so new data is appended.
    // Result: "original" (8) + "reused" (6) = 14 bytes.
    uint64_t file_size = 0;
    ASSERT_TRUE(env_->GetFileSize(new_name, &file_size).ok());
    ASSERT_EQ(file_size, 14);

    BlueFS::FileReader *r;
    ASSERT_EQ(fs_->open_for_read("db", "test_new.dat", &r, false), 0);
    char buf[32];
    int64_t nread = fs_->read(r, 0, sizeof(buf), nullptr, buf);
    ASSERT_EQ(nread, 14);
    ASSERT_EQ(std::string(buf, nread), "originalreused");
    fs_->close_reader(r);
}

// =====================================================================
// 2.5: Directory + File operations
// =====================================================================
TEST_F(BlueRocksEnvTest, FileExists) {
    const std::string fname = "db/test_exist.dat";

    // Should not exist initially
    ASSERT_TRUE(env_->FileExists(fname).IsNotFound());

    // Create file
    std::unique_ptr<rocksdb::WritableFile> f;
    ASSERT_TRUE(env_->NewWritableFile(fname, &f, rocksdb::EnvOptions()).ok());
    ASSERT_TRUE(f->Append(rocksdb::Slice("data")).ok());
    ASSERT_TRUE(f->Close().ok());
    f.reset();

    // Should exist now
    ASSERT_TRUE(env_->FileExists(fname).ok());
}

TEST_F(BlueRocksEnvTest, CreateAndDeleteDir) {
    const std::string dirname = "testdir";

    ASSERT_TRUE(env_->CreateDir(dirname).ok());

    // Duplicate create should fail
    auto s = env_->CreateDir(dirname);
    ASSERT_TRUE(s.IsIOError());

    // Create dir if missing should succeed
    ASSERT_TRUE(env_->CreateDirIfMissing(dirname).ok());

    // Delete dir
    ASSERT_TRUE(env_->DeleteDir(dirname).ok());

    // Deleted dir not found
    ASSERT_TRUE(env_->NewDirectory(dirname, nullptr).IsNotFound());
}

TEST_F(BlueRocksEnvTest, GetChildren) {
    ASSERT_EQ(fs_->mkdir("testdir"), 0);

    // Create files via BlueFS
    BlueFS::FileWriter *w;
    ASSERT_EQ(fs_->open_for_write("testdir", "a.dat", &w, false), 0);
    fs_->close_writer(w);
    ASSERT_EQ(fs_->open_for_write("testdir", "b.dat", &w, false), 0);
    fs_->close_writer(w);
    ASSERT_EQ(fs_->open_for_write("testdir", "c.dat", &w, false), 0);
    fs_->close_writer(w);

    std::vector<std::string> children;
    ASSERT_TRUE(env_->GetChildren("testdir", &children).ok());
    ASSERT_EQ(children.size(), 3);
    std::sort(children.begin(), children.end());
    EXPECT_EQ(children[0], "a.dat");
    EXPECT_EQ(children[1], "b.dat");
    EXPECT_EQ(children[2], "c.dat");
}

TEST_F(BlueRocksEnvTest, DeleteFile) {
    const std::string fname = "db/test_del.dat";

    std::unique_ptr<rocksdb::WritableFile> f;
    ASSERT_TRUE(env_->NewWritableFile(fname, &f, rocksdb::EnvOptions()).ok());
    ASSERT_TRUE(f->Append(rocksdb::Slice("data")).ok());
    ASSERT_TRUE(f->Close().ok());
    f.reset();

    ASSERT_TRUE(env_->DeleteFile(fname).ok());
    ASSERT_TRUE(env_->FileExists(fname).IsNotFound());
}

TEST_F(BlueRocksEnvTest, GetFileSize) {
    const std::string fname = "db/test_size.dat";

    std::unique_ptr<rocksdb::WritableFile> f;
    ASSERT_TRUE(env_->NewWritableFile(fname, &f, rocksdb::EnvOptions()).ok());
    ASSERT_TRUE(f->Append(rocksdb::Slice("12345")).ok());
    ASSERT_TRUE(f->Close().ok());
    f.reset();

    uint64_t size = 0;
    ASSERT_TRUE(env_->GetFileSize(fname, &size).ok());
    ASSERT_EQ(size, 5);
}

TEST_F(BlueRocksEnvTest, RenameFile) {
    const std::string src = "db/test_src.dat";
    const std::string dst = "db/test_dst.dat";

    std::unique_ptr<rocksdb::WritableFile> f;
    ASSERT_TRUE(env_->NewWritableFile(src, &f, rocksdb::EnvOptions()).ok());
    ASSERT_TRUE(f->Append(rocksdb::Slice("rename_test")).ok());
    ASSERT_TRUE(f->Close().ok());
    f.reset();

    ASSERT_TRUE(env_->RenameFile(src, dst).ok());
    ASSERT_TRUE(env_->FileExists(src).IsNotFound());

    uint64_t size = 0;
    ASSERT_TRUE(env_->GetFileSize(dst, &size).ok());
    ASSERT_EQ(size, 11);  // "rename_test" = 11 chars
}

TEST_F(BlueRocksEnvTest, LockAndUnlockFile) {
    const std::string fname = "db/test_lock.dat";

    rocksdb::FileLock *lock = nullptr;
    ASSERT_TRUE(env_->LockFile(fname, &lock).ok());
    ASSERT_NE(lock, nullptr);

    // Second lock on same file should fail
    rocksdb::FileLock *lock2 = nullptr;
    auto s = env_->LockFile(fname, &lock2);
    ASSERT_FALSE(s.ok());

    ASSERT_TRUE(env_->UnlockFile(lock).ok());
}

TEST_F(BlueRocksEnvTest, GetFileModificationTime) {
    const std::string fname = "db/test_mtime.dat";

    std::unique_ptr<rocksdb::WritableFile> f;
    ASSERT_TRUE(env_->NewWritableFile(fname, &f, rocksdb::EnvOptions()).ok());
    ASSERT_TRUE(f->Append(rocksdb::Slice("data")).ok());
    ASSERT_TRUE(f->Close().ok());
    f.reset();

    uint64_t mtime = 0;
    ASSERT_TRUE(env_->GetFileModificationTime(fname, &mtime).ok());
    // mtime is 0 in our simplified BlueFS
    ASSERT_EQ(mtime, 0);
}

TEST_F(BlueRocksEnvTest, NewDirectory) {
    ASSERT_TRUE(env_->CreateDir("somedir").ok());

    std::unique_ptr<rocksdb::Directory> dir;
    ASSERT_TRUE(env_->NewDirectory("somedir", &dir).ok());
    ASSERT_NE(dir, nullptr);

    // Fsync directory
    ASSERT_TRUE(dir->Fsync().ok());

    // Non-existent directory
    ASSERT_TRUE(env_->NewDirectory("nonexistent", &dir).IsNotFound());
}

TEST_F(BlueRocksEnvTest, AreFilesSame) {
    const std::string f1 = "db/test_same_a.dat";
    const std::string f2 = "db/test_same_b.dat";

    std::unique_ptr<rocksdb::WritableFile> f;
    ASSERT_TRUE(env_->NewWritableFile(f1, &f, rocksdb::EnvOptions()).ok());
    ASSERT_TRUE(f->Close().ok());
    f.reset();
    ASSERT_TRUE(env_->NewWritableFile(f2, &f, rocksdb::EnvOptions()).ok());
    ASSERT_TRUE(f->Close().ok());
    f.reset();

    bool same = false;
    ASSERT_TRUE(env_->AreFilesSame(f1, f2, &same).ok());
    // Different paths -> not same
    ASSERT_FALSE(same);

    // Same path -> same
    ASSERT_TRUE(env_->AreFilesSame(f1, f1, &same).ok());
    ASSERT_TRUE(same);

    // Directory path triggers dir_exists branch
    ASSERT_TRUE(env_->AreFilesSame("db", "db", &same).ok());
    ASSERT_TRUE(same);
}

// =====================================================================
// 2.6: Logger
// =====================================================================
TEST_F(BlueRocksEnvTest, NewLogger) {
    std::shared_ptr<rocksdb::Logger> logger;
    ASSERT_TRUE(env_->NewLogger("ignored.log", &logger).ok());
    ASSERT_NE(logger, nullptr);

    // Verify log level roundtrip
    logger->SetInfoLogLevel(rocksdb::DEBUG_LEVEL);
    ASSERT_EQ(logger->GetInfoLogLevel(), rocksdb::DEBUG_LEVEL);
    logger->SetInfoLogLevel(rocksdb::WARN_LEVEL);
    ASSERT_EQ(logger->GetInfoLogLevel(), rocksdb::WARN_LEVEL);

    // Verify that logging at WARN level produces output while DEBUG is filtered
    // Redirect stderr to a pipe and capture
    int pipefd[2];
    ASSERT_EQ(::pipe(pipefd), 0);
    int saved_stderr = ::dup(STDERR_FILENO);
    ASSERT_GE(saved_stderr, 0);
    ASSERT_NE(::dup2(pipefd[1], STDERR_FILENO), -1);
    ::close(pipefd[1]);

    logger->SetInfoLogLevel(rocksdb::WARN_LEVEL);
    // Logv requires va_list, so use a lambda to forward variadic args
    auto log_msg = [&](rocksdb::InfoLogLevel lvl, const char *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        logger->Logv(lvl, fmt, ap);
        va_end(ap);
    };
    log_msg(rocksdb::DEBUG_LEVEL, "should_not_appear");
    log_msg(rocksdb::WARN_LEVEL, "should_appear 99");

    // Restore stderr
    ::dup2(saved_stderr, STDERR_FILENO);
    ::close(saved_stderr);

    // Read captured output
    char buf[4096];
    ssize_t n = ::read(pipefd[0], buf, sizeof(buf) - 1);
    ::close(pipefd[0]);
    ASSERT_GT(n, 0);
    buf[n] = '\0';

    // DEBUG message should be filtered out; WARN should appear
    ASSERT_EQ(std::string(buf, n).find("should_not_appear"), std::string::npos);
    ASSERT_NE(std::string(buf, n).find("should_appear 99"), std::string::npos);
    ASSERT_NE(std::string(buf, n).find("[rocksdb/WARN]"), std::string::npos);
}

// =====================================================================
// 2.7: Integration + misc
// =====================================================================
TEST_F(BlueRocksEnvTest, GetAbsolutePath) {
    std::string path;
    ASSERT_TRUE(env_->GetAbsolutePath("db/test.db", &path).ok());
    ASSERT_EQ(path, "/db/test.db");
}

TEST_F(BlueRocksEnvTest, GetTestDirectory) {
    std::string p1, p2;
    ASSERT_TRUE(env_->GetTestDirectory(&p1).ok());
    ASSERT_TRUE(env_->GetTestDirectory(&p2).ok());
    ASSERT_EQ(p1, p2);  // Subsequent calls return the same directory
    ASSERT_TRUE(p1.find("cxxlab_rocksdb_test_dir") != std::string::npos);
}

TEST_F(BlueRocksEnvTest, AbsolutePathEscape) {
    // Files with absolute paths should be forwarded to the default POSIX Env.
    char tmpl[] = "/tmp/bluere_abs_XXXXXX";
    int fd = ::mkstemp(tmpl);
    ASSERT_GE(fd, 0);
    std::string content = "absolute-path-test";
    ASSERT_EQ(::pwrite(fd, content.data(), content.size(), 0),
              static_cast<ssize_t>(content.size()));
    ::close(fd);

    // Verify the file exists via POSIX Env directly
    auto *posix_env = rocksdb::Env::Default();
    ASSERT_TRUE(posix_env->FileExists(tmpl).ok());

    // NewSequentialFile — absolute path forwarded to POSIX
    {
        std::unique_ptr<rocksdb::SequentialFile> f;
        auto s = env_->NewSequentialFile(tmpl, &f, rocksdb::EnvOptions());
        ASSERT_TRUE(s.ok()) << s.ToString();
        char buf[256];
        rocksdb::Slice result;
        ASSERT_TRUE(f->Read(sizeof(buf), &result, buf).ok());
        ASSERT_EQ(std::string(result.data(), result.size()), content);
    }
    // NewRandomAccessFile — absolute path forwarded to POSIX
    {
        std::unique_ptr<rocksdb::RandomAccessFile> f;
        auto s = env_->NewRandomAccessFile(tmpl, &f, rocksdb::EnvOptions());
        ASSERT_TRUE(s.ok()) << s.ToString();
        char scratch[32];
        rocksdb::Slice result;
        ASSERT_TRUE(f->Read(0, content.size(), &result, scratch).ok());
        ASSERT_EQ(std::string(result.data(), result.size()), content);
    }
    // FileExists — absolute path forwarded to POSIX
    {
        auto s = env_->FileExists(tmpl);
        ASSERT_TRUE(s.ok()) << s.ToString();
        auto s2 = env_->FileExists("/nonexistent_path_xyz");
        ASSERT_TRUE(s2.IsNotFound());
    }
    // NewDirectory — absolute path forwarded to POSIX (/tmp always exists)
    {
        std::unique_ptr<rocksdb::Directory> dir;
        auto s = env_->NewDirectory("/tmp", &dir);
        ASSERT_TRUE(s.ok()) << s.ToString();
    }

    ::unlink(tmpl);
}

TEST_F(BlueRocksEnvTest, WriteReadIntegration) {
    const std::string fname = "db/test_integration.dat";
    const std::string expected = "Integration test data for BlueRocksEnv!";

    // Write via WritableFile
    {
        std::unique_ptr<rocksdb::WritableFile> f;
        ASSERT_TRUE(
            env_->NewWritableFile(fname, &f, rocksdb::EnvOptions()).ok());
        ASSERT_TRUE(f->Append(rocksdb::Slice(expected)).ok());
        ASSERT_TRUE(f->Sync().ok());
        ASSERT_EQ(f->GetFileSize(), expected.size());
        ASSERT_TRUE(f->Close().ok());
    }

    // Verify via SequentialFile
    {
        std::unique_ptr<rocksdb::SequentialFile> f;
        ASSERT_TRUE(
            env_->NewSequentialFile(fname, &f, rocksdb::EnvOptions()).ok());
        char buf[256];
        rocksdb::Slice result;
        ASSERT_TRUE(f->Read(sizeof(buf), &result, buf).ok());
        ASSERT_EQ(std::string(result.data(), result.size()), expected);
    }

    // Verify via RandomAccessFile
    {
        std::unique_ptr<rocksdb::RandomAccessFile> f;
        ASSERT_TRUE(
            env_->NewRandomAccessFile(fname, &f, rocksdb::EnvOptions()).ok());
        char buf[256];
        rocksdb::Slice result;
        ASSERT_TRUE(f->Read(0, expected.size(), &result, buf).ok());
        ASSERT_EQ(std::string(result.data(), result.size()), expected);
    }

    // Verify file size
    {
        uint64_t size = 0;
        ASSERT_TRUE(env_->GetFileSize(fname, &size).ok());
        ASSERT_EQ(size, expected.size());
    }

    // Delete and verify gone
    ASSERT_TRUE(env_->DeleteFile(fname).ok());
    ASSERT_TRUE(env_->FileExists(fname).IsNotFound());
}

TEST_F(BlueRocksEnvTest, FileNotExist) {
    EXPECT_TRUE(env_->FileExists("db/nope.dat").IsNotFound());
    uint64_t dummy = 0;
    EXPECT_TRUE(env_->GetFileSize("db/nope.dat", &dummy).IsNotFound());

    std::unique_ptr<rocksdb::SequentialFile> f;
    EXPECT_TRUE(
        env_->NewSequentialFile("db/nope.dat", &f, rocksdb::EnvOptions())
            .IsNotFound());

    std::unique_ptr<rocksdb::RandomAccessFile> rf;
    EXPECT_TRUE(
        env_->NewRandomAccessFile("db/nope.dat", &rf, rocksdb::EnvOptions())
            .IsNotFound());
}
