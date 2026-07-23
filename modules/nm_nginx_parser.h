/**
 * @file nm_nginx_parser.h
 * @brief Managed Nginx parsing, inventories, isolated validation, and candidate
 * commits.
 *
 * @internal These declarations match the project's single-translation-unit
 * implementation and are not a stable external ABI.
 */
#ifndef NM_NGINX_PARSER_H
#define NM_NGINX_PARSER_H

#include "nm_core.h"

NM_BEGIN_DECLS

/**
 * @brief Loads a text file into an owning vector of lines.
 * @param path Filesystem or procfs path.
 * @param lines String vector receiving or supplying text lines.
 */
NM_INTERNAL void load_lines(const char *path, StrVec *lines);

/**
 * @brief Writes a vector of lines to a file with the requested mode.
 * @param path Filesystem or procfs path.
 * @param lines String vector receiving or supplying text lines.
 * @param mode POSIX permission mode.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int write_lines(const char *path, const StrVec *lines, mode_t mode);

/**
 * @brief Appends a listener inventory record using bounded copies.
 * @param v Destination inventory vector.
 * @param file Configuration file path stored in an inventory record.
 * @param listen Nginx listen directive value.
 * @param server Server name or endpoint text.
 */
NM_INTERNAL void listener_vec_push(ListenerVec *v, const char *file,
                                   const char *listen, const char *server);

/**
 * @brief Appends an upstream inventory record using bounded copies.
 * @param v Destination inventory vector.
 * @param file Configuration file path stored in an inventory record.
 * @param name Executable, upstream, environment, or block name.
 * @param target Live target path or generated target identifier.
 */
NM_INTERNAL void upstream_vec_push(UpstreamVec *v, const char *file,
                                   const char *name, const char *target);

/**
 * @brief Extracts and normalizes the value of a named Nginx directive.
 * @param line One Nginx configuration line.
 * @param directive Directive name to extract.
 * @param out Caller-provided output object or buffer.
 * @param cap Total capacity of the output buffer in bytes.
 * @return Nonzero on success; otherwise zero.
 */
NM_INTERNAL intS directive_value(const char *line, const char *directive,
                                 char *out, size_t cap);

/**
 * @brief Tests whether a line opens a named Nginx block.
 * @param line One Nginx configuration line.
 * @param keyword Nginx block keyword.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS block_start(const char *line, const char *keyword);

/**
 * @brief Parses all managed files into a listener inventory.
 * @param listeners Destination listener inventory.
 */
NM_INTERNAL void build_listener_inventory(ListenerVec *listeners);

/**
 * @brief Parses all managed files into an upstream inventory.
 * @param upstreams Destination upstream inventory.
 */
NM_INTERNAL void build_upstream_inventory(UpstreamVec *upstreams);

/**
 * @brief Extracts a TCP port from an Nginx endpoint specification.
 * @param spec Nginx endpoint specification.
 * @return The parsed port, or zero when no valid port is present.
 */
NM_INTERNAL unsigned endpoint_port(const char *spec);

/**
 * @brief Reports whether a managed listener endpoint is active and Nginx-owned.
 * @param spec Nginx endpoint specification.
 * @return A borrowed pointer that remains valid for the lifetime described by
 * the implementation.
 */
NM_INTERNAL const char *managed_listener_status(const char *spec);

/**
 * @brief Prints current managed listener and upstream inventories.
 */
NM_INTERNAL void display_inventory(void);

/**
 * @brief Tests whether any script-managed configuration files exist.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS has_managed_configurations(void);

/**
 * @brief Writes captured process output and guarantees a trailing newline.
 * @param b Buffer object.
 * @param out Caller-provided output object or buffer.
 */
NM_INTERNAL void print_captured(const Buffer *b, FILE *out);

/**
 * @brief Creates a unique isolated directory for candidate validation.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS make_validation_dir(void);

/**
 * @brief Copies the current Nginx configuration into the validation directory.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS copy_validation_tree(void);

/**
 * @brief Appends a string while replacing every literal occurrence of a token.
 * @param input Source string.
 * @param from Literal token to replace.
 * @param to Replacement text.
 * @param out Caller-provided output object or buffer.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int replace_all(const char *input, const char *from, const char *to,
                            Buffer *out);

/**
 * @brief Tests a generated candidate in an isolated Nginx configuration tree.
 * @param target Live target path or generated target identifier.
 * @param candidate Generated candidate file path.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS validate_nginx_candidate(const char *target,
                                          const char *candidate);

/**
 * @brief Validates and installs the active generated Nginx candidate.
 * @param target Live target path or generated target identifier.
 * @param mode POSIX permission mode.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS commit_nginx_file(const char *target, mode_t mode);

/**
 * @brief Validates and installs an explicitly selected managed-file candidate.
 * @param target Live target path or generated target identifier.
 * @param mode POSIX permission mode.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS finalize_selected_file(const char *target, mode_t mode);

NM_END_DECLS

#endif
