#ifndef NM_VALIDATORS_C
#define NM_VALIDATORS_C

#ifndef NM_AMALGAMATED_BUILD
#define NM_AMALGAMATED_BUILD 1
#endif

#include "nm_validators.h"

static intS valid_hostname_common(const char *s, intS require_dot, intS allow_underscore) {
	size_t n = strlen(s);
	if (!n || n > 253 || s[0] == '.' || s[n - 1] == '.' || strstr(s, ".."))
		return 0;
	intS dots = 0;
	const char *label = s;
	for (size_t i = 0; i <= n; ++i) {
		unsigned char c = (unsigned char)s[i];
		if (c == '.' || c == '\0') {
			size_t ln = (size_t)(s + i - label);
			if (!ln || ln > 63 || label[0] == '-' || label[ln - 1] == '-') return 0;
			++dots;
			label = s + i + 1;
		} else if (!(isalnum(c) || c == '-' || (allow_underscore && c == '_'))) {
			return 0;
		}
	}
	return !require_dot || dots >= 2; /* includes final terminator */
}

static intS validate_domain(const char *s) {
	return valid_hostname_common(s, 1, 0);
}

static intS validate_hostname(const char *s) __attribute__((unused));
static intS validate_hostname(const char *s) {
	return valid_hostname_common(s, 0, 0);
}

static intS validate_upstream_hostname(const char *s) {
	return valid_hostname_common(s, 0, 1);
}

static intS parse_port(const char *s, unsigned *out) {
	if (!s || !*s) return 0;
	unsigned long value = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
		if (!isdigit(*p)) return 0;
		value = value * 10 + (*p - '0');
		if (value > 65535) return 0;
	}
	if (!value) return 0;
	if (out) *out = (unsigned)value;
	return 1;
}

static intS validate_ipv4(const char *s) {
	intS parts = 0;
	const char *p = s;
	while (*p) {
		if (++parts > 4) return 0;
		if (!isdigit((unsigned char)*p)) return 0;
		unsigned value = 0;
		do {
			value = value * 10 + (unsigned)(*p - '0');
			if (value > 255) return 0;
			++p;
		} while (isdigit((unsigned char)*p));
		if (*p == '.') ++p;
		else if (*p) return 0;
	}
	return parts == 4 && s[0] && s[strlen(s) - 1] != '.';
}

static intS validate_ipv6(const char *s) {
	if (!strchr(s, ':') || strstr(s, ":::")) return 0;
	for (const char *p = s; *p; ++p)
		if (!(isxdigit((unsigned char)*p) || *p == ':' || *p == '.')) return 0;
	char normalized[INET6_ADDRSTRLEN + 32];
	if (str_copy(normalized, sizeof(normalized), s) < 0) return 0;
	char *last_colon = strrchr(normalized, ':');
	if (last_colon && strchr(last_colon, '.')) {
		char ip[64];
		if (str_copy(ip, sizeof(ip), last_colon + 1) < 0 || !validate_ipv4(ip)) return 0;
		unsigned a, b, c, d;
		if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
		snprintf(last_colon + 1, (size_t)(normalized + sizeof(normalized) - last_colon - 1),
				 "%x:%x", (a << 8) | b, (c << 8) | d);
	}
	struct in6_addr addr;
	return inet_pton(AF_INET6, normalized, &addr) == 1;
}

static intS validate_proxy_host(const char *s) {
	return strspn(s, "0123456789.") == strlen(s)
		? validate_ipv4(s) : validate_upstream_hostname(s);
}

static void prompt_domain(const char *prompt) {
	char input[512];
	for (;;) {
		printf("%s: ", prompt);
		fflush(stdout);
		if (!read_input(input, sizeof(input)))
			fatalf("Input ended before a domain was provided.");
		lower_ascii(input);
		if (validate_domain(input)) {
			str_copy(app.domain, sizeof(app.domain), input);
			return;
		}
		log_warn("Invalid domain. Use a DNS name such as example.com: at least one dot, no spaces, labels containing only letters, digits, and hyphens, and no leading/trailing hyphens.");
	}
}

static void prompt_existing_file(const char *label, char *out, size_t cap) {
	char input[PATH_MAX];
	for (;;) {
		printf("%s: ", label);
		fflush(stdout);
		if (!read_input(input, sizeof(input)))
			fatalf("Input ended before a file path was provided.");
		if (!validate_absolute_path(input)) {
			log_warn("Enter an absolute path using safe path characters and no spaces.");
			continue;
		}
		if (!path_is_file(input)) {
			log_warn("File not found: %s", input);
			continue;
		}
		if (access(input, R_OK) < 0) {
			log_warn("File is not readable: %s", input);
			continue;
		}
		if (str_copy(out, cap, input) < 0) fatalf("File path is too long.");
		return;
	}
}

static void prompt_port(const char *label, unsigned default_port, unsigned *out) {
	char input[64];
	for (;;) {
		printf("%s [%u]: ", label, default_port);
		fflush(stdout);
		if (!read_input(input, sizeof(input)))
			fatalf("Input ended before a port was provided.");
		unsigned port;
		if (!*input) port = default_port;
		else if (!parse_port(input, &port)) {
			log_warn("Enter a numeric TCP port from 1 through 65535.");
			continue;
		}
		if (port_in_use(port)) {
			log_warn("TCP port %u currently has a listening socket.", port);
			if (!confirm("Continue only if this listener belongs to the Nginx configuration being replaced?", 0))
				continue;
		}
		*out = port;
		return;
	}
}

static intS validate_location_path(const char *s) {
	return s && *s;
}

static intS validate_proxy_url(const char *url) {
	const char *rest;
	if (starts_with(url, "http://")) rest = url + 7;
	else if (starts_with(url, "https://")) rest = url + 8;
	else return 0;
	static const char *allowed =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./:@?&=%+,[]-";
	if (strspn(url, allowed) != strlen(url)) return 0;
	char authority[512];
	size_t n = strcspn(rest, "/?");
	if (!n || n >= sizeof(authority)) return 0;
	memcpy(authority, rest, n);
	authority[n] = '\0';
	if (strpbrk(authority, "@&=,")) return 0;
	if (authority[0] == '[') {
		char *end = strchr(authority, ']');
		if (!end) return 0;
		*end = '\0';
		if (!validate_ipv6(authority + 1)) return 0;
		if (!end[1]) return 1;
		return end[1] == ':' && parse_port(end + 2, NULL);
	}
	char *colon = strchr(authority, ':');
	if (colon) {
		if (strchr(colon + 1, ':')) return 0;
		*colon++ = '\0';
		if (!parse_port(colon, NULL)) return 0;
	}
	return validate_proxy_host(authority);
}

static void client_max_body_size(unsigned https_port) {
	app.body_size_mb = 0;
	char answer[64];
	for (;;) {
		printf("\nThis will control the maximum allowed size of the client request body; important for large file uploads or JSON payloads.\n");
		printf("Do you want to set a custom max body size for this node? [y/N]: ");
		fflush(stdout);
		if (!read_input(answer, sizeof(answer))) { putchar('\n'); return; }
		if (!*answer || !strcmp(answer, "n") || !strcmp(answer, "N")) return;
		if (!strcmp(answer, "y") || !strcmp(answer, "Y") || !strcmp(answer, "yes")) break;
		log_warn("Answer y/n?");
	}
	char input[64];
	for (;;) {
		printf("What limit in MB do you want? (e.g. 100): ");
		fflush(stdout);
		if (!read_input(input, sizeof(input)))
			fatalf("Input ended before a client_max_body_size limit was provided.");
		if (!*input || strspn(input, "0123456789") != strlen(input)) {
			log_warn("Enter a positive whole number in MB using digits only.");
			continue;
		}
		char *normalized = input;
		while (*normalized == '0') ++normalized;
		if (!*normalized) {
			log_warn("The limit must be greater than zero.");
			continue;
		}
		unsigned long value = strtoul(normalized, NULL, 10);
		if (!value || value > UINT_MAX) {
			log_warn("Enter a positive whole number in MB using digits only.");
			continue;
		}
		app.body_size_mb = (unsigned)value;
		log_success("Custom client_max_body_size selected for HTTPS port %u: %u MB",
					https_port, app.body_size_mb);
		return;
	}
}

static void find_random_port(unsigned minimum, unsigned maximum) {
	uint32_t span = maximum - minimum + 1;
	for (unsigned i = 0; i < 4096; ++i) {
		unsigned candidate = minimum + random_u32() % span;
		if (!port_in_use(candidate)) {
			app.selected_free_port = candidate;
			return;
		}
	}
	fatalf("Could not find a free TCP port in the requested range.");
}

static void bounded_domain_token(const char *domain, size_t maximum, char *out, size_t cap) {
	char safe[256];
	size_t n = strlen(domain);
	if (n >= sizeof(safe)) fatalf("Internal error: domain token is too long.");
	for (size_t i = 0; i <= n; ++i)
		safe[i] = domain[i] == '.' || domain[i] == '-' ? '_' : domain[i];
	if (n <= maximum) {
		if (str_copy(out, cap, safe) < 0) fatalf("Internal error: bounded identifier buffer is too small.");
		return;
	}
	Buffer line;
	buffer_init(&line);
	buffer_printf(&line, "%s\n", domain);
	uint32_t checksum = posix_cksum_bytes((unsigned char *)line.data, line.len);
	buffer_free(&line);
	char suffix[32];
	snprintf(suffix, sizeof(suffix), "%u", checksum);
	size_t prefix = maximum - strlen(suffix) - 1;
	if (prefix < 1 || prefix + strlen(suffix) + 2 > cap)
		fatalf("Internal error: bounded identifier length is too small.");
	memcpy(out, safe, prefix);
	out[prefix] = '_';
	strcpy(out + prefix + 1, suffix);
}

static void stream_upstream_name(const char *domain, unsigned public_port,
								 unsigned backend_port, char *out, size_t cap) {
	char safe[256];
	size_t n = strlen(domain);
	for (size_t i = 0; i <= n; ++i)
		safe[i] = domain[i] == '.' || domain[i] == '-' ? '_' : domain[i];
	safe[n > 36 ? 36 : n] = '\0';
	int rc = public_port == 443
		? str_printf(out, cap, "sni_%u_443_%s", backend_port, safe)
		: str_printf(out, cap, "sni_%u_%s", public_port, safe);
	if (rc < 0) fatalf("Internal error: stream upstream identifier is too long.");
}

#endif
