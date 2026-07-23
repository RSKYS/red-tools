/**
 * @file nm_manage.h
 * @brief Modification and removal of existing script-managed Nginx nodes.
 *
 * @internal These declarations match the project's single-translation-unit
 * implementation and are not a stable external ABI.
 */
#ifndef NM_MANAGE_H
#define NM_MANAGE_H

#include "nm_core.h"

NM_BEGIN_DECLS

/**
 * @brief Returns a newly allocated line with exact endpoint port tokens
 * replaced.
 * @param line One Nginx configuration line.
 * @param old_port Existing TCP port.
 * @param new_port Replacement TCP port.
 * @return A newly allocated string that the caller must release with free(), or
 * NULL on failure.
 */
NM_INTERNAL char *replace_port_tokens_alloc(const char *line, unsigned old_port,
                                            unsigned new_port) NM_MALLOC;

/**
 * @brief Returns a newly allocated string with every literal token replaced.
 * @param line One Nginx configuration line.
 * @param old Literal token to replace.
 * @param replacement Replacement text.
 * @return A newly allocated string that the caller must release with free(), or
 * NULL on failure.
 */
NM_INTERNAL char *replace_literal_alloc(const char *line, const char *old,
                                        const char *replacement) NM_MALLOC;

/**
 * @brief Returns a newly allocated listen line with one endpoint port replaced.
 * @param line One Nginx configuration line.
 * @param old_port Existing TCP port.
 * @param new_port Replacement TCP port.
 * @return A newly allocated string that the caller must release with free(), or
 * NULL on failure.
 */
NM_INTERNAL char *replace_listen_endpoint_alloc(const char *line,
                                                unsigned old_port,
                                                unsigned new_port) NM_MALLOC;

/**
 * @brief Builds a managed-file candidate with a selected listener port changed.
 * @param source Source configuration file path or source text.
 * @param old_port Existing TCP port.
 * @param new_port Replacement TCP port.
 * @param output Generated candidate output path.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS replace_listener_port(const char *source, unsigned old_port,
                                       unsigned new_port, const char *output);

/**
 * @brief Builds a managed-file candidate with one selected upstream server port
 * changed.
 * @param source Source configuration file path or source text.
 * @param wanted_name Selected upstream name.
 * @param wanted_target Selected upstream server endpoint.
 * @param old_port Existing TCP port.
 * @param new_port Replacement TCP port.
 * @param output Generated candidate output path.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS replace_upstream_port_candidate(
    const char *source, const char *wanted_name, const char *wanted_target,
    unsigned old_port, unsigned new_port, const char *output);

/**
 * @brief Builds a candidate with the selected server/listener block removed.
 * @param source Source configuration file path or source text.
 * @param wanted_listen Selected listen directive value.
 * @param wanted_server Selected server_name value.
 * @param output Generated candidate output path.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS remove_listener_candidate(const char *source,
                                           const char *wanted_listen,
                                           const char *wanted_server,
                                           const char *output);

/**
 * @brief Tests whether a stream map route targets a selected upstream.
 * @param line One Nginx configuration line.
 * @param wanted Exact upstream or route name being searched for.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS map_route_targets(const char *line, const char *wanted);

/**
 * @brief Builds a candidate with a selected upstream and dependent routes
 * removed.
 * @param source Source configuration file path or source text.
 * @param wanted_name Selected upstream name.
 * @param output Generated candidate output path.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS remove_upstream_candidate(const char *source,
                                           const char *wanted_name,
                                           const char *output);

/**
 * @brief Prompts for and applies a replacement listener port.
 * @param old_port Existing TCP port.
 */
NM_INTERNAL void change_listener_port(unsigned old_port);

/**
 * @brief Prompts for and applies a replacement upstream server port.
 * @param old_port Existing TCP port.
 */
NM_INTERNAL void change_upstream_port(unsigned old_port);

/**
 * @brief Executes the modification workflow for a selected listener record.
 * @param item Selected inventory record.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS modify_managed_listener(const Listener *item);

/**
 * @brief Executes the modification workflow for a selected upstream record.
 * @param item Selected inventory record.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS modify_managed_upstream(const Upstream *item);

/**
 * @brief Executes the removal workflow for a selected listener record.
 * @param item Selected inventory record.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS remove_managed_listener(const Listener *item);

/**
 * @brief Executes the removal workflow for a selected upstream record.
 * @param item Selected inventory record.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS remove_managed_upstream(const Upstream *item);

/**
 * @brief Displays selectable managed nodes and performs one requested
 * management action.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS manage_existing_confs(void);

NM_END_DECLS

#endif
