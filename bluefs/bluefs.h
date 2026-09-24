#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "blk/allocator.h"
#include "blk/block_device.h"
#include "blk/io_context.h"
#include "bluefs/bluefs_config.h"
#include "bluefs/bluefs_types.h"
#include "bluefs/bluefs_volume_selector.h"
#include "common/buffer.h"
#include "common/common_fwd.h"

namespace TOPNSPC {

class BlueFS {
public:
    static constexpr unsigned MAX_BDEV = 3;
    static constexpr unsigned BDEV_WAL = 0;
    static constexpr unsigned BDEV_DB = 1;
    static constexpr unsigned BDEV_SLOW = 2;

    static constexpr unsigned SUPER_OFFSET = 4096;
    static constexpr unsigned SUPER_LENGTH = 4096;

    static constexpr unsigned BLUEFS_LOG_INITIAL = 4 << 20;

    struct File;
    struct Dir;
    struct FileWriter;
    struct FileReader;
    struct FileLock;

    using FileRef = std::shared_ptr<File>;
    using DirRef = std::shared_ptr<Dir>;

    explicit BlueFS(const BlueFSConfig &cfg);
    ~BlueFS();

    // 设备管理
    int add_block_device(unsigned id, const std::string &path, bool trim = false,
                         uint64_t reserved = 0,
                         bluefs_shared_alloc_context_t *shared_alloc = nullptr);
    uint64_t get_block_device_size(unsigned id) const;

    uint64_t get_total(unsigned id) const;
    uint64_t get_free(unsigned id);

    // 卷选择器
    void set_volume_selector(BlueFSVolumeSelector *vs) { vselector_.reset(vs); }

    // 超级块（内部，暴露给测试）
    int _write_super(int dev);
    int _open_super();
    void _init_alloc();
    void _stop_alloc();

    // === Phase 1.4: mkfs / mount / umount ===
    int mkfs(uint64_t bluefs_alloc_size);
    int mount();
    void umount(bool avoid_compact = false);

    // === Phase 1.5: 目录操作 ===
    int mkdir(std::string_view dirname);
    int rmdir(std::string_view dirname);
    bool dir_exists(std::string_view dirname);
    int readdir(std::string_view dirname, std::vector<std::string> *ls);

    // 元数据同步
    int sync_metadata(bool avoid_compact = false);

    // === Phase 1.6: 文件创建/关闭 ===
    int open_for_write(std::string_view dirname, std::string_view filename,
                       FileWriter **h, bool overwrite = false);
    int open_for_read(std::string_view dirname, std::string_view filename,
                      FileReader **h, bool random = false);
    int close_writer(FileWriter *h);
    void close_reader(FileReader *h);

    // === Phase 1.7: 文件读写 ===
    int64_t read(FileReader *h, uint64_t off, size_t len, bufferlist *outbl,
                 char *out = nullptr);
    int64_t read_random(FileReader *h, uint64_t off, uint64_t len, char *out);
    int append_try_flush(FileWriter *h, const char *buf, size_t len);
    int flush(FileWriter *h, bool force = false);
    int fsync(FileWriter *h);

    // === Phase 1.11: 文件管理 ===
    int unlink(std::string_view dirname, std::string_view filename);
    int truncate(FileWriter *h, uint64_t offset);

    // 文件操作
    int stat(std::string_view dirname, std::string_view filename,
             uint64_t *size, uint64_t *mtime = nullptr);
    int rename(std::string_view old_dirname, std::string_view old_filename,
               std::string_view new_dirname, std::string_view new_filename);

    // BlueRocksEnv 接口
    int lock_file(std::string_view dirname, std::string_view filename,
                  FileLock **p);
    int unlock_file(FileLock *l);
    void invalidate_cache(FileRef f, uint64_t offset, uint64_t len);
    void flush_range(FileWriter *h, uint64_t offset, uint64_t length);
    int preallocate(FileRef f, uint64_t offset, uint64_t len);
    uint64_t get_used();
    uint64_t get_used(unsigned id);

    // 测试/调试
    const bluefs_super_t &get_super() const { return super_; }
    bluefs_super_t &get_mutable_super() { return super_; }
    int compact_log();

private:
    // =====================================================================
    // 内存数据结构
    // =====================================================================

public:
    struct File {
        bluefs_fnode_t fnode;
        uint64_t dirty_seq = 0;
        int refs = 0;
        bool locked = false;
        bool deleted = false;
        bool is_dirty = false;
        void *vselector_hint = nullptr;

        std::atomic_int num_readers{0};
        std::atomic_int num_writers{0};
        std::atomic_int num_reading{0};

        File() = default;
        explicit File(uint64_t ino) : fnode(ino, 0, 0) {}
    };

    struct Dir {
        std::map<std::string, FileRef> file_map;
        Dir() = default;
    };

    struct FileWriter {
        FileRef file;
        uint64_t pos = 0;
        bufferlist buffer;
        bufferlist tail_block;
        std::mutex lock;

        int writer_type = 0;
        int write_hint = 0;

        std::vector<IOContext *> iocv{nullptr, nullptr, nullptr};
        std::vector<bool> dirty_devs{false, false, false};

        explicit FileWriter(FileRef f) : file(std::move(f)) {
            ++file->num_writers;
        }
        ~FileWriter() {
            --file->num_writers;
        }

        unsigned get_buffer_length() const { return buffer.length(); }
        uint64_t get_effective_write_pos() const { return pos + buffer.length(); }

        bufferlist flush_buffer(uint64_t block_size, uint64_t block_mask);

        void append(const char *buf, size_t len) {
            buffer.append(buf, len);
        }
        void append(bufferlist &bl) {
            buffer.claim_append(bl);
        }
    };

    struct FileReaderBuffer {
        uint64_t bl_off = 0;
        bufferlist bl;
        uint64_t pos = 0;
        uint64_t max_prefetch = 0;

        FileReaderBuffer() = default;
        explicit FileReaderBuffer(uint64_t mpf) : max_prefetch(mpf) {}

        uint64_t get_buf_end() const { return bl_off + bl.length(); }
        uint64_t get_buf_remaining(uint64_t p) const {
            if (p >= bl_off && p < bl_off + bl.length())
                return bl_off + bl.length() - p;
            return 0;
        }

        void skip(size_t n) { pos += n; }

        void invalidate_cache(uint64_t offset, uint64_t length) {
            if (offset >= bl_off && offset < get_buf_end()) {
                bl.clear();
                bl_off = 0;
            }
        }
    };

    struct FileReader {
        FileRef file;
        FileReaderBuffer buf;
        bool random = false;
        bool ignore_eof = false;

        FileReader() = default;
        FileReader(FileRef f, uint64_t mpf, bool rand, bool ie)
            : file(f), buf(mpf), random(rand), ignore_eof(ie) {
            ++file->num_readers;
        }
        ~FileReader() {
            --file->num_readers;
        }
    };

    struct FileLock {
        FileRef file;
        explicit FileLock(FileRef f) : file(std::move(f)) {}
    };

private:
    BlueFSConfig cfg_;

    // 设备管理
    // 所有权说明：
    // - bdev_[id]: BlueFS 拥有，通过 add_block_device() 创建，析构时删除
    // - ioc_[id]: BlueFS 拥有，与 bdev_ 对应，析构时删除
    // - alloc_[id]: 分两种情况
    //   * 普通分配器: BlueFS 拥有，通过 Allocator::create() 创建
    //   * 共享分配器 (is_shared_alloc(id) == true): 非拥有，指向外部对象
    //     (通常是 BlueStore 的分配器)，由外部管理生命周期
    std::vector<BlockDevice *> bdev_;
    std::vector<IOContext *> ioc_;
    std::vector<uint64_t> block_reserved_;
    std::vector<Allocator *> alloc_;
    std::vector<uint64_t> alloc_size_;

    // 共享分配上下文
    // 指向外部分配器（如 BlueStore 的分配器），BlueFS 只是使用者，不拥有
    // 当 alloc_[id] 指向 shared_alloc_->a 时，BlueFS 不应删除该分配器
    static constexpr unsigned NO_SHARED = unsigned(-1);
    bluefs_shared_alloc_context_t *shared_alloc_ = nullptr;
    unsigned shared_alloc_id_ = NO_SHARED;
    bool is_shared_alloc(unsigned id) const { return id == shared_alloc_id_; }

    // 超级块
    bluefs_super_t super_;

    // 卷选择器
    std::unique_ptr<BlueFSVolumeSelector> vselector_;

    // 目录 / 文件
    struct {
        std::mutex lock;
        std::map<std::string, DirRef> dir_map;
        std::unordered_map<uint64_t, FileRef> file_map;
    } nodes_;

    uint64_t ino_last_ = 0;

    // 日志状态
    struct {
        std::mutex lock;
        uint64_t seq_live = 1;
        FileWriter *writer = nullptr;
        bluefs_transaction_t t;
    } log_;

    // 脏文件跟踪
    struct {
        std::mutex lock;
        uint64_t seq_stable = 0;
        uint64_t seq_live = 1;
        std::map<uint64_t, std::vector<FileRef>> files;
        std::vector<std::map<uint64_t, uint64_t>> pending_release;
    } dirty_;

    // 内部辅助
    uint64_t _get_total(unsigned id) const;
    FileRef _get_file(uint64_t ino);
    void _drop_link(FileRef file);
    bool _file_exists(uint64_t ino);

    // 分配
    int _allocate(uint8_t prefer_bdev, uint64_t len, uint64_t alloc_unit,
                  bluefs_fnode_t *node, uint64_t *hint = nullptr);

    // 写入器
    FileWriter *_create_writer(FileRef f);
    void _drain_writer(FileWriter *h);
    void _close_writer(FileWriter *h);
    void _flush_bdev(FileWriter *h);
    void _flush_special(FileWriter *h);
    int _flush_data(FileWriter *h, uint64_t offset, uint64_t length,
                    bool buffered);
    int _flush_F(FileWriter *h, bool force);
    int _flush_range_F(FileWriter *h, uint64_t offset, uint64_t length);

    // 读取
    int64_t _read(FileReader *h, uint64_t off, size_t len, bufferlist *outbl,
                  char *out);
    int64_t _read_random(FileReader *h, uint64_t off, uint64_t len, char *out);

    // 日志压缩
    uint64_t _estimate_log_size();
    bool _should_compact();
    void _compact_log_dump_metadata(bluefs_transaction_t *t);
    int _compact_log_async();
    void _maybe_compact_log();
    std::atomic<bool> log_is_compacting_{false};

    // 日志
    int _flush_log_data(bufferlist &bl);
    int _replay(bool no_stdout);
    void _signal_dirty_to_log(FileWriter *h);
    int _flush_and_sync_log(uint64_t want_seq = 0);
    int _maybe_extend_log();
    int _consume_dirty(uint64_t seq);
};

}  // namespace TOPNSPC
