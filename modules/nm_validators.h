/**
 * @file nm_validators.h
 * @brief Input validation, interactive prompts, ports, addresses, URLs, and
 * identifiers.
 *
 * @internal These declarations match the project's single-translation-unit
 * implementation and are not a stable external ABI.
 */
#ifndef NM_VALIDATORS_H
#define NM_VALIDATORS_H

#include "nm_core.h"

NM_BEGIN_DECLS

/**
 * @brief Implements shared hostname validation with configurable dot and
 * underscore rules.
 * @param s Input string.
 * @param require_dot Nonzero to require a multi-label hostname.
 * @param allow_underscore Nonzero to permit underscore characters in labels.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS valid_hostname_common(const char *s, intS require_dot,
                                       intS allow_underscore);

/**
 * @brief Validates a public DNS domain name.
 * @param s Input string.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS validate_domain(const char *s);

/**
 * @brief Validates a DNS hostname without requiring a dot.
 * @param s Input string.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS validate_hostname(const char *s) NM_UNUSED;

/**
 * @brief Validates an upstream hostname and permits underscores.
 * @param s Input string.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS validate_upstream_hostname(const char *s);

/**
 * @brief Parses a decimal TCP port in the inclusive range 1 through 65535.
 * @param s Input string.
 * @param out Caller-provided output object or buffer.
 * @return Nonzero on success; otherwise zero.
 */
NM_INTERNAL intS parse_port(const char *s, unsigned *out);

/**
 * @brief Validates a dotted-decimal IPv4 address.
 * @param s Input string.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS validate_ipv4(const char *s);

/**
 * @brief Validates an IPv6 address, including an embedded IPv4 tail.
 * @param s Input string.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS validate_ipv6(const char *s);

/**
 * @brief Validates either an IPv4 address or an upstream hostname.
 * @param s Input string.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS validate_proxy_host(const char *s);

/**
 * @brief Prompts until a valid normalized public domain is supplied.
 * @param prompt Prompt text shown to the user.
 */
NM_INTERNAL void prompt_domain(const char *prompt);

/**
 * @brief Prompts until a safe, readable existing file is supplied.
 * @param label Human-readable input label.
 * @param out Caller-provided output object or buffer.
 * @param cap Total capacity of the output buffer in bytes.
 */
NM_INTERNAL void prompt_existing_file(const char *label, char *out, size_t cap);

/**
 * @brief Prompts for a TCP port, applies a default, and checks active
 * listeners.
 * @param label Human-readable input label.
 * @param default_port Port selected when the user submits an empty answer.
 * @param out Caller-provided output object or buffer.
 */
NM_INTERNAL void prompt_port(const char *label, unsigned default_port,
                             unsigned *out);

/**
 * @brief Validates an Nginx location expression.
 * @param s Input string.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS validate_location_path(const char *s);

/**
 * @brief Validates a supported HTTP or HTTPS upstream URL.
 * @param url Candidate HTTP or HTTPS upstream URL.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS validate_proxy_url(const char *url);

/**
 * @brief Prompts for an optional per-node client request body limit.
 * @param https_port Public HTTPS port associated with the node.
 */
NM_INTERNAL void client_max_body_size(unsigned https_port);

/**
 * @brief Finds an unused TCP port in an inclusive range and stores it in
 * application state.
 * @param minimum Inclusive lower bound for random port selection.
 * @param maximum Inclusive upper bound or maximum token length, depending on
 * function.
 */
NM_INTERNAL void find_random_port(unsigned minimum, unsigned maximum);

/**
 * @brief Converts a domain into a bounded identifier with a checksum suffix
 * when necessary.
 * @param domain Validated public domain.
 * @param maximum Inclusive upper bound or maximum token length, depending on
 * function.
 * @param out Caller-provided output object or buffer.
 * @param cap Total capacity of the output buffer in bytes.
 */
NM_INTERNAL void bounded_domain_token(const char *domain, size_t maximum,
                                      char *out, size_t cap);

/**
 * @brief Builds a bounded Nginx stream upstream identifier.
 * @param domain Validated public domain.
 * @param public_port Public TLS/SNI listener port.
 * @param backend_port Backend TCP port.
 * @param out Caller-provided output object or buffer.
 * @param cap Total capacity of the output buffer in bytes.
 */
NM_INTERNAL void stream_upstream_name(const char *domain, unsigned public_port,
                                      unsigned backend_port, char *out,
                                      size_t cap);

NM_END_DECLS

#endif
