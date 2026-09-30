// Copyright 2024 cxxlab authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common_fwd.h"
#include "formatter.h"

namespace TOPNSPC {

enum PerfCounterType : uint8_t {
    PERFCOUNTER_NONE = 0,
    PERFCOUNTER_TIME = 0x1,
    PERFCOUNTER_U64 = 0x2,
    PERFCOUNTER_LONGRUNAVG = 0x4,
    PERFCOUNTER_COUNTER = 0x8,
};

enum PerfCounterUnit : uint8_t {
    UNIT_NONE = 0,
    UNIT_BYTES = 1,
};

struct PerfCounterData {
    const char *name = nullptr;
    const char *description = nullptr;
    const char *nick = nullptr;
    uint8_t prio = 0;
    PerfCounterType type = PERFCOUNTER_NONE;
    PerfCounterUnit unit = UNIT_NONE;
    std::atomic<uint64_t> sum_{0};
    std::atomic<uint64_t> avgcount_{0};
    std::atomic<uint64_t> avgcount2_{0};

    PerfCounterData() = default;
    PerfCounterData(const PerfCounterData &other);
    PerfCounterData &operator=(const PerfCounterData &) = delete;

    std::pair<uint64_t, uint64_t> read_avg() const;
    void reset();
};

class PerfCounters;

class PerfCountersBuilder {
public:
    static constexpr int PRIO_CRITICAL = 10;
    static constexpr int PRIO_INTERESTING = 8;
    static constexpr int PRIO_USEFUL = 5;
    static constexpr int PRIO_UNINTERESTING = 2;
    static constexpr int PRIO_DEBUGONLY = 0;

    PerfCountersBuilder(std::string name, int first, int last);
    ~PerfCountersBuilder();

    void add_u64(int idx, const char *name, const char *description = nullptr,
                 const char *nick = nullptr, int prio = 0,
                 PerfCounterUnit unit = UNIT_NONE);
    void add_u64_counter(int idx, const char *name,
                         const char *description = nullptr,
                         const char *nick = nullptr, int prio = 0,
                         PerfCounterUnit unit = UNIT_NONE);
    void add_u64_avg(int idx, const char *name,
                     const char *description = nullptr,
                     const char *nick = nullptr, int prio = 0,
                     PerfCounterUnit unit = UNIT_NONE);
    void add_time_avg(int idx, const char *name,
                      const char *description = nullptr,
                      const char *nick = nullptr, int prio = 0);

    std::unique_ptr<PerfCounters> create_perf_counters();

    void set_prio_default(int prio) { prio_default_ = prio; }

private:
    void add_impl(int idx, const char *name, const char *description,
                  const char *nick, int prio, PerfCounterType type,
                  PerfCounterUnit unit);

    std::unique_ptr<PerfCounters> perf_counters_;
    int first_;
    int last_;
    int prio_default_ = 0;
};

class PerfCounters {
public:
    template <typename T>
    struct avg_tracker {
        std::pair<uint64_t, T> last;
        std::pair<uint64_t, T> cur;
        avg_tracker() : last(0, 0), cur(0, 0) {}
        T current_avg() const {
            if (cur.first == last.first) return 0;
            return (cur.second - last.second) / (cur.first - last.first);
        }
        void consume_next(const std::pair<uint64_t, T> &next) {
            last = cur;
            cur = next;
        }
    };

    ~PerfCounters();

    void inc(int idx, uint64_t v = 1);
    void dec(int idx, uint64_t v = 1);
    void set(int idx, uint64_t v);
    uint64_t get(int idx) const;

    void tinc(int idx, uint64_t nanos);
    void tset(int idx, uint64_t nanos);
    uint64_t tget(int idx) const;
    std::pair<uint64_t, uint64_t> get_tavg_ns(int idx) const;
    std::pair<uint64_t, uint64_t> get_avg(int idx) const;

    void reset();
    void dump(Formatter *f, bool schema = false) const;

    const std::string &get_name() const { return name_; }
    void set_enabled(bool e) { enabled_.store(e, std::memory_order_relaxed); }
    bool is_enabled() const { return enabled_.load(std::memory_order_relaxed); }

private:
    PerfCounters(std::string name, int first, int last);
    PerfCounters(const PerfCounters &) = delete;
    PerfCounters &operator=(const PerfCounters &) = delete;

    PerfCounterData &data(int idx);
    const PerfCounterData &data(int idx) const;

    std::string name_;
    int first_;
    int last_;
    std::atomic<bool> enabled_{true};
    std::vector<PerfCounterData> data_;

    friend class PerfCountersBuilder;
};

class PerfGuard {
public:
    PerfGuard(PerfCounters *counters, int idx);
    ~PerfGuard();

    PerfGuard(const PerfGuard &) = delete;
    PerfGuard &operator=(const PerfGuard &) = delete;

private:
    std::chrono::steady_clock::time_point start_;
    PerfCounters *counters_;
    int idx_;
};

}  // namespace TOPNSPC
