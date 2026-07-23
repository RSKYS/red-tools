/**
 * @file nm_generators.h
 * @brief Nginx HTTP/stream configuration generation and SNI route merging.
 *
 * @internal These declarations match the project's single-translation-unit
 * implementation and are not a stable external ABI.
 */
#ifndef NM_GENERATORS_H
#define NM_GENERATORS_H

#include "nm_core.h"

NM_BEGIN_DECLS

/**
 * @brief Returns the comment prefix used to enable or disable generated IPv6
 * listeners.
 * @return A borrowed pointer that remains valid for the lifetime described by
 * the implementation.
 */
NM_INTERNAL const char *ipv6_prefix(void);

/**
 * @brief Writes a generated buffer to a temporary candidate and installs it
 * with optional Nginx validation.
 * @param target Live target path or generated target identifier.
 * @param b Buffer object.
 * @param mode POSIX permission mode.
 * @param validate Nonzero to run isolated nginx -t validation before
 * installation.
 * @param failure Fatal error text used if installation cannot complete.
 */
NM_INTERNAL void install_buffer_target(const char *target, Buffer *b,
                                       mode_t mode, intS validate,
                                       const char *failure);

/**
 * @brief Generates the forced-SSL redirect snippet for a selected HTTPS port.
 * @param https_port Public HTTPS port associated with the node.
 */
NM_INTERNAL void write_forced_ssl_snippet(unsigned https_port);

/**
 * @brief Generates the default forced-SSL redirect snippet.
 */
NM_INTERNAL void write_redirect_snippet(void);

/**
 * @brief Ensures required snippets and include directives exist.
 */
NM_INTERNAL void ensure_support_files(void);

/**
 * @brief Generates the public port-80 redirect configuration.
 */
NM_INTERNAL void write_redirect_80(void);

/**
 * @brief Ensures nginx.conf includes script-managed stream configurations.
 */
NM_INTERNAL void ensure_stream_include(void);

/**
 * @brief Tests whether a server_name directive contains an exact domain token.
 * @param value Directive value or server endpoint being inspected or
 * normalized.
 * @param domain Validated public domain.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS server_name_contains(const char *value, const char *domain);

/**
 * @brief Generates or updates the domain-specific public HTTP redirect.
 */
NM_INTERNAL void write_domain_redirect_80(void);

/**
 * @brief Generates the redirect server for a custom HTTP/HTTPS port pair.
 */
NM_INTERNAL void write_custom_redirect_config(void);

/**
 * @brief Tests whether a stream map declaration targets a specific variable.
 * @param line One Nginx configuration line.
 * @param variable Nginx map variable.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS map_header_matches(const char *line, const char *variable);

/**
 * @brief Extracts an upstream block name from a declaration line.
 * @param line One Nginx configuration line.
 * @param out Caller-provided output object or buffer.
 * @param cap Total capacity of the output buffer in bytes.
 * @return Nonzero on success; otherwise zero.
 */
NM_INTERNAL intS upstream_header_name(const char *line, char *out, size_t cap);

/**
 * @brief Finds the upstream currently selected for a domain in a stream route
 * file.
 * @param path Filesystem or procfs path.
 * @param domain Validated public domain.
 * @param map_variable Nginx stream map variable name.
 * @param out Caller-provided output object or buffer.
 * @param cap Total capacity of the output buffer in bytes.
 * @return Nonzero on success; otherwise zero.
 */
NM_INTERNAL intS find_stream_route_upstream(const char *path,
                                            const char *domain,
                                            const char *map_variable, char *out,
                                            size_t cap);

/**
 * @brief Tests whether a stream map route line belongs to a domain.
 * @param line One Nginx configuration line.
 * @param domain Validated public domain.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS route_line_for_domain(const char *line, const char *domain);

/**
 * @brief Merges one domain route and upstream into an existing stream
 * configuration.
 * @param source Source configuration file path or source text.
 * @param domain Validated public domain.
 * @param upstream Generated upstream name.
 * @param backend_port Backend TCP port.
 * @param map_variable Nginx stream map variable name.
 * @param reject_upstream Fallback upstream used for unmatched SNI.
 * @param existing_upstream Previously routed upstream, when one exists.
 * @param output Generated candidate output path.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS merge_stream_sni_candidate(
    const char *source, const char *domain, const char *upstream,
    unsigned backend_port, const char *map_variable,
    const char *reject_upstream, const char *existing_upstream,
    const char *output);

/**
 * @brief Generates or updates SNI stream routing for the selected public port.
 * @param public_port Public TLS/SNI listener port.
 */
NM_INTERNAL void write_stream_sni_config(unsigned public_port);

NM_END_DECLS

#endif
