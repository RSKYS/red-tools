/**
 * @file nm_types.h
 * @brief Shared constants, status values, and data structures.
 */
#ifndef NM_TYPES_H
#define NM_TYPES_H

#include "nm_portability.h"
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @typedef intS
 * @brief Unsigned short int used for bounded flags and small counters.
 *
 * This type must not be used for file descriptors, process status values,
 * standard-library return values, or signed error codes.
 */
typedef unsigned short int intS;

/**
 * @brief Retained compatibility short int.
 *
 * The object declaration is intentionally separate from @ref intS .
 */

#ifndef PATH_MAX
/**
 * @brief Conservative path buffer size used when the host omits PATH_MAX.
 *
 * Linux normally provides PATH_MAX through <limits.h>. The fallback preserves
 * the project's existing structure layout; it is not a claim that every
 * filesystem accepts paths of this length.
 */
#define PATH_MAX 4096
#endif

/** @brief Stable application identifier used in generated metadata. */
#define MANAGER_ID "nginx-manager"

/** @brief On-disk management-state schema version. */
#define STATE_VERSION "1"

/** @brief Human-readable application release version. */
#define VERSION "1.0.0"

/**
 * @enum NmStatus
 * @brief Conventional status codes for defensive helpers and integrations.
 */
typedef enum NmStatus {
  NM_STATUS_OK = 0, /**< Operation completed successfully. */
  NM_STATUS_INVALID_ARGUMENT =
      -1,                   /**< A boundary received an invalid argument. */
  NM_STATUS_NO_MEMORY = -2, /**< Memory allocation or growth failed. */
  NM_STATUS_IO_ERROR = -3, /**< A filesystem or process I/O operation failed. */
  NM_STATUS_OVERFLOW = -4, /**< A value exceeded fixed or dynamic capacity. */
  NM_STATUS_VALIDATION_ERROR = -5 /**< Input failed validation. */
} NmStatus;

/**
 * @enum NmLogLevel
 * @brief Severity values exposed to optional logging integrations.
 */
typedef enum NmLogLevel {
  NM_LOG_LEVEL_INFO = 0,    /**< Informational operational message. */
  NM_LOG_LEVEL_SUCCESS = 1, /**< Successful operation or milestone. */
  NM_LOG_LEVEL_WARNING = 2, /**< Recoverable or user-correctable condition. */
  NM_LOG_LEVEL_ERROR = 3    /**< Failed operation requiring attention. */
} NmLogLevel;

/**
 * @struct StrVec
 * @brief Owning dynamic vector of NUL-terminated strings.
 */
typedef struct StrVec {
  char **items; /**< Heap array of individually owned string pointers. */
  size_t len;   /**< Number of initialized entries in @ref items. */
  size_t cap;   /**< Allocated entry capacity of @ref items. */
} StrVec;

/**
 * @struct Buffer
 * @brief Owning dynamically sized byte buffer maintained as a C string.
 */
typedef struct Buffer {
  char *data; /**< Heap storage, or NULL before the first append. */
  size_t len; /**< Number of payload bytes, excluding the terminating NUL. */
  size_t cap; /**< Allocated byte capacity, including room for the NUL. */
} Buffer;

/**
 * @struct Listener
 * @brief Parsed script-managed Nginx listener record.
 */
typedef struct Listener {
  char file[PATH_MAX]; /**< Configuration file containing the listener. */
  char listen[256];    /**< Normalized listen directive value. */
  char server[512];    /**< Associated server_name value or display fallback. */
} Listener;

/**
 * @struct ListenerVec
 * @brief Owning dynamic vector of @ref Listener records.
 */
typedef struct ListenerVec {
  Listener *items; /**< Heap array of listener records. */
  size_t len;      /**< Number of initialized records. */
  size_t cap;      /**< Allocated record capacity. */
} ListenerVec;

/**
 * @struct Upstream
 * @brief Parsed script-managed Nginx upstream server record.
 */
typedef struct Upstream {
  char file[PATH_MAX]; /**< Configuration file containing the upstream. */
  char name[256];      /**< Nginx upstream block name. */
  char target[512];    /**< Selected server endpoint inside the upstream. */
} Upstream;

/**
 * @struct UpstreamVec
 * @brief Owning dynamic vector of @ref Upstream records.
 */
typedef struct UpstreamVec {
  Upstream *items; /**< Heap array of upstream records. */
  size_t len;      /**< Number of initialized records. */
  size_t cap;      /**< Allocated record capacity. */
} UpstreamVec;

/**
 * @struct App
 * @brief Process-wide state for the interactive Nginx manager.
 *
 * The current program is intentionally single-threaded. Access must be
 * serialized if worker threads are introduced later.
 */
typedef struct App {
  char nginx_conf_dir[PATH_MAX];  /**< Root Nginx configuration directory. */
  char nginx_main_conf[PATH_MAX]; /**< Main nginx.conf path. */
  char nginx_bin[PATH_MAX];       /**< Resolved Nginx executable path. */
  char state_file[PATH_MAX];      /**< Persistent manager-state path. */
  char current_tmp[PATH_MAX];     /**< Active temporary candidate path. */
  char validation_dir[PATH_MAX];  /**< Isolated candidate-validation tree. */
  char backup_dir[PATH_MAX];      /**< Current transaction backup directory. */
  char manifest[PATH_MAX];        /**< Current transaction manifest path. */

  char mode[32];            /**< Selected operation mode. */
  char domain[256];         /**< Validated public DNS name. */
  char cert_file[PATH_MAX]; /**< Selected leaf/full-chain certificate. */
  char key_file[PATH_MAX];  /**< Selected private-key path. */
  char ca_file[PATH_MAX];   /**< Optional CA/intermediate path. */
  char ssl_certificate_file[PATH_MAX]; /**< Effective certificate served by
                                          Nginx. */
  char generated_chain_file[PATH_MAX]; /**< Generated combined-chain path. */
  char site_config[PATH_MAX];   /**< Installed HTTP/HTTPS site configuration. */
  char stream_config[PATH_MAX]; /**< Installed stream/SNI configuration. */
  char http_redirect_config[PATH_MAX]; /**< Installed HTTP redirect
                                          configuration. */
  char redirect_snippet[PATH_MAX];     /**< Installed forced-SSL snippet. */
  char location_fragment[PATH_MAX]; /**< Generated temporary location fragment.
                                     */
  char upstream_fragment[PATH_MAX]; /**< Generated temporary upstream fragment.
                                     */
  char
      managed_installed_at[64]; /**< Original manager installation timestamp. */
  char managed_last_configured_at[64]; /**< Most recent configuration timestamp.
                                        */
  char managed_node_action[512];       /**< Human-readable management action. */
  char managed_node_description[PATH_MAX +
                                512]; /**< Selected-node description. */

  unsigned http_port;          /**< Selected public/custom HTTP port. */
  unsigned https_port;         /**< Selected public/custom HTTPS port. */
  unsigned tls_internal_port;  /**< Loopback TLS termination port. */
  unsigned backend_port;       /**< Selected application backend port. */
  unsigned selected_free_port; /**< Temporary random free-port result. */
  unsigned managed_new_port;   /**< Replacement port in management mode. */
  unsigned body_size_mb;       /**< Optional client_max_body_size in MiB. */

  intS ipv6_enabled;       /**< Nonzero when IPv6 listeners are enabled. */
  intS tls_passthrough;    /**< Nonzero when raw TLS is forwarded unchanged. */
  intS managed_existing;   /**< Nonzero when persistent state was loaded. */
  intS transaction_active; /**< Nonzero while rollback is available. */
  intS rollback_done;      /**< Nonzero after rollback has run. */
} App;

NM_STATIC_ASSERT(sizeof(((Listener *)0)->listen) == 256,
                 "Listener.listen layout must remain stable");
NM_STATIC_ASSERT(sizeof(((Upstream *)0)->name) == 256,
                 "Upstream.name layout must remain stable");

#endif
