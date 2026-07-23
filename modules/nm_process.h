/**
 * @file nm_process.h
 * @brief Process execution, executable discovery, port ownership, and
 * randomness.
 *
 * @internal These declarations match the project's single-translation-unit
 * implementation and are not a stable external ABI.
 */
#ifndef NM_PROCESS_H
#define NM_PROCESS_H

#include "nm_core.h"

NM_BEGIN_DECLS

/**
 * @brief Checks whether a path identifies an executable regular file.
 * @param path Filesystem or procfs path.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS executable_file(const char *path);

/**
 * @brief Resolves an executable name through PATH or validates an explicit
 * path.
 * @param name Executable, upstream, environment, or block name.
 * @param out Caller-provided output object or buffer.
 * @param cap Total capacity of the output buffer in bytes.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS command_path(const char *name, char *out, size_t cap);

/**
 * @brief Tests whether an executable can be resolved.
 * @param name Executable, upstream, environment, or block name.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS command_exists(const char *name);

/**
 * @brief Forks and executes a command, optionally suppressing or capturing
 * output.
 * @param argv NULL-terminated argument vector; element zero names the
 * executable.
 * @param quiet Nonzero to redirect uncaptured child output to /dev/null.
 * @param capture Optional initialized buffer receiving combined stdout and
 * stderr.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int run_process(char *const argv[], intS quiet, Buffer *capture);

/**
 * @brief Runs a required command and terminates through fatalf() on failure.
 * @param description Human-readable operation description used for logging.
 * @param argv NULL-terminated argument vector; element zero names the
 * executable.
 */
NM_INTERNAL void run_or_die(const char *description, char *const argv[]);

/**
 * @brief Scans a Linux procfs TCP table for a listening port and optionally
 * records socket inodes.
 * @param path Filesystem or procfs path.
 * @param port TCP port number.
 * @param inodes Optional destination vector for matching socket inode strings.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS read_proc_listen_table(const char *path, unsigned port,
                                        StrVec *inodes);

/**
 * @brief Checks both IPv4 and IPv6 procfs tables for a listening TCP port.
 * @param port TCP port number.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS port_in_use(unsigned port);

/**
 * @brief Determines whether a numeric procfs process entry belongs to Nginx.
 * @param pid_name Numeric procfs process-directory name.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS pid_is_nginx(const char *pid_name);

/**
 * @brief Determines whether a listening port is owned by an Nginx process.
 * @param port TCP port number.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS port_owned_by_nginx(unsigned port);

/**
 * @brief Returns a best-effort 32-bit random value using the kernel RNG with a
 * deterministic fallback.
 * @return A 32-bit pseudo-random value.
 */
NM_INTERNAL uint32_t random_u32(void);

NM_END_DECLS

#endif
