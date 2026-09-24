// Copyright 2024 cxxlab authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <list>
#include <mutex>

#include "common_fwd.h"

namespace TOPNSPC {

// Throttle: 基于槽位的资源节流器，支持 FIFO 公平排队
//
// 采用 Ceph Throttle 的 per-waiter condition_variable 模式，保证先到先得。
// 超大请求 (n > max) 仅在 count > max 时等待，允许单次通过以防止死锁。
class Throttle {
public:
    explicit Throttle(uint64_t max);
    ~Throttle();

    Throttle(const Throttle&) = delete;
    Throttle& operator=(const Throttle&) = delete;

    // 阻塞获取 n 个资源（FIFO 公平排队）
    void get(uint64_t n);

    // 非阻塞尝试（检查容量 + 队列公平性）
    // 返回 true 表示成功获取，false 表示容量不足或有排队等待者
    bool get_or_fail(uint64_t n);

    // 带超时的尝试获取（cxxlab 扩展）
    // 返回 true 表示成功获取，false 表示超时
    bool try_get(uint64_t n, std::chrono::milliseconds timeout);

    // 无条件获取（强制，用于必须执行的场景）
    // 返回获取后的 count 值
    uint64_t take(uint64_t n);

    // 归还 n 个资源，唤醒队首等待者
    void put(uint64_t n);

    // 重置上限（动态调整）
    void reset_max(uint64_t new_max);

    // 重置计数为 0，唤醒所有等待者
    void reset();

    // 查询当前状态
    uint64_t get_current() const;
    uint64_t get_max() const;
    uint64_t get_available() const;
    bool past_midpoint() const;

private:
    // 判断是否应该等待
    bool should_wait(uint64_t n) const;

    std::atomic<uint64_t> count_{0};
    std::atomic<uint64_t> max_;
    mutable std::mutex lock_;
    std::list<std::condition_variable> conds_;
};

}  // namespace TOPNSPC
