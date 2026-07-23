#ifndef NM_GENERATORS_C
#define NM_GENERATORS_C

#ifndef NM_AMALGAMATED_BUILD
#define NM_AMALGAMATED_BUILD 1
#endif

#include "nm_generators.h"

static const char *ipv6_prefix(void) {
	return app.ipv6_enabled ? "" : "# ";
}

static void install_buffer_target(const char *target, Buffer *b, mode_t mode,
								  intS validate, const char *failure) {
	make_temp_for(target);
	if (write_file_mode(app.current_tmp, b->data ? b->data : "", b->len, 0600) < 0) {
		buffer_free(b);
		fatalf("Could not write generated configuration for %s.", target);
	}
	buffer_free(b);
	if (validate) {
		if (!commit_nginx_file(target, mode)) fatalf("%s", failure);
	} else {
		commit_temp_file(target, mode);
	}
}

static void write_forced_ssl_snippet(unsigned https_port) {
	char target[PATH_MAX];
	str_printf(target, sizeof(target), "%s/snippets/redirect_%u.forcessl.conf",
			   app.nginx_conf_dir, https_port);
	Buffer b;
	buffer_init(&b);
	if (https_port == 443) {
		buffer_append(&b,
			"# nginx-manager: Reusable HTTP-to-HTTPS redirect for public domain listeners.\n"
			"# Include only from an HTTP server block with a strict server_name.\n"
			"return 301 https://$host$request_uri;\n");
	} else {
		buffer_printf(&b,
			"# nginx-manager: Reusable HTTP-to-HTTPS redirect for custom port %u.\n"
			"# Include only from an HTTP server block with a strict server_name.\n"
			"return 301 https://$host:%u$request_uri;\n", https_port, https_port);
	}
	install_buffer_target(target, &b, 0644, 0, "");
	str_copy(app.redirect_snippet, sizeof(app.redirect_snippet), target);
}

static void write_redirect_snippet(void) {
	write_forced_ssl_snippet(443);
	app.redirect_snippet[0] = '\0';
}

static void ensure_support_files(void) {
	char path[PATH_MAX];
	str_printf(path, sizeof(path), "%s/snippets/redirect_443.forcessl.conf",
			   app.nginx_conf_dir);
	if (!path_is_file(path)) {
		log_warn("The managed HTTPS redirect snippet is missing; recreating it.");
		write_redirect_snippet();
	}
}

static void write_redirect_80(void) {
	char target[PATH_MAX];
	str_printf(target, sizeof(target), "%s/conf.d/redirect_80.conf", app.nginx_conf_dir);
	Buffer b;
	buffer_init(&b);
	buffer_printf(&b,
		"# nginx-manager: Public HTTP catch-all.\n"
		"# Unknown Host headers are closed without returning content.\n"
		"server {\n"
		"	listen 80 default_server;\n"
		"	%slisten [::]:80 default_server;\n"
		"	server_name _;\n\n"
		"	server_tokens off;\n"
		"	return 444;\n"
		"}\n", ipv6_prefix());
	install_buffer_target(target, &b, 0644, 1,
		"The generated public HTTP redirect configuration failed validation.");
}

static void ensure_stream_include(void) {
	if (!path_is_file(app.nginx_main_conf))
		fatalf("%s does not exist after Nginx installation.", app.nginx_main_conf);
	StrVec lines;
	strvec_init(&lines);
	load_lines(app.nginx_main_conf, &lines);
	for (size_t i = 0; i < lines.len; ++i) {
		char *copy = xstrdup(lines.items[i]);
		char *s = trim_in_place(copy);
		intS found = starts_with(s, "include ") &&
					strstr(s, "stream-conf.d/*.conf") && ends_with(s, ";");
		free(copy);
		if (found) {
			strvec_free(&lines);
			log_info("The stream configuration include is already present in nginx.conf.");
			return;
		}
	}
	Buffer out;
	buffer_init(&out);
	intS inserted = 0, pending_stream = 0;
	for (size_t i = 0; i < lines.len; ++i) {
		char copy[4096];
		str_copy(copy, sizeof(copy), lines.items[i]);
		char *s = trim_in_place(copy);
		if (!inserted && starts_with(s, "stream") && strchr(s, '{')) {
			char *last = strrchr(lines.items[i], '}');
			if (last && !*trim_in_place(last + 1)) {
				buffer_append_n(&out, lines.items[i], (size_t)(last - lines.items[i]));
				buffer_printf(&out, "\n	include %s/stream-conf.d/*.conf;\n}%s\n",
							  app.nginx_conf_dir, last + 1);
			} else {
				buffer_printf(&out, "%s\n	include %s/stream-conf.d/*.conf;\n",
							  lines.items[i], app.nginx_conf_dir);
			}
			inserted = 1;
			continue;
		}
		if (!inserted && !strcmp(s, "stream")) {
			buffer_printf(&out, "%s\n", lines.items[i]);
			pending_stream = 1;
			continue;
		}
		if (!inserted && pending_stream) {
			buffer_printf(&out, "%s\n", lines.items[i]);
			if (strchr(s, '{')) {
				buffer_printf(&out, "	include %s/stream-conf.d/*.conf;\n",
							  app.nginx_conf_dir);
				inserted = 1;
				pending_stream = 0;
			}
			continue;
		}
		buffer_printf(&out, "%s\n", lines.items[i]);
	}
	if (!inserted) {
		buffer_printf(&out,
			"\n# nginx-manager: SNI/TCP routing configuration.\n"
			"stream {\n"
			"	include %s/stream-conf.d/*.conf;\n"
			"}\n", app.nginx_conf_dir);
	}
	strvec_free(&lines);
	make_temp_for(app.nginx_main_conf);
	if (write_file_mode(app.current_tmp, out.data, out.len, 0600) < 0) {
		buffer_free(&out);
		fatalf("Could not add the stream include to nginx.conf.");
	}
	buffer_free(&out);
	commit_temp_file(app.nginx_main_conf, 0644);
}

static intS server_name_contains(const char *value, const char *domain) {
	char copy[1024];
	if (str_copy(copy, sizeof(copy), value) < 0) return 0;
	char *save = NULL;
	for (char *token = strtok_r(copy, " \t", &save); token;
		 token = strtok_r(NULL, " \t", &save))
		if (!strcmp(token, domain)) return 1;
	return 0;
}

static void write_domain_redirect_80(void) {
	char target[PATH_MAX];
	str_printf(target, sizeof(target), "%s/conf.d/redirect_80.conf", app.nginx_conf_dir);
	Buffer b;
	buffer_init(&b);
	intS can_merge = path_is_file(target) && is_script_managed_file(target) &&
					file_contains_literal(target, "listen 80 default_server");
	if (can_merge) {
		StrVec lines;
		strvec_init(&lines);
		load_lines(target, &lines);
		for (size_t i = 0; i < lines.len;) {
			char copy[4096];
			str_copy(copy, sizeof(copy), lines.items[i]);
			char *s = trim_in_place(copy);
			char comment[512];
			snprintf(comment, sizeof(comment), "# Public HTTP redirect for %s.", app.domain);
			if (!strcmp(s, comment)) { ++i; continue; }
			if (block_start(lines.items[i], "server")) {
				size_t start = i;
				int depth = 0;
				intS matched = 0;
				do {
					char value[512];
					if (directive_value(lines.items[i], "server_name", value, sizeof(value)) &&
						server_name_contains(value, app.domain)) matched = 1;
					depth += count_char(lines.items[i], '{') - count_char(lines.items[i], '}');
					++i;
				} while (i < lines.len && depth > 0);
				if (!matched)
					for (size_t j = start; j < i; ++j) buffer_printf(&b, "%s\n", lines.items[j]);
				continue;
			}
			buffer_printf(&b, "%s\n", lines.items[i++]);
		}
		strvec_free(&lines);
		buffer_append(&b, "\n");
	} else {
		buffer_printf(&b,
			"# nginx-manager: Public HTTP catch-all.\n"
			"# Unknown Host headers are closed without returning content.\n"
			"server {\n"
			"	listen 80 default_server;\n"
			"	%slisten [::]:80 default_server;\n"
			"	server_name _;\n\n"
			"	server_tokens off;\n"
			"	return 444;\n"
			"}\n\n", ipv6_prefix());
	}
	buffer_printf(&b,
		"# Public HTTP redirect for %s.\n"
		"server {\n"
		"	listen 80;\n"
		"	%slisten [::]:80;\n"
		"	server_name %s;\n\n"
		"	server_tokens off;\n"
		"	include %s/snippets/redirect_443.forcessl.conf;\n"
		"}\n", app.domain, ipv6_prefix(), app.domain, app.nginx_conf_dir);
	install_buffer_target(target, &b, 0644, 1,
		"The generated domain redirect configuration failed validation.");
	str_copy(app.http_redirect_config, sizeof(app.http_redirect_config), target);
}

static void write_custom_redirect_config(void) {
	char target[PATH_MAX];
	str_printf(target, sizeof(target), "%s/conf.d/redirect_%u.conf",
			   app.nginx_conf_dir, app.http_port);
	Buffer b;
	buffer_init(&b);
	buffer_printf(&b,
		"# nginx-manager: Custom-port HTTP redirect for %s.\n"
		"# Unknown Host headers on port %u are closed without content.\n"
		"server {\n"
		"	listen %u default_server;\n"
		"	%slisten [::]:%u default_server;\n"
		"	server_name _;\n\n"
		"	server_tokens off;\n"
		"	return 444;\n"
		"}\n\n"
		"server {\n"
		"	listen %u;\n"
		"	%slisten [::]:%u;\n"
		"	server_name %s;\n\n"
		"	if ($host != \"%s\") {\n"
		"		return 444;\n"
		"	}\n\n"
		"	server_tokens off;\n"
		"	include %s;\n"
		"}\n",
		app.domain, app.http_port, app.http_port, ipv6_prefix(), app.http_port,
		app.http_port, ipv6_prefix(), app.http_port, app.domain, app.domain,
		app.redirect_snippet);
	install_buffer_target(target, &b, 0644, 1,
		"The generated custom-port redirect configuration failed validation.");
	str_copy(app.http_redirect_config, sizeof(app.http_redirect_config), target);
}

static intS map_header_matches(const char *line, const char *variable) {
	char copy[4096];
	str_copy(copy, sizeof(copy), line);
	char *s = trim_in_place(copy);
	if (!starts_with(s, "map ")) return 0;
	char dest[256];
	if (sscanf(s, "map $ssl_preread_server_name $%255s", dest) != 1) return 0;
	char *brace = strchr(dest, '{');
	if (brace) *brace = '\0';
	return !strcmp(trim_in_place(dest), variable);
}

static intS upstream_header_name(const char *line, char *out, size_t cap) {
	if (!block_start(line, "upstream")) return 0;
	char copy[4096];
	str_copy(copy, sizeof(copy), line);
	char *s = trim_in_place(copy) + strlen("upstream");
	s = trim_in_place(s);
	char *end = strpbrk(s, " \t{");
	if (!end) return 0;
	*end = '\0';
	return str_copy(out, cap, s) == 0;
}

static intS find_stream_route_upstream(const char *path, const char *domain,
									  const char *map_variable,
									  char *out, size_t cap) {
	StrVec lines;
	strvec_init(&lines);
	load_lines(path, &lines);
	intS in = 0, found = 0;
	int depth = 0;
	for (size_t i = 0; i < lines.len && !found; ++i) {
		if (!in && map_header_matches(lines.items[i], map_variable)) {
			in = 1;
			depth = 0;
		}
		if (!in) continue;
		char copy[4096];
		str_copy(copy, sizeof(copy), lines.items[i]);
		char *s = trim_in_place(copy);
		if (*s && *s != '#' && !starts_with(s, "default ")) {
			char *space = strpbrk(s, " \t");
			if (space) {
				*space++ = '\0';
				space = trim_in_place(space);
				size_t n = strlen(space);
				if (n && space[n - 1] == ';') space[n - 1] = '\0';
				if (!strcmp(s, domain)) {
					found = str_copy(out, cap, trim_in_place(space)) == 0;
				}
			}
		}
		depth += count_char(lines.items[i], '{') - count_char(lines.items[i], '}');
		if (depth == 0) in = 0;
	}
	strvec_free(&lines);
	return found;
}

static intS route_line_for_domain(const char *line, const char *domain) {
	char copy[4096];
	str_copy(copy, sizeof(copy), line);
	char *s = trim_in_place(copy);
	if (!*s || *s == '#' || starts_with(s, "default ")) return 0;
	char *space = strpbrk(s, " \t");
	if (!space) return 0;
	*space = '\0';
	return !strcmp(s, domain);
}

static intS merge_stream_sni_candidate(const char *source, const char *domain,
									  const char *upstream, unsigned backend_port,
									  const char *map_variable,
									  const char *reject_upstream,
									  const char *existing_upstream,
									  const char *output) {
	StrVec lines;
	strvec_init(&lines);
	load_lines(source, &lines);
	Buffer out;
	buffer_init(&out);
	intS in_map = 0, map_found = 0, route_written = 0;
	int map_depth = 0;
	intS upstream_written = 0;
	for (size_t i = 0; i < lines.len;) {
		const char *line = lines.items[i];
		if (!in_map && map_header_matches(line, map_variable)) {
			in_map = 1;
			map_found = 1;
			map_depth = count_char(line, '{') - count_char(line, '}');
			buffer_printf(&out, "%s\n", line);
			++i;
			continue;
		}
		if (in_map) {
			int next_depth = map_depth + count_char(line, '{') - count_char(line, '}');
			if (next_depth == 0) {
				if (!route_written) {
					buffer_printf(&out, "	%s %s;\n", domain, upstream);
					route_written = 1;
				}
				buffer_printf(&out, "%s\n", line);
				in_map = 0;
				map_depth = 0;
				++i;
				continue;
			}
			if (route_line_for_domain(line, domain)) {
				buffer_printf(&out, "	%s %s;\n", domain, upstream);
				route_written = 1;
			} else {
				buffer_printf(&out, "%s\n", line);
			}
			map_depth = next_depth;
			++i;
			continue;
		}
		char name[256];
		if (upstream_header_name(line, name, sizeof(name)) &&
			(!strcmp(name, upstream) ||
			 (*existing_upstream && !strcmp(name, existing_upstream)))) {
			int depth = 0;
			do {
				depth += count_char(lines.items[i], '{') - count_char(lines.items[i], '}');
				++i;
			} while (i < lines.len && depth > 0);
			if (!upstream_written) {
				buffer_printf(&out, "upstream %s {\n	server 127.0.0.1:%u;\n}\n\n",
							  upstream, backend_port);
				upstream_written = 1;
			}
			continue;
		}
		if (!upstream_written &&
			((upstream_header_name(line, name, sizeof(name)) &&
			  !strcmp(name, reject_upstream)) || block_start(line, "server"))) {
			buffer_printf(&out, "upstream %s {\n	server 127.0.0.1:%u;\n}\n\n",
						  upstream, backend_port);
			upstream_written = 1;
		}
		buffer_printf(&out, "%s\n", line);
		++i;
	}
	if (!upstream_written)
		buffer_printf(&out, "upstream %s {\n	server 127.0.0.1:%u;\n}\n\n",
					  upstream, backend_port);
	intS ok = map_found && route_written && upstream_written &&
			 write_file_mode(output, out.data, out.len, 0600) == 0;
	buffer_free(&out);
	strvec_free(&lines);
	return ok;
}

static void write_stream_sni_config(unsigned public_port) {
	char target[PATH_MAX], map_variable[128], reject[128], upstream[256], existing[256] = "";
	str_printf(target, sizeof(target), "%s/stream-conf.d/redirect_%u.conf",
			   app.nginx_conf_dir, public_port);
	str_printf(map_variable, sizeof(map_variable), "redirect_%u_tls_backend", public_port);
	str_printf(reject, sizeof(reject), "redirect_%u_reject", public_port);
	unsigned backend = app.tls_passthrough ? app.backend_port : app.tls_internal_port;
	unsigned name_port = public_port == 443
		? (app.backend_port ? app.backend_port : backend) : 0;
	stream_upstream_name(app.domain, public_port, name_port, upstream, sizeof(upstream));
	if (path_is_file(target))
		(void)find_stream_route_upstream(target, app.domain, map_variable,
										 existing, sizeof(existing));
	make_temp_for(target);
	if (path_is_file(target)) {
		if (!is_script_managed_file(target)) {
			cleanup_temp();
			fatalf("Refusing to merge into an unmanaged TLS stream configuration: %s", target);
		}
		if (!merge_stream_sni_candidate(target, app.domain, upstream, backend,
										map_variable, reject, existing, app.current_tmp)) {
			cleanup_temp();
			fatalf("Could not safely merge %s into the existing TLS stream configuration: %s",
				   app.domain, target);
		}
		if (*existing && strcmp(existing, upstream))
			log_info("Migrating SNI upstream name for %s from %s to %s.",
					 app.domain, existing, upstream);
		else
			log_info("Preserving existing SNI routes in %s and updating only %s.",
					 target, app.domain);
	} else {
		Buffer b;
		buffer_init(&b);
		buffer_printf(&b,
			"# nginx-manager: Public TLS/SNI gateway on port %u.\n"
			"# Each configured SNI name is retained until its managed upstream is explicitly removed.\n\n"
			"map $ssl_preread_server_name $%s {\n"
			"	default %s;\n"
			"	%s %s;\n"
			"}\n\n"
			"upstream %s {\n"
			"	server 127.0.0.1:%u;\n"
			"}\n\n"
			"# Unknown or absent SNI is rejected through a local discard endpoint.\n"
			"upstream %s {\n"
			"	server 127.0.0.1:9;\n"
			"}\n\n"
			"server {\n"
			"	listen %u;\n"
			"	%slisten [::]:%u;\n\n"
			"	ssl_preread on;\n"
			"	proxy_pass $%s;\n"
			"	proxy_connect_timeout 2s;\n"
			"	proxy_timeout 1h;\n"
			"}\n",
			public_port, map_variable, reject, app.domain, upstream,
			upstream, backend, reject, public_port, ipv6_prefix(), public_port,
			map_variable);
		if (write_file_mode(app.current_tmp, b.data, b.len, 0600) < 0) {
			buffer_free(&b);
			fatalf("Could not generate TLS stream configuration.");
		}
		buffer_free(&b);
	}
	if (!commit_nginx_file(target, 0644))
		fatalf("The generated TLS stream configuration failed validation.");
	str_copy(app.stream_config, sizeof(app.stream_config), target);
}

#endif
