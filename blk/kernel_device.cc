#include "blk/kernel_device.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <unistd.h>

#include "blk/aio.h"
#include "blk/io_context.h"
#include "common/cassert.h"

namespace TOPNSPC {

static constexpr int kMaxReap = 256;

namespace {

int64_t _get_device_size(int fd) {
    int64_t s = ::lseek(fd, 0, SEEK_END);
    if (s < 0)
        return -errno;
    return s;
}

int _get_block_size(int fd) {
#ifdef BLKSSZGET
    int val = 0;
    if (::ioctl(fd, BLKSSZGET, &val) == 0 && val > 0)
        return val;
#endif
    return 4096;
}

int _get_optimal_io_size(int fd) {
#ifdef BLKIOOPT
    int val = 0;
    if (::ioctl(fd, BLKIOOPT, &val) == 0 && val > 0)
        return val;
#endif
    return 0;
}

bool _is_rotational(int fd) {
#ifdef BLKROTATIONAL
    int val = 1;
    ::ioctl(fd, BLKROTATIONAL, &val);
    return val != 0;
#else
    return true;
#endif
}

bool _supports_discard(int fd) {
#ifdef BLKDISCARD
    // 用零长度 range 探测：不下发真实 discard，r==0 表示支持
    uint64_t range[2] = {0, 0};
    int r = ::ioctl(fd, BLKDISCARD, &range);
    return r == 0;
#else
    return false;
#endif
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------
KernelDevice::~KernelDevice() {
    close();
}

int KernelDevice::open(const std::string &path) {
    fd_direct_ = ::open(path.c_str(), O_RDWR | O_DIRECT | O_CLOEXEC);
    if (fd_direct_ < 0)
        return -errno;

    fd_buffered_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_buffered_ < 0) {
        int saved = errno;
        ::close(fd_direct_);
        fd_direct_ = -1;
        return -saved;
    }

    // Acquire exclusive lock to prevent concurrent access
    if (::flock(fd_direct_, LOCK_EX | LOCK_NB) < 0) {
        int saved = errno;
        ::close(fd_direct_);
        ::close(fd_buffered_);
        fd_direct_ = fd_buffered_ = -1;
        return -saved;
    }

    size = _get_device_size(fd_direct_);
    block_size = _get_block_size(fd_direct_);
    optimal_io_size = _get_optimal_io_size(fd_direct_);
    rotational = _is_rotational(fd_direct_);
    support_discard_ = _supports_discard(fd_direct_);

    // Align size down to block boundary to avoid I/O errors at device end
    if (size > 0 && block_size > 0) {
        size = (size / block_size) * block_size;
    }

    unsigned iodepth = std::max(16u, std::min(128u, (unsigned)(size / block_size / 4)));
    if (iodepth < 16)
        iodepth = 16;

    io_queue_ = std::make_unique<aio_queue_t>(iodepth);
    {
        std::vector<int> fds{fd_direct_};
        int r = io_queue_->init(fds);
        if (r < 0) {
            ::close(fd_direct_);
            ::close(fd_buffered_);
            fd_direct_ = fd_buffered_ = -1;
            io_queue_.reset();
            return r;
        }
    }

    ::posix_fadvise(fd_direct_, 0, 0, POSIX_FADV_RANDOM);
    ::posix_fadvise(fd_buffered_, 0, 0, POSIX_FADV_RANDOM);

    stop_.store(false);
    aio_thread_ = std::thread(&KernelDevice::_aio_thread, this);

    return 0;
}

void KernelDevice::close() {
    if (fd_direct_ < 0 && fd_buffered_ < 0)
        return;

    stop_.store(true);
    if (aio_thread_.joinable())
        aio_thread_.join();

    if (io_queue_) {
        io_queue_->shutdown();
        io_queue_.reset();
    }

    if (fd_direct_ >= 0) {
        ::close(fd_direct_);
        fd_direct_ = -1;
    }
    if (fd_buffered_ >= 0) {
        ::close(fd_buffered_);
        fd_buffered_ = -1;
    }
}

// ---------------------------------------------------------------------------
// Synchronous IO
// ---------------------------------------------------------------------------
int KernelDevice::read(uint64_t off, uint64_t len, bufferlist *pbl,
                       IOContext *ioc, bool buffered) {
    if (!buffered && !is_valid_io(off, len))
        return -EINVAL;

    int fd = buffered ? fd_buffered_ : fd_direct_;
    if (fd < 0)
        return -EBADF;

    auto buf = TOPNSPC::buffer::create_aligned(len, block_size);
    int r = ::pread(fd, buf->get_data(), len, off);
    if (r < 0)
        return -errno;

    pbl->push_back(std::move(buf));
    return r;
}

int KernelDevice::read_random(uint64_t off, uint64_t len, char *buf,
                              bool buffered) {
    if (len == 0 || off + len < off)
        return -EINVAL;

    int fd = buffered ? fd_buffered_ : fd_direct_;
    if (fd < 0)
        return -EBADF;

    // O_DIRECT 未对齐读：对齐读整块后 memcpy 子集
    if (!buffered && (off & (block_size - 1) || len & (block_size - 1))) {
        uint64_t aligned_off = off & ~static_cast<uint64_t>(block_size - 1);
        uint64_t aligned_end =
            (off + len + block_size - 1) & ~static_cast<uint64_t>(block_size - 1);
        uint64_t aligned_len = aligned_end - aligned_off;

        auto aligned_buf =
            TOPNSPC::buffer::create_aligned(aligned_len, block_size);
        int r = ::pread(fd, aligned_buf->get_data(), aligned_len, aligned_off);
        if (r < 0)
            return -errno;
        if (uint64_t(r) < (off - aligned_off) + len)
            return -EIO;
        memcpy(buf, aligned_buf->get_data() + (off - aligned_off), len);
        return len;
    }

    int r = ::pread(fd, buf, len, off);
    if (r < 0)
        return -errno;
    return r;
}

int KernelDevice::write(uint64_t off, bufferlist &bl, bool buffered,
                        int write_hint) {
    if (!buffered && !is_valid_io(off, bl.length()))
        return -EINVAL;

    int fd = buffered ? fd_buffered_ : fd_direct_;
    if (fd < 0)
        return -EBADF;

    if (!buffered)
        bl.rebuild_aligned(block_size);

    std::vector<struct iovec> iov;
    bl.prepare_iov(&iov);

    size_t left = bl.length();
    size_t idx = 0;
    uint64_t o = off;
    while (left > 0) {
        ssize_t r = ::pwritev(fd, &iov[idx], iov.size() - idx, o);
        if (r < 0)
            return -errno;

        size_t written = r;
        left -= written;
        o += written;

        while (written > 0 && idx < iov.size()) {
            if (written < (size_t)iov[idx].iov_len) {
                iov[idx].iov_base = (char *)iov[idx].iov_base + written;
                iov[idx].iov_len -= written;
                written = 0;
            } else {
                written -= iov[idx].iov_len;
                ++idx;
            }
        }
    }

    io_since_flush_.store(true);
    return bl.length();
}

int KernelDevice::flush() {
    std::lock_guard l(flush_mutex_);
    bool expect = true;
    if (!io_since_flush_.compare_exchange_strong(expect, false))
        return 0;

    int r = ::fdatasync(fd_direct_);
    if (r < 0)
        return -errno;
    return 0;
}

// ---------------------------------------------------------------------------
// Async IO
// ---------------------------------------------------------------------------
int KernelDevice::aio_read(uint64_t off, uint64_t len, bufferlist *pbl,
                           IOContext *ioc) {
    if (!is_valid_io(off, len))
        return -EINVAL;

    if (aio_ && dio_) {
        auto buf = TOPNSPC::buffer::create_aligned(len, block_size);

        ioc->pending_aios.emplace_back(ioc, fd_direct_);
        auto &aio = ioc->pending_aios.back();
        aio.bl.push_back(std::move(buf));
        aio.bl.prepare_iov(&aio.iov);
        aio.preadv(off, len);
        pbl->append(aio.bl);
        ioc->num_pending.fetch_add(1);
        return 0;
    }

    return read(off, len, pbl, ioc, false);
}

int KernelDevice::aio_write(uint64_t off, bufferlist &bl, IOContext *ioc,
                            bool buffered, int write_hint) {
    if (!is_valid_io(off, bl.length()))
        return -EINVAL;

    if (aio_ && !buffered && dio_) {
        bl.rebuild_aligned(block_size);

        ioc->pending_aios.emplace_back(ioc, fd_direct_);
        auto &aio = ioc->pending_aios.back();
        bl.prepare_iov(&aio.iov);
        aio.bl.claim_append(bl);
        aio.pwritev(off, aio.bl.length());
        ioc->num_pending.fetch_add(1);
        return 0;
    }

    return write(off, bl, buffered, write_hint);
}

void KernelDevice::aio_submit(IOContext *ioc) {
    int num = ioc->num_pending.load();
    if (num == 0)
        return;

    auto old_begin = ioc->running_aios.begin();
    ioc->running_aios.splice(ioc->running_aios.begin(), ioc->pending_aios);
    ioc->num_running.fetch_add(num);
    ioc->num_pending.store(0);

    int retries = 16;
    int r = io_queue_->submit_batch(ioc->running_aios.begin(), old_begin,
                                    ioc, &retries);
    if (r < 0) {
        // 提交失败（EAGAIN 耗尽或非 EAGAIN 错误）：fail-fast 对齐 Ceph
        // 防止未提交 aio 留在 running_aios 导致 num_running 永不归零 → aio_wait 死锁
        ioc->set_return_value(r);
        cxxlab_assert(false);
    }
}

// ---------------------------------------------------------------------------
// Discard
// ---------------------------------------------------------------------------
int KernelDevice::discard(uint64_t off, uint64_t len) {
    if (!support_discard_)
        return -EOPNOTSUPP;
    if (off + len < off)
        return -EINVAL;

    uint64_t range[2] = {off, len};
    int r = ::ioctl(fd_direct_, BLKDISCARD, &range);
    if (r < 0)
        return -errno;
    return 0;
}

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------
int KernelDevice::invalidate_cache(uint64_t off, uint64_t len) {
    return ::posix_fadvise(fd_direct_, off, len, POSIX_FADV_DONTNEED);
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------
int KernelDevice::collect_metadata(
    const std::string &prefix,
    std::map<std::string, std::string> *pm) const {
    (*pm)[prefix + "type"] = "kernel";
    (*pm)[prefix + "rotational"] = rotational ? "1" : "0";
    (*pm)[prefix + "size"] = std::to_string(size);
    (*pm)[prefix + "block_size"] = std::to_string(block_size);
    (*pm)[prefix + "optimal_io_size"] = std::to_string(optimal_io_size);
    (*pm)[prefix + "support_discard"] = support_discard_ ? "true" : "false";
    return 0;
}

// ---------------------------------------------------------------------------
// AIO completion thread
// ---------------------------------------------------------------------------
void KernelDevice::_aio_thread() {
    while (!stop_.load()) {
        aio_t *aios[kMaxReap];
        int r = io_queue_->get_next_completed(50, aios, kMaxReap);
        if (r < 0)
            continue;

        for (int i = 0; i < r; ++i) {
            io_since_flush_.store(true);

            auto *ioc = static_cast<IOContext *>(aios[i]->priv);
            if (!ioc)
                continue;

            long res = aios[i]->get_return_value();
            if (res < 0 || (uint64_t)res != aios[i]->length) {
                ioc->set_return_value(-EIO);
                // 不 continue，fall-through 到 num_running 递减 + 唤醒/回调
            }

            if (ioc->priv && aio_callback) {
                if (ioc->num_running.fetch_sub(1) == 1)
                    aio_callback(aio_callback_priv, ioc);
            } else {
                // wait mode: signal the condition variable
                ioc->try_aio_wake();
            }
        }
    }
}

}  // namespace TOPNSPC
