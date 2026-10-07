#ifndef COMMON_CASSERT_H
#define COMMON_CASSERT_H

#include <string>

#include "common_fwd.h"

#ifndef __STRING
#define __STRING(x) #x
#endif

#if defined(HAVE_PRETTY_FUNC)
#define __COMMON_ASSERT_FUNCTION __PRETTY_FUNCTION__
#elif defined(HAVE_FUNC)
#define __COMMON_ASSERT_FUNCTION __func__
#else
#define __COMMON_ASSERT_FUNCTION ((const char *)0)
#endif

#define __COMMON_ASSERT_VOID_CAST static_cast<void>

namespace TOPNSPC {

[[noreturn]] void __common_assert_fail(const char *assertion,
                                       const char *file,
                                       int line,
                                       const char *function);
[[noreturn]] void __common_assertf_fail(const char *assertion,
                                        const char *file,
                                        int line,
                                        const char *function,
                                        const char *msg, ...);
void __common_assert_warn(const char *assertion,
                          const char *file,
                          int line,
                          const char *function);
[[noreturn]] void __common_abort(const char *file,
                                 int line,
                                 const char *func,
                                 const std::string &msg);
[[noreturn]] void __common_abortf(const char *file,
                                  int line,
                                  const char *func,
                                  const char *msg, ...);

}  // namespace TOPNSPC

// ── Primary macros ──
//
// Expression form (not do { } while(0)) so that commas inside the expansion
// are protected by parentheses and the macro works correctly when used as a
// GTest argument.

#define common_assert(expr)                 \
    ((expr)                                 \
         ? __COMMON_ASSERT_VOID_CAST(0)     \
         : ::TOPNSPC::__common_assert_fail( \
               __STRING(expr), __FILE__, __LINE__, __COMMON_ASSERT_FUNCTION))

#define assert_warn(expr)                   \
    ((expr)                                 \
         ? __COMMON_ASSERT_VOID_CAST(0)     \
         : ::TOPNSPC::__common_assert_warn( \
               __STRING(expr), __FILE__, __LINE__, __COMMON_ASSERT_FUNCTION))

#define common_abort(...)       \
    ::TOPNSPC::__common_abortf( \
        __FILE__, __LINE__, __COMMON_ASSERT_FUNCTION, __VA_ARGS__)

#define common_abort_msg(msg)  \
    ::TOPNSPC::__common_abort( \
        __FILE__, __LINE__, __COMMON_ASSERT_FUNCTION, msg)

#define common_assertf(expr, ...)                  \
    ((expr)                                        \
         ? __COMMON_ASSERT_VOID_CAST(0)            \
         : ::TOPNSPC::__common_assertf_fail(       \
               __STRING(expr), __FILE__, __LINE__, \
               __COMMON_ASSERT_FUNCTION, __VA_ARGS__))

// ── Global-namespace aliases with cxxlab_ prefix ──

#define cxxlab_assert(expr) common_assert(expr)
#define cxxlab_abort(msg) common_abort_msg(msg)
#define cxxlab_abortf(...) common_abort(__VA_ARGS__)
#define cxxlab_abort_msg(msg) common_abort_msg(msg)
#define cxxlab_assertf(expr, ...) common_assertf(expr, __VA_ARGS__)
#define cxxlab_assert_warn(expr) assert_warn(expr)

#endif  // COMMON_CASSERT_H
