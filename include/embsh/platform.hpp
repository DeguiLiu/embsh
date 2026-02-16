/**
 * @file platform.hpp
 * @brief Minimal platform detection, assertion macros, and compiler hints.
 */

#ifndef EMBSH_PLATFORM_HPP_
#define EMBSH_PLATFORM_HPP_

#include <cstdint>
#include <cstdio>
#include <cstdlib>

// ============================================================================
// Platform detection
// ============================================================================

#if defined(__linux__)
#define EMBSH_PLATFORM_LINUX 1
#else
#define EMBSH_PLATFORM_LINUX 0
#endif

// ============================================================================
// Compiler hints
// ============================================================================

#if defined(__GNUC__) || defined(__clang__)
#define EMBSH_LIKELY(x) __builtin_expect(!!(x), 1)
#define EMBSH_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define EMBSH_UNUSED __attribute__((unused))
#else
#define EMBSH_LIKELY(x) (x)
#define EMBSH_UNLIKELY(x) (x)
#define EMBSH_UNUSED
#endif

// ============================================================================
// Token pasting
// ============================================================================

#define EMBSH_CONCAT_IMPL(a, b) a##b
#define EMBSH_CONCAT(a, b) EMBSH_CONCAT_IMPL(a, b)

// ============================================================================
// Assertion
// ============================================================================

#define EMBSH_ASSERT(cond)                                                   \
  do {                                                                       \
    if (EMBSH_UNLIKELY(!(cond))) {                                           \
      std::fprintf(stderr, "EMBSH ASSERT FAILED: %s (%s:%d)\n", #cond,      \
                   __FILE__, __LINE__);                                      \
      std::abort();                                                          \
    }                                                                        \
  } while (0)

#endif  // EMBSH_PLATFORM_HPP_
