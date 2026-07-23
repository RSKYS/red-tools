/**
 * @file nm_portability.h
 * @brief Compiler, platform, diagnostics, and defensive-programming helpers.
 *
 * This project is built as a single translation unit by including the module
 * implementation files from scripts/nginx-manager.c.  The macros in this file
 * keep that build model explicit while remaining accepted by modern GCC and
 * Clang versions on Linux.
 */
#ifndef NM_PORTABILITY_H
#define NM_PORTABILITY_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

/** @brief Stringifies a preprocessor token after macro expansion. */
#define NM_STRINGIFY_IMPL(value) #value
/** @brief Stringifies a preprocessor token after macro expansion. */
#define NM_STRINGIFY(value) NM_STRINGIFY_IMPL(value)

/** @brief Concatenates two preprocessor tokens after macro expansion. */
#define NM_CONCAT_IMPL(left, right) left##right
/** @brief Concatenates two preprocessor tokens after macro expansion. */
#define NM_CONCAT(left, right) NM_CONCAT_IMPL(left, right)

#if defined(__GNUC__) || defined(__clang__)
/** @brief Marks a printf-compatible function and enables format checking. */
#define NM_PRINTF(format_index, first_arg)                                     \
  __attribute__((format(printf, format_index, first_arg)))
/** @brief Marks a function that never returns to its caller. */
#define NM_NORETURN __attribute__((noreturn))
/** @brief Marks a function whose return value should not be discarded. */
#define NM_NODISCARD __attribute__((warn_unused_result))
/** @brief Marks an intentionally unused declaration or object. */
#define NM_UNUSED __attribute__((unused))
/** @brief Marks an allocator-like function whose result does not alias live
 * objects. */
#define NM_MALLOC __attribute__((malloc))
/** @brief Associates an allocation result size with a size parameter. */
#define NM_ALLOC_SIZE(index) __attribute__((alloc_size(index)))
/** @brief Declares one or more pointer parameters as non-null. */
#define NM_NONNULL(...) __attribute__((nonnull(__VA_ARGS__)))
/** @brief Informs the compiler that a branch is expected to be true. */
#define NM_LIKELY(expression) __builtin_expect(!!(expression), 1)
/** @brief Informs the compiler that a branch is expected to be false. */
#define NM_UNLIKELY(expression) __builtin_expect(!!(expression), 0)
/** @brief Pushes the active compiler diagnostic state. */
#define NM_DIAGNOSTIC_PUSH _Pragma("GCC diagnostic push")
/** @brief Restores the previously pushed compiler diagnostic state. */
#define NM_DIAGNOSTIC_POP _Pragma("GCC diagnostic pop")
/** @brief Disables a GCC/Clang diagnostic named by a string literal. */
#define NM_DIAGNOSTIC_IGNORE(option) \
  _Pragma(NM_STRINGIFY(GCC diagnostic ignored option))
#else
#define NM_PRINTF(format_index, first_arg)
#define NM_NORETURN
#define NM_NODISCARD
#define NM_UNUSED
#define NM_MALLOC
#define NM_ALLOC_SIZE(index)
#define NM_NONNULL(...)
#define NM_LIKELY(expression) (!!(expression))
#define NM_UNLIKELY(expression) (!!(expression))
#define NM_DIAGNOSTIC_PUSH
#define NM_DIAGNOSTIC_POP
#define NM_DIAGNOSTIC_IGNORE(option)
#endif

#if defined(__cplusplus)
/** @brief Opens a C-linkage declaration region when included from C++. */
#define NM_BEGIN_DECLS extern "C" {
/** @brief Closes a C-linkage declaration region when included from C++. */
#define NM_END_DECLS }
#else
#define NM_BEGIN_DECLS
#define NM_END_DECLS
#endif

/**
 * @brief Declares an internal function in the amalgamated translation unit.
 *
 * Every module implementation is included into one C translation unit, so
 * internal declarations must have internal linkage to match their definitions.
 */
#if defined(NM_AMALGAMATED_BUILD)
#define NM_INTERNAL static
#else
#define NM_INTERNAL extern
#endif

/** @brief Compile-time assertion with a descriptive diagnostic message. */
#define NM_STATIC_ASSERT(condition, message)                                   \
  _Static_assert((condition), message)

/** @brief Returns the number of elements in a true C array. */
#define NM_ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))

/**
 * @brief Backward-compatible array-length spelling used by existing sources.
 * @warning Do not pass a pointer; the result is meaningful only for arrays.
 */
#define ARRAY_LEN(array) NM_ARRAY_LEN(array)

/**
 * @brief Releases an allocated pointer and clears the lvalue.
 *
 * Clearing the pointer prevents accidental reuse and makes repeated cleanup
 * paths idempotent.
 */
#define NM_FREE_AND_NULL(pointer)    \
  do {                               \
    free(pointer);                   \
    (pointer) = NULL;                \
  } while (0)

/**
 * @brief Closes a valid file descriptor and resets it to -1.
 *
 * Close failures are intentionally ignored in cleanup-only paths.  Operations
 * that need to report close errors must call close() explicitly.
 */
#define NM_CLOSE_FD(descriptor)      \
  do {                               \
    if ((descriptor) >= 0) {         \
      (void)close(descriptor);       \
      (descriptor) = -1;             \
    }                                \
  } while (0)

/** @brief Returns from a void function when a required pointer is null. */
#define NM_RETURN_IF_NULL(pointer)   \
  do {                               \
    if (NM_UNLIKELY((pointer) == NULL)) {                                      \
      errno = EINVAL;                \
      return;                        \
    }                                \
  } while (0)

/** @brief Returns a caller-supplied value when a required pointer is null. */
#define NM_RETURN_VALUE_IF_NULL(pointer, value)                                \
  do {                               \
    if (NM_UNLIKELY((pointer) == NULL)) {                                      \
      errno = EINVAL;                \
      return (value);                \
    }                                \
  } while (0)

/** @brief Sets errno and returns a caller-supplied value when a condition
 * holds. */
#define NM_RETURN_ERROR_IF(condition, error_number, value)                     \
  do {                               \
    if (NM_UNLIKELY(condition)) {    \
      errno = (error_number);        \
      return (value);                \
    }                                \
  } while (0)

/** @brief Jumps to a cleanup label when a condition holds. */
#define NM_GOTO_IF(condition, label) \
  do {                               \
    if (NM_UNLIKELY(condition)) {    \
      goto label;                    \
    }                                \
  } while (0)

/**
 * @brief Optional compile-time logging integration hook.
 *
 * Integrators may define NM_LOG_HOOK(level, label, message) before including
 * project headers.  The default implementation has no side effects.
 */
#ifndef NM_LOG_HOOK
#define NM_LOG_HOOK(level, label, message)                                     \
  do {                               \
    (void)(level);                   \
    (void)(label);                   \
    (void)(message);                 \
  } while (0)
#endif

#endif
