/**
 * @file nm_certificates.h
 * @brief Certificate discovery, bootstrap generation, chain handling, and
 * validation.
 *
 * @internal These declarations match the project's single-translation-unit
 * implementation and are not a stable external ABI.
 */
#ifndef NM_CERTIFICATES_H
#define NM_CERTIFICATES_H

#include "nm_core.h"

NM_BEGIN_DECLS

/**
 * @brief Ensures the manager bootstrap certificate directory and pair exist.
 */
NM_INTERNAL void bootstrap_certificates(void);

/**
 * @brief Selects or creates the internal bootstrap certificate pair.
 */
NM_INTERNAL void bootstrap_certificate_pair(void);

/**
 * @brief Discovers common certificate locations and prompts for the selected
 * pair.
 */
NM_INTERNAL void select_certificate_paths(void);

/**
 * @brief Uses OpenSSL to verify certificate/key consistency and certificate
 * validity.
 */
NM_INTERNAL void validate_certificate_pair(void);

/**
 * @brief Prompts for an optional CA/intermediate certificate path.
 */
NM_INTERNAL void select_ca_certificate(void);

/**
 * @brief Validates the selected CA/intermediate certificate file.
 */
NM_INTERNAL void validate_ca_certificate(void);

/**
 * @brief Configures the effective certificate chain for an HTTPS listener.
 * @param https_port Public HTTPS port associated with the node.
 */
NM_INTERNAL void configure_ca_certificate(unsigned https_port);

NM_END_DECLS

#endif
