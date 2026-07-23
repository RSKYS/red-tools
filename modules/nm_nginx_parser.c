#ifndef NM_NGINX_PARSER_C
#define NM_NGINX_PARSER_C

#ifndef NM_AMALGAMATED_BUILD
#define NM_AMALGAMATED_BUILD 1
#endif

#include "nm_nginx_parser.h"

static void load_lines(const char *path, StrVec *lines) {
	FILE *fp = fopen(path, "r");
	if (!fp) return;
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	while ((n = getline(&line, &cap, fp)) >= 0) {
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
		strvec_push(lines, line);
	}
	free(line);
	fclose(fp);
}

static int write_lines(const char *path, const StrVec *lines, mode_t mode) {
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0) return -1;
	int rc = 0;
	for (size_t i = 0; i < lines->len; ++i) {
		if (write_all_fd(fd, lines->items[i], strlen(lines->items[i])) < 0 ||
			write_all_fd(fd, "\n", 1) < 0) { rc = -1; break; }
	}
	if (close(fd) < 0 && rc == 0) rc = -1;
	return rc;
}

static void listener_vec_push(ListenerVec *v, const char *file,
							  const char *listen, const char *server) {
	if (v->len == v->cap) {
		size_t cap = v->cap ? v->cap * 2 : 16;
		v->items = xrealloc(v->items, cap * sizeof(*v->items));
		v->cap = cap;
	}
	Listener *item = &v->items[v->len++];
	memset(item, 0, sizeof(*item));
	str_copy(item->file, sizeof(item->file), file);
	str_copy(item->listen, sizeof(item->listen), listen);
	str_copy(item->server, sizeof(item->server), server);
}

static void upstream_vec_push(UpstreamVec *v, const char *file,
							  const char *name, const char *target) {
	if (v->len == v->cap) {
		size_t cap = v->cap ? v->cap * 2 : 16;
		v->items = xrealloc(v->items, cap * sizeof(*v->items));
		v->cap = cap;
	}
	Upstream *item = &v->items[v->len++];
	memset(item, 0, sizeof(*item));
	str_copy(item->file, sizeof(item->file), file);
	str_copy(item->name, sizeof(item->name), name);
	str_copy(item->target, sizeof(item->target), target);
}

static intS directive_value(const char *line, const char *directive,
						   char *out, size_t cap) {
	char copy[4096];
	if (str_copy(copy, sizeof(copy), line) < 0) return 0;
	char *s = trim_in_place(copy);
	size_t n = strlen(directive);
	if (strncmp(s, directive, n) || !isspace((unsigned char)s[n])) return 0;
	s = trim_in_place(s + n);
	size_t len = strlen(s);
	if (len && s[len - 1] == ';') {
		s[--len] = '\0';
		s = trim_in_place(s);
	}
	return str_copy(out, cap, s) == 0;
}

static intS block_start(const char *line, const char *keyword) {
	char copy[4096];
	if (str_copy(copy, sizeof(copy), line) < 0) return 0;
	char *s = trim_in_place(copy);
	size_t n = strlen(keyword);
	return !strncmp(s, keyword, n) &&
		   (isspace((unsigned char)s[n]) || s[n] == '{') &&
		   strchr(s, '{') != NULL;
}

static void build_listener_inventory(ListenerVec *listeners) {
	StrVec files;
	strvec_init(&files);
	managed_conf_files(&files);
	for (size_t f = 0; f < files.len; ++f) {
		StrVec lines;
		strvec_init(&lines);
		load_lines(files.items[f], &lines);
		intS in_server = 0;
		int depth = 0;
		StrVec listens;
		strvec_init(&listens);
		char server_name[512] = "";
		for (size_t i = 0; i < lines.len; ++i) {
			const char *line = lines.items[i];
			if (!in_server && block_start(line, "server")) {
				in_server = 1;
				depth = 0;
				strvec_free(&listens);
				strvec_init(&listens);
				server_name[0] = '\0';
			}
			if (!in_server) continue;
			char value[512];
			if (directive_value(line, "listen", value, sizeof(value))) strvec_push(&listens, value);
			if (directive_value(line, "server_name", value, sizeof(value)))
				str_copy(server_name, sizeof(server_name), value);
			depth += count_char(line, '{') - count_char(line, '}');
			if (depth == 0) {
				const char *name = server_name[0] ? server_name : "stream/SNI gateway";
				for (size_t j = 0; j < listens.len; ++j)
					listener_vec_push(listeners, files.items[f], listens.items[j], name);
				in_server = 0;
			}
		}
		strvec_free(&listens);
		strvec_free(&lines);
	}
	strvec_free(&files);
}

static void build_upstream_inventory(UpstreamVec *upstreams) {
	StrVec files;
	strvec_init(&files);
	managed_conf_files(&files);
	for (size_t f = 0; f < files.len; ++f) {
		StrVec lines;
		strvec_init(&lines);
		load_lines(files.items[f], &lines);
		intS in = 0;
		int depth = 0;
		char name[256] = "";
		for (size_t i = 0; i < lines.len; ++i) {
			const char *line = lines.items[i];
			if (!in && block_start(line, "upstream")) {
				char copy[4096];
				if (str_copy(copy, sizeof(copy), line) < 0) continue;
				char *s = trim_in_place(copy) + strlen("upstream");
				s = trim_in_place(s);
				char *end = strpbrk(s, " \t{");
				if (!end) continue;
				*end = '\0';
				if (!*s) continue;
				str_copy(name, sizeof(name), s);
				in = 1;
				depth = 0;
			}
			if (!in) continue;
			char value[512];
			if (directive_value(line, "server", value, sizeof(value)))
				upstream_vec_push(upstreams, files.items[f], name, value);
			depth += count_char(line, '{') - count_char(line, '}');
			if (depth == 0) in = 0;
		}
		strvec_free(&lines);
	}
	strvec_free(&files);
}

static unsigned endpoint_port(const char *spec) {
	char value[512];
	str_copy(value, sizeof(value), spec);
	char *space = strpbrk(value, " \t");
	if (space) *space = '\0';
	char *colon = strrchr(value, ':');
	const char *port = colon ? colon + 1 : value;
	unsigned result = 0;
	return parse_port(port, &result) ? result : 0;
}

static const char *managed_listener_status(const char *spec) {
	unsigned port = endpoint_port(spec);
	if (!port) return "CONFIGURED";
	if (port_owned_by_nginx(port)) return "ACTIVE";
	if (port_in_use(port)) return "OTHER";
	return "INACTIVE";
}

static void display_inventory(void) {
	ListenerVec listeners = {0};
	UpstreamVec upstreams = {0};
	build_listener_inventory(&listeners);
	build_upstream_inventory(&upstreams);
	printf("\n\033[1mScript-managed Nginx listeners and servers\033[0m\n");
	if (listeners.len) {
		printf("%-9s %-34s %-30s %s\n", "STATUS", "LISTEN", "SERVER", "CONFIGURATION");
		printf("%-9s %-34s %-30s %s\n", "---------",
			   "----------------------------------",
			   "------------------------------", "-------------");
		for (size_t i = 0; i < listeners.len; ++i) {
			Listener *v = &listeners.items[i];
			printf("%-9s %-34s %-30s %s\n", managed_listener_status(v->listen),
				   v->listen, v->server, v->file);
		}
	} else {
		printf("No active script-managed listener configurations were found.\n");
	}
	if (upstreams.len) {
		printf("\n%-34s %-38s %s\n", "UPSTREAM", "SERVER", "CONFIGURATION");
		printf("%-34s %-38s %s\n", "----------------------------------",
			   "--------------------------------------", "-------------");
		for (size_t i = 0; i < upstreams.len; ++i) {
			Upstream *v = &upstreams.items[i];
			printf("%-34s %-38s %s\n", v->name, v->target, v->file);
		}
	}
	putchar('\n');
	free(listeners.items);
	free(upstreams.items);
}

static intS has_managed_configurations(void) {
	StrVec files;
	strvec_init(&files);
	managed_conf_files(&files);
	intS result = files.len != 0;
	strvec_free(&files);
	return result;
}

static void print_captured(const Buffer *b, FILE *out) {
	if (b->len) fwrite(b->data, 1, b->len, out);
	if (!b->len || b->data[b->len - 1] != '\n') fputc('\n', out);
	fflush(out);
}

static intS make_validation_dir(void) {
	cleanup_validation_dir();
	for (unsigned i = 0; i < 100; ++i) {
		if (str_printf(app.validation_dir, sizeof(app.validation_dir),
					   "%s/nginx-validation.%ld.%u", app.backup_dir,
					   (long)getpid(), i) < 0) break;
		if (mkdir(app.validation_dir, 0700) == 0) return 1;
	}
	app.validation_dir[0] = '\0';
	return 0;
}

static intS copy_validation_tree(void) {
	DIR *dir = opendir(app.nginx_conf_dir);
	if (!dir) return 0;
	intS ok = 1;
	struct dirent *de;
	while ((de = readdir(dir))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") ||
			!strcmp(de->d_name, "backups")) continue;
		char src[PATH_MAX], dst[PATH_MAX];
		if (str_printf(src, sizeof(src), "%s/%s", app.nginx_conf_dir, de->d_name) < 0 ||
			str_printf(dst, sizeof(dst), "%s/%s", app.validation_dir, de->d_name) < 0 ||
			copy_path_recursive(src, dst) < 0) {
			ok = 0;
			break;
		}
	}
	closedir(dir);
	return ok;
}

static int replace_all(const char *input, const char *from, const char *to, Buffer *out) {
	size_t nf = strlen(from);
	if (!nf) return buffer_append(out, input);
	const char *p = input, *hit;
	while ((hit = strstr(p, from))) {
		if (buffer_append_n(out, p, (size_t)(hit - p)) < 0 ||
			buffer_append(out, to) < 0) return -1;
		p = hit + nf;
	}
	return buffer_append(out, p);
}

static intS validate_nginx_candidate(const char *target, const char *candidate) {
	size_t prefix = strlen(app.nginx_conf_dir);
	if (strncmp(target, app.nginx_conf_dir, prefix) || target[prefix] != '/') {
		log_error("Cannot validate a generated Nginx file outside %s: %s",
				  app.nginx_conf_dir, target);
		return 0;
	}
	const char *relative = target + prefix + 1;
	if (!make_validation_dir()) {
		log_error("Could not create an isolated Nginx validation directory.");
		return 0;
	}
	log_info("Building an isolated validation copy for %s", target);
	if (!copy_validation_tree()) {
		log_error("Could not copy the current Nginx configuration into the validation directory.");
		cleanup_validation_dir();
		return 0;
	}
	char shadow[PATH_MAX], shadow_dir[PATH_MAX];
	if (str_printf(shadow, sizeof(shadow), "%s/%s", app.validation_dir, relative) < 0 ||
		parent_dir(shadow, shadow_dir, sizeof(shadow_dir)) < 0 ||
		mkdir_p(shadow_dir, 0755) < 0) {
		log_error("Could not prepare the candidate validation path: %s", shadow_dir);
		cleanup_validation_dir();
		return 0;
	}
	if (remove_tree(shadow) < 0 && errno != ENOENT) {
		log_error("Could not replace the shadow copy of the validation target.");
		cleanup_validation_dir();
		return 0;
	}
	if (copy_file_preserve(candidate, shadow) < 0) {
		log_error("Could not copy the generated candidate into the validation tree.");
		cleanup_validation_dir();
		return 0;
	}
	Buffer main_out;
	buffer_init(&main_out);
	StrVec main_lines;
	strvec_init(&main_lines);
	load_lines(app.nginx_main_conf, &main_lines);
	intS rewrite_ok = path_is_file(app.nginx_main_conf);
	for (size_t i = 0; rewrite_ok && i < main_lines.len; ++i) {
		Buffer rewritten;
		buffer_init(&rewritten);
		if (replace_all(main_lines.items[i], app.nginx_conf_dir,
						app.validation_dir, &rewritten) < 0 ||
			buffer_append_n(&main_out, rewritten.data ? rewritten.data : "",
							rewritten.len) < 0 ||
			buffer_append(&main_out, "\n") < 0) rewrite_ok = 0;
		buffer_free(&rewritten);
	}
	strvec_free(&main_lines);
	if (!rewrite_ok) {
		buffer_free(&main_out);
		log_error("Could not generate the isolated validation entry point.");
		cleanup_validation_dir();
		return 0;
	}
	char test_main[PATH_MAX];
	str_printf(test_main, sizeof(test_main), "%s/nginx-manager-validation.conf",
			   app.validation_dir);
	if (write_file_mode(test_main, main_out.data, main_out.len, 0600) < 0) {
		buffer_free(&main_out);
		log_error("Could not generate the isolated validation entry point.");
		cleanup_validation_dir();
		return 0;
	}
	buffer_free(&main_out);
	Buffer output;
	buffer_init(&output);
	log_info("Running nginx -t against the generated candidate before installation...");
	char *argv[] = {app.nginx_bin, "-t", "-c", test_main, NULL};
	int rc = run_process(argv, 0, &output);
	if (rc == 0) {
		print_captured(&output, stdout);
		log_success("Candidate validation passed: %s", target);
		buffer_free(&output);
		cleanup_validation_dir();
		return 1;
	}
	print_captured(&output, stderr);
	log_error("Candidate validation failed; the live target was not modified: %s", target);
	buffer_free(&output);
	cleanup_validation_dir();
	return 0;
}

static intS commit_nginx_file(const char *target, mode_t mode) {
	if (!app.current_tmp[0])
		fatalf("Internal error: no generated Nginx candidate is ready for %s.", target);
	if (!validate_nginx_candidate(target, app.current_tmp)) {
		cleanup_temp();
		return 0;
	}
	commit_temp_file(target, mode);
	return 1;
}

static intS finalize_selected_file(const char *target, mode_t mode) {
	if (!app.current_tmp[0])
		fatalf("Internal error: no managed configuration candidate is ready for %s.", target);
	if (!is_script_managed_file(target)) {
		cleanup_temp();
		log_error("The selected file is no longer recognized as script-managed: %s", target);
		return 0;
	}
	if (!validate_nginx_candidate(target, app.current_tmp)) {
		cleanup_temp();
		return 0;
	}
	if (chmod(app.current_tmp, mode) < 0)
		fatalf("Could not set permissions on the managed configuration candidate for %s.", target);
	log_warn("Updating the selected script-managed configuration: %s", target);
	log_info("No overwrite confirmation is required because this node was selected explicitly; a transactional rollback backup will be created.");
	if (!backup_existing_file(target))
		fatalf("Could not create the rollback backup for %s.", target);
	if (rename(app.current_tmp, target) < 0)
		fatalf("Could not atomically install the updated managed configuration: %s", target);
	app.current_tmp[0] = '\0';
	log_success("Updated managed configuration: %s", target);
	return 1;
}

#endif
