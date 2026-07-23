/**
 * @file nm_core.h
 * @brief Core memory, string, buffering, input, logging, and checksum services.
 */
#ifndef NM_CORE_H
#define NM_CORE_H

#include "nm_types.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

NM_BEGIN_DECLS

/**
 * @brief Process-wide mutable application state.
 * @warning The current application is single-threaded; serialize access if
 * extended.
 */
NM_INTERNAL App app;

/**
 * @brief Allocates at least one byte or terminates the process.
 * @param n Requested byte count; zero is normalized to one byte.
 * @return Newly allocated storage that must be released with free().
 */
NM_INTERNAL void *xmalloc(size_t n) NM_MALLOC NM_ALLOC_SIZE(1);

/**
 * @brief Resizes an allocation or terminates the process.
 * @param p Existing allocation or NULL.
 * @param n Requested byte count; zero is normalized to one byte.
 * @return Resized allocation that must be released with free().
 */
NM_INTERNAL void *xrealloc(void *p, size_t n) NM_ALLOC_SIZE(2);

/**
 * @brief Duplicates a NUL-terminated string or terminates the process.
 * @param s Source string.
 * @return Newly allocated duplicate that must be released with free().
 */
NM_INTERNAL char *xstrdup(const char *s) NM_MALLOC;

/**
 * @brief Copies a string into a fixed-capacity destination.
 * @param dst Destination buffer.
 * @param cap Total destination capacity in bytes.
 * @param src NUL-terminated source string.
 * @return 0 on success; -1 with errno set on invalid arguments or overflow.
 */
NM_INTERNAL int str_copy(char *dst, size_t cap, const char *src);

/**
 * @brief Formats text into a fixed-capacity destination.
 * @param dst Destination buffer.
 * @param cap Total destination capacity in bytes.
 * @param fmt printf-compatible format string.
 * @return 0 on success; -1 with errno set on failure or truncation.
 */
NM_INTERNAL int str_printf(char *dst, size_t cap, const char *fmt, ...)
    NM_PRINTF(3, 4);

/**
 * @brief Returns a non-empty environment value or a fallback string.
 * @param name Environment variable name.
 * @param fallback Value used when the variable is absent or empty.
 * @return Borrowed pointer to the environment value or fallback.
 */
NM_INTERNAL const char *env_default(const char *name, const char *fallback);

/**
 * @brief Writes one colorized variadic log record.
 * @param out Destination stream.
 * @param color ANSI color prefix.
 * @param label Severity label.
 * @param fmt printf-compatible format string.
 * @param ap Variadic argument list matching @p fmt.
 */
NM_INTERNAL void vlog_message(FILE *out, const char *color, const char *label,
                              const char *fmt, va_list ap);

/** @brief Logs an informational record. @param fmt printf-compatible format
 * string. */
NM_INTERNAL void log_info(const char *fmt, ...) NM_PRINTF(1, 2);
/** @brief Logs a successful-operation record. @param fmt printf-compatible
 * format string. */
NM_INTERNAL void log_success(const char *fmt, ...) NM_PRINTF(1, 2);
/** @brief Logs a warning record. @param fmt printf-compatible format string. */
NM_INTERNAL void log_warn(const char *fmt, ...) NM_PRINTF(1, 2);
/** @brief Logs an error record. @param fmt printf-compatible format string. */
NM_INTERNAL void log_error(const char *fmt, ...) NM_PRINTF(1, 2);

/**
 * @brief Logs a fatal error, performs cleanup/rollback, and exits.
 * @param fmt printf-compatible format string.
 */
NM_INTERNAL void fatalf(const char *fmt, ...) NM_PRINTF(1, 2) NM_NORETURN;

/** @brief Initializes an empty buffer. @param b Buffer object to initialize. */
NM_INTERNAL void buffer_init(Buffer *b);
/** @brief Releases all buffer storage and resets the object. @param b Buffer
 * object. */
NM_INTERNAL void buffer_free(Buffer *b);
/**
 * @brief Ensures capacity for an additional payload.
 * @param b Buffer object.
 * @param extra Number of payload bytes to append.
 * @return 0 on success; -1 on invalid state or size overflow.
 */
NM_INTERNAL int buffer_reserve(Buffer *b, size_t extra);
/**
 * @brief Appends exactly @p n bytes and maintains a trailing NUL.
 * @param b Destination buffer.
 * @param s Source bytes.
 * @param n Number of bytes to append.
 * @return 0 on success; -1 on invalid arguments or overflow.
 */
NM_INTERNAL int buffer_append_n(Buffer *b, const char *s, size_t n);
/**
 * @brief Appends a NUL-terminated string.
 * @param b Destination buffer.
 * @param s Source string.
 * @return 0 on success; -1 on invalid arguments or overflow.
 */
NM_INTERNAL int buffer_append(Buffer *b, const char *s);
/**
 * @brief Appends formatted text.
 * @param b Destination buffer.
 * @param fmt printf-compatible format string.
 * @return 0 on success; -1 on invalid arguments or overflow.
 */
NM_INTERNAL int buffer_printf(Buffer *b, const char *fmt, ...) NM_PRINTF(2, 3);

/** @brief Initializes an empty string vector. @param v Vector to initialize. */
NM_INTERNAL void strvec_init(StrVec *v);
/** @brief Releases all strings and vector storage. @param v Vector to destroy.
 */
NM_INTERNAL void strvec_free(StrVec *v);
/**
 * @brief Appends an owned duplicate of a string.
 * @param v Destination vector.
 * @param s Source string.
 */
NM_INTERNAL void strvec_push(StrVec *v, const char *s);
/**
 * @brief Tests whether a vector contains an exact string.
 * @param v Vector to inspect.
 * @param s String to find.
 * @return Nonzero when found; otherwise zero.
 */
NM_INTERNAL intS strvec_contains(const StrVec *v, const char *s);

/**
 * @brief Reads one line from standard input and removes CR/LF terminators.
 * @param dst Destination buffer.
 * @param cap Destination capacity.
 * @return Nonzero when a line was read; zero on EOF, error, or invalid input.
 */
NM_INTERNAL intS read_input(char *dst, size_t cap);
/**
 * @brief Prompts repeatedly for a yes/no answer.
 * @param prompt Prompt text.
 * @param default_yes Nonzero to make an empty answer mean yes.
 * @return Nonzero for yes; zero for no or end-of-input.
 */
NM_INTERNAL intS confirm(const char *prompt, intS default_yes);

/**
 * @brief Trims leading and trailing whitespace in place.
 * @param s Mutable NUL-terminated string.
 * @return Pointer to the first non-space byte, or NULL for a null input.
 */
NM_INTERNAL char *trim_in_place(char *s);
/** @brief Counts occurrences of a byte. @param s Source string. @param wanted
 * Byte to count. @return Count. */
NM_INTERNAL int count_char(const char *s, char wanted);
/** @brief Tests a string prefix. @param s Source string. @param prefix Prefix.
 * @return Nonzero on match. */
NM_INTERNAL intS starts_with(const char *s, const char *prefix);
/** @brief Tests a string suffix. @param s Source string. @param suffix Suffix.
 * @return Nonzero on match. */
NM_INTERNAL intS ends_with(const char *s, const char *suffix);
/** @brief Converts ASCII letters to lowercase in place. @param s Mutable
 * string. */
NM_INTERNAL void lower_ascii(char *s);

/**
 * @brief Computes the POSIX cksum CRC-32 value for a byte sequence.
 * @param data Input bytes; may be NULL only when @p len is zero.
 * @param len Input length.
 * @return POSIX cksum checksum.
 */
NM_INTERNAL uint32_t posix_cksum_bytes(const unsigned char *data, size_t len);

NM_END_DECLS

#endif
