/**
 * @file nm_runtime.h
 * @brief Runtime path initialization, privilege checks, state persistence, and
 * package setup.
 *
 * @internal These declarations match the project's single-translation-unit
 * implementation and are not a stable external ABI.
 */
#ifndef NM_RUNTIME_H
#define NM_RUNTIME_H

#include "nm_core.h"

NM_BEGIN_DECLS

/**
 * @brief Validates a safe absolute filesystem path accepted by runtime
 * configuration.
 * @param s Input string.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS validate_absolute_path(const char *s);

/**
 * @brief Resolves environment overrides and initializes all core runtime paths.
 */
NM_INTERNAL void initialize_runtime_paths(void);

/**
 * @brief Loads and validates persistent manager state from disk.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS load_management_state(void);

/**
 * @brief Serializes current persistent manager state to disk.
 */
NM_INTERNAL void write_management_state(void);

/**
 * @brief Terminates unless the effective user ID is root.
 */
NM_INTERNAL void require_root(void);

/**
 * @brief Prompts for and stores IPv6-listener preference.
 */
NM_INTERNAL void prompt_ipv6_support(void);

/**
 * @brief Creates required Nginx and manager directories with controlled
 * permissions.
 */
NM_INTERNAL void ensure_directories(void);

/**
 * @brief Validates runtime paths loaded from existing manager state.
 */
NM_INTERNAL void validate_existing_managed_runtime(void);

/**
 * @brief Installs required Nginx and certificate packages through the available
 * package manager.
 */
NM_INTERNAL void install_packages(void);

/**
 * @brief Disables the distribution default Nginx site transactionally.
 */
NM_INTERNAL void remove_default_site(void);

NM_END_DECLS

#endif
