#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "armor.h"
#include "buffer.h"
#include "buffer_error.h"
#include "cassert.h"
#include "error.h"
#include "intarith.h"
#include "likely.h"
#include "page.h"
#include "safe_io.h"
#include "valgrind.h"

#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(expression)              \
    ({                                              \
        __typeof__(expression) __result;            \
        do {                                        \
            __result = (expression);                \
        } while (__result == -1 && errno == EINTR); \
        __result;                                   \
    })
#endif

#ifndef VOID_TEMP_FAILURE_RETRY
#define VOID_TEMP_FAILURE_RETRY(expression) \
    static_cast<void>(TEMP_FAILURE_RETRY(expression))
#endif

#define BUFFER_ALLOC_UNIT 4096u
#define BUFFER_APPEND_SIZE (BUFFER_ALLOC_UNIT - sizeof(raw_combined))
#define BUFFER_ALLOC_UNIT_MAX \
    std::size_t { 256 * 1024 }

namespace TOPNSPC {

static std::atomic<unsigned> buffer_cached_crc{0};
static std::atomic<unsigned> buffer_cached_crc_adjusted{0};
static std::atomic<unsigned> buffer_missed_crc{0};
static bool buffer_track_crc = (getenv("CXXLAB_BUFFER_TRACK") != nullptr);

void buffer::track_cached_crc(bool b) {
    buffer_track_crc = b;
}

int buffer::get_cached_crc() {
    return buffer_cached_crc;
}

int buffer::get_cached_crc_adjusted() {
    return buffer_cached_crc_adjusted;
}

int buffer::get_missed_crc() {
    return buffer_missed_crc;
}

class buffer::raw_combined : public buffer::raw {
    size_t alignment;

public:
    raw_combined(char *dataptr, unsigned l, unsigned align)
        : raw(dataptr, l),
          alignment(align) {
    }
    raw *clone_empty() override {
        return create(len, alignment).release();
    }

    static unique_leakable_ptr<buffer::raw>
    create(unsigned len,
           unsigned align) {
        align = std::max<unsigned>(align, sizeof(void *));
        size_t rawlen = round_up_to(sizeof(buffer::raw_combined),
                                    alignof(buffer::raw_combined));
        size_t datalen = round_up_to(len, alignof(buffer::raw_combined));
        char *ptr = 0;
        int r = ::posix_memalign((void **)(void *)&ptr, align, rawlen + datalen);
        if (r)
            throw std::bad_alloc();
        if (!ptr)
            throw std::bad_alloc();

        return unique_leakable_ptr<buffer::raw>(
            new (ptr + datalen) raw_combined(ptr, len, align));
    }

    static void operator delete(void *ptr) {
        raw_combined *raw = (raw_combined *)ptr;
        free((void *)raw->data);
    }
};

class buffer::raw_malloc : public buffer::raw {
public:
    explicit raw_malloc(unsigned l) : raw(l) {
        if (len) {
            data = (char *)malloc(len);
            if (!data)
                throw std::bad_alloc();
        } else {
            data = 0;
        }
    }
    raw_malloc(unsigned l, char *b) : raw(b, l) {
    }
    ~raw_malloc() override {
        free(data);
    }
    raw *clone_empty() override {
        return new raw_malloc(len);
    }
};

class buffer::raw_posix_aligned : public buffer::raw {
    unsigned align;

public:
    raw_posix_aligned(unsigned l, unsigned _align) : raw(l) {
        align = std::max<unsigned>(_align, sizeof(void *));
        int r = ::posix_memalign((void **)(void *)&data, align, len);
        if (r)
            throw std::bad_alloc();
        if (!data)
            throw std::bad_alloc();
    }
    ~raw_posix_aligned() override {
        free(data);
    }
    raw *clone_empty() override {
        return new raw_posix_aligned(len, align);
    }
};

class buffer::raw_char : public buffer::raw {
public:
    explicit raw_char(unsigned l) : raw(l) {
        if (len)
            data = new char[len];
        else
            data = 0;
    }
    raw_char(unsigned l, char *b) : raw(b, l) {
    }
    ~raw_char() override {
        delete[] data;
    }
    raw *clone_empty() override {
        return new raw_char(len);
    }
};

class buffer::raw_claimed_char : public buffer::raw {
public:
    explicit raw_claimed_char(unsigned l, char *b) : raw(b, l) {
    }
    ~raw_claimed_char() override {
    }
    raw *clone_empty() override {
        return new raw_char(len);
    }
};

class buffer::raw_static : public buffer::raw {
public:
    raw_static(const char *d, unsigned l) : raw((char *)d, l) {}
    ~raw_static() override {}
    raw *clone_empty() override {
        return new buffer::raw_char(len);
    }
};

class buffer::raw_claim_buffer : public buffer::raw {
    deleter del;

public:
    raw_claim_buffer(const char *b, unsigned l, deleter d)
        : raw((char *)b, l), del(std::move(d)) {}
    ~raw_claim_buffer() override {}
    raw *clone_empty() override {
        return new buffer::raw_char(len);
    }
};

unique_leakable_ptr<buffer::raw> buffer::copy(const char *c, unsigned len) {
    auto r = buffer::create_aligned(len, sizeof(size_t));
    memcpy(r->get_data(), c, len);
    return r;
}

unique_leakable_ptr<buffer::raw> buffer::create(unsigned len) {
    return buffer::create_aligned(len, sizeof(size_t));
}

unique_leakable_ptr<buffer::raw> buffer::create(unsigned len, char c) {
    auto ret = buffer::create_aligned(len, sizeof(size_t));
    memset(ret->get_data(), c, len);
    return ret;
}

unique_leakable_ptr<buffer::raw>
buffer::claim_char(unsigned len, char *buf) {
    return unique_leakable_ptr<buffer::raw>(
        new raw_claimed_char(len, buf));
}

unique_leakable_ptr<buffer::raw> buffer::create_malloc(unsigned len) {
    return unique_leakable_ptr<buffer::raw>(new raw_malloc(len));
}

unique_leakable_ptr<buffer::raw>
buffer::claim_malloc(unsigned len, char *buf) {
    return unique_leakable_ptr<buffer::raw>(new raw_malloc(len, buf));
}

unique_leakable_ptr<buffer::raw>
buffer::create_static(unsigned len, char *buf) {
    return unique_leakable_ptr<buffer::raw>(new raw_static(buf, len));
}

unique_leakable_ptr<buffer::raw>
buffer::claim_buffer(unsigned len, char *buf, deleter del) {
    return unique_leakable_ptr<buffer::raw>(
        new raw_claim_buffer(buf, len, std::move(del)));
}

unique_leakable_ptr<buffer::raw> buffer::create_aligned(
    unsigned len, unsigned align) {
    if ((align & ~page().mask) == 0 ||
        len >= page().size * 2) {
        return unique_leakable_ptr<buffer::raw>(new raw_posix_aligned(len, align));
    }
    return raw_combined::create(len, align);
}

unique_leakable_ptr<buffer::raw> buffer::create_page_aligned(unsigned len) {
    return create_aligned(len, page().size);
}

unique_leakable_ptr<buffer::raw> buffer::create_small_page_aligned(unsigned len) {
    if (len < page().size) {
        return create_aligned(len, BUFFER_ALLOC_UNIT);
    } else {
        return create_aligned(len, page().size);
    }
}

buffer::ptr::ptr(unique_leakable_ptr<raw> r)
    : _raw(r.release()),
      _off(0),
      _len(_raw->get_len()) {
    _raw->nref.store(1, std::memory_order_release);
}

buffer::ptr::ptr(unsigned l) : _off(0), _len(l) {
    _raw = buffer::create(l).release();
    _raw->nref.store(1, std::memory_order_release);
}

buffer::ptr::ptr(const char *d, unsigned l) : _off(0), _len(l) {
    _raw = buffer::copy(d, l).release();
    _raw->nref.store(1, std::memory_order_release);
}

buffer::ptr::ptr(const ptr &p) : _raw(p._raw), _off(p._off), _len(p._len) {
    if (_raw) {
        _raw->nref++;
    }
}

buffer::ptr::ptr(ptr &&p) noexcept : _raw(p._raw), _off(p._off), _len(p._len) {
    p._raw = nullptr;
    p._off = p._len = 0;
}

buffer::ptr::ptr(const ptr &p, unsigned o, unsigned l)
    : _raw(p._raw), _off(p._off + o), _len(l) {
    cxxlab_assert(o + l <= p._len);
    cxxlab_assert(_raw);
    _raw->nref++;
}

buffer::ptr::ptr(const ptr &p, unique_leakable_ptr<raw> r)
    : _raw(r.release()),
      _off(p._off),
      _len(p._len) {
    _raw->nref.store(1, std::memory_order_release);
}

buffer::ptr &buffer::ptr::operator=(const ptr &p) {
    if (p._raw) {
        p._raw->nref++;
    }
    buffer::raw *raw = p._raw;
    release();
    if (raw) {
        _raw = raw;
        _off = p._off;
        _len = p._len;
    } else {
        _off = _len = 0;
    }
    return *this;
}

buffer::ptr &buffer::ptr::operator=(ptr &&p) noexcept {
    release();
    buffer::raw *raw = p._raw;
    if (raw) {
        _raw = raw;
        _off = p._off;
        _len = p._len;
        p._raw = nullptr;
        p._off = p._len = 0;
    } else {
        _off = _len = 0;
    }
    return *this;
}

unique_leakable_ptr<buffer::raw> buffer::ptr::clone() {
    return _raw->clone();
}

void buffer::ptr::swap(ptr &other) noexcept {
    raw *r = _raw;
    unsigned o = _off;
    unsigned l = _len;
    _raw = other._raw;
    _off = other._off;
    _len = other._len;
    other._raw = r;
    other._off = o;
    other._len = l;
}

void buffer::ptr::release() {
    if (auto *const cached_raw = std::exchange(_raw, nullptr);
        cached_raw) {
        const bool last_one =
            (1 == cached_raw->nref.load(std::memory_order_acquire));
        if (likely(last_one) || --cached_raw->nref == 0) {
            ANNOTATE_HAPPENS_AFTER(&cached_raw->nref);
            ANNOTATE_HAPPENS_BEFORE_FORGET_ALL(&cached_raw->nref);
            delete cached_raw;
        } else {
            ANNOTATE_HAPPENS_BEFORE(&cached_raw->nref);
        }
    }
}

const char *buffer::ptr::c_str() const {
    cxxlab_assert(_raw);
    return _raw->get_data() + _off;
}

char *buffer::ptr::c_str() {
    cxxlab_assert(_raw);
    return _raw->get_data() + _off;
}

const char *buffer::ptr::end_c_str() const {
    cxxlab_assert(_raw);
    return _raw->get_data() + _off + _len;
}

char *buffer::ptr::end_c_str() {
    cxxlab_assert(_raw);
    return _raw->get_data() + _off + _len;
}

unsigned buffer::ptr::unused_tail_length() const {
    return _raw ? _raw->get_len() - (_off + _len) : 0;
}

const char &buffer::ptr::operator[](unsigned n) const {
    cxxlab_assert(_raw);
    cxxlab_assert(n < _len);
    return _raw->get_data()[_off + n];
}

char &buffer::ptr::operator[](unsigned n) {
    cxxlab_assert(_raw);
    cxxlab_assert(n < _len);
    return _raw->get_data()[_off + n];
}

const char *buffer::ptr::raw_c_str() const {
    cxxlab_assert(_raw);
    return _raw->get_data();
}

unsigned buffer::ptr::raw_length() const {
    cxxlab_assert(_raw);
    return _raw->get_len();
}

int buffer::ptr::raw_nref() const {
    cxxlab_assert(_raw);
    return _raw->nref;
}

void buffer::ptr::copy_out(unsigned o, unsigned l, char *dest) const {
    cxxlab_assert(_raw);
    if (o + l > _len)
        throw end_of_buffer();
    char *src = _raw->get_data() + _off + o;
    maybe_inline_memcpy(dest, src, l, 8);
}

unsigned buffer::ptr::wasted() const {
    return _raw->get_len() - _len;
}

int buffer::ptr::cmp(const ptr &o) const {
    int l = _len < o._len ? _len : o._len;
    if (l) {
        int r = memcmp(c_str(), o.c_str(), l);
        if (r)
            return r;
    }
    if (_len < o._len)
        return -1;
    if (_len > o._len)
        return 1;
    return 0;
}

bool buffer::ptr::is_zero() const {
    return mem_is_zero(c_str(), _len);
}

unsigned buffer::ptr::append(char c) {
    cxxlab_assert(_raw);
    cxxlab_assert(1 <= unused_tail_length());
    char *ptr = _raw->get_data() + _off + _len;
    *ptr = c;
    _len++;
    return _len + _off;
}

unsigned buffer::ptr::append(const char *p, unsigned l) {
    cxxlab_assert(_raw);
    cxxlab_assert(l <= unused_tail_length());
    char *c = _raw->get_data() + _off + _len;
    maybe_inline_memcpy(c, p, l, 32);
    _len += l;
    return _len + _off;
}

unsigned buffer::ptr::append_zeros(unsigned l) {
    cxxlab_assert(_raw);
    cxxlab_assert(l <= unused_tail_length());
    char *c = _raw->get_data() + _off + _len;
    memset(c, 0, l);
    _len += l;
    return _len + _off;
}

void buffer::ptr::copy_in(unsigned o, unsigned l, const char *src, bool crc_reset) {
    cxxlab_assert(_raw);
    cxxlab_assert(o <= _len);
    cxxlab_assert(o + l <= _len);
    char *dest = _raw->get_data() + _off + o;
    if (crc_reset)
        _raw->invalidate_crc();
    maybe_inline_memcpy(dest, src, l, 64);
}

void buffer::ptr::zero(bool crc_reset) {
    if (crc_reset)
        _raw->invalidate_crc();
    memset(c_str(), 0, _len);
}

void buffer::ptr::zero(unsigned o, unsigned l, bool crc_reset) {
    cxxlab_assert(o + l <= _len);
    if (crc_reset)
        _raw->invalidate_crc();
    memset(c_str() + o, 0, l);
}

template <bool B>
buffer::ptr::iterator_impl<B> &buffer::ptr::iterator_impl<B>::operator+=(size_t len) {
    pos += len;
    if (pos > end_ptr)
        throw end_of_buffer();
    return *this;
}

template buffer::ptr::iterator_impl<false> &
buffer::ptr::iterator_impl<false>::operator+=(size_t len);
template buffer::ptr::iterator_impl<true> &
buffer::ptr::iterator_impl<true>::operator+=(size_t len);

template <bool is_const>
buffer::list::iterator_impl<is_const>::iterator_impl(bl_t *l, unsigned o)
    : bl(l), ls(&bl->_buffers), p(ls->begin()), off(0), p_off(0) {
    *this += o;
}

template <bool is_const>
buffer::list::iterator_impl<is_const>::iterator_impl(const buffer::list::iterator &i)
    : iterator_impl<is_const>(i.bl, i.off, i.p, i.p_off) {}

template <bool is_const>
auto buffer::list::iterator_impl<is_const>::operator+=(unsigned o)
    -> iterator_impl & {
    p_off += o;
    while (p != ls->end()) {
        if (p_off >= p->length()) {
            p_off -= p->length();
            p++;
        } else {
            break;
        }
    }
    if (p == ls->end() && p_off) {
        throw end_of_buffer();
    }
    off += o;
    return *this;
}

template <bool is_const>
void buffer::list::iterator_impl<is_const>::seek(unsigned o) {
    p = ls->begin();
    off = p_off = 0;
    *this += o;
}

template <bool is_const>
char buffer::list::iterator_impl<is_const>::operator*() const {
    if (p == ls->end())
        throw end_of_buffer();
    return (*p)[p_off];
}

template <bool is_const>
buffer::list::iterator_impl<is_const> &
buffer::list::iterator_impl<is_const>::operator++() {
    if (p == ls->end())
        throw end_of_buffer();
    *this += 1;
    return *this;
}

template <bool is_const>
buffer::ptr buffer::list::iterator_impl<is_const>::get_current_ptr() const {
    if (p == ls->end())
        throw end_of_buffer();
    return ptr(*p, p_off, p->length() - p_off);
}

template <bool is_const>
bool buffer::list::iterator_impl<is_const>::is_pointing_same_raw(
    const ptr &other) const {
    if (p == ls->end())
        throw end_of_buffer();
    return p->_raw == other._raw;
}

template <bool is_const>
void buffer::list::iterator_impl<is_const>::copy(unsigned len, char *dest) {
    if (p == ls->end()) seek(off);
    while (len > 0) {
        if (p == ls->end())
            throw end_of_buffer();

        unsigned howmuch = p->length() - p_off;
        if (len < howmuch) howmuch = len;
        p->copy_out(p_off, howmuch, dest);
        dest += howmuch;

        len -= howmuch;
        *this += howmuch;
    }
}

template <bool is_const>
void buffer::list::iterator_impl<is_const>::copy(unsigned len, ptr &dest) {
    copy_deep(len, dest);
}

template <bool is_const>
void buffer::list::iterator_impl<is_const>::copy_deep(unsigned len, ptr &dest) {
    if (!len) {
        return;
    }
    if (p == ls->end())
        throw end_of_buffer();
    dest = create(len);
    copy(len, dest.c_str());
}

template <bool is_const>
void buffer::list::iterator_impl<is_const>::copy_shallow(unsigned len,
                                                         ptr &dest) {
    if (!len) {
        return;
    }
    if (p == ls->end())
        throw end_of_buffer();
    unsigned howmuch = p->length() - p_off;
    if (howmuch < len) {
        dest = create(len);
        copy(len, dest.c_str());
    } else {
        dest = ptr(*p, p_off, len);
        *this += len;
    }
}

template <bool is_const>
void buffer::list::iterator_impl<is_const>::copy(unsigned len, list &dest) {
    if (p == ls->end())
        seek(off);
    while (len > 0) {
        if (p == ls->end())
            throw end_of_buffer();

        unsigned howmuch = p->length() - p_off;
        if (len < howmuch)
            howmuch = len;
        dest.append(*p, p_off, howmuch);

        len -= howmuch;
        *this += howmuch;
    }
}

template <bool is_const>
void buffer::list::iterator_impl<is_const>::copy(unsigned len, std::string &dest) {
    if (p == ls->end())
        seek(off);
    while (len > 0) {
        if (p == ls->end())
            throw end_of_buffer();

        unsigned howmuch = p->length() - p_off;
        const char *c_str = p->c_str();
        if (len < howmuch)
            howmuch = len;
        dest.append(c_str + p_off, howmuch);

        len -= howmuch;
        *this += howmuch;
    }
}

template <bool is_const>
void buffer::list::iterator_impl<is_const>::copy_all(list &dest) {
    if (p == ls->end())
        seek(off);
    while (1) {
        if (p == ls->end())
            return;

        unsigned howmuch = p->length() - p_off;
        const char *c_str = p->c_str();
        dest.append(c_str + p_off, howmuch);

        *this += howmuch;
    }
}

template <bool is_const>
size_t buffer::list::iterator_impl<is_const>::get_ptr_and_advance(
    size_t want, const char **data) {
    if (p == ls->end()) {
        seek(off);
        if (p == ls->end()) {
            return 0;
        }
    }
    *data = p->c_str() + p_off;
    size_t l = std::min<size_t>(p->length() - p_off, want);
    p_off += l;
    if (p_off == p->length()) {
        ++p;
        p_off = 0;
    }
    off += l;
    return l;
}

template <bool is_const>
uint32_t buffer::list::iterator_impl<is_const>::crc32c(
    size_t length, uint32_t crc) {
    length = std::min<size_t>(length, get_remaining());
    while (length > 0) {
        const char *p;
        size_t l = get_ptr_and_advance(length, &p);
        crc = calc_crc32((unsigned char *)p, l, crc);
        length -= l;
    }
    return crc;
}

template class buffer::list::iterator_impl<true>;
template class buffer::list::iterator_impl<false>;

buffer::list::iterator::iterator(bl_t *l, unsigned o)
    : iterator_impl(l, o) {}

buffer::list::iterator::iterator(bl_t *l, unsigned o, list_iter_t ip, unsigned po)
    : iterator_impl(l, o, ip, po) {}

void buffer::list::iterator::copy_in(unsigned len, const char *src, bool crc_reset) {
    if (p == ls->end())
        seek(off);
    while (len > 0) {
        if (p == ls->end())
            throw end_of_buffer();

        unsigned howmuch = p->length() - p_off;
        if (len < howmuch)
            howmuch = len;
        p->copy_in(p_off, howmuch, src, crc_reset);

        src += howmuch;
        len -= howmuch;
        *this += howmuch;
    }
}

void buffer::list::iterator::copy_in(unsigned len, const list &otherl) {
    if (p == ls->end())
        seek(off);
    unsigned left = len;
    for (const auto &node : otherl._buffers) {
        unsigned l = node.length();
        if (left < l)
            l = left;
        copy_in(l, node.c_str());
        left -= l;
        if (left == 0)
            break;
    }
}

void buffer::list::swap(list &other) noexcept {
    std::swap(_len, other._len);
    std::swap(_num, other._num);
    std::swap(_carriage, other._carriage);
    _buffers.swap(other._buffers);
}

bool buffer::list::contents_equal(const buffer::list &other) const {
    if (length() != other.length())
        return false;

    if (true) {
        auto a = std::cbegin(_buffers);
        auto b = std::cbegin(other._buffers);
        unsigned aoff = 0, boff = 0;
        while (a != std::cend(_buffers)) {
            unsigned len = a->length() - aoff;
            if (len > b->length() - boff)
                len = b->length() - boff;
            if (memcmp(a->c_str() + aoff, b->c_str() + boff, len) != 0)
                return false;
            aoff += len;
            if (aoff == a->length()) {
                aoff = 0;
                ++a;
            }
            boff += len;
            if (boff == b->length()) {
                boff = 0;
                ++b;
            }
        }
        return true;
    }

    if (false) {
        bufferlist::const_iterator me = begin();
        bufferlist::const_iterator him = other.begin();
        while (!me.end()) {
            if (*me != *him)
                return false;
            ++me;
            ++him;
        }
        return true;
    }
}

bool buffer::list::contents_equal(const void *const other,
                                  size_t length) const {
    if (this->length() != length) {
        return false;
    }

    const auto *other_buf = reinterpret_cast<const char *>(other);
    for (const auto &bp : buffers()) {
        cxxlab_assert(bp.length() <= length);
        if (std::memcmp(bp.c_str(), other_buf, bp.length()) != 0) {
            return false;
        } else {
            length -= bp.length();
            other_buf += bp.length();
        }
    }

    return true;
}

bool buffer::list::is_provided_buffer(const char *const dst) const {
    if (_buffers.empty()) {
        return false;
    }
    return (is_contiguous() && (_buffers.front().c_str() == dst));
}

bool buffer::list::is_aligned(const unsigned align) const {
    for (const auto &node : _buffers) {
        if (!node.is_aligned(align)) {
            return false;
        }
    }
    return true;
}

bool buffer::list::is_n_align_sized(const unsigned align) const {
    for (const auto &node : _buffers) {
        if (!node.is_n_align_sized(align)) {
            return false;
        }
    }
    return true;
}

bool buffer::list::is_aligned_size_and_memory(
    const unsigned align_size,
    const unsigned align_memory) const {
    for (const auto &node : _buffers) {
        if (!node.is_aligned(align_memory) || !node.is_n_align_sized(align_size)) {
            return false;
        }
    }
    return true;
}

bool buffer::list::is_zero() const {
    for (const auto &node : _buffers) {
        if (!node.is_zero()) {
            return false;
        }
    }
    return true;
}

void buffer::list::zero() {
    for (auto &node : _buffers) {
        node.zero();
    }
}

void buffer::list::zero(const unsigned o, const unsigned l) {
    cxxlab_assert(o + l <= _len);
    unsigned p = 0;
    for (auto &node : _buffers) {
        if (p + node.length() > o) {
            if (p >= o && p + node.length() <= o + l) {
                node.zero();
            } else if (p >= o) {
                node.zero(0, o + l - p);
            } else if (p + node.length() <= o + l) {
                node.zero(o - p, node.length() - (o - p));
            } else {
                node.zero(o - p, l);
            }
        }
        p += node.length();
        if (o + l <= p) {
            break;
        }
    }
}

bool buffer::list::is_contiguous() const {
    return _num <= 1;
}

bool buffer::list::is_n_page_sized() const {
    return is_n_align_sized(page().size);
}

bool buffer::list::is_page_aligned() const {
    return is_aligned(page().size);
}

uint64_t buffer::list::get_wasted_space() const {
    if (_num == 1)
        return _buffers.back().wasted();

    std::vector<const raw *> raw_vec;
    raw_vec.reserve(_num);
    for (const auto &p : _buffers)
        raw_vec.push_back(p._raw);
    std::sort(raw_vec.begin(), raw_vec.end());

    uint64_t total = 0;
    const raw *last = nullptr;
    for (const auto r : raw_vec) {
        if (r == last)
            continue;
        last = r;
        total += r->get_len();
    }
    if (total <= length())
        return 0;
    return total - length();
}

void buffer::list::rebuild() {
    if (_len == 0) {
        _carriage = &always_empty_bptr;
        _buffers.clear_and_dispose();
        _num = 0;
        return;
    }
    if ((_len & ~page().mask) == 0)
        rebuild(ptr_node::create(buffer::create_page_aligned(_len)));
    else
        rebuild(ptr_node::create(buffer::create(_len)));
}

void buffer::list::rebuild(
    std::unique_ptr<buffer::ptr_node, buffer::ptr_node::disposer> nb) {
    unsigned pos = 0;
    for (auto &node : _buffers) {
        nb->copy_in(pos, node.length(), node.c_str(), false);
        pos += node.length();
    }
    _buffers.clear_and_dispose();
    if (likely(nb->length())) {
        _carriage = nb.get();
        _buffers.push_back(*nb.release());
        _num = 1;
    } else {
        _carriage = &always_empty_bptr;
        _num = 0;
    }
    invalidate_crc();
}

bool buffer::list::rebuild_aligned(unsigned align) {
    return rebuild_aligned_size_and_memory(align, align);
}

bool buffer::list::rebuild_aligned_size_and_memory(unsigned align_size,
                                                   unsigned align_memory,
                                                   unsigned max_buffers) {
    bool had_to_rebuild = false;

    if (max_buffers && _num > max_buffers && _len > (max_buffers * align_size)) {
        align_size = round_up_to(round_up_to(_len, max_buffers) / max_buffers, align_size);
    }
    auto p = std::begin(_buffers);
    auto p_prev = _buffers.before_begin();
    while (p != std::end(_buffers)) {
        if (p->is_aligned(align_memory) && p->is_n_align_sized(align_size)) {
            p_prev = p++;
            continue;
        }

        list unaligned;
        unsigned offset = 0;
        do {
            offset += p->length();
            auto p_after = _buffers.erase_after(p_prev);
            _num -= 1;
            unaligned._buffers.push_back(*p);
            unaligned._len += p->length();
            unaligned._num += 1;
            p = p_after;
        } while (p != std::end(_buffers) &&
                 (!p->is_aligned(align_memory) ||
                  !p->is_n_align_sized(align_size) ||
                  (offset % align_size)));
        if (!(unaligned.is_contiguous() && unaligned._buffers.front().is_aligned(align_memory))) {
            unaligned.rebuild(
                ptr_node::create(
                    buffer::create_aligned(unaligned._len, align_memory)));
            had_to_rebuild = true;
        }
        if (unaligned.get_num_buffers()) {
            _buffers.insert_after(p_prev, *ptr_node::create(unaligned._buffers.front()).release());
            _num += 1;
        }
        ++p_prev;
    }
    return had_to_rebuild;
}

bool buffer::list::rebuild_page_aligned() {
    return rebuild_aligned(page().size);
}

void buffer::list::reserve(size_t prealloc) {
    if (get_append_buffer_unused_tail_length() < prealloc) {
        auto ptr = ptr_node::create(buffer::create_small_page_aligned(prealloc));
        ptr->set_length(0);
        _carriage = ptr.get();
        _buffers.push_back(*ptr.release());
        _num += 1;
    }
}

void buffer::list::claim_append(list &bl) {
    _len += bl._len;
    _num += bl._num;
    _buffers.splice_back(bl._buffers);
    bl.clear();
}

void buffer::list::append(char c) {
    unsigned gap = get_append_buffer_unused_tail_length();
    if (!gap) {
        auto buf = ptr_node::create(
            raw_combined::create(BUFFER_APPEND_SIZE, 0));
        buf->set_length(0);
        _carriage = buf.get();
        _buffers.push_back(*buf.release());
        _num += 1;
    } else if (unlikely(_carriage != &_buffers.back())) {
        auto bptr = ptr_node::create(*_carriage, _carriage->length(), 0);
        _carriage = bptr.get();
        _buffers.push_back(*bptr.release());
        _num += 1;
    }
    _carriage->append(c);
    _len++;
}

buffer::ptr_node buffer::list::always_empty_bptr;

buffer::ptr_node &buffer::list::refill_append_space(const unsigned len) {
    size_t need = round_up_to(len, sizeof(size_t)) + sizeof(raw_combined);
    size_t alen = round_up_to(need, BUFFER_ALLOC_UNIT);
    if (_carriage == &_buffers.back()) {
        size_t nlen = round_up_to(_carriage->raw_length(), BUFFER_ALLOC_UNIT) * 2;
        nlen = std::min(nlen, BUFFER_ALLOC_UNIT_MAX);
        alen = std::max(alen, nlen);
    }
    alen -= sizeof(raw_combined);

    auto new_back =
        ptr_node::create(raw_combined::create(alen, 0));
    new_back->set_length(0);
    _carriage = new_back.get();
    _buffers.push_back(*new_back.release());
    _num += 1;
    return _buffers.back();
}

void buffer::list::append(const char *data, unsigned len) {
    _len += len;

    const unsigned free_in_last = get_append_buffer_unused_tail_length();
    const unsigned first_round = std::min(len, free_in_last);
    if (first_round) {
        if (unlikely(_carriage != &_buffers.back())) {
            auto bptr = ptr_node::create(*_carriage, _carriage->length(), 0);
            _carriage = bptr.get();
            _buffers.push_back(*bptr.release());
            _num += 1;
        }
        _carriage->append(data, first_round);
    }

    const unsigned second_round = len - first_round;
    if (second_round) {
        auto &new_back = refill_append_space(second_round);
        new_back.append(data + first_round, second_round);
    }
}

buffer::list::reserve_t buffer::list::obtain_contiguous_space(
    const unsigned len) {
    if (unlikely(get_append_buffer_unused_tail_length() < len)) {
        auto new_back =
            buffer::ptr_node::create(buffer::create(len)).release();
        new_back->set_length(0);
        _buffers.push_back(*new_back);
        _num += 1;
        _carriage = new_back;
        return {new_back->c_str(), &new_back->_len, &_len};
    } else {
        cxxlab_assert(!_buffers.empty());
        if (unlikely(_carriage != &_buffers.back())) {
            auto bptr = ptr_node::create(*_carriage, _carriage->length(), 0);
            _carriage = bptr.get();
            _buffers.push_back(*bptr.release());
            _num += 1;
        }
        return {_carriage->end_c_str(), &_carriage->_len, &_len};
    }
}

void buffer::list::append(const ptr &bp) {
    push_back(bp);
}

void buffer::list::append(ptr &&bp) {
    push_back(std::move(bp));
}

void buffer::list::append(const ptr &bp, unsigned off, unsigned len) {
    cxxlab_assert(len + off <= bp.length());
    if (!_buffers.empty()) {
        ptr &l = _buffers.back();
        if (l._raw == bp._raw && l.end() == bp.start() + off) {
            l.set_length(l.length() + len);
            _len += len;
            return;
        }
    }
    _buffers.push_back(*ptr_node::create(bp, off, len).release());
    _len += len;
    _num += 1;
}

void buffer::list::append(const list &bl) {
    _len += bl._len;
    _num += bl._num;
    for (const auto &node : bl._buffers) {
        _buffers.push_back(*ptr_node::create(node).release());
    }
}

void buffer::list::append(std::istream &in) {
    while (!in.eof()) {
        std::string s;
        getline(in, s);
        append(s.c_str(), s.length());
        if (s.length())
            append("\n", 1);
    }
}

buffer::list::contiguous_filler buffer::list::append_hole(const unsigned len) {
    _len += len;

    if (unlikely(get_append_buffer_unused_tail_length() < len)) {
        auto &new_back = refill_append_space(len);
        new_back.set_length(len);
        return {new_back.c_str()};
    } else if (unlikely(_carriage != &_buffers.back())) {
        auto bptr = ptr_node::create(*_carriage, _carriage->length(), 0);
        _carriage = bptr.get();
        _buffers.push_back(*bptr.release());
        _num += 1;
    }
    _carriage->set_length(_carriage->length() + len);
    return {_carriage->end_c_str() - len};
}

void buffer::list::prepend_zero(unsigned len) {
    auto bp = ptr_node::create(len);
    bp->zero(false);
    _len += len;
    _num += 1;
    _buffers.push_front(*bp.release());
}

void buffer::list::append_zero(unsigned len) {
    _len += len;

    const unsigned free_in_last = get_append_buffer_unused_tail_length();
    const unsigned first_round = std::min(len, free_in_last);
    if (first_round) {
        if (unlikely(_carriage != &_buffers.back())) {
            auto bptr = ptr_node::create(*_carriage, _carriage->length(), 0);
            _carriage = bptr.get();
            _buffers.push_back(*bptr.release());
            _num += 1;
        }
        _carriage->append_zeros(first_round);
    }

    const unsigned second_round = len - first_round;
    if (second_round) {
        auto &new_back = refill_append_space(second_round);
        new_back.set_length(second_round);
        new_back.zero(false);
    }
}

const char &buffer::list::operator[](unsigned n) const {
    if (n >= _len)
        throw end_of_buffer();

    for (const auto &node : _buffers) {
        if (n >= node.length()) {
            n -= node.length();
            continue;
        }
        return node[n];
    }
    cxxlab_abort_msg("unreachable");
}

char *buffer::list::c_str() {
    if (const auto len = length(); len == 0) {
        return nullptr;
    } else if (len != _buffers.front().length()) {
        rebuild();
    }
    return _buffers.front().c_str();
}

std::string buffer::list::to_str() const {
    std::string s;
    s.reserve(length());
    for (const auto &node : _buffers) {
        if (node.length()) {
            s.append(node.c_str(), node.length());
        }
    }
    return s;
}

void buffer::list::substr_of(const list &other, unsigned off, unsigned len) {
    if (off + len > other.length())
        throw end_of_buffer();

    clear();

    auto curbuf = std::cbegin(other._buffers);
    while (off > 0 && off >= curbuf->length()) {
        off -= (*curbuf).length();
        ++curbuf;
    }
    cxxlab_assert(len == 0 || curbuf != std::cend(other._buffers));

    while (len > 0) {
        if (off + len < curbuf->length()) {
            _buffers.push_back(*ptr_node::create(*curbuf, off, len).release());
            _len += len;
            _num += 1;
            break;
        }

        unsigned howmuch = curbuf->length() - off;
        _buffers.push_back(*ptr_node::create(*curbuf, off, howmuch).release());
        _len += howmuch;
        _num += 1;
        len -= howmuch;
        off = 0;
        ++curbuf;
    }
}

void buffer::list::splice(unsigned off, unsigned len, list *claim_by) {
    if (len == 0)
        return;

    if (off >= length())
        throw end_of_buffer();

    cxxlab_assert(len > 0);

    auto curbuf = std::begin(_buffers);
    auto curbuf_prev = _buffers.before_begin();
    while (off > 0) {
        cxxlab_assert(curbuf != std::end(_buffers));
        if (off >= (*curbuf).length()) {
            off -= (*curbuf).length();
            curbuf_prev = curbuf++;
        } else {
            break;
        }
    }

    if (off) {
        _buffers.insert_after(curbuf_prev,
                              *ptr_node::create(*curbuf, 0, off).release());
        _len += off;
        _num += 1;
        ++curbuf_prev;
    }

    while (len > 0) {
        if (const auto to_drop = off + len; to_drop < curbuf->length()) {
            if (claim_by)
                claim_by->append(*curbuf, off, len);
            curbuf->set_offset(to_drop + curbuf->offset());
            curbuf->set_length(curbuf->length() - to_drop);
            _len -= to_drop;
            break;
        }

        unsigned howmuch = curbuf->length() - off;
        if (claim_by)
            claim_by->append(*curbuf, off, howmuch);
        _len -= curbuf->length();
        if (curbuf == _carriage) {
            curbuf = _buffers.erase_after(curbuf_prev);
            _carriage->set_offset(_carriage->offset() + _carriage->length());
            _carriage->set_length(0);
            _buffers.push_back(*_carriage);
        } else {
            curbuf = _buffers.erase_after_and_dispose(curbuf_prev);
            _num -= 1;
        }
        len -= howmuch;
        off = 0;
    }
}

void buffer::list::write(int off, int len, std::ostream &out) const {
    list s;
    s.substr_of(*this, off, len);
    for (const auto &node : s._buffers) {
        if (node.length()) {
            out.write(node.c_str(), node.length());
        }
    }
}

void buffer::list::encode_base64(buffer::list &o) {
    bufferptr bp(length() * 4 / 3 + 3);
    int l = armor(bp.c_str(), bp.c_str() + bp.length(), c_str(), c_str() + length());
    bp.set_length(l);
    o.push_back(std::move(bp));
}

void buffer::list::decode_base64(buffer::list &e) {
    bufferptr bp(4 + ((e.length() * 3) / 4));
    int l = unarmor(bp.c_str(), bp.c_str() + bp.length(), e.c_str(), e.c_str() + e.length());
    if (l < 0) {
        std::ostringstream oss;
        oss << "decode_base64: decoding failed:\n";
        hexdump(oss);
        throw buffer::malformed_input(oss.str().c_str());
    }
    cxxlab_assert(l <= (int)bp.length());
    bp.set_length(l);
    push_back(std::move(bp));
}

ssize_t buffer::list::pread_file(const char *fn, uint64_t off, uint64_t len, std::string *error) {
    int fd = TEMP_FAILURE_RETRY(::open(fn, O_RDONLY | O_CLOEXEC));
    if (fd < 0) {
        int err = errno;
        std::ostringstream oss;
        oss << "can't open " << fn << ": " << cpp_strerror(err);
        *error = oss.str();
        return -err;
    }

    struct stat st;
    memset(&st, 0, sizeof(st));
    if (::fstat(fd, &st) < 0) {
        int err = errno;
        std::ostringstream oss;
        oss << "bufferlist::read_file(" << fn << "): stat error: "
            << cpp_strerror(err);
        *error = oss.str();
        VOID_TEMP_FAILURE_RETRY(::close(fd));
        return -err;
    }

    if (off > (uint64_t)st.st_size) {
        std::ostringstream oss;
        oss << "bufferlist::read_file(" << fn << "): read error: size < offset";
        *error = oss.str();
        VOID_TEMP_FAILURE_RETRY(::close(fd));
        return 0;
    }

    if (len > st.st_size - off) {
        len = st.st_size - off;
    }
    ssize_t ret = lseek64(fd, off, SEEK_SET);
    if (ret != (ssize_t)off) {
        return -errno;
    }

    ret = read_fd(fd, len);
    if (ret < 0) {
        std::ostringstream oss;
        oss << "bufferlist::read_file(" << fn << "): read error:"
            << cpp_strerror(ret);
        *error = oss.str();
        VOID_TEMP_FAILURE_RETRY(::close(fd));
        return ret;
    } else if (ret != (ssize_t)len) {
        std::ostringstream oss;
        oss << "bufferlist::read_file(" << fn << "): warning: got premature EOF.";
        *error = oss.str();
    }
    VOID_TEMP_FAILURE_RETRY(::close(fd));
    return 0;
}

int buffer::list::read_file(const char *fn, std::string *error) {
    int fd = TEMP_FAILURE_RETRY(::open(fn, O_RDONLY | O_CLOEXEC));
    if (fd < 0) {
        int err = errno;
        std::ostringstream oss;
        oss << "can't open " << fn << ": " << cpp_strerror(err);
        *error = oss.str();
        return -err;
    }

    struct stat st;
    memset(&st, 0, sizeof(st));
    if (::fstat(fd, &st) < 0) {
        int err = errno;
        std::ostringstream oss;
        oss << "bufferlist::read_file(" << fn << "): stat error: "
            << cpp_strerror(err);
        *error = oss.str();
        VOID_TEMP_FAILURE_RETRY(::close(fd));
        return -err;
    }

    ssize_t ret = read_fd(fd, st.st_size);
    if (ret < 0) {
        std::ostringstream oss;
        oss << "bufferlist::read_file(" << fn << "): read error:"
            << cpp_strerror(ret);
        *error = oss.str();
        VOID_TEMP_FAILURE_RETRY(::close(fd));
        return ret;
    } else if (ret != st.st_size) {
        std::ostringstream oss;
        oss << "bufferlist::read_file(" << fn << "): warning: got premature EOF.";
        *error = oss.str();
    }
    VOID_TEMP_FAILURE_RETRY(::close(fd));
    return 0;
}

ssize_t buffer::list::read_fd(int fd, size_t len) {
    auto bp = ptr_node::create(buffer::create(len));
    ssize_t ret = safe_read(fd, (void *)bp->c_str(), len);
    if (ret >= 0) {
        bp->set_length(ret);
        push_back(std::move(bp));
    }
    return ret;
}

ssize_t buffer::list::recv_fd(int fd, size_t len) {
    auto bp = ptr_node::create(buffer::create(len));
    ssize_t ret = safe_recv(fd, (void *)bp->c_str(), len);
    if (ret >= 0) {
        bp->set_length(ret);
        push_back(std::move(bp));
    }
    return ret;
}

int buffer::list::write_file(const char *fn, int mode) {
    int fd = TEMP_FAILURE_RETRY(::open(fn, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode));
    if (fd < 0) {
        int err = errno;
        spdlog::error("bufferlist::write_file({}): failed to open file: {}",
                      fn, cpp_strerror(err));
        return -err;
    }
    int ret = write_fd(fd);
    if (ret) {
        spdlog::error("bufferlist::write_fd({}): write_fd error: {}",
                      fn, cpp_strerror(ret));
        VOID_TEMP_FAILURE_RETRY(::close(fd));
        return ret;
    }
    if (TEMP_FAILURE_RETRY(::close(fd))) {
        int err = errno;
        spdlog::error("bufferlist::write_file({}): close error: {}",
                      fn, cpp_strerror(err));
        return -err;
    }
    return 0;
}

static int do_writev(int fd, struct iovec *vec, uint64_t offset, unsigned veclen, unsigned bytes) {
    while (bytes > 0) {
        ssize_t r = 0;
        r = ::pwritev(fd, vec, veclen, offset);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }

        bytes -= r;
        offset += r;
        if (bytes == 0) break;

        while (r > 0) {
            if (vec[0].iov_len <= (size_t)r) {
                r -= vec[0].iov_len;
                ++vec;
                --veclen;
            } else {
                vec[0].iov_base = (char *)vec[0].iov_base + r;
                vec[0].iov_len -= r;
                break;
            }
        }
    }
    return 0;
}

int buffer::list::write_fd(int fd) const {
    iovec iov[IOV_MAX];
    int iovlen = 0;
    ssize_t bytes = 0;

    auto p = std::cbegin(_buffers);
    while (p != std::cend(_buffers)) {
        if (p->length() > 0) {
            iov[iovlen].iov_base = (void *)p->c_str();
            iov[iovlen].iov_len = p->length();
            bytes += p->length();
            iovlen++;
        }
        ++p;

        if (iovlen == IOV_MAX ||
            p == _buffers.end()) {
            iovec *start = iov;
            int num = iovlen;
            ssize_t wrote;
retry:
            wrote = ::writev(fd, start, num);
            if (wrote < 0) {
                int err = errno;
                if (err == EINTR)
                    goto retry;
                return -err;
            }
            if (wrote < bytes) {
                while ((size_t)wrote >= start[0].iov_len) {
                    wrote -= start[0].iov_len;
                    bytes -= start[0].iov_len;
                    start++;
                    num--;
                }
                if (wrote > 0) {
                    start[0].iov_len -= wrote;
                    start[0].iov_base = (char *)start[0].iov_base + wrote;
                    bytes -= wrote;
                }
                goto retry;
            }
            iovlen = 0;
            bytes = 0;
        }
    }
    return 0;
}

int buffer::list::send_fd(int fd) const {
    return buffer::list::write_fd(fd);
}

int buffer::list::write_fd(int fd, uint64_t offset) const {
    iovec iov[IOV_MAX];

    auto p = std::cbegin(_buffers);
    uint64_t left_pbrs = get_num_buffers();
    while (left_pbrs) {
        ssize_t bytes = 0;
        unsigned iovlen = 0;
        uint64_t size = std::min<uint64_t>(left_pbrs, IOV_MAX);
        left_pbrs -= size;
        while (size > 0) {
            iov[iovlen].iov_base = (void *)p->c_str();
            iov[iovlen].iov_len = p->length();
            iovlen++;
            bytes += p->length();
            ++p;
            size--;
        }

        int r = do_writev(fd, iov, offset, iovlen, bytes);
        if (r < 0)
            return r;
        offset += bytes;
    }
    return 0;
}

buffer::list::iov_vec_t buffer::list::prepare_iovs() const {
    size_t index = 0;
    uint64_t off = 0;
    iov_vec_t iovs{_num / IOV_MAX + 1};
    auto it = iovs.begin();
    for (auto &bp : _buffers) {
        if (index == 0) {
            it->offset = off;
            it->length = 0;
            size_t nr_iov_created = std::distance(iovs.begin(), it);
            it->iov.resize(
                std::min(_num - IOV_MAX * nr_iov_created, (size_t)IOV_MAX));
        }
        it->iov[index].iov_base = (void *)bp.c_str();
        it->iov[index].iov_len = bp.length();
        off += bp.length();
        it->length += bp.length();
        if (++index == IOV_MAX) {
            ++it;
            index = 0;
        }
    }
    return iovs;
}

uint32_t buffer::list::crc32c(uint32_t crc) const {
    int cache_misses = 0;
    int cache_hits = 0;
    int cache_adjusts = 0;

    for (const auto &node : _buffers) {
        if (node.length()) {
            raw *const r = node._raw;
            std::pair<size_t, size_t> ofs(node.offset(), node.offset() + node.length());
            std::pair<uint32_t, uint32_t> ccrc;
            if (r->get_crc(ofs, &ccrc)) {
                if (ccrc.first == crc) {
                    crc = ccrc.second;
                    cache_hits++;
                } else {
                    crc = ccrc.second ^ calc_crc32(NULL, node.length(), ccrc.first ^ crc) ^ calc_crc32(NULL, node.length(), 0);
                    cache_adjusts++;
                }
            } else {
                cache_misses++;
                uint32_t base = crc;
                crc = calc_crc32((unsigned char *)node.c_str(), node.length(), crc);
                r->set_crc(ofs, std::make_pair(base, crc));
            }
        }
    }

    if (buffer_track_crc) {
        if (cache_adjusts)
            buffer_cached_crc_adjusted += cache_adjusts;
        if (cache_hits)
            buffer_cached_crc += cache_hits;
        if (cache_misses)
            buffer_missed_crc += cache_misses;
    }

    return crc;
}

void buffer::list::invalidate_crc() {
    for (const auto &node : _buffers) {
        if (node._raw) {
            node._raw->invalidate_crc();
        }
    }
}

void buffer::list::write_stream(std::ostream &out) const {
    for (const auto &node : _buffers) {
        if (node.length() > 0) {
            out.write(node.c_str(), node.length());
        }
    }
}

void buffer::list::hexdump(std::ostream &out, bool trailing_newline) const {
    if (!length())
        return;

    std::ios_base::fmtflags original_flags = out.flags();

    out.setf(std::ios::right);
    out.fill('0');

    unsigned per = 16;
    char last_row_char = '\0';
    bool was_same = false, did_star = false;
    for (unsigned o = 0; o < length(); o += per) {
        if (o == 0) {
            last_row_char = (*this)[o];
        }

        if (o + per < length()) {
            bool row_is_same = true;
            for (unsigned i = 0; i < per && o + i < length(); i++) {
                char current_char = (*this)[o + i];
                if (current_char != last_row_char) {
                    if (i == 0) {
                        last_row_char = current_char;
                        was_same = false;
                        did_star = false;
                    } else {
                        row_is_same = false;
                    }
                }
            }
            if (row_is_same) {
                if (was_same) {
                    if (!did_star) {
                        out << "\n*";
                        did_star = true;
                    }
                    continue;
                }
                was_same = true;
            } else {
                was_same = false;
                did_star = false;
            }
        }
        if (o)
            out << "\n";
        out << std::hex << std::setw(8) << o << " ";

        unsigned i;
        for (i = 0; i < per && o + i < length(); i++) {
            if (i == 8)
                out << ' ';
            out << " " << std::setw(2) << ((unsigned)(*this)[o + i] & 0xff);
        }
        for (; i < per; i++) {
            if (i == 8)
                out << ' ';
            out << "   ";
        }

        out << "  |";
        for (i = 0; i < per && o + i < length(); i++) {
            char c = (*this)[o + i];
            if (isupper(c) || islower(c) || isdigit(c) || c == ' ' || ispunct(c))
                out << c;
            else
                out << '.';
        }
        out << '|' << std::dec;
    }
    if (trailing_newline) {
        out << "\n"
            << std::hex << std::setw(8) << length();
        out << "\n";
    }

    out.flags(original_flags);
}

buffer::list buffer::list::static_from_mem(char *c, size_t l) {
    list bl;
    bl.push_back(ptr_node::create(create_static(l, c)));
    return bl;
}

buffer::list buffer::list::static_from_cstring(char *c) {
    return static_from_mem(c, std::strlen(c));
}

buffer::list buffer::list::static_from_string(std::string &s) {
    return static_from_mem(const_cast<char *>(s.data()), s.length());
}

bool buffer::ptr_node::dispose_if_hypercombined(
    buffer::ptr_node *const) {
    return false;
}

std::unique_ptr<buffer::ptr_node, buffer::ptr_node::disposer>
buffer::ptr_node::create_hypercombined(unique_leakable_ptr<buffer::raw> r) {
    return std::unique_ptr<buffer::ptr_node, buffer::ptr_node::disposer>(
        new ptr_node(std::move(r)));
}

buffer::ptr_node *buffer::ptr_node::cloner::operator()(
    const buffer::ptr_node &clone_this) {
    return new ptr_node(clone_this);
}

std::ostream &buffer::operator<<(std::ostream &out, const buffer::raw &r) {
    return out << "buffer::raw("
               << (void *)r.get_data() << " len " << r.get_len()
               << " nref " << r.nref.load() << ")";
}

std::ostream &buffer::operator<<(std::ostream &out, const buffer::ptr &bp) {
    if (bp.have_raw())
        out << "buffer::ptr(" << bp.offset() << "~" << bp.length()
            << " " << (void *)bp.c_str()
            << " in raw " << (void *)bp.raw_c_str()
            << " len " << bp.raw_length()
            << " nref " << bp.raw_nref() << ")";
    else
        out << "buffer:ptr(" << bp.offset() << "~" << bp.length() << " no raw)";
    return out;
}

std::ostream &buffer::operator<<(std::ostream &out, const buffer::list &bl) {
    out << "buffer::list(len=" << bl.length() << ",\n";

    for (const auto &node : bl.buffers()) {
        out << "\t" << node;
        if (&node != &bl.buffers().back()) {
            out << ",\n";
        }
    }
    out << "\n)";
    return out;
}

void buffer::list::page_aligned_appender::_refill(size_t len) {
    const unsigned alloc =
        std::max(min_alloc,
                 shift_round_up(static_cast<unsigned>(len),
                                static_cast<unsigned>(page().shift)));
    auto new_back =
        ptr_node::create(buffer::create_page_aligned(alloc));
    new_back->set_length(0);
    bl.push_back(std::move(new_back));
}

}  // namespace TOPNSPC
