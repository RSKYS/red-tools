/**
 * @file nm_filesystem.h
 * @brief Filesystem traversal, safe temporary files, backups, and atomic
 * installation.
 *
 * @internal These declarations match the project's single-translation-unit
 * implementation and are not a stable external ABI.
 */
#ifndef NM_FILESYSTEM_H
#define NM_FILESYSTEM_H

#include "nm_core.h"

NM_BEGIN_DECLS

/**
 * @brief Tests whether a path exists without following its final symbolic link.
 * @param path Filesystem or procfs path.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS path_exists_l(const char *path);

/**
 * @brief Tests whether a path resolves to a regular file.
 * @param path Filesystem or procfs path.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS path_is_file(const char *path);

/**
 * @brief Tests whether a path resolves to a directory.
 * @param path Filesystem or procfs path.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS path_is_dir(const char *path);

/**
 * @brief Tests whether a path is a readable regular file.
 * @param path Filesystem or procfs path.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS path_readable_file(const char *path);

/**
 * @brief Copies the lexical parent directory of a path into a caller buffer.
 * @param path Filesystem or procfs path.
 * @param out Caller-provided output object or buffer.
 * @param cap Total capacity of the output buffer in bytes.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int parent_dir(const char *path, char *out, size_t cap);

/**
 * @brief Returns the lexical final component of a path.
 * @param path Filesystem or procfs path.
 * @return A borrowed pointer that remains valid for the lifetime described by
 * the implementation.
 */
NM_INTERNAL const char *base_name(const char *path);

/**
 * @brief Creates a directory hierarchy using the requested mode.
 * @param path Filesystem or procfs path.
 * @param mode POSIX permission mode.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int mkdir_p(const char *path, mode_t mode);

/**
 * @brief Appends an entire binary file to an initialized dynamic buffer.
 * @param path Filesystem or procfs path.
 * @param out Caller-provided output object or buffer.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int read_file(const char *path, Buffer *out);

/**
 * @brief Writes an exact byte sequence to a file descriptor, retrying
 * interrupted writes.
 * @param fd Open file descriptor.
 * @param data Source byte sequence.
 * @param len Number of source bytes.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int write_all_fd(int fd, const void *data, size_t len);

/**
 * @brief Atomically at descriptor level creates/truncates a file, writes bytes,
 * and enforces its mode.
 * @param path Filesystem or procfs path.
 * @param data Source byte sequence.
 * @param len Number of source bytes.
 * @param mode POSIX permission mode.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int write_file_mode(const char *path, const void *data, size_t len,
                                mode_t mode);

/**
 * @brief Appends an exact byte sequence to a file, creating it with private
 * permissions if absent.
 * @param path Filesystem or procfs path.
 * @param data Source byte sequence.
 * @param len Number of source bytes.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int append_file(const char *path, const void *data, size_t len);

/**
 * @brief Copies a regular file while preserving permissions and timestamps
 * where possible.
 * @param src Source filesystem path.
 * @param dst Destination filesystem path or destination buffer.
 * @param st Metadata obtained for the source path.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int copy_regular_file(const char *src, const char *dst,
                                  const struct stat *st);

/**
 * @brief Recursively copies a directory tree while preserving metadata where
 * possible.
 * @param src Source filesystem path.
 * @param dst Destination filesystem path or destination buffer.
 * @param st Metadata obtained for the source path.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int copy_directory(const char *src, const char *dst,
                               const struct stat *st);

/**
 * @brief Copies a supported regular file, directory, or symbolic link
 * recursively.
 * @param src Source filesystem path.
 * @param dst Destination filesystem path or destination buffer.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int copy_path_recursive(const char *src, const char *dst);

/**
 * @brief Recursively removes a path without following symbolic links.
 * @param path Filesystem or procfs path.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int remove_tree(const char *path);

/**
 * @brief Moves a path, falling back to copy-and-remove across filesystems.
 * @param src Source filesystem path.
 * @param dst Destination filesystem path or destination buffer.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int move_path(const char *src, const char *dst);

/**
 * @brief Copies a regular file or symbolic link while preserving metadata.
 * @param src Source filesystem path.
 * @param dst Destination filesystem path or destination buffer.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int copy_file_preserve(const char *src, const char *dst);

/**
 * @brief Removes and clears the current temporary candidate path.
 */
NM_INTERNAL void cleanup_temp(void);

/**
 * @brief Removes and clears the isolated Nginx validation tree.
 */
NM_INTERNAL void cleanup_validation_dir(void);

/**
 * @brief Creates a collision-resistant temporary file beside a target path.
 * @param target Live target path or generated target identifier.
 */
NM_INTERNAL void make_temp_for(const char *target);

/**
 * @brief Tests whether a buffer contains at least one non-whitespace byte.
 * @param b Buffer object.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS buffer_has_nonspace(const Buffer *b);

/**
 * @brief Tests whether a readable file contains a literal substring.
 * @param path Filesystem or procfs path.
 * @param needle Literal substring to find.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS file_contains_literal(const char *path, const char *needle);

/**
 * @brief Verifies that a configuration file carries the manager ownership
 * marker.
 * @param path Filesystem or procfs path.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS is_script_managed_file(const char *path);

/**
 * @brief qsort-compatible comparator for string pointers.
 * @param a Pointer to the first qsort comparison operand.
 * @param b Buffer object.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int compare_strings(const void *a, const void *b);

/**
 * @brief Collects and sorts script-managed Nginx configuration paths.
 * @param files Function input or output value.
 */
NM_INTERNAL void managed_conf_files(StrVec *files);

/**
 * @brief Derives the transaction backup path corresponding to a live target.
 * @param target Live target path or generated target identifier.
 * @param out Caller-provided output object or buffer.
 * @param cap Total capacity of the output buffer in bytes.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL int backup_name_for(const char *target, char *out, size_t cap);

/**
 * @brief Appends one normalized change record to the transaction manifest.
 * @param type Transaction manifest operation code.
 * @param target Live target path or generated target identifier.
 * @param backup Rollback backup path; may be empty when not applicable.
 */
NM_INTERNAL void record_change(char type, const char *target,
                               const char *backup);

/**
 * @brief Creates a rollback backup for an existing target when needed.
 * @param target Live target path or generated target identifier.
 * @return Zero or a positive success value as documented by the implementation;
 * a negative/zero failure value otherwise.
 */
NM_INTERNAL intS backup_existing_file(const char *target);

/**
 * @brief Identifies generated redirect configuration paths.
 * @param target Live target path or generated target identifier.
 * @return Nonzero when the predicate is true; otherwise zero.
 */
NM_INTERNAL intS is_redirect_conf(const char *target);

/**
 * @brief Applies overwrite policy and creates a rollback backup before
 * replacement.
 * @param target Live target path or generated target identifier.
 */
NM_INTERNAL void prepare_for_replace(const char *target);

/**
 * @brief Validates permissions and atomically installs the active temporary
 * file.
 * @param target Live target path or generated target identifier.
 * @param mode POSIX permission mode.
 */
NM_INTERNAL void commit_temp_file(const char *target, mode_t mode);

NM_END_DECLS

#endif
