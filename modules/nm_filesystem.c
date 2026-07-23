#ifndef NM_FILESYSTEM_C
#define NM_FILESYSTEM_C

#ifndef NM_AMALGAMATED_BUILD
#define NM_AMALGAMATED_BUILD 1
#endif

#include "nm_filesystem.h"

static intS path_exists_l(const char *path) {
	struct stat st;
	return lstat(path, &st) == 0;
}

static intS path_is_file(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static intS path_is_dir(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static intS path_readable_file(const char *path) {
	return path_is_file(path) && access(path, R_OK) == 0;
}

static int parent_dir(const char *path, char *out, size_t cap) {
	if (str_copy(out, cap, path) < 0) return -1;
	char *slash = strrchr(out, '/');
	if (!slash) return str_copy(out, cap, ".");
	if (slash == out) slash[1] = '\0';
	else *slash = '\0';
	return 0;
}

static const char *base_name(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static int mkdir_p(const char *path, mode_t mode) {
	char tmp[PATH_MAX];
	if (str_copy(tmp, sizeof(tmp), path) < 0) {
		errno = ENAMETOOLONG;
		return -1;
	}
	size_t n = strlen(tmp);
	if (!n) { errno = EINVAL; return -1; }
	if (n > 1 && tmp[n - 1] == '/') tmp[n - 1] = '\0';
	for (char *p = tmp + 1; *p; ++p) {
		if (*p != '/') continue;
		*p = '\0';
		if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
		*p = '/';
	}
	if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
	return path_is_dir(tmp) ? 0 : -1;
}

static int read_file(const char *path, Buffer *out) {
	FILE *fp = fopen(path, "rb");
	if (!fp) return -1;
	char chunk[8192];
	size_t n;
	while ((n = fread(chunk, 1, sizeof(chunk), fp)) != 0) {
		if (buffer_append_n(out, chunk, n) < 0) {
			fclose(fp);
			errno = EOVERFLOW;
			return -1;
		}
	}
	int ok = ferror(fp) ? -1 : 0;
	fclose(fp);
	return ok;
}

static int write_all_fd(int fd, const void *data, size_t len) {
	const unsigned char *p = data;
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int write_file_mode(const char *path, const void *data, size_t len, mode_t mode) {
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0) return -1;
	int rc = write_all_fd(fd, data, len);
	if (rc == 0 && fchmod(fd, mode) < 0) rc = -1;
	if (close(fd) < 0 && rc == 0) rc = -1;
	return rc;
}

static int append_file(const char *path, const void *data, size_t len) {
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	if (fd < 0) return -1;
	int rc = write_all_fd(fd, data, len);
	if (close(fd) < 0 && rc == 0) rc = -1;
	return rc;
}

static int copy_regular_file(const char *src, const char *dst, const struct stat *st) {
	int in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0) return -1;
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st->st_mode & 07777);
	if (out < 0) { close(in); return -1; }
	char buf[16384];
	int rc = 0;
	for (;;) {
		ssize_t n = read(in, buf, sizeof(buf));
		if (n == 0) break;
		if (n < 0) {
			if (errno == EINTR) continue;
			rc = -1;
			break;
		}
		if (write_all_fd(out, buf, (size_t)n) < 0) { rc = -1; break; }
	}
	if (rc == 0 && fchmod(out, st->st_mode & 07777) < 0) rc = -1;
	close(in);
	if (close(out) < 0 && rc == 0) rc = -1;
	struct timespec times[2] = {st->st_atim, st->st_mtim};
	if (rc == 0) (void)utimensat(AT_FDCWD, dst, times, 0);
	return rc;
}

static int copy_path_recursive(const char *src, const char *dst);

static int copy_directory(const char *src, const char *dst, const struct stat *st) {
	if (mkdir(dst, st->st_mode & 07777) < 0 && errno != EEXIST) return -1;
	DIR *dir = opendir(src);
	if (!dir) return -1;
	int rc = 0;
	struct dirent *de;
	while ((de = readdir(dir))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		char child_src[PATH_MAX], child_dst[PATH_MAX];
		if (str_printf(child_src, sizeof(child_src), "%s/%s", src, de->d_name) < 0 ||
			str_printf(child_dst, sizeof(child_dst), "%s/%s", dst, de->d_name) < 0 ||
			copy_path_recursive(child_src, child_dst) < 0) {
			rc = -1;
			break;
		}
	}
	closedir(dir);
	if (rc == 0) {
		(void)chmod(dst, st->st_mode & 07777);
		struct timespec times[2] = {st->st_atim, st->st_mtim};
		(void)utimensat(AT_FDCWD, dst, times, 0);
	}
	return rc;
}

static int copy_path_recursive(const char *src, const char *dst) {
	struct stat st;
	if (lstat(src, &st) < 0) return -1;
	if (S_ISREG(st.st_mode)) return copy_regular_file(src, dst, &st);
	if (S_ISDIR(st.st_mode)) return copy_directory(src, dst, &st);
	if (S_ISLNK(st.st_mode)) {
		char target[PATH_MAX];
		ssize_t n = readlink(src, target, sizeof(target) - 1);
		if (n < 0) return -1;
		target[n] = '\0';
		(void)unlink(dst);
		return symlink(target, dst);
	}
	errno = ENOTSUP;
	return -1;
}

static int remove_tree(const char *path) {
	struct stat st;
	if (lstat(path, &st) < 0) return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return unlink(path);
	DIR *dir = opendir(path);
	if (!dir) return -1;
	int rc = 0;
	struct dirent *de;
	while ((de = readdir(dir))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		char child[PATH_MAX];
		if (str_printf(child, sizeof(child), "%s/%s", path, de->d_name) < 0 ||
			remove_tree(child) < 0) {
			rc = -1;
			break;
		}
	}
	closedir(dir);
	if (rc == 0 && rmdir(path) < 0) rc = -1;
	return rc;
}

static int move_path(const char *src, const char *dst) {
	if (rename(src, dst) == 0) return 0;
	if (errno != EXDEV) return -1;
	if (copy_path_recursive(src, dst) < 0) return -1;
	return remove_tree(src);
}

static int copy_file_preserve(const char *src, const char *dst) {
	struct stat st;
	if (lstat(src, &st) < 0) return -1;
	if (S_ISLNK(st.st_mode)) {
		char target[PATH_MAX];
		ssize_t n = readlink(src, target, sizeof(target) - 1);
		if (n < 0) return -1;
		target[n] = '\0';
		(void)unlink(dst);
		return symlink(target, dst);
	}
	if (!S_ISREG(st.st_mode)) { errno = EINVAL; return -1; }
	return copy_regular_file(src, dst, &st);
}

static void cleanup_temp(void) {
	if (app.current_tmp[0]) {
		(void)unlink(app.current_tmp);
		app.current_tmp[0] = '\0';
	}
}

static void cleanup_validation_dir(void) {
	if (app.validation_dir[0]) {
		(void)remove_tree(app.validation_dir);
		app.validation_dir[0] = '\0';
	}
}

static void make_temp_for(const char *target) {
	char dir[PATH_MAX];
	if (parent_dir(target, dir, sizeof(dir)) < 0)
		fatalf("Could not create a safe temporary file beside %s.", target);
	cleanup_temp();
	for (unsigned attempt = 0; attempt < 100; ++attempt) {
		if (str_printf(app.current_tmp, sizeof(app.current_tmp),
					   "%s/.%s.tmp.%ld.%u", dir, base_name(target),
					   (long)getpid(), attempt) < 0) break;
		int fd = open(app.current_tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd >= 0) {
			close(fd);
			return;
		}
	}
	app.current_tmp[0] = '\0';
	fatalf("Could not create a safe temporary file beside %s.", target);
}

static intS buffer_has_nonspace(const Buffer *b) {
	for (size_t i = 0; i < b->len; ++i)
		if (!isspace((unsigned char)b->data[i])) return 1;
	return 0;
}

static intS file_contains_literal(const char *path, const char *needle) {
	Buffer b;
	buffer_init(&b);
	intS found = read_file(path, &b) == 0 && b.data != NULL && strstr(b.data, needle) != NULL;
	buffer_free(&b);
	return found;
}

static intS is_script_managed_file(const char *path) {
	if (!path_is_file(path)) return 0;
	FILE *fp = fopen(path, "r");
	if (!fp) return 0;
	char line[4096];
	intS found = 0;
	while (!found && fgets(line, sizeof(line), fp)) {
		char *s = trim_in_place(line);
		found = starts_with(s, "# Managed") || starts_with(s, "# managed") ||
			starts_with(s, "# nginx-manager: Public HTTP catch-all") ||
			starts_with(s, "# nginx-manager: Custom-port HTTP redirect") ||
			starts_with(s, "# nginx-manager: Public TLS/SNI gateway") ||
			starts_with(s, "# nginx-manager: HTTPS reverse-proxy virtual host") ||
			starts_with(s, "# nginx-manager: Custom-port HTTP/HTTPS reverse proxy");
	}
	fclose(fp);
	return found;
}

static int compare_strings(const void *a, const void *b) {
	const char *const *left = a;
	const char *const *right = b;
	return strcmp(*left, *right);
}

static void managed_conf_files(StrVec *files) {
	static const struct { const char *dir; const char *pattern; } groups[] = {
		{"conf.d", "redirect_*.conf"},
		{"conf.d", "reverse_*.conf"},
		{"conf.d", "proxy_*.conf"},
		{"stream-conf.d", "redirect_*.conf"}
	};
	for (size_t d = 0; d < ARRAY_LEN(groups); ++d) {
		char path[PATH_MAX];
		if (str_printf(path, sizeof(path), "%s/%s", app.nginx_conf_dir, groups[d].dir) < 0) continue;
		DIR *dir = opendir(path);
		if (!dir) continue;
		StrVec matches;
		strvec_init(&matches);
		struct dirent *de;
		while ((de = readdir(dir))) {
			if (fnmatch(groups[d].pattern, de->d_name, 0) != 0) continue;
			char file[PATH_MAX];
			if (str_printf(file, sizeof(file), "%s/%s", path, de->d_name) == 0 &&
				is_script_managed_file(file)) strvec_push(&matches, file);
		}
		closedir(dir);
		if (matches.len > 1)
			qsort(matches.items, matches.len, sizeof(*matches.items), compare_strings);
		for (size_t i = 0; i < matches.len; ++i) strvec_push(files, matches.items[i]);
		strvec_free(&matches);
	}
}

static int backup_name_for(const char *target, char *out, size_t cap) {
	Buffer b;
	buffer_init(&b);
	if (buffer_printf(&b, "%s/", app.backup_dir) < 0) goto fail;
	const char *p = target;
	if (*p == '/') ++p;
	for (; *p; ++p) {
		if (buffer_append(&b, *p == '/' ? "__" : (char[2]){*p, '\0'}) < 0) goto fail;
	}
	if (str_copy(out, cap, b.data ? b.data : "") < 0) goto fail;
	buffer_free(&b);
	return 0;
fail:
	buffer_free(&b);
	return -1;
}

static void record_change(char type, const char *target, const char *backup) {
	Buffer b;
	buffer_init(&b);
	if (buffer_printf(&b, "%c|%s|%s\n", type, target, backup ? backup : "") < 0 ||
		append_file(app.manifest, b.data, b.len) < 0) {
		buffer_free(&b);
		fatalf("Could not update the transaction manifest.");
	}
	buffer_free(&b);
}

static intS backup_existing_file(const char *target) {
	char backup[PATH_MAX];
	if (backup_name_for(target, backup, sizeof(backup)) < 0 ||
		copy_file_preserve(target, backup) < 0) return 0;
	record_change('O', target, backup);
	log_success("Rollback backup created: %s", backup);
	return 1;
}

static intS is_redirect_conf(const char *target) {
	char prefix1[PATH_MAX], prefix2[PATH_MAX];
	str_printf(prefix1, sizeof(prefix1), "%s/conf.d/redirect_", app.nginx_conf_dir);
	str_printf(prefix2, sizeof(prefix2), "%s/stream-conf.d/redirect_", app.nginx_conf_dir);
	return (starts_with(target, prefix1) || starts_with(target, prefix2)) &&
		   ends_with(target, ".conf");
}

static void prepare_for_replace(const char *target) {
	if (path_exists_l(target)) {
		if (is_redirect_conf(target) && is_script_managed_file(target)) {
			log_warn("Automatically replacing script-managed redirect configuration: %s", target);
			log_info("No overwrite confirmation is required; a transactional rollback backup will be created automatically.");
			if (!backup_existing_file(target))
				fatalf("Could not create the automatic rollback backup for %s.", target);
			return;
		}
		log_warn("The existing file is not recognized as an automatically replaceable script-managed redirect configuration: %s", target);
		log_warn("Overwriting it may remove manually maintained directives or configuration blocks.");
		if (!confirm("Overwrite this file?", 0))
			fatalf("Stopped without overwriting %s.", target);
		if (confirm("Create a rollback backup first?", 1)) {
			if (!backup_existing_file(target))
				fatalf("Could not create the rollback backup for %s.", target);
		} else {
			log_warn("No rollback backup will be available for %s.", target);
			if (!confirm("Continue without a backup?", 0))
				fatalf("Stopped before overwriting %s.", target);
			record_change('X', target, "");
		}
	} else {
		record_change('N', target, "");
		log_info("Creating new managed configuration file: %s", target);
	}
}

static void commit_temp_file(const char *target, mode_t mode) {
	if (!app.current_tmp[0])
		fatalf("Internal error: no temporary file is ready for %s.", target);
	if (chmod(app.current_tmp, mode) < 0)
		fatalf("Could not set permissions on the temporary file for %s.", target);
	prepare_for_replace(target);
	if (rename(app.current_tmp, target) < 0)
		fatalf("Could not atomically install %s.", target);
	app.current_tmp[0] = '\0';
	log_success("Installed: %s", target);
}

#endif
