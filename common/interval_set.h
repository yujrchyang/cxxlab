#pragma once

#include <cstddef>
#include <cstdint>
#include <map>

#include "cassert.h"
#include "common/denc.h"
#include "common_fwd.h"

namespace TOPNSPC {

template <typename T>
class interval_set {
public:
    using Map = std::map<T, T>;

    class const_iterator {
        friend class interval_set;
        typename Map::const_iterator it_;
        explicit const_iterator(typename Map::const_iterator it) : it_(it) {}

    public:
        const auto &operator*() const { return *it_; }
        const auto *operator->() const { return &*it_; }
        const_iterator &operator++() {
            ++it_;
            return *this;
        }
        bool operator==(const const_iterator &o) const { return it_ == o.it_; }
        bool operator!=(const const_iterator &o) const { return it_ != o.it_; }
        T get_start() const { return it_->first; }
        T get_len() const { return it_->second; }
    };

    const_iterator begin() const { return const_iterator(m_.begin()); }
    const_iterator end() const { return const_iterator(m_.end()); }
    bool empty() const { return m_.empty(); }
    // 总覆盖长度（与 Ceph interval_set::size() 语义一致）
    T size() const { return _size; }
    // 区间数量
    size_t num_intervals() const { return m_.size(); }

    void insert(T off, T len) {
        if (len == 0) return;
        T old_len = 0;
        auto it = m_.lower_bound(off);
        while (it != m_.end() && it->first <= off + len) {
            len = std::max(len, it->first + it->second - off);
            old_len += it->second;
            it = m_.erase(it);
        }
        if (it != m_.begin()) {
            auto prev = it;
            --prev;
            if (prev->first + prev->second >= off) {
                T new_end = std::max(off + len, prev->first + prev->second);
                len = new_end - prev->first;
                off = prev->first;
                old_len += prev->second;
                m_.erase(prev);
            }
        }
        m_[off] = len;
        _size += len - old_len;
    }

    void erase(T off, T len) {
        if (len == 0) return;
        T old_len = 0;
        T new_len = 0;
        auto it = m_.lower_bound(off);
        if (it != m_.begin()) {
            auto prev = it;
            --prev;
            if (prev->first + prev->second > off) {
                old_len += prev->second;
                // lower_bound 语义保证 prev->first < off，无需额外检查
                auto old_end = prev->second;
                prev->second = off - prev->first;
                new_len += prev->second;
                if (off + len < prev->first + old_end) {
                    T right_len = (prev->first + old_end) - (off + len);
                    m_[off + len] = right_len;
                    new_len += right_len;
                }
            }
        }
        T end = off + len;
        while (it != m_.end() && it->first < end) {
            T it_end = it->first + it->second;
            old_len += it->second;
            if (it_end > end) {
                T right_len = it_end - end;
                m_[end] = right_len;
                new_len += right_len;
            }
            it = m_.erase(it);
        }
        _size += new_len - old_len;
    }

    void clear() {
        m_.clear();
        _size = 0;
    }

    void swap(interval_set &o) {
        m_.swap(o.m_);
        std::swap(_size, o._size);
    }

    T range_start() const {
        cxxlab_assert(!m_.empty());
        return m_.begin()->first;
    }
    T range_end() const {
        cxxlab_assert(!m_.empty());
        return m_.rbegin()->first + m_.rbegin()->second;
    }

    void insert(const interval_set &other) {
        for (auto &[off, len] : other.m_)
            insert(off, len);
    }

    void encode(bufferlist &bl) const {
        denc(m_, bl);
    }

    void decode(buffer::ptr::const_iterator &p) {
        denc(m_, p);
        _recalc_size();
    }

    void decode(bufferlist::const_iterator &p) {
        denc(m_, p);
        _recalc_size();
    }

    const Map &get_map() const { return m_; }

private:
    Map m_;
    T _size = 0;

    void _recalc_size() {
        _size = 0;
        for (const auto &[off, len] : m_) {
            _size += len;
        }
    }
};

template <typename T>
struct denc_traits<interval_set<T>> {
    static constexpr bool supported = true;
    static constexpr bool featured = false;
    static constexpr bool bounded = false;
    static constexpr bool need_contiguous = true;

    static void bound_encode(const interval_set<T> &v, size_t &p) {
        denc(v.get_map(), p);
    }

    static void encode(const interval_set<T> &v,
                       buffer::list::contiguous_appender &p) {
        denc(v.get_map(), p);
    }

    static void decode(interval_set<T> &v, buffer::ptr::const_iterator &p) {
        v.decode(p);
    }
};

}  // namespace TOPNSPC
