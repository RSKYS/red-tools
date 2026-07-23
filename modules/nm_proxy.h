/**
 * @file nm_proxy.h
 * @brief Reverse-proxy layout collection, upstream discovery, and site
 * generation.
 *
 * @internal These declarations match the project's single-translation-unit
 * implementation and are not a stable external ABI.
 */
#ifndef NM_PROXY_H
#define NM_PROXY_H

#include "nm_core.h"

NM_BEGIN_DECLS

/**
 * @brief Appends standard reverse-proxy headers, timeout handling, and
 * forwarding metadata.
 * @param b Buffer object.
 * @param forwarded_port Client-facing port written into forwarding headers.
 * @param timeout Optional Nginx timeout value.
 */
NM_INTERNAL void append_common_proxy_headers(Buffer *b, unsigned forwarded_port,
                                             const char *timeout);

/**
 * @brief Collects validated custom Nginx directives into a generated fragment.
 * @param out Caller-provided output object or buffer.
 */
NM_INTERNAL void collect_custom_parameters(Buffer *out);

/**
 * @brief Tests whether a generated fragment already contains an exact location
 * block.
 * @param b Buffer object.
 * @param path Filesystem or procfs path.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS location_already_added(const Buffer *b, const char *path);

/**
 * @brief Interactively collects static, proxy, WebSocket, and custom location
 * blocks.
 * @param forwarded_port Client-facing port written into forwarding headers.
 */
NM_INTERNAL void collect_custom_locations(unsigned forwarded_port);

/**
 * @brief Selects a proxy layout and generates the corresponding location
 * fragment.
 * @param forwarded_port Client-facing port written into forwarding headers.
 */
NM_INTERNAL void choose_proxy_layout(unsigned forwarded_port);

/**
 * @brief Extracts referenced named upstreams from proxy_pass directives.
 * @param source Source configuration file path or source text.
 * @param names Destination vector of referenced upstream names.
 */
NM_INTERNAL void extract_proxy_upstreams(const char *source, StrVec *names);

/**
 * @brief Tests whether a configuration file defines a named upstream.
 * @param path Filesystem or procfs path.
 * @param wanted Exact upstream or route name being searched for.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS file_defines_upstream(const char *path, const char *wanted);

/**
 * @brief Recursively searches configuration paths for a named upstream
 * definition.
 * @param dir Directory searched recursively.
 * @param wanted Exact upstream or route name being searched for.
 * @param excluded Optional path excluded from recursive search.
 * @param found Caller-provided buffer receiving the discovered path.
 * @param cap Total capacity of the output buffer in bytes.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS find_upstream_recursive(const char *dir, const char *wanted,
                                         const char *excluded, char *found,
                                         size_t cap);

/**
 * @brief Validates and normalizes an Nginx upstream server endpoint.
 * @param value Directive value or server endpoint being inspected or
 * normalized.
 * @return Nonzero on success; otherwise zero.
 */
NM_INTERNAL intS normalize_upstream_server(char *value);

/**
 * @brief Collects referenced upstream definitions into a generated target
 * fragment.
 * @param locations Generated location-fragment path.
 * @param target Live target path or generated target identifier.
 */
NM_INTERNAL void collect_required_upstreams(const char *locations,
                                            const char *target);

/**
 * @brief Appends the complete contents of a file to a destination buffer.
 * @param dst Destination filesystem path or destination buffer.
 * @param path Filesystem or procfs path.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int append_file_to_buffer(Buffer *dst, const char *path);

/**
 * @brief Generates, validates, and installs an HTTPS virtual-host
 * configuration.
 * @param listen Nginx listen directive value.
 * @param target Live target path or generated target identifier.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS write_https_site_config(const char *listen,
                                         const char *target);

/**
 * @brief Generates, validates, and installs a custom-port HTTP/HTTPS
 * configuration.
 * @param target Live target path or generated target identifier.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS write_custom_config(const char *target);

NM_END_DECLS

#endif
