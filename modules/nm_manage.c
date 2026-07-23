#ifndef NM_MANAGE_C
#define NM_MANAGE_C

#ifndef NM_AMALGAMATED_BUILD
#define NM_AMALGAMATED_BUILD 1
#endif

#include "nm_manage.h"

static char *replace_port_tokens_alloc(const char *line, unsigned old_port,
									   unsigned new_port) {
	char old_text[16], new_text[16];
	snprintf(old_text, sizeof(old_text), "%u", old_port);
	snprintf(new_text, sizeof(new_text), "%u", new_port);
	Buffer out;
	buffer_init(&out);
	const char *p = line;
	size_t old_len = strlen(old_text);
	while (*p) {
		const char *hit = strstr(p, old_text);
		if (!hit) { buffer_append(&out, p); break; }
		intS left_ok = hit == line || !isdigit((unsigned char)hit[-1]);
		intS right_ok = !isdigit((unsigned char)hit[old_len]);
		if (!left_ok || !right_ok) {
			buffer_append_n(&out, p, (size_t)(hit - p + 1));
			p = hit + 1;
			continue;
		}
		buffer_append_n(&out, p, (size_t)(hit - p));
		buffer_append(&out, new_text);
		p = hit + old_len;
	}
	if (!out.data) return xstrdup("");
	return out.data;
}

static char *replace_literal_alloc(const char *line, const char *old,
								   const char *replacement) {
	Buffer out;
	buffer_init(&out);
	replace_all(line, old, replacement, &out);
	return out.data ? out.data : xstrdup("");
}

static char *replace_listen_endpoint_alloc(const char *line, unsigned old_port,
										   unsigned new_port) {
	char *copy = xstrdup(line);
	char *s = copy;
	while (isspace((unsigned char)*s)) ++s;
	if (!starts_with(s, "listen") || !isspace((unsigned char)s[6])) return copy;
	s += 6;
	while (isspace((unsigned char)*s)) ++s;
	char *end = s;
	while (*end && !isspace((unsigned char)*end) && *end != ';') ++end;
	char token[512];
	size_t n = (size_t)(end - s);
	if (n >= sizeof(token)) return copy;
	memcpy(token, s, n);
	token[n] = '\0';
	char old_text[16], new_text[16];
	snprintf(old_text, sizeof(old_text), "%u", old_port);
	snprintf(new_text, sizeof(new_text), "%u", new_port);
	intS replace = !strcmp(token, old_text);
	size_t tl = strlen(token), ol = strlen(old_text);
	if (!replace && tl > ol + 1 && token[tl - ol - 1] == ':' &&
		!strcmp(token + tl - ol, old_text)) replace = 1;
	if (!replace) return copy;
	Buffer out;
	buffer_init(&out);
	buffer_append_n(&out, copy, (size_t)(s - copy));
	if (!strcmp(token, old_text)) buffer_append(&out, new_text);
	else {
		buffer_append_n(&out, token, tl - ol);
		buffer_append(&out, new_text);
	}
	buffer_append(&out, end);
	free(copy);
	return out.data;
}

static intS replace_listener_port(const char *source, unsigned old_port,
								 unsigned new_port, const char *output) {
	StrVec lines, transformed;
	strvec_init(&lines);
	strvec_init(&transformed);
	load_lines(source, &lines);
	for (size_t i = 0; i < lines.len; ++i) {
		char copy[4096];
		str_copy(copy, sizeof(copy), lines.items[i]);
		char *s = trim_in_place(copy);
		char *next;
		if (starts_with(s, "listen ") || starts_with(s, "listen\t")) {
			next = replace_listen_endpoint_alloc(lines.items[i], old_port, new_port);
		} else {
			intS port_aware =
				((strstr(lines.items[i], "return 301 ") ||
				  strstr(lines.items[i], "return 302 ") ||
				  strstr(lines.items[i], "return 307 ") ||
				  strstr(lines.items[i], "return 308 ")) &&
				 strstr(lines.items[i], "https://")) ||
				strstr(lines.items[i], "proxy_set_header X-Forwarded-Port") ||
				strstr(lines.items[i], "redirect_") ||
				strstr(lines.items[i], "sni_");
			next = port_aware
				? replace_port_tokens_alloc(lines.items[i], old_port, new_port)
				: xstrdup(lines.items[i]);
		}
		strvec_push(&transformed, next);
		free(next);
	}
	intS ok = write_lines(output, &transformed, 0600) == 0;
	strvec_free(&lines);
	strvec_free(&transformed);
	return ok;
}

static intS replace_upstream_port_candidate(const char *source, const char *wanted_name,
										   const char *wanted_target,
										   unsigned old_port, unsigned new_port,
										   const char *output) {
	StrVec lines, transformed;
	strvec_init(&lines);
	strvec_init(&transformed);
	load_lines(source, &lines);
	char old_prefix[64], new_name[256];
	str_printf(old_prefix, sizeof(old_prefix), "sni_%u_443_", old_port);
	if (starts_with(wanted_name, old_prefix))
		str_printf(new_name, sizeof(new_name), "sni_%u_443_%s",
				   new_port, wanted_name + strlen(old_prefix));
	else
		str_copy(new_name, sizeof(new_name), wanted_name);
	intS in = 0, matched = 0;
	int depth = 0;
	for (size_t i = 0; i < lines.len; ++i) {
		const char *original = lines.items[i];
		char header[256];
		if (!in && upstream_header_name(original, header, sizeof(header)) &&
			!strcmp(header, wanted_name)) {
			in = 1;
			depth = 0;
		}
		char *line = strcmp(new_name, wanted_name)
			? replace_literal_alloc(original, wanted_name, new_name)
			: xstrdup(original);
		if (in) {
			char value[512];
			if (directive_value(original, "server", value, sizeof(value)) &&
				!strcmp(trim_in_place(value), wanted_target)) {
				char *replaced = replace_port_tokens_alloc(line, old_port, new_port);
				free(line);
				line = replaced;
				matched = 1;
			}
		}
		strvec_push(&transformed, line);
		free(line);
		if (in) {
			depth += count_char(original, '{') - count_char(original, '}');
			if (depth == 0) in = 0;
		}
	}
	intS ok = matched && write_lines(output, &transformed, 0600) == 0;
	strvec_free(&lines);
	strvec_free(&transformed);
	return ok;
}

static intS remove_listener_candidate(const char *source, const char *wanted_listen,
									 const char *wanted_server, const char *output) {
	StrVec lines, result;
	strvec_init(&lines);
	strvec_init(&result);
	load_lines(source, &lines);
	intS removed = 0;
	for (size_t i = 0; i < lines.len;) {
		if (block_start(lines.items[i], "server")) {
			size_t start = i;
			int depth = 0;
			intS listen_match = 0;
			char server_name[512] = "";
			do {
				char value[512];
				if (directive_value(lines.items[i], "listen", value, sizeof(value)) &&
					!strcmp(trim_in_place(value), wanted_listen)) listen_match = 1;
				if (directive_value(lines.items[i], "server_name", value, sizeof(value)))
					str_copy(server_name, sizeof(server_name), trim_in_place(value));
				depth += count_char(lines.items[i], '{') - count_char(lines.items[i], '}');
				++i;
			} while (i < lines.len && depth > 0);
			intS name_match = !strcmp(wanted_server, "stream/SNI gateway")
				? !server_name[0] : !strcmp(server_name, wanted_server);
			if (!removed && listen_match && name_match) {
				removed = 1;
			} else {
				for (size_t j = start; j < i; ++j) strvec_push(&result, lines.items[j]);
			}
			continue;
		}
		strvec_push(&result, lines.items[i++]);
	}
	intS ok = removed && write_lines(output, &result, 0600) == 0;
	strvec_free(&lines);
	strvec_free(&result);
	return ok;
}

static intS map_route_targets(const char *line, const char *wanted) {
	char copy[4096];
	str_copy(copy, sizeof(copy), line);
	char *s = trim_in_place(copy);
	if (!*s || *s == '#' || starts_with(s, "default ")) return 0;
	char *space = strpbrk(s, " \t");
	if (!space) return 0;
	space = trim_in_place(space);
	size_t n = strlen(space);
	if (n && space[n - 1] == ';') space[n - 1] = '\0';
	return !strcmp(trim_in_place(space), wanted);
}

static intS remove_upstream_candidate(const char *source, const char *wanted_name,
									 const char *output) {
	StrVec lines, result;
	strvec_init(&lines);
	strvec_init(&result);
	load_lines(source, &lines);
	intS removed = 0;
	for (size_t i = 0; i < lines.len;) {
		char name[256];
		if (!removed && upstream_header_name(lines.items[i], name, sizeof(name)) &&
			!strcmp(name, wanted_name)) {
			int depth = 0;
			do {
				depth += count_char(lines.items[i], '{') - count_char(lines.items[i], '}');
				++i;
			} while (i < lines.len && depth > 0);
			removed = 1;
			continue;
		}
		if (!map_route_targets(lines.items[i], wanted_name))
			strvec_push(&result, lines.items[i]);
		++i;
	}
	intS ok = removed && write_lines(output, &result, 0600) == 0;
	strvec_free(&lines);
	strvec_free(&result);
	return ok;
}

static void change_listener_port(unsigned old_port) {
	char input[64];
	for (;;) {
		printf("New listener port (current %u): ", old_port);
		fflush(stdout);
		if (!read_input(input, sizeof(input)))
			fatalf("Input ended before a replacement listener port was provided.");
		unsigned port;
		if (!parse_port(input, &port)) {
			log_warn("Enter a numeric TCP port from 1 through 65535.");
			continue;
		}
		if (port == old_port) {
			log_warn("The replacement listener port must differ from the current port.");
			continue;
		}
		if (port_in_use(port)) {
			log_warn("TCP port %u currently has a listening socket.", port);
			if (!confirm("Use this listener port anyway?", 0)) continue;
		}
		app.managed_new_port = port;
		return;
	}
}

static void change_upstream_port(unsigned old_port) {
	char input[64];
	for (;;) {
		printf("New backend port (current %u): ", old_port);
		fflush(stdout);
		if (!read_input(input, sizeof(input)))
			fatalf("Input ended before a replacement backend port was provided.");
		unsigned port;
		if (!parse_port(input, &port)) {
			log_warn("Enter a numeric TCP port from 1 through 65535.");
			continue;
		}
		if (port == old_port) {
			log_warn("The replacement backend port must differ from the current port.");
			continue;
		}
		app.managed_new_port = port;
		return;
	}
}

static intS modify_managed_listener(const Listener *item) {
	unsigned old_port = endpoint_port(item->listen);
	if (!old_port) {
		log_error("Could not determine the numeric port from managed listener: %s", item->listen);
		return 0;
	}
	for (;;) {
		change_listener_port(old_port);
		unsigned new_port = app.managed_new_port;
		make_temp_for(item->file);
		if (!replace_listener_port(item->file, old_port, new_port, app.current_tmp))
			fatalf("Could not generate the listener-port replacement candidate.");
		log_warn("Every port-aware reference to %u in %s will be updated to %u so related redirects, forwarded-port headers, and generated identifiers remain consistent.",
				 old_port, item->file, new_port);
		if (finalize_selected_file(item->file, 0644)) {
			strcpy(app.mode, "manage");
			str_printf(app.managed_node_action, sizeof(app.managed_node_action),
					   "Changed listener port %u to %u", old_port, new_port);
			str_printf(app.managed_node_description, sizeof(app.managed_node_description),
					   "%s in %s", item->server, item->file);
			return 1;
		}
		log_warn("The listener-port change was rejected by nginx -t; the live configuration was not modified.");
		if (!confirm("Try a different listener port?", 1)) return 0;
	}
}

static intS modify_managed_upstream(const Upstream *item) {
	unsigned old_port = endpoint_port(item->target);
	if (!old_port) {
		log_error("Could not determine the numeric backend port from upstream target: %s",
				  item->target);
		return 0;
	}
	for (;;) {
		change_upstream_port(old_port);
		unsigned new_port = app.managed_new_port;
		make_temp_for(item->file);
		if (!replace_upstream_port_candidate(item->file, item->name, item->target,
											 old_port, new_port, app.current_tmp)) {
			cleanup_temp();
			log_error("Could not locate the selected upstream server directive in %s.",
					  item->file);
			return 0;
		}
		if (finalize_selected_file(item->file, 0644)) {
			strcpy(app.mode, "manage");
			str_printf(app.managed_node_action, sizeof(app.managed_node_action),
					   "Changed upstream port %u to %u", old_port, new_port);
			str_printf(app.managed_node_description, sizeof(app.managed_node_description),
					   "%s in %s", item->name, item->file);
			return 1;
		}
		log_warn("The upstream-port change was rejected by nginx -t; the live configuration was not modified.");
		if (!confirm("Try a different backend port?", 1)) return 0;
	}
}

static intS remove_managed_listener(const Listener *item) {
	log_warn("Removing this listener removes its complete server block from %s.", item->file);
	log_warn("Related redirects or upstreams in other blocks are preserved and must remain valid.");
	char prompt[1024];
	str_printf(prompt, sizeof(prompt), "Remove listener '%s' for '%s'?",
			   item->listen, item->server);
	if (!confirm(prompt, 0)) return 0;
	make_temp_for(item->file);
	if (!remove_listener_candidate(item->file, item->listen, item->server,
								   app.current_tmp)) {
		cleanup_temp();
		log_error("Could not locate the selected listener server block in %s.", item->file);
		return 0;
	}
	if (!finalize_selected_file(item->file, 0644)) {
		log_warn("Nginx rejected the removal, usually because another managed node still depends on this server block.");
		return 0;
	}
	strcpy(app.mode, "manage");
	str_printf(app.managed_node_action, sizeof(app.managed_node_action),
			   "Removed listener %s", item->listen);
	str_printf(app.managed_node_description, sizeof(app.managed_node_description),
			   "%s from %s", item->server, item->file);
	return 1;
}

static intS remove_managed_upstream(const Upstream *item) {
	log_warn("Removing this node removes the complete upstream '%s' block from %s.",
			 item->name, item->file);
	log_warn("Matching SNI map routes in the same managed file are removed with it; unrelated routes and upstreams are preserved.");
	log_warn("Nginx validation will prevent removal while any other remaining directive still references it.");
	char prompt[512];
	str_printf(prompt, sizeof(prompt), "Remove upstream '%s'?", item->name);
	if (!confirm(prompt, 0)) return 0;
	make_temp_for(item->file);
	if (!remove_upstream_candidate(item->file, item->name, app.current_tmp)) {
		cleanup_temp();
		log_error("Could not locate upstream '%s' in %s.", item->name, item->file);
		return 0;
	}
	if (!finalize_selected_file(item->file, 0644)) {
		log_warn("Nginx rejected the upstream removal because the resulting configuration is not valid.");
		return 0;
	}
	strcpy(app.mode, "manage");
	str_printf(app.managed_node_action, sizeof(app.managed_node_action),
			   "Removed upstream %s", item->name);
	str_printf(app.managed_node_description, sizeof(app.managed_node_description),
			   "%s from %s", item->target, item->file);
	return 1;
}

static intS manage_existing_confs(void) {
	ListenerVec listeners = {0};
	UpstreamVec upstreams = {0};
	build_listener_inventory(&listeners);
	build_upstream_inventory(&upstreams);
	size_t total = listeners.len + upstreams.len;
	if (!total) {
		log_warn("No script-managed listener, server, or upstream nodes are currently available to manage.");
		free(listeners.items); free(upstreams.items);
		return 0;
	}
	char input[64];
	for (;;) {
		printf("\n\033[1mManage existing configurations\033[0m\n");
		size_t number = 1;
		for (size_t i = 0; i < listeners.len; ++i, ++number)
			printf("  %zu) Listener  %-28s Server: %-28s %s\n", number,
				   listeners.items[i].listen, listeners.items[i].server,
				   listeners.items[i].file);
		for (size_t i = 0; i < upstreams.len; ++i, ++number)
			printf("  %zu) Upstream  %-28s Target: %-28s %s\n", number,
				   upstreams.items[i].name, upstreams.items[i].target,
				   upstreams.items[i].file);
		printf("  0) Return to the previous menu\n");
		printf("Select a managed node [0-%zu]: ", total);
		fflush(stdout);
		if (!read_input(input, sizeof(input)))
			fatalf("Input ended before a managed node was selected.");
		char *end;
		errno = 0;
		unsigned long choice = strtoul(input, &end, 10);
		if (!*input || *end || errno || choice > total) {
			log_warn("Enter a number from 0 through %zu.", total);
			continue;
		}
		if (!choice) { free(listeners.items); free(upstreams.items); return 0; }
		intS listener = choice <= listeners.len;
		size_t index = listener ? choice - 1 : choice - listeners.len - 1;
		const char *kind = listener ? "listener" : "upstream";
		const char *name = listener ? listeners.items[index].listen : upstreams.items[index].name;
		printf("\nSelected %s: %s\n", kind, name);
		printf("  1) Modify its port\n");
		printf("  2) Remove this node\n");
		printf("  3) Choose another node\n");
		for (;;) {
			printf("Action [1-3]: ");
			fflush(stdout);
			if (!read_input(input, sizeof(input)))
				fatalf("Input ended before a management action was selected.");
			intS success = 0;
			if (!strcmp(input, "1")) {
				success = listener
					? modify_managed_listener(&listeners.items[index])
					: modify_managed_upstream(&upstreams.items[index]);
				if (success) { free(listeners.items); free(upstreams.items); return 1; }
				log_warn("No managed configuration was changed.");
				break;
			}
			if (!strcmp(input, "2")) {
				success = listener
					? remove_managed_listener(&listeners.items[index])
					: remove_managed_upstream(&upstreams.items[index]);
				if (success) { free(listeners.items); free(upstreams.items); return 1; }
				log_warn("No managed configuration was removed.");
				break;
			}
			if (!strcmp(input, "3")) break;
			log_warn("Enter 1, 2, or 3.");
		}
	}
}

#endif
