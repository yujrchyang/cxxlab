// Copyright 2024 cxxlab authors
// SPDX-License-Identifier: Apache-2.0

#include "perf_counter.h"

#include <cstring>

#include "cassert.h"

namespace TOPNSPC {

PerfCounterData::PerfCounterData(const PerfCounterData &other)
    : name(other.name),
      description(other.description),
      nick(other.nick),
      prio(other.prio),
      type(other.type),
      unit(other.unit),
      sum_(other.sum_.load(std::memory_order_relaxed)),
      avgcount_(other.avgcount_.load(std::memory_order_relaxed)),
      avgcount2_(other.avgcount2_.load(std::memory_order_relaxed)) {}

std::pair<uint64_t, uint64_t> PerfCounterData::read_avg() const {
    uint64_t sum, count;
    do {
        count = avgcount2_.load(std::memory_order_acquire);
        sum = sum_.load(std::memory_order_relaxed);
    } while (avgcount_.load(std::memory_order_relaxed) != count);
    return {sum, count};
}

void PerfCounterData::reset() {
    if (type != PERFCOUNTER_U64) {
        sum_.store(0, std::memory_order_relaxed);
        avgcount_.store(0, std::memory_order_relaxed);
        avgcount2_.store(0, std::memory_order_relaxed);
    }
}

PerfCountersBuilder::PerfCountersBuilder(std::string name, int first, int last)
    : perf_counters_(new PerfCounters(std::move(name), first, last)),
      first_(first),
      last_(last) {}

PerfCountersBuilder::~PerfCountersBuilder() = default;

void PerfCountersBuilder::add_impl(int idx, const char *name,
                                   const char *description, const char *nick,
                                   int prio, PerfCounterType type,
                                   PerfCounterUnit unit) {
    cxxlab_assert(idx > first_);
    cxxlab_assert(idx < last_);
    cxxlab_assert(!nick || strlen(nick) <= 4);
    cxxlab_assert(type != PERFCOUNTER_NONE);
    cxxlab_assert(type & (PERFCOUNTER_U64 | PERFCOUNTER_TIME));
    PerfCounterData &d = perf_counters_->data(idx);
    d.name = name;
    d.description = description;
    d.nick = nick;
    d.prio = static_cast<uint8_t>(prio + prio_default_);
    d.type = type;
    d.unit = unit;
}

void PerfCountersBuilder::add_u64(int idx, const char *name,
                                  const char *description, const char *nick,
                                  int prio, PerfCounterUnit unit) {
    add_impl(idx, name, description, nick, prio,
             static_cast<PerfCounterType>(PERFCOUNTER_U64), unit);
}

void PerfCountersBuilder::add_u64_counter(int idx, const char *name,
                                          const char *description,
                                          const char *nick, int prio,
                                          PerfCounterUnit unit) {
    add_impl(idx, name, description, nick, prio,
             static_cast<PerfCounterType>(PERFCOUNTER_U64 | PERFCOUNTER_COUNTER),
             unit);
}

void PerfCountersBuilder::add_u64_avg(int idx, const char *name,
                                      const char *description, const char *nick,
                                      int prio, PerfCounterUnit unit) {
    add_impl(idx, name, description, nick, prio,
             static_cast<PerfCounterType>(PERFCOUNTER_U64 | PERFCOUNTER_LONGRUNAVG),
             unit);
}

void PerfCountersBuilder::add_time_avg(int idx, const char *name,
                                       const char *description,
                                       const char *nick, int prio) {
    add_impl(idx, name, description, nick, prio,
             static_cast<PerfCounterType>(PERFCOUNTER_TIME | PERFCOUNTER_LONGRUNAVG),
             UNIT_NONE);
}

std::unique_ptr<PerfCounters> PerfCountersBuilder::create_perf_counters() {
    return std::move(perf_counters_);
}

PerfCounters::PerfCounters(std::string name, int first, int last)
    : name_(std::move(name)), first_(first), last_(last) {
    int size = last - first - 1;
    if (size > 0) {
        data_.resize(size);
    }
}

PerfCounters::~PerfCounters() = default;

PerfCounterData &PerfCounters::data(int idx) {
    cxxlab_assert(idx > first_);
    cxxlab_assert(idx < last_);
    return data_[idx - first_ - 1];
}

const PerfCounterData &PerfCounters::data(int idx) const {
    cxxlab_assert(idx > first_);
    cxxlab_assert(idx < last_);
    return data_[idx - first_ - 1];
}

void PerfCounters::inc(int idx, uint64_t v) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    PerfCounterData &d = data(idx);
    if (!(d.type & PERFCOUNTER_U64)) return;
    if (d.type & PERFCOUNTER_LONGRUNAVG) {
        d.avgcount_.fetch_add(1, std::memory_order_relaxed);
        d.sum_.fetch_add(v, std::memory_order_relaxed);
        d.avgcount2_.fetch_add(1, std::memory_order_release);
    } else {
        d.sum_.fetch_add(v, std::memory_order_relaxed);
    }
}

void PerfCounters::dec(int idx, uint64_t v) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    PerfCounterData &d = data(idx);
    cxxlab_assert(!(d.type & PERFCOUNTER_LONGRUNAVG));
    if (!(d.type & PERFCOUNTER_U64)) return;
    d.sum_.fetch_sub(v, std::memory_order_relaxed);
}

void PerfCounters::set(int idx, uint64_t v) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    PerfCounterData &d = data(idx);
    if (!(d.type & PERFCOUNTER_U64)) return;
    if (d.type & PERFCOUNTER_LONGRUNAVG) {
        d.avgcount_.fetch_add(1, std::memory_order_relaxed);
        d.sum_.store(v, std::memory_order_relaxed);
        d.avgcount2_.fetch_add(1, std::memory_order_release);
    } else {
        d.sum_.store(v, std::memory_order_relaxed);
    }
}

uint64_t PerfCounters::get(int idx) const {
    if (!enabled_.load(std::memory_order_relaxed)) return 0;
    const PerfCounterData &d = data(idx);
    if (!(d.type & PERFCOUNTER_U64)) return 0;
    return d.sum_.load(std::memory_order_relaxed);
}

void PerfCounters::tinc(int idx, uint64_t nanos) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    PerfCounterData &d = data(idx);
    if (!(d.type & PERFCOUNTER_TIME)) return;
    if (d.type & PERFCOUNTER_LONGRUNAVG) {
        d.avgcount_.fetch_add(1, std::memory_order_relaxed);
        d.sum_.fetch_add(nanos, std::memory_order_relaxed);
        d.avgcount2_.fetch_add(1, std::memory_order_release);
    } else {
        d.sum_.fetch_add(nanos, std::memory_order_relaxed);
    }
}

void PerfCounters::tset(int idx, uint64_t nanos) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    PerfCounterData &d = data(idx);
    if (!(d.type & PERFCOUNTER_TIME)) return;
    cxxlab_assert(!(d.type & PERFCOUNTER_LONGRUNAVG));
    d.sum_.store(nanos, std::memory_order_relaxed);
}

uint64_t PerfCounters::tget(int idx) const {
    if (!enabled_.load(std::memory_order_relaxed)) return 0;
    const PerfCounterData &d = data(idx);
    if (!(d.type & PERFCOUNTER_TIME)) return 0;
    return d.sum_.load(std::memory_order_relaxed);
}

std::pair<uint64_t, uint64_t> PerfCounters::get_tavg_ns(int idx) const {
    if (!enabled_.load(std::memory_order_relaxed)) return {0, 0};
    const PerfCounterData &d = data(idx);
    if (!(d.type & PERFCOUNTER_TIME)) return {0, 0};
    if (!(d.type & PERFCOUNTER_LONGRUNAVG)) return {0, 0};
    return d.read_avg();
}

std::pair<uint64_t, uint64_t> PerfCounters::get_avg(int idx) const {
    if (!enabled_.load(std::memory_order_relaxed)) return {0, 0};
    const PerfCounterData &d = data(idx);
    if (!(d.type & PERFCOUNTER_LONGRUNAVG)) return {0, 0};
    return d.read_avg();
}

void PerfCounters::reset() {
    for (auto &d : data_) {
        d.reset();
    }
}

void PerfCounters::dump(Formatter *f, bool schema) const {
    f->open_object_section(name_);
    for (const auto &d : data_) {
        if (!d.name) continue;

        if (schema) {
            f->open_object_section(d.name);
            f->dump_int("type", static_cast<int>(d.type));
            if (d.type & PERFCOUNTER_COUNTER) {
                f->dump_string("metric_type", "counter");
            } else {
                f->dump_string("metric_type", "gauge");
            }
            if (d.type & PERFCOUNTER_LONGRUNAVG) {
                if (d.type & PERFCOUNTER_TIME) {
                    f->dump_string("value_type", "real-integer-pair");
                } else {
                    f->dump_string("value_type", "integer-integer-pair");
                }
            } else {
                if (d.type & PERFCOUNTER_TIME) {
                    f->dump_string("value_type", "real");
                } else {
                    f->dump_string("value_type", "integer");
                }
            }
            f->dump_string("description", d.description ? d.description : "");
            f->dump_string("nick", d.nick ? d.nick : "");
            f->dump_int("priority", d.prio);
            if (d.unit == UNIT_BYTES) {
                f->dump_string("units", "bytes");
            } else {
                f->dump_string("units", "none");
            }
            f->close_section();
            continue;
        }

        if (d.type & PERFCOUNTER_LONGRUNAVG) {
            f->open_object_section(d.name);
            auto a = d.read_avg();
            f->dump_unsigned("avgcount", a.second);
            f->dump_unsigned("sum", a.first);
            if ((d.type & PERFCOUNTER_TIME) && a.second > 0) {
                f->dump_unsigned("avgtime", a.first / a.second);
            }
            f->close_section();
        } else {
            uint64_t v = d.sum_.load(std::memory_order_relaxed);
            f->dump_unsigned(d.name, v);
        }
    }
    f->close_section();
}

PerfGuard::PerfGuard(PerfCounters *counters, int idx)
    : start_(std::chrono::steady_clock::now()),
      counters_(counters),
      idx_(idx) {}

PerfGuard::~PerfGuard() {
    if (!counters_) return;
    auto elapsed = std::chrono::steady_clock::now() - start_;
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    counters_->tinc(idx_, static_cast<uint64_t>(ns));
}

}  // namespace TOPNSPC
