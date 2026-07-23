#ifndef NM_PROXY_C
#define NM_PROXY_C

#ifndef NM_AMALGAMATED_BUILD
#define NM_AMALGAMATED_BUILD 1
#endif

#include "nm_proxy.h"

static void append_common_proxy_headers(Buffer *b, unsigned forwarded_port,
										const char *timeout) {
	buffer_printf(b,
		"		proxy_http_version 1.1;\n"
		"		proxy_set_header Host $http_host;\n"
		"		proxy_set_header X-Forwarded-Host $http_host;\n"
		"		proxy_set_header X-Forwarded-Port %u;\n"
		"		proxy_set_header X-Real-IP $remote_addr;\n"
		"		proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;\n"
		"		proxy_set_header X-Forwarded-Proto $scheme;\n"
		"		proxy_connect_timeout 10s;\n"
		"		proxy_send_timeout %s;\n"
		"		proxy_read_timeout %s;\n",
		forwarded_port, timeout, timeout);
}

static void collect_custom_parameters(Buffer *out) {
	char line[4096];
	for (;;) {
		out->len = 0;
		if (out->data) out->data[0] = '\0';
		printf("\nPaste raw Nginx directives exactly as they should appear inside the location block.\n");
		printf("Single-line semicolon-separated and multi-line formats are both supported.\n");
		printf("Examples:\n");
		printf("  proxy_pass http://api_backend; proxy_set_header Host $http_host;\n");
		printf("  proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;\n");
		printf("Enter END on a line by itself when finished.\n\n");
		size_t lines = 0;
		for (;;) {
			if (!read_input(line, sizeof(line)))
				fatalf("Input ended before the custom Nginx directives were completed with END.");
			if (!strcmp(line, "END")) break;
			buffer_printf(out, "%s\n", line);
			++lines;
		}
		if (lines && buffer_has_nonspace(out)) {
			if (lines == 1)
				log_success("Captured one custom directive line; semicolon-separated directives will be parsed directly by Nginx.");
			else
				log_success("Captured %zu custom directive lines for Nginx validation.", lines);
			return;
		}
		log_warn("At least one non-empty Nginx directive is required. Enter the directives again.");
	}
}

static intS location_already_added(const Buffer *b, const char *path) {
	char marker[1024];
	if (str_printf(marker, sizeof(marker), "# LOCATION: %s", path) < 0) return 0;
	return b->data && strstr(b->data, marker);
}

static void collect_custom_locations(unsigned forwarded_port) {
	Buffer fragment;
	buffer_init(&fragment);
	printf("\n\033[1mCustom location examples:\033[0m\n");
	printf("  %-13s %s\n", "Static files:",
		   "location / { root /var/www/site; try_files $uri $uri/ =404; }");
	printf("  %-13s %s\n", "HTTP proxy:",
		   "location /api/ { proxy_pass http://127.0.0.1:3001; ... }");
	printf("  %-13s %s\n", "WebSocket:",
		   "location /ws/ { proxy_pass http://127.0.0.1:3002; Upgrade headers... }");
	printf("  %-13s %s\n", "Regex match:", "location ~ ^/(api) { ... }");
	printf("  %-13s %s\n\n", "PHP regex:", "location ~* \\.php$ { ... }");
	size_t count = 0;
	for (;;) {
		char path[1024];
		for (;;) {
			printf("Location match after the location keyword (for example /, ~ ^/(api), or ~* \\.php$): ");
			fflush(stdout);
			if (!read_input(path, sizeof(path)))
				fatalf("Input ended while collecting location blocks.");
			if (!validate_location_path(path)) {
				log_warn("The location match cannot be empty. Prefix, exact, named, regex, and other Nginx-supported location forms are accepted and will be checked by nginx -t.");
				continue;
			}
			if (location_already_added(&fragment, path)) {
				log_warn("That exact location match has already been configured.");
				continue;
			}
			break;
		}
		printf("\nSelect the location type:\n");
		printf("  1) Static files\n");
		printf("  2) Standard HTTP/HTTPS reverse proxy\n");
		printf("  3) WebSocket reverse proxy\n");
		printf("  4) Custom parameters\n");
		unsigned type = 0;
		char input[64];
		for (;;) {
			printf("Choice [1-4]: ");
			fflush(stdout);
			if (!read_input(input, sizeof(input)))
				fatalf("Input ended while selecting a location type.");
			if (strlen(input) == 1 && input[0] >= '1' && input[0] <= '4') {
				type = (unsigned)(input[0] - '0');
				break;
			}
			log_warn("Enter 1, 2, 3, or 4.");
		}
		buffer_printf(&fragment, "\n# LOCATION: %s\n", path);
		if (type == 1) {
			char root[PATH_MAX], index_name[256];
			for (;;) {
				printf("Absolute static root path [/var/www/site]: ");
				fflush(stdout);
				if (!read_input(root, sizeof(root)))
					fatalf("Input ended while reading the static root.");
				if (!*root) strcpy(root, "/var/www/site");
				if (validate_absolute_path(root)) break;
				log_warn("Enter an absolute path with no spaces or Nginx control characters.");
			}
			printf("Index filename [index.html]: ");
			fflush(stdout);
			if (!read_input(index_name, sizeof(index_name)))
				fatalf("Input ended while reading the index filename.");
			if (!*index_name) strcpy(index_name, "index.html");
			if (strspn(index_name,
					   "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-")
				!= strlen(index_name))
				fatalf("Unsafe index filename: %s", index_name);
			buffer_printf(&fragment,
				"	location %s {\n"
				"		root %s;\n"
				"		index %s;\n"
				"		try_files $uri $uri/ =404;\n"
				"		autoindex off;\n"
				"	}\n", path, root, index_name);
		} else if (type == 2 || type == 3) {
			char upstream[1024];
			for (;;) {
				printf("Upstream URL (for example http://127.0.0.1:3001): ");
				fflush(stdout);
				if (!read_input(upstream, sizeof(upstream)))
					fatalf("Input ended while reading the upstream URL.");
				if (validate_proxy_url(upstream)) break;
				log_warn("Use a valid http:// or https:// URL with a domain, IPv4 address, or bracketed IPv6 address; credentials and Nginx control characters are rejected.");
			}
			buffer_printf(&fragment, "	location %s {\n		proxy_pass %s;\n",
						  path, upstream);
			append_common_proxy_headers(&fragment, forwarded_port,
										type == 3 ? "3600s" : "60s");
			if (type == 3)
				buffer_append(&fragment,
					"		proxy_set_header Upgrade $http_upgrade;\n"
					"		proxy_set_header Connection \"upgrade\";\n"
					"		proxy_buffering off;\n"
					"		proxy_cache off;\n");
			buffer_append(&fragment, "	}\n");
		} else {
			Buffer custom;
			buffer_init(&custom);
			collect_custom_parameters(&custom);
			buffer_printf(&fragment, "	location %s {\n", path);
			const char *p = custom.data;
			while (p && *p) {
				const char *nl = strchr(p, '\n');
				size_t n = nl ? (size_t)(nl - p) : strlen(p);
				buffer_append(&fragment, "		");
				buffer_append_n(&fragment, p, n);
				buffer_append(&fragment, "\n");
				p = nl ? nl + 1 : p + n;
			}
			buffer_append(&fragment, "	}\n");
			buffer_free(&custom);
			log_success("Custom parameters added for location %s; the complete candidate will now be checked by nginx -t.", path);
		}
		++count;
		log_success("Added location block: location %s", path);
		if (!confirm("Add another location block?", 0)) break;
		putchar('\n');
	}
	if (!count) fatalf("At least one location block is required.");
	str_printf(app.location_fragment, sizeof(app.location_fragment),
			   "%s/location-blocks.conf", app.backup_dir);
	if (write_file_mode(app.location_fragment, fragment.data, fragment.len, 0600) < 0) {
		buffer_free(&fragment);
		fatalf("Could not create the location-block workspace.");
	}
	buffer_free(&fragment);
}

static void choose_proxy_layout(unsigned forwarded_port) {
	printf("\nSelect how this HTTPS virtual host should serve traffic:\n");
	printf("  1) Proxy every request to one manually selected backend port\n");
	printf("  2) Build custom static/proxy/WebSocket/custom-parameter location blocks interactively\n");
	char input[64];
	for (;;) {
		printf("Choice [1]: ");
		fflush(stdout);
		if (!read_input(input, sizeof(input)))
			fatalf("Input ended before a proxy layout was selected.");
		if (!*input || !strcmp(input, "1")) {
			for (;;) {
				printf("Desired backend port [3000]: ");
				fflush(stdout);
				if (!read_input(input, sizeof(input)))
					fatalf("Input ended before a backend port was provided.");
				if (!*input) app.backend_port = 3000;
				else if (!parse_port(input, &app.backend_port)) {
					log_warn("Enter a numeric TCP port from 1 through 65535.");
					continue;
				}
				break;
			}
			Buffer fragment;
			buffer_init(&fragment);
			buffer_printf(&fragment,
				"	location / {\n"
				"		proxy_pass http://127.0.0.1:%u;\n", app.backend_port);
			append_common_proxy_headers(&fragment, forwarded_port, "3600s");
			buffer_append(&fragment,
				"		proxy_set_header Upgrade $http_upgrade;\n"
				"		proxy_set_header Connection \"upgrade\";\n"
				"	}\n");
			str_printf(app.location_fragment, sizeof(app.location_fragment),
					   "%s/location-blocks.conf", app.backup_dir);
			if (write_file_mode(app.location_fragment, fragment.data,
								fragment.len, 0600) < 0) {
				buffer_free(&fragment);
				fatalf("Could not create the proxy workspace.");
			}
			buffer_free(&fragment);
			log_success("Selected backend port: %u", app.backend_port);
			return;
		}
		if (!strcmp(input, "2")) {
			collect_custom_locations(forwarded_port);
			return;
		}
		log_warn("Enter 1 or 2.");
	}
}

static void extract_proxy_upstreams(const char *source, StrVec *names) {
	Buffer b;
	buffer_init(&b);
	if (read_file(source, &b) < 0) { buffer_free(&b); return; }
	const char *p = b.data;
	while (p && (p = strstr(p, "proxy_pass"))) {
		p += strlen("proxy_pass");
		while (isspace((unsigned char)*p)) ++p;
		const char *start;
		if (starts_with(p, "http://")) start = p + 7;
		else if (starts_with(p, "https://")) start = p + 8;
		else continue;
		const char *end = start;
		while (*end && *end != '/' && *end != ';' && !isspace((unsigned char)*end)) ++end;
		size_t n = (size_t)(end - start);
		if (n && n < 256) {
			char name[256];
			memcpy(name, start, n);
			name[n] = '\0';
			intS valid = (isalpha((unsigned char)name[0]) != 0 || name[0] == '_');
			for (size_t i = 1; valid && i < n; ++i)
				valid = isalnum((unsigned char)name[i]) != 0 || name[i] == '_' || name[i] == '-';
			if (valid && strcmp(name, "localhost") && !strvec_contains(names, name))
				strvec_push(names, name);
		}
		p = end;
	}
	buffer_free(&b);
}

static intS file_defines_upstream(const char *path, const char *wanted) {
	StrVec lines;
	strvec_init(&lines);
	load_lines(path, &lines);
	intS found = 0;
	for (size_t i = 0; i < lines.len && !found; ++i) {
		char name[256];
		if (upstream_header_name(lines.items[i], name, sizeof(name)) &&
			!strcmp(name, wanted)) found = 1;
	}
	strvec_free(&lines);
	return found;
}

static intS find_upstream_recursive(const char *dir, const char *wanted,
								   const char *excluded, char *found, size_t cap) {
	DIR *dp = opendir(dir);
	if (!dp) return 0;
	struct dirent *de;
	intS result = 0;
	while (!result && (de = readdir(dp))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		char path[PATH_MAX];
		if (str_printf(path, sizeof(path), "%s/%s", dir, de->d_name) < 0) continue;
		char backups_prefix[PATH_MAX];
		str_printf(backups_prefix, sizeof(backups_prefix), "%s/backups/",
				   app.nginx_conf_dir);
		if (starts_with(path, backups_prefix) || !strcmp(path, excluded)) continue;
		struct stat st;
		if (lstat(path, &st) < 0 || S_ISLNK(st.st_mode)) continue;
		if (S_ISDIR(st.st_mode))
			result = find_upstream_recursive(path, wanted, excluded, found, cap);
		else if (S_ISREG(st.st_mode) && file_defines_upstream(path, wanted)) {
			str_copy(found, cap, path);
			result = 1;
		}
	}
	closedir(dp);
	return result;
}

static intS normalize_upstream_server(char *value) {
	char *s = trim_in_place(value);
	if (starts_with(s, "server") && isspace((unsigned char)s[6]))
		s = trim_in_place(s + 6);
	size_t n = strlen(s);
	if (n && s[n - 1] == ';') s[--n] = '\0';
	s = trim_in_place(s);
	if (!*s || strpbrk(s, "{};")) return 0;
	if (s != value) memmove(value, s, strlen(s) + 1);
	return 1;
}

static void collect_required_upstreams(const char *locations, const char *target) {
	str_printf(app.upstream_fragment, sizeof(app.upstream_fragment),
			   "%s/http-upstreams.conf", app.backup_dir);
	Buffer out;
	buffer_init(&out);
	StrVec names;
	strvec_init(&names);
	extract_proxy_upstreams(locations, &names);
	for (size_t i = 0; i < names.len; ++i) {
		char found[PATH_MAX];
		if (find_upstream_recursive(app.nginx_conf_dir, names.items[i], target,
									found, sizeof(found))) {
			log_success("Named upstream %s is already defined in %s", names.items[i], found);
			continue;
		}
		log_warn("Custom proxy_pass references named upstream '%s', but no matching HTTP-level upstream block will remain after replacing %s.",
				 names.items[i], target);
		log_info("A named proxy_pass target must be declared outside all server and location blocks.");
		char server[1024];
		for (;;) {
			printf("Backend server for upstream %s (for example 127.0.0.1:8080): ",
				   names.items[i]);
			fflush(stdout);
			if (!read_input(server, sizeof(server)))
				fatalf("Input ended before a server was provided for upstream %s.",
					   names.items[i]);
			if (normalize_upstream_server(server)) break;
			log_warn("Enter one Nginx upstream server value without braces or embedded semicolons, such as 127.0.0.1:8080.");
		}
		buffer_printf(&out,
			"# nginx-manager: HTTP upstream inferred from custom proxy_pass directives.\n"
			"# UPSTREAM: %s\n"
			"upstream %s {\n"
			"	server %s;\n"
			"}\n\n", names.items[i], names.items[i], server);
		log_success("Added HTTP upstream %s -> %s", names.items[i], server);
	}
	strvec_free(&names);
	if (write_file_mode(app.upstream_fragment, out.data ? out.data : "",
						out.len, 0600) < 0) {
		buffer_free(&out);
		fatalf("Could not create the HTTP-upstream workspace.");
	}
	buffer_free(&out);
}

static int append_file_to_buffer(Buffer *dst, const char *path) {
	Buffer src;
	buffer_init(&src);
	if (read_file(path, &src) < 0) { buffer_free(&src); return -1; }
	int rc = buffer_append_n(dst, src.data, src.len);
	buffer_free(&src);
	return rc;
}

static intS write_https_site_config(const char *listen, const char *target) {
	collect_required_upstreams(app.location_fragment, target);
	Buffer b;
	buffer_init(&b);
	struct stat st;
	if (stat(app.upstream_fragment, &st) == 0 && st.st_size > 0)
		append_file_to_buffer(&b, app.upstream_fragment);
	buffer_printf(&b,
		"# nginx-manager: HTTPS reverse-proxy virtual host for %s.\n"
		"# Host validation is intentionally strict.\n\n"
		"server {\n"
		"	%s\n", app.domain, listen);
	if (app.body_size_mb)
		buffer_printf(&b, "	client_max_body_size %um;\n", app.body_size_mb);
	buffer_printf(&b,
		"	server_name %s;\n\n"
		"	if ($host != \"%s\") {\n"
		"		return 444;\n"
		"	}\n\n"
		"	server_tokens off;\n\n"
		"	ssl_certificate %s;\n"
		"	ssl_certificate_key %s;\n",
		app.domain, app.domain, app.cert_file, app.key_file);
	if (app.ca_file[0])
		buffer_printf(&b, "	ssl_trusted_certificate %s;\n", app.ca_file);
	buffer_append(&b,
		"	ssl_protocols TLSv1.2 TLSv1.3;\n"
		"	ssl_session_timeout 1d;\n"
		"	ssl_session_cache shared:SSL:10m;\n"
		"	ssl_session_tickets off;\n\n"
		"	keepalive_timeout 65s;\n\n"
		"	add_header Strict-Transport-Security \"max-age=31536000\" always;\n"
		"	add_header X-Content-Type-Options \"nosniff\" always;\n"
		"	add_header X-Frame-Options \"SAMEORIGIN\" always;\n"
		"	add_header Referrer-Policy \"strict-origin-when-cross-origin\" always;\n\n");
	if (append_file_to_buffer(&b, app.location_fragment) < 0) {
		buffer_free(&b);
		fatalf("Could not append location blocks to %s.", target);
	}
	buffer_append(&b, "}\n");
	make_temp_for(target);
	if (write_file_mode(app.current_tmp, b.data, b.len, 0600) < 0) {
		buffer_free(&b);
		fatalf("Could not generate %s.", target);
	}
	buffer_free(&b);
	if (!commit_nginx_file(target, 0644)) return 0;
	str_copy(app.site_config, sizeof(app.site_config), target);
	return 1;
}

static intS write_custom_config(const char *target) {
	collect_required_upstreams(app.location_fragment, target);
	Buffer b;
	buffer_init(&b);
	struct stat st;
	if (stat(app.upstream_fragment, &st) == 0 && st.st_size > 0)
		append_file_to_buffer(&b, app.upstream_fragment);
	buffer_printf(&b,
		"# nginx-manager: Custom-port HTTP/HTTPS reverse proxy for %s.\n"
		"# This file does not alter public port-80 or port-443 routing.\n\n"
		"server {\n"
		"	listen %u;\n"
		"	%slisten [::]:%u;\n"
		"	server_name %s;\n\n"
		"	if ($host != \"%s\") {\n"
		"		return 444;\n"
		"	}\n\n"
		"	server_tokens off;\n"
		"	return 301 https://$host:%u$request_uri;\n"
		"}\n\n"
		"server {\n"
		"	listen %u ssl;\n"
		"	%slisten [::]:%u ssl;\n",
		app.domain, app.http_port, ipv6_prefix(), app.http_port, app.domain,
		app.domain, app.https_port, app.https_port, ipv6_prefix(), app.https_port);
	if (app.body_size_mb)
		buffer_printf(&b, "	client_max_body_size %um;\n", app.body_size_mb);
	buffer_printf(&b,
		"	server_name %s;\n\n"
		"	if ($host != \"%s\") {\n"
		"		return 444;\n"
		"	}\n\n"
		"	server_tokens off;\n\n"
		"	ssl_certificate %s;\n"
		"	ssl_certificate_key %s;\n",
		app.domain, app.domain, app.cert_file, app.key_file);
	if (app.ca_file[0])
		buffer_printf(&b, "	ssl_trusted_certificate %s;\n", app.ca_file);
	buffer_append(&b,
		"	ssl_protocols TLSv1.2 TLSv1.3;\n"
		"	ssl_session_timeout 1d;\n"
		"	ssl_session_cache shared:SSL:10m;\n"
		"	ssl_session_tickets off;\n\n"
		"	keepalive_timeout 65s;\n\n"
		"	add_header Strict-Transport-Security \"max-age=31536000\" always;\n"
		"	add_header X-Content-Type-Options \"nosniff\" always;\n"
		"	add_header X-Frame-Options \"SAMEORIGIN\" always;\n"
		"	add_header Referrer-Policy \"strict-origin-when-cross-origin\" always;\n\n");
	if (append_file_to_buffer(&b, app.location_fragment) < 0) {
		buffer_free(&b);
		fatalf("Could not append custom location blocks.");
	}
	buffer_append(&b, "}\n");
	make_temp_for(target);
	if (write_file_mode(app.current_tmp, b.data, b.len, 0600) < 0) {
		buffer_free(&b);
		fatalf("Could not generate %s.", target);
	}
	buffer_free(&b);
	if (!commit_nginx_file(target, 0644)) return 0;
	str_copy(app.site_config, sizeof(app.site_config), target);
	return 1;
}

#endif
