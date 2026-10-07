#include <cxxabi.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "cassert.h"
#include "logger.h"

namespace TOPNSPC {
namespace {

// ── Backtrace (integrated, no separate module) ─────────────────

class Backtrace {
public:
    static constexpr int max_frames = 32;

    Backtrace() {
        size_ = backtrace(array_, max_frames);
    }

    std::string str() const {
        auto syms = backtrace_symbols(array_, size_);
        if (!syms) return {};

        std::string out;
        // 跳过帧 0（Backtrace 构造函数自身），从帧 1 开始
        for (size_t i = 1; i < size_; i++) {
            out += "  ";
            out += std::to_string(i);
            out += ": ";
            out += demangle(syms[i]);
            out += "\n";
        }
        free(syms);
        return out;
    }

private:
    static std::string demangle(const char *name) {
        const char *begin = nullptr;
        const char *end = nullptr;
        for (const char *j = name; *j; ++j) {
            if (*j == '(') {
                begin = j + 1;
            } else if (*j == '+') {
                end = j;
            }
        }
        if (begin && end && begin < end) {
            std::string mangled(begin, end);
            if (mangled.compare(0, 2, "_Z") == 0) {
                int status = 0;
                char *demangled = abi::__cxa_demangle(
                    mangled.c_str(), nullptr, nullptr, &status);
                if (demangled) {
                    std::string result(1, '(');
                    result += demangled;
                    result += end;
                    free(demangled);
                    return result;
                }
            }
            return mangled + "()";
        }
        return name;
    }

    void *array_[max_frames]{};
    size_t size_{0};
};

// ── Crash logger (delegates to logger module) ───────────────────

Logger &get_crash_logger() {
    return TOPNSPC::get_crash_logger();
}

[[noreturn]] void emit_and_abort(const std::string &msg) {
    auto &log = get_crash_logger();
    if (log.valid()) {
        try {
            log->error("{}", msg);
            log->flush();
        } catch (...) {
            fprintf(stderr, "%s\n", msg.c_str());
        }
    } else {
        fprintf(stderr, "%s\n", msg.c_str());
    }
    abort();
}

void emit_warn(const std::string &msg) {
    auto &log = get_crash_logger();
    if (log.valid()) {
        try {
            log->warn("{}", msg);
            log->flush();
        } catch (...) {
            fprintf(stderr, "%s\n", msg.c_str());
        }
    } else {
        fprintf(stderr, "%s\n", msg.c_str());
    }
}

std::string build_header(const char *file, int line,
                         const char *func, const char *kind) {
    auto ts = get_timestamp();
    auto pid = getpid();
    auto tid = (unsigned long long)pthread_self();
    auto tname = get_thread_name();

    std::string h;
    h += file;
    h += ": In function '";
    h += func;
    h += "' process ";
    h += std::to_string(pid);
    h += " thread ";
    h += tname;
    h += " (";
    h += std::to_string(tid);
    h += ") time ";
    h += ts;
    h += "\n";
    h += file;
    h += ": ";
    h += std::to_string(line);
    h += ": ";
    h += kind;
    return h;
}

}  // anonymous namespace

// ── Public API ─────────────────────────────────────────────────

[[gnu::cold]] void __common_assert_fail(const char *assertion,
                                        const char *file, int line,
                                        const char *func) {
    auto bt = Backtrace{};
    std::string msg = build_header(file, line, func, "FAILED common_assert");
    msg += "(";
    msg += assertion;
    msg += ")\n";
    msg += bt.str();
    emit_and_abort(msg);
}

[[gnu::cold]] void __common_assertf_fail(const char *assertion,
                                         const char *file, int line,
                                         const char *func,
                                         const char *msg_fmt, ...) {
    auto bt = Backtrace{};
    std::string msg = build_header(file, line, func, "FAILED common_assert");
    msg += "(";
    msg += assertion;
    msg += ")\n";
    msg += "Assertion details: ";
    va_list args;
    va_start(args, msg_fmt);
    char details[4096];
    vsnprintf(details, sizeof(details), msg_fmt, args);
    va_end(args);
    msg += details;
    msg += "\n";
    msg += bt.str();
    emit_and_abort(msg);
}

[[gnu::cold]] void __common_abort(const char *file, int line,
                                  const char *func,
                                  const std::string &abort_msg) {
    auto bt = Backtrace{};
    std::string msg = build_header(file, line, func, "common_abort_msg");
    msg += "(\"";
    msg += abort_msg;
    msg += "\")\n";
    msg += bt.str();
    emit_and_abort(msg);
}

[[gnu::cold]] void __common_abortf(const char *file, int line,
                                   const char *func,
                                   const char *msg_fmt, ...) {
    auto bt = Backtrace{};
    std::string msg = build_header(file, line, func, "abort");
    msg += "()\n";
    msg += "Abort details: ";
    va_list args;
    va_start(args, msg_fmt);
    char details[4096];
    vsnprintf(details, sizeof(details), msg_fmt, args);
    va_end(args);
    msg += details;
    msg += "\n";
    msg += bt.str();
    emit_and_abort(msg);
}

void __common_assert_warn(const char *assertion,
                          const char *file, int line,
                          const char *func) {
    auto bt = Backtrace{};
    std::string msg = "WARNING: common_assert(";
    msg += assertion;
    msg += ") at: ";
    msg += file;
    msg += ": ";
    msg += std::to_string(line);
    msg += " ";
    msg += func;
    msg += "()\n";
    msg += bt.str();
    emit_warn(msg);
}

}  // namespace TOPNSPC
