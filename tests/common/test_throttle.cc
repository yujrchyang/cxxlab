// Copyright 2024 cxxlab authors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "common/throttle.h"

using namespace TOPNSPC;

// ============================================================================
// 功能测试
// ============================================================================

TEST(Throttle, BasicGetPut) {
    Throttle t(100);
    ASSERT_EQ(t.get_current(), 0);
    ASSERT_EQ(t.get_max(), 100);

    t.get(30);
    ASSERT_EQ(t.get_current(), 30);

    t.get(20);
    ASSERT_EQ(t.get_current(), 50);

    t.put(10);
    ASSERT_EQ(t.get_current(), 40);

    t.put(40);
    ASSERT_EQ(t.get_current(), 0);
}

TEST(Throttle, GetOrFailSuccess) {
    Throttle t(100);
    ASSERT_TRUE(t.get_or_fail(50));
    ASSERT_EQ(t.get_current(), 50);

    ASSERT_TRUE(t.get_or_fail(30));
    ASSERT_EQ(t.get_current(), 80);
}

TEST(Throttle, GetOrFailInsufficientCapacity) {
    Throttle t(100);
    ASSERT_TRUE(t.get_or_fail(80));
    ASSERT_EQ(t.get_current(), 80);

    // 容量不足
    ASSERT_FALSE(t.get_or_fail(30));
    ASSERT_EQ(t.get_current(), 80);
}

TEST(Throttle, TryGetSuccess) {
    Throttle t(100);
    ASSERT_TRUE(t.try_get(50, std::chrono::milliseconds(100)));
    ASSERT_EQ(t.get_current(), 50);
}

TEST(Throttle, TryGetTimeout) {
    Throttle t(100);
    t.get(100);  // 占满

    auto start = std::chrono::steady_clock::now();
    ASSERT_FALSE(t.try_get(10, std::chrono::milliseconds(100)));
    auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 90);
}

TEST(Throttle, TakeUnconditional) {
    Throttle t(100);
    ASSERT_TRUE(t.get_or_fail(80));
    ASSERT_EQ(t.get_current(), 80);

    // take 可以超过 max
    uint64_t result = t.take(50);
    ASSERT_EQ(result, 130);
    ASSERT_EQ(t.get_current(), 130);
}

TEST(Throttle, GetBlocksWhenFull) {
    Throttle t(100);
    t.get(100);

    std::atomic<bool> got{false};
    std::thread worker([&] {
        t.get(10);
        got.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_FALSE(got.load());

    t.put(10);
    worker.join();
    ASSERT_TRUE(got.load());
    ASSERT_EQ(t.get_current(), 100);
}

TEST(Throttle, ResetMax) {
    Throttle t(100);
    t.get(80);
    ASSERT_EQ(t.get_max(), 100);

    t.reset_max(200);
    ASSERT_EQ(t.get_max(), 200);
    ASSERT_EQ(t.get_current(), 80);

    // 现在可以再获取
    ASSERT_TRUE(t.get_or_fail(100));
    ASSERT_EQ(t.get_current(), 180);
}

TEST(Throttle, ResetCount) {
    Throttle t(100);
    t.get(80);
    ASSERT_EQ(t.get_current(), 80);

    t.reset();
    ASSERT_EQ(t.get_current(), 0);
}

TEST(Throttle, ResetWakesWaiters) {
    Throttle t(100);
    t.get(100);

    std::atomic<bool> got{false};
    std::thread worker([&] {
        t.get(10);
        got.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_FALSE(got.load());

    t.reset();
    worker.join();
    ASSERT_TRUE(got.load());
}

TEST(Throttle, PastMidpoint) {
    Throttle t(100);
    ASSERT_FALSE(t.past_midpoint());

    t.get(49);
    ASSERT_FALSE(t.past_midpoint());

    t.get(1);
    ASSERT_TRUE(t.past_midpoint());

    t.get(10);
    ASSERT_TRUE(t.past_midpoint());
}

TEST(Throttle, GetAvailable) {
    Throttle t(100);
    ASSERT_EQ(t.get_available(), 100);

    t.get(30);
    ASSERT_EQ(t.get_available(), 70);

    t.take(80);  // 强制获取，count = 110，超过 max
    ASSERT_EQ(t.get_available(), 0);  // 超过 max 时返回 0
}

TEST(Throttle, ConcurrentAccess) {
    Throttle t(1000);
    std::atomic<int> count{0};
    const int num_threads = 10;
    const int ops_per_thread = 100;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < ops_per_thread; ++j) {
                t.get(1);
                count.fetch_add(1);
                t.put(1);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    ASSERT_EQ(count.load(), num_threads * ops_per_thread);
    ASSERT_EQ(t.get_current(), 0);
}

// ============================================================================
// FIFO 公平性测试
// ============================================================================

TEST(Throttle, FIFOOrder) {
    Throttle t(100);
    t.get(100);  // 占满

    std::vector<int> order;
    std::mutex order_mutex;

    std::thread t1([&] {
        t.get(10);
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(1);
        t.put(10);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::thread t2([&] {
        t.get(10);
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(2);
        t.put(10);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::thread t3([&] {
        t.get(10);
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(3);
        t.put(10);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    t.put(100);  // 释放资源，按 FIFO 顺序唤醒

    t1.join();
    t2.join();
    t3.join();

    ASSERT_EQ(order.size(), 3);
    ASSERT_EQ(order[0], 1);
    ASSERT_EQ(order[1], 2);
    ASSERT_EQ(order[2], 3);
}

TEST(Throttle, GetOrFailRespectsQueue) {
    Throttle t(100);
    t.get(100);

    // 启动一个等待者
    std::thread waiter([&] {
        t.get(10);
        t.put(10);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 即使有容量（100 - 100 = 0，但 put 后会释放），
    // get_or_fail 也应该返回 false（因为有排队等待者）
    // 注意：这里当前容量是 0，所以 get_or_fail 会失败
    ASSERT_FALSE(t.get_or_fail(10));

    t.put(100);  // 释放资源，唤醒等待者
    waiter.join();
}

TEST(Throttle, OversizedRequestNoDeadlock) {
    Throttle t(100);

    // 超大请求 (n > max) 在 count=0 时应该立即返回
    t.get(150);
    ASSERT_EQ(t.get_current(), 150);

    t.put(150);
    ASSERT_EQ(t.get_current(), 0);
}

TEST(Throttle, OversizedRequestBlocksWhenOverMax) {
    Throttle t(100);
    t.take(120);  // 强制超过 max

    std::atomic<bool> got{false};
    std::thread worker([&] {
        t.get(150);  // 超大请求，但 count > max，应该阻塞
        got.store(true);
        t.put(150);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_FALSE(got.load());

    t.put(120);  // 释放，count 回到 0
    worker.join();
    ASSERT_TRUE(got.load());
}

// ============================================================================
// 性能测试
// ============================================================================

TEST(Throttle, HighConcurrency) {
    Throttle t(10000);
    std::atomic<int> success_count{0};
    const int num_threads = 100;
    const int ops_per_thread = 10;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < ops_per_thread; ++j) {
                t.get(10);
                success_count.fetch_add(1);
                t.put(10);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    ASSERT_EQ(success_count.load(), num_threads * ops_per_thread);
    ASSERT_EQ(t.get_current(), 0);
}

TEST(Throttle, ExhaustionAndRecovery) {
    Throttle t(100);
    t.get(100);  // 占满

    std::atomic<int> acquired{0};
    const int num_workers = 5;

    std::vector<std::thread> workers;
    for (int i = 0; i < num_workers; ++i) {
        workers.emplace_back([&] {
            t.get(10);
            acquired.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            t.put(10);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_EQ(acquired.load(), 0);  // 全部阻塞

    t.put(100);  // 释放资源

    for (auto& worker : workers) {
        worker.join();
    }

    ASSERT_EQ(acquired.load(), num_workers);
    ASSERT_EQ(t.get_current(), 0);
}

TEST(Throttle, PutWakesBlockedRequest) {
    Throttle t(100);
    t.get(90);

    std::atomic<bool> got{false};
    std::thread worker([&] {
        t.get(20);  // 需要 20，但只有 10 可用
        got.store(true);
        t.put(20);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_FALSE(got.load());

    t.put(15);  // 释放 15，现在有 25 可用
    worker.join();
    ASSERT_TRUE(got.load());
}
