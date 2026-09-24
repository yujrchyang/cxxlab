#include "bluestore/blue_rocks_env.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

#include "bluefs/bluefs.h"
#include "common/cassert.h"
#include "rocksdb/env.h"

namespace TOPNSPC {

// =====================================================================
// BlueFSRocksdbLogger — a rocksdb::Logger that writes to stderr
// =====================================================================
class BlueFSRocksdbLogger : public rocksdb::Logger {
public:
    explicit BlueFSRocksdbLogger(
        const rocksdb::InfoLogLevel log_level = rocksdb::InfoLogLevel::INFO_LEVEL)
        : Logger(log_level) {}

    void Logv(const rocksdb::InfoLogLevel log_level, const char *format,
              va_list ap) override {
        if (log_level < GetInfoLogLevel()) return;
        static const char *kLevelNames[] = {
            "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "HEADER"};
        const char *level_name = (log_level < rocksdb::NUM_INFO_LOG_LEVELS)
            ? kLevelNames[log_level]
            : "UNKNOWN";
        std::fprintf(stderr, "[rocksdb/%s] ", level_name);
        std::vfprintf(stderr, format, ap);
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
    }
};

rocksdb::Logger *CreateRocksdbLogger() {
    return new BlueFSRocksdbLogger();
}

namespace {

rocksdb::Status err_to_status(int r) {
    switch (r) {
    case 0:
        return rocksdb::Status::OK();
    case -ENOENT:
        return rocksdb::Status::NotFound(rocksdb::Status::kNone);
    case -EINVAL:
        return rocksdb::Status::InvalidArgument(rocksdb::Status::kNone);
    case -EIO:
    case -EEXIST:
        return rocksdb::Status::IOError(rocksdb::Status::kNone);
    case -ENOSPC:
        return rocksdb::Status::IOError("No space left on device");
    case -ENOMEM:
        return rocksdb::Status::IOError("Out of memory");
    case -ENOLCK:
        return rocksdb::Status::IOError(std::strerror(r));
    default:
        cxxlab_assert(!"unrecognized error code");
        return rocksdb::Status::NotSupported(rocksdb::Status::kNone);
    }
}

// Split "dir/file" into {dir, file}
std::pair<std::string_view, std::string_view>
split(const std::string &fn) {
    size_t slash = fn.rfind('/');
    cxxlab_assert(slash != fn.npos);
    size_t file_begin = slash + 1;
    while (slash && fn[slash - 1] == '/')
        --slash;
    return {std::string_view(fn.data(), slash),
            std::string_view(fn.data() + file_begin,
                             fn.size() - file_begin)};
}

// A file abstraction for reading sequentially through a file
class BlueRocksSequentialFile : public rocksdb::SequentialFile {
    BlueFS *fs_;
    BlueFS::FileReader *h_;

public:
    BlueRocksSequentialFile(BlueFS *fs, BlueFS::FileReader *h)
        : fs_(fs), h_(h) {}
    ~BlueRocksSequentialFile() override {
        fs_->close_reader(h_);
    }

    rocksdb::Status Read(size_t n, rocksdb::Slice *result,
                         char *scratch) override {
        int64_t r = fs_->read(h_, h_->buf.pos, n, nullptr, scratch);
        cxxlab_assert(r >= 0);
        *result = rocksdb::Slice(scratch, r);
        return rocksdb::Status::OK();
    }

    rocksdb::Status Skip(uint64_t n) override {
        h_->buf.skip(n);
        return rocksdb::Status::OK();
    }

    rocksdb::Status InvalidateCache(size_t offset, size_t length) override {
        h_->buf.invalidate_cache(offset, length);
        fs_->invalidate_cache(h_->file, offset, length);
        return rocksdb::Status::OK();
    }
};

// A file abstraction for randomly reading the contents of a file
class BlueRocksRandomAccessFile : public rocksdb::RandomAccessFile {
    BlueFS *fs_;
    BlueFS::FileReader *h_;

public:
    BlueRocksRandomAccessFile(BlueFS *fs, BlueFS::FileReader *h)
        : fs_(fs), h_(h) {}
    ~BlueRocksRandomAccessFile() override {
        fs_->close_reader(h_);
    }

    rocksdb::Status Read(uint64_t offset, size_t n, rocksdb::Slice *result,
                         char *scratch) const override {
        int64_t r = fs_->read_random(h_, offset, n, scratch);
        cxxlab_assert(r >= 0);
        *result = rocksdb::Slice(scratch, r);
        return rocksdb::Status::OK();
    }

    size_t GetUniqueId(char *id, size_t max_size) const override {
        return std::snprintf(id, max_size, "%016llx",
                             static_cast<unsigned long long>(h_->file->fnode.ino));
    }

    rocksdb::Status Prefetch(uint64_t offset, size_t n) override {
        fs_->read(h_, offset, n, nullptr, nullptr);
        return rocksdb::Status::OK();
    }

    void Hint(AccessPattern pattern) override {
        if (pattern == RANDOM)
            h_->buf.max_prefetch = 4096;
        else if (pattern == SEQUENTIAL)
            h_->buf.max_prefetch = 65536;
    }

    rocksdb::Status InvalidateCache(size_t offset, size_t length) override {
        h_->buf.invalidate_cache(offset, length);
        fs_->invalidate_cache(h_->file, offset, length);
        return rocksdb::Status::OK();
    }

    bool use_direct_io() const override {
        return false;
    }
};

// A file abstraction for sequential writing
class BlueRocksWritableFile : public rocksdb::WritableFile {
    BlueFS *fs_;
    BlueFS::FileWriter *h_;

public:
    BlueRocksWritableFile(BlueFS *fs, BlueFS::FileWriter *h)
        : fs_(fs), h_(h) {}
    ~BlueRocksWritableFile() override {
        fs_->close_writer(h_);
    }

    rocksdb::Status Append(const rocksdb::Slice &data) override {
        fs_->append_try_flush(h_, data.data(), data.size());
        return rocksdb::Status::OK();
    }

    rocksdb::Status PositionedAppend(
        const rocksdb::Slice & /*data*/,
        uint64_t /*offset*/) override {
        return rocksdb::Status::NotSupported();
    }

    rocksdb::Status Truncate(uint64_t /*size*/) override {
        return rocksdb::Status::OK();
    }

    rocksdb::Status Close() override {
        fs_->fsync(h_);
        size_t block_size;
        size_t last_allocated_block;
        GetPreallocationStatus(&block_size, &last_allocated_block);
        if (last_allocated_block > 0) {
            int r = fs_->truncate(h_, h_->pos);
            if (r < 0) return err_to_status(r);
        }
        return rocksdb::Status::OK();
    }

    rocksdb::Status Flush() override {
        fs_->flush(h_);
        return rocksdb::Status::OK();
    }

    rocksdb::Status Sync() override {
        fs_->fsync(h_);
        return rocksdb::Status::OK();
    }

    bool IsSyncThreadSafe() const override {
        return true;
    }

    bool use_direct_io() const override {
        return false;
    }

    void SetWriteLifeTimeHint(rocksdb::Env::WriteLifeTimeHint hint) override {
        h_->write_hint = static_cast<int>(hint);
    }

    uint64_t GetFileSize() override {
        return h_->file->fnode.size + h_->get_buffer_length();
    }

    size_t GetUniqueId(char *id, size_t max_size) const override {
        return std::snprintf(id, max_size, "%016llx",
                             static_cast<unsigned long long>(h_->file->fnode.ino));
    }

    rocksdb::Status InvalidateCache(size_t offset, size_t length) override {
        fs_->fsync(h_);
        fs_->invalidate_cache(h_->file, offset, length);
        return rocksdb::Status::OK();
    }

    rocksdb::Status RangeSync(uint64_t offset, uint64_t nbytes) override {
        uint64_t partial = offset & 4095;
        offset -= partial;
        nbytes += partial;
        nbytes &= ~static_cast<uint64_t>(4095);
        if (nbytes)
            fs_->flush_range(h_, offset, nbytes);
        return rocksdb::Status::OK();
    }

protected:
    rocksdb::Status Allocate(uint64_t offset, uint64_t len) override {
        int r = fs_->preallocate(h_->file, offset, len);
        return err_to_status(r);
    }
};

class BlueRocksDirectory : public rocksdb::Directory {
    BlueFS *fs_;

public:
    explicit BlueRocksDirectory(BlueFS *fs) : fs_(fs) {}

    rocksdb::Status Fsync() override {
        fs_->sync_metadata(false);
        return rocksdb::Status::OK();
    }
};

class BlueRocksFileLock : public rocksdb::FileLock {
public:
    BlueFS *fs;
    BlueFS::FileLock *lock;

    BlueRocksFileLock(BlueFS *fs, BlueFS::FileLock *l)
        : fs(fs), lock(l) {}
    ~BlueRocksFileLock() override {
        if (lock) {
            // Safety net: if caller destroyed the wrapper without calling
            // UnlockFile, release the underlying BlueFS lock.
            fs->unlock_file(lock);
        }
    }
};

}  // anonymous namespace

// =====================================================================
// BlueRocksEnv
// =====================================================================

BlueRocksEnv::BlueRocksEnv(BlueFS *fs)
    : EnvWrapper(rocksdb::Env::Default()),
      fs_(fs) {}

rocksdb::Status BlueRocksEnv::NewSequentialFile(
    const std::string &fname,
    std::unique_ptr<rocksdb::SequentialFile> *result,
    const rocksdb::EnvOptions &options) {
    if (!fname.empty() && fname[0] == '/')
        return target()->NewSequentialFile(fname, result, options);
    auto [dir, file] = split(fname);
    BlueFS::FileReader *h;
    int r = fs_->open_for_read(dir, file, &h, false);
    if (r < 0) return err_to_status(r);
    result->reset(new BlueRocksSequentialFile(fs_, h));
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::NewRandomAccessFile(
    const std::string &fname,
    std::unique_ptr<rocksdb::RandomAccessFile> *result,
    const rocksdb::EnvOptions &options) {
    if (!fname.empty() && fname[0] == '/')
        return target()->NewRandomAccessFile(fname, result, options);
    auto [dir, file] = split(fname);
    BlueFS::FileReader *h;
    int r = fs_->open_for_read(dir, file, &h, true);
    if (r < 0) return err_to_status(r);
    result->reset(new BlueRocksRandomAccessFile(fs_, h));
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::NewWritableFile(
    const std::string &fname,
    std::unique_ptr<rocksdb::WritableFile> *result,
    const rocksdb::EnvOptions &options) {
    auto [dir, file] = split(fname);
    BlueFS::FileWriter *h;
    int r = fs_->open_for_write(dir, file, &h, false);
    if (r < 0) return err_to_status(r);
    result->reset(new BlueRocksWritableFile(fs_, h));
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::ReuseWritableFile(
    const std::string &new_fname,
    const std::string &old_fname,
    std::unique_ptr<rocksdb::WritableFile> *result,
    const rocksdb::EnvOptions &options) {
    auto [old_dir, old_file] = split(old_fname);
    auto [new_dir, new_file] = split(new_fname);

    int r = fs_->rename(old_dir, old_file, new_dir, new_file);
    if (r < 0) return err_to_status(r);

    BlueFS::FileWriter *h;
    r = fs_->open_for_write(new_dir, new_file, &h, true);
    if (r < 0) return err_to_status(r);
    result->reset(new BlueRocksWritableFile(fs_, h));
    fs_->sync_metadata(false);
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::NewDirectory(
    const std::string &name,
    std::unique_ptr<rocksdb::Directory> *result) {
    if (!name.empty() && name[0] == '/')
        return target()->NewDirectory(name, result);
    if (!fs_->dir_exists(name))
        return rocksdb::Status::NotFound(name, std::strerror(ENOENT));
    result->reset(new BlueRocksDirectory(fs_));
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::FileExists(const std::string &fname) {
    if (!fname.empty() && fname[0] == '/')
        return target()->FileExists(fname);
    auto [dir, file] = split(fname);
    if (fs_->stat(dir, file, nullptr, nullptr) == 0)
        return rocksdb::Status::OK();
    return err_to_status(-ENOENT);
}

rocksdb::Status BlueRocksEnv::GetChildren(
    const std::string &dir,
    std::vector<std::string> *result) {
    result->clear();
    int r = fs_->readdir(dir, result);
    if (r < 0)
        return rocksdb::Status::NotFound(dir, std::strerror(ENOENT));
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::DeleteFile(const std::string &fname) {
    auto [dir, file] = split(fname);
    int r = fs_->unlink(dir, file);
    if (r < 0) return err_to_status(r);
    fs_->sync_metadata(false);
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::CreateDir(const std::string &dirname) {
    int r = fs_->mkdir(dirname);
    if (r < 0) return err_to_status(r);
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::CreateDirIfMissing(const std::string &dirname) {
    int r = fs_->mkdir(dirname);
    if (r < 0 && r != -EEXIST)
        return err_to_status(r);
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::DeleteDir(const std::string &dirname) {
    int r = fs_->rmdir(dirname);
    if (r < 0) return err_to_status(r);
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::GetFileSize(
    const std::string &fname,
    uint64_t *file_size) {
    auto [dir, file] = split(fname);
    int r = fs_->stat(dir, file, file_size, nullptr);
    if (r < 0) return err_to_status(r);
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::GetFileModificationTime(
    const std::string &fname,
    uint64_t *file_mtime) {
    auto [dir, file] = split(fname);
    int r = fs_->stat(dir, file, nullptr, file_mtime);
    if (r < 0) return err_to_status(r);
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::RenameFile(
    const std::string &src,
    const std::string &target) {
    auto [old_dir, old_file] = split(src);
    auto [new_dir, new_file] = split(target);

    int r = fs_->rename(old_dir, old_file, new_dir, new_file);
    if (r < 0) return err_to_status(r);
    fs_->sync_metadata(false);
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::LinkFile(
    const std::string & /*src*/,
    const std::string & /*target*/) {
    cxxlab_abort("LinkFile not supported");
}

rocksdb::Status BlueRocksEnv::AreFilesSame(
    const std::string &first,
    const std::string &second,
    bool *res) {
    for (auto &path : {first, second}) {
        if (!path.empty() && path[0] == '/')
            return target()->AreFilesSame(first, second, res);
        if (fs_->dir_exists(path)) continue;
        auto [dir, file] = split(path);
        int r = fs_->stat(dir, file, nullptr, nullptr);
        if (!r) continue;
        if (r == -ENOENT)
            return rocksdb::Status::NotFound("AreFilesSame", path);
        return err_to_status(r);
    }
    *res = (first == second);
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::LockFile(
    const std::string &fname,
    rocksdb::FileLock **lock) {
    auto [dir, file] = split(fname);
    BlueFS::FileLock *l = nullptr;
    int r = fs_->lock_file(dir, file, &l);
    if (r < 0) return err_to_status(r);
    *lock = new BlueRocksFileLock(fs_, l);
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::UnlockFile(rocksdb::FileLock *lock) {
    auto *l = static_cast<BlueRocksFileLock *>(lock);
    int r = fs_->unlock_file(l->lock);
    l->lock = nullptr;  // prevent double-unlock in destructor
    if (r < 0) return err_to_status(r);
    delete lock;
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::GetAbsolutePath(
    const std::string &db_path,
    std::string *output_path) {
    *output_path = "/" + db_path;
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::NewLogger(
    const std::string & /*fname*/,
    std::shared_ptr<rocksdb::Logger> *result) {
    // Simplified: create a logger that writes to stderr
    result->reset(CreateRocksdbLogger());
    return rocksdb::Status::OK();
}

rocksdb::Status BlueRocksEnv::GetTestDirectory(std::string *path) {
    *path = "cxxlab_rocksdb_test_dir";
    return rocksdb::Status::OK();
}

}  // namespace TOPNSPC
