// Copyright 2024 cxxlab authors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>

#include "throttle.h"

namespace TOPNSPC {

Throttle::Throttle(uint64_t max) : max_(max) {}

Throttle::~Throttle() = default;

bool Throttle::should_wait(uint64_t n) const {
    uint64_t m = max_.load(std::memory_order_relaxed);
    uint64_t cur = count_.load(std::memory_order_relaxed);
    // 正常请求：获取后会超限则等待
    // 超大请求：仅在已超限时等待（防止死锁）
    return m &&
           ((n <= m && cur + n > m) ||
            (n >= m && cur > m));
}

void Throttle::get(uint64_t n) {
    std::unique_lock<std::mutex> lock(lock_);
    if (!should_wait(n) && conds_.empty()) {
        count_.fetch_add(n, std::memory_order_relaxed);
        return;
    }

    // FIFO 排队：在链表尾部添加自己的 CV
    conds_.emplace_back();
    auto it = std::prev(conds_.end());

    // 等待直到：不超限 且 位于队首
    it->wait(lock, [this, n, &cv = *it] {
        return !should_wait(n) && &cv == &conds_.front();
    });

    // 获取资源
    count_.fetch_add(n, std::memory_order_relaxed);

    // 移除自己的 CV
    conds_.erase(it);

    // 唤醒下一个等待者
    if (!conds_.empty()) {
        conds_.front().notify_one();
    }
}

bool Throttle::get_or_fail(uint64_t n) {
    std::lock_guard<std::mutex> lock(lock_);
    // 如果有等待者，即使有容量也返回 false（保证公平性）
    if (should_wait(n) || !conds_.empty()) {
        return false;
    }
    count_.fetch_add(n, std::memory_order_relaxed);
    return true;
}

bool Throttle::try_get(uint64_t n, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(lock_);
    if (!should_wait(n) && conds_.empty()) {
        count_.fetch_add(n, std::memory_order_relaxed);
        return true;
    }

    // FIFO 排队
    conds_.emplace_back();
    auto it = std::prev(conds_.end());

    // 带超时等待
    bool success = it->wait_for(lock, timeout, [this, n, &cv = *it] {
        return !should_wait(n) && &cv == &conds_.front();
    });

    if (!success) {
        // 超时，移除 CV
        conds_.erase(it);
        return false;
    }

    // 获取资源
    count_.fetch_add(n, std::memory_order_relaxed);

    // 移除自己的 CV
    conds_.erase(it);

    // 唤醒下一个等待者
    if (!conds_.empty()) {
        conds_.front().notify_one();
    }
    return true;
}

uint64_t Throttle::take(uint64_t n) {
    std::lock_guard<std::mutex> lock(lock_);
    count_.fetch_add(n, std::memory_order_relaxed);
    return count_.load(std::memory_order_relaxed);
}

void Throttle::put(uint64_t n) {
    std::lock_guard<std::mutex> lock(lock_);
    count_.fetch_sub(n, std::memory_order_relaxed);
    // 只唤醒队首等待者
    if (!conds_.empty()) {
        conds_.front().notify_one();
    }
}

void Throttle::reset_max(uint64_t new_max) {
    std::lock_guard<std::mutex> lock(lock_);
    max_.store(new_max, std::memory_order_relaxed);
}

void Throttle::reset() {
    std::lock_guard<std::mutex> lock(lock_);
    count_.store(0, std::memory_order_relaxed);
    // 唤醒所有等待者
    for (auto& cv : conds_) {
        cv.notify_all();
    }
}

uint64_t Throttle::get_current() const {
    return count_.load(std::memory_order_relaxed);
}

uint64_t Throttle::get_max() const {
    return max_.load(std::memory_order_relaxed);
}

uint64_t Throttle::get_available() const {
    uint64_t m = max_.load(std::memory_order_relaxed);
    uint64_t cur = count_.load(std::memory_order_relaxed);
    return (m > cur) ? (m - cur) : 0;
}

bool Throttle::past_midpoint() const {
    uint64_t m = max_.load(std::memory_order_relaxed);
    uint64_t cur = count_.load(std::memory_order_relaxed);
    return cur >= (m / 2);
}

}  // namespace TOPNSPC
