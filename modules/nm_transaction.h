/**
 * @file nm_transaction.h
 * @brief Transactional rollback, signal recovery, and timestamp management.
 *
 * @internal These declarations match the project's single-translation-unit
 * implementation and are not a stable external ABI.
 */
#ifndef NM_TRANSACTION_H
#define NM_TRANSACTION_H

#include "nm_core.h"

NM_BEGIN_DECLS

/**
 * @brief Signal number pending safe-context interruption handling.
 *
 * The async signal handler only assigns this sig_atomic_t object and emits a
 * fixed write(2) message. Cleanup and rollback run later in normal context.
 */
NM_INTERNAL volatile sig_atomic_t nm_pending_signal;

/**
 * @brief Performs deferred cleanup and rollback after a termination signal.
 *
 * This function is safe to call at ordinary function boundaries and exits with
 * the historical interruption status after restoring managed state.
 */
NM_INTERNAL void check_interrupted(void);

/**
 * @brief Restores or removes transaction-managed paths according to the
 * manifest.
 */
NM_INTERNAL void rollback_transaction(void);

/**
 * @brief Handles termination signals by cleaning temporary resources and
 * attempting rollback.
 * @param signo Signal number delivered by the operating system.
 */
NM_INTERNAL void signal_handler(int signo);

/**
 * @brief Installs process signal handlers used by transactional cleanup.
 */
NM_INTERNAL void install_signal_handlers(void);

/**
 * @brief Formats the current UTC time using the manager state timestamp format.
 * @param out Caller-provided output object or buffer.
 * @param cap Total capacity of the output buffer in bytes.
 */
NM_INTERNAL void format_time_utc(char *out, size_t cap);

/**
 * @brief Creates a fresh backup directory and transaction manifest.
 */
NM_INTERNAL void init_transaction(void);

NM_END_DECLS

#endif
