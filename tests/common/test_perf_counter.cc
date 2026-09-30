// Copyright 2024 cxxlab authors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

#include "common/formatter.h"
#include "common/perf_counter.h"

using namespace TOPNSPC;

namespace {
enum TestCounter {
    l_test_first = 0,
    l_test_u64_gauge = 1,
    l_test_u64_counter = 2,
    l_test_u64_avg = 3,
    l_test_time_avg = 4,
    l_test_last,
};

std::unique_ptr<PerfCounters> make_test_counters() {
    PerfCountersBuilder b("test", l_test_first, l_test_last);
    b.add_u64(l_test_u64_gauge, "gauge", "a gauge", "g",
              PerfCountersBuilder::PRIO_USEFUL, UNIT_BYTES);
    b.add_u64_counter(l_test_u64_counter, "counter", "a counter", "c",
                      PerfCountersBuilder::PRIO_CRITICAL);
    b.add_u64_avg(l_test_u64_avg, "u64_avg", "an integer avg", "ua",
                  PerfCountersBuilder::PRIO_USEFUL);
    b.add_time_avg(l_test_time_avg, "time_avg", "a time avg", "ta",
                   PerfCountersBuilder::PRIO_CRITICAL);
    return b.create_perf_counters();
}
}  // namespace

// ============================================================================
// PerfCountersBuilder + PerfCounters construction
// ============================================================================

TEST(PerfCounterTest, BuilderCreatesCounters) {
    auto pc = make_test_counters();
    ASSERT_NE(pc, nullptr);
    EXPECT_EQ(pc->get_name(), "test");
    EXPECT_TRUE(pc->is_enabled());
}

TEST(PerfCounterTest, BuilderRejectsOutOfRangeIndex) {
    PerfCountersBuilder b("bad", l_test_first, l_test_last);
    EXPECT_DEATH(b.add_u64(l_test_first, "zero"), ".*");
    EXPECT_DEATH(b.add_u64(l_test_last, "last"), ".*");
}

// ============================================================================
// inc / dec / set / get
// ============================================================================

TEST(PerfCounterTest, IncCounter) {
    auto pc = make_test_counters();
    pc->inc(l_test_u64_counter);
    pc->inc(l_test_u64_counter, 9);
    EXPECT_EQ(pc->get(l_test_u64_counter), 10);
}

TEST(PerfCounterTest, DecCounter) {
    auto pc = make_test_counters();
    pc->set(l_test_u64_counter, 100);
    pc->dec(l_test_u64_counter, 30);
    EXPECT_EQ(pc->get(l_test_u64_counter), 70);
}

TEST(PerfCounterTest, SetGauge) {
    auto pc = make_test_counters();
    pc->set(l_test_u64_gauge, 42);
    EXPECT_EQ(pc->get(l_test_u64_gauge), 42);
    pc->set(l_test_u64_gauge, 99);
    EXPECT_EQ(pc->get(l_test_u64_gauge), 99);
}

TEST(PerfCounterTest, IncOnU64AvgBumpsAvgcount) {
    auto pc = make_test_counters();
    pc->inc(l_test_u64_avg, 5);
    pc->inc(l_test_u64_avg, 10);
    auto [sum, count] = pc->get_avg(l_test_u64_avg);
    EXPECT_EQ(sum, 15);
    EXPECT_EQ(count, 2);
}

TEST(PerfCounterTest, GetOnNonU64ReturnsZero) {
    auto pc = make_test_counters();
    EXPECT_EQ(pc->get(l_test_time_avg), 0);
}

// ============================================================================
// tinc / tset / tget / get_tavg_ns
// ============================================================================

TEST(PerfCounterTest, TincTimeAvg) {
    auto pc = make_test_counters();
    pc->tinc(l_test_time_avg, 1000);
    pc->tinc(l_test_time_avg, 3000);
    auto [sum, count] = pc->get_tavg_ns(l_test_time_avg);
    EXPECT_EQ(sum, 4000);
    EXPECT_EQ(count, 2);
}

TEST(PerfCounterTest, TgetReturnsZeroForNonTime) {
    auto pc = make_test_counters();
    pc->inc(l_test_u64_counter, 5);
    EXPECT_EQ(pc->tget(l_test_u64_counter), 0);
}

TEST(PerfCounterTest, GetTavgNsReturnsZeroForNonTimeAvg) {
    auto pc = make_test_counters();
    pc->inc(l_test_u64_avg, 5);
    auto [sum, count] = pc->get_tavg_ns(l_test_u64_avg);
    EXPECT_EQ(sum, 0);
    EXPECT_EQ(count, 0);
}

// ============================================================================
// reset
// ============================================================================

TEST(PerfCounterTest, ResetClearsCountersAndAvgs) {
    auto pc = make_test_counters();
    pc->inc(l_test_u64_counter, 10);
    pc->inc(l_test_u64_avg, 20);
    pc->set(l_test_u64_gauge, 99);

    pc->reset();

    EXPECT_EQ(pc->get(l_test_u64_counter), 0);
    auto [sum, count] = pc->get_avg(l_test_u64_avg);
    EXPECT_EQ(sum, 0);
    EXPECT_EQ(count, 0);
}

TEST(PerfCounterTest, ResetPreservesGauges) {
    auto pc = make_test_counters();
    pc->set(l_test_u64_gauge, 77);
    pc->reset();
    EXPECT_EQ(pc->get(l_test_u64_gauge), 77);
}

// ============================================================================
// set_enabled
// ============================================================================

TEST(PerfCounterTest, DisabledSkipsOperations) {
    auto pc = make_test_counters();
    pc->set_enabled(false);
    pc->inc(l_test_u64_counter, 10);
    pc->tinc(l_test_time_avg, 1000);
    pc->set(l_test_u64_gauge, 42);
    pc->set_enabled(true);
    EXPECT_EQ(pc->get(l_test_u64_counter), 0);
    EXPECT_EQ(pc->get(l_test_u64_gauge), 0);
    auto [sum, count] = pc->get_tavg_ns(l_test_time_avg);
    EXPECT_EQ(sum, 0);
    EXPECT_EQ(count, 0);
}

TEST(PerfCounterTest, ReEnabledResumes) {
    auto pc = make_test_counters();
    pc->set_enabled(false);
    pc->inc(l_test_u64_counter, 10);
    pc->set_enabled(true);
    pc->inc(l_test_u64_counter, 5);
    EXPECT_EQ(pc->get(l_test_u64_counter), 5);
}

// ============================================================================
// dump
// ============================================================================

TEST(PerfCounterTest, DumpDataMode) {
    auto pc = make_test_counters();
    pc->inc(l_test_u64_counter, 7);
    pc->inc(l_test_u64_avg, 3);
    pc->inc(l_test_u64_avg, 5);
    pc->tinc(l_test_time_avg, 2000);

    JSONFormatter f;
    pc->dump(&f, false);
    std::ostringstream os;
    f.flush(os);
    std::string out = os.str();

    EXPECT_NE(out.find("\"counter\""), std::string::npos);
    EXPECT_NE(out.find("\"u64_avg\""), std::string::npos);
    EXPECT_NE(out.find("\"avgcount\""), std::string::npos);
    EXPECT_NE(out.find("\"time_avg\""), std::string::npos);
}

TEST(PerfCounterTest, DumpSchemaMode) {
    auto pc = make_test_counters();

    JSONFormatter f;
    pc->dump(&f, true);
    std::ostringstream os;
    f.flush(os);
    std::string out = os.str();

    EXPECT_NE(out.find("\"type\""), std::string::npos);
    EXPECT_NE(out.find("\"metric_type\""), std::string::npos);
    EXPECT_NE(out.find("\"value_type\""), std::string::npos);
    EXPECT_NE(out.find("\"priority\""), std::string::npos);
    EXPECT_NE(out.find("\"counter\""), std::string::npos);
    EXPECT_NE(out.find("\"gauge\""), std::string::npos);
    EXPECT_NE(out.find("\"bytes\""), std::string::npos);
}

// ============================================================================
// PerfGuard RAII timer
// ============================================================================

TEST(PerfCounterTest, PerfGuardRecordsElapsed) {
    auto pc = make_test_counters();
    {
        PerfGuard guard(pc.get(), l_test_time_avg);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto [sum, count] = pc->get_tavg_ns(l_test_time_avg);
    EXPECT_EQ(count, 1);
    EXPECT_GE(sum, 4'000'000);
}

TEST(PerfCounterTest, PerfGuardNullCountersNoCrash) {
    PerfGuard guard(nullptr, l_test_time_avg);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// ============================================================================
// Concurrent inc stress
// ============================================================================

TEST(PerfCounterTest, ConcurrentIncStress) {
    auto pc = make_test_counters();
    constexpr int kThreads = 8;
    constexpr int kIters = 10000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&pc]() {
            for (int i = 0; i < kIters; ++i) {
                pc->inc(l_test_u64_counter);
            }
        });
    }
    for (auto &th : threads) {
        th.join();
    }
    EXPECT_EQ(pc->get(l_test_u64_counter), kThreads * kIters);
}

TEST(PerfCounterTest, ConcurrentTincStress) {
    auto pc = make_test_counters();
    constexpr int kThreads = 4;
    constexpr int kIters = 5000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&pc]() {
            for (int i = 0; i < kIters; ++i) {
                pc->tinc(l_test_time_avg, 100);
            }
        });
    }
    for (auto &th : threads) {
        th.join();
    }
    auto [sum, count] = pc->get_tavg_ns(l_test_time_avg);
    EXPECT_EQ(count, kThreads * kIters);
    EXPECT_EQ(sum, kThreads * kIters * 100ULL);
}
