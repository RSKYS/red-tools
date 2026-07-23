#ifndef NM_RUNTIME_C
#define NM_RUNTIME_C

#ifndef NM_AMALGAMATED_BUILD
#define NM_AMALGAMATED_BUILD 1
#endif

#include "nm_runtime.h"

static intS validate_absolute_path(const char *s) {
	if (!s || s[0] != '/') return 0;
	if (strstr(s, "//") || ends_with(s, "/..") || strstr(s, "/../")) return 0;
	static const char *allowed =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./+,:=@%-";
	return strspn(s, allowed) == strlen(s);
}

static void initialize_runtime_paths(void) {
	const char *dir = env_default("NGINX_CONF_DIR", "/etc/nginx");
	if (str_copy(app.nginx_conf_dir, sizeof(app.nginx_conf_dir), dir) < 0)
		fatalf("NGINX_CONF_DIR is too long.");
	char default_main[PATH_MAX], default_state[PATH_MAX];
	if (str_printf(default_main, sizeof(default_main), "%s/nginx.conf", app.nginx_conf_dir) < 0 ||
		str_printf(default_state, sizeof(default_state), "%s/.nginx-manager.conf", app.nginx_conf_dir) < 0)
		fatalf("Nginx runtime path is too long.");
	if (str_copy(app.nginx_main_conf, sizeof(app.nginx_main_conf),
				 env_default("NGINX_MAIN_CONF", default_main)) < 0 ||
		str_copy(app.nginx_bin, sizeof(app.nginx_bin), env_default("NGINX_BIN", "nginx")) < 0 ||
		str_copy(app.state_file, sizeof(app.state_file),
				 env_default("NGINX_RPM_STATE_FILE", default_state)) < 0)
		fatalf("A configured runtime path is too long.");
	if (!validate_absolute_path(app.nginx_conf_dir))
		fatalf("NGINX_CONF_DIR must be an absolute path without spaces or Nginx control characters.");
	if (!validate_absolute_path(app.nginx_main_conf))
		fatalf("NGINX_MAIN_CONF must be an absolute path without spaces or Nginx control characters.");
	if (!validate_absolute_path(app.state_file))
		fatalf("NGINX_RPM_STATE_FILE must be an absolute path without spaces or Nginx control characters.");
}

static intS load_management_state(void) {
	if (!path_is_file(app.state_file)) return 0;
	if (access(app.state_file, R_OK) < 0) {
		log_warn("The management state file is unreadable: %s", app.state_file);
		return 0;
	}
	char manager_id[64] = "", version[32] = "", installed[16] = "",
		 configured[16] = "", ipv6[16] = "", conf_dir[PATH_MAX] = "",
		 main_conf[PATH_MAX] = "", installed_at[64] = "", configured_at[64] = "";
	FILE *fp = fopen(app.state_file, "r");
	if (!fp) return 0;
	char line[PATH_MAX + 128];
	while (fgets(line, sizeof(line), fp)) {
		char *nl = strchr(line, '\n');
		if (nl) *nl = '\0';
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq++ = '\0';
#define LOAD_FIELD(key, dst) if (!strcmp(line, key)) (void)str_copy(dst, sizeof(dst), eq)
		LOAD_FIELD("MANAGER_ID", manager_id);
		else LOAD_FIELD("STATE_VERSION", version);
		else LOAD_FIELD("INSTALLED_BY_SCRIPT", installed);
		else LOAD_FIELD("CONFIGURED_BY_SCRIPT", configured);
		else LOAD_FIELD("IPV6_ENABLED", ipv6);
		else LOAD_FIELD("NGINX_CONF_DIR", conf_dir);
		else LOAD_FIELD("NGINX_MAIN_CONF", main_conf);
		else LOAD_FIELD("INSTALLED_AT", installed_at);
		else LOAD_FIELD("LAST_CONFIGURED_AT", configured_at);
#undef LOAD_FIELD
	}
	fclose(fp);
	if (strcmp(manager_id, MANAGER_ID) || strcmp(version, STATE_VERSION) ||
		strcmp(installed, "1") || strcmp(configured, "1") ||
		strcmp(conf_dir, app.nginx_conf_dir) || strcmp(main_conf, app.nginx_main_conf)) {
		log_warn("Ignoring invalid or incompatible management state: %s", app.state_file);
		return 0;
	}
	if (!strcmp(ipv6, "1")) app.ipv6_enabled = 1;
	else if (!strcmp(ipv6, "0")) app.ipv6_enabled = 0;
	else {
		log_warn("Ignoring management state with an invalid IPv6 setting: %s", app.state_file);
		return 0;
	}
	app.managed_existing = 1;
	str_copy(app.managed_installed_at, sizeof(app.managed_installed_at), installed_at);
	str_copy(app.managed_last_configured_at, sizeof(app.managed_last_configured_at), configured_at);
	return 1;
}

static void write_management_state(void) {
	if (!app.managed_installed_at[0])
		format_time_utc(app.managed_installed_at, sizeof(app.managed_installed_at));
	format_time_utc(app.managed_last_configured_at, sizeof(app.managed_last_configured_at));
	Buffer b;
	buffer_init(&b);
	buffer_printf(&b,
		"MANAGER_ID=nginx-manager\n"
		"STATE_VERSION=1\n"
		"INSTALLED_BY_SCRIPT=1\n"
		"CONFIGURED_BY_SCRIPT=1\n"
		"IPV6_ENABLED=%d\n"
		"NGINX_CONF_DIR=%s\n"
		"NGINX_MAIN_CONF=%s\n"
		"INSTALLED_AT=%s\n"
		"LAST_CONFIGURED_AT=%s\n",
		app.ipv6_enabled, app.nginx_conf_dir, app.nginx_main_conf,
		app.managed_installed_at, app.managed_last_configured_at);
	make_temp_for(app.state_file);
	if (write_file_mode(app.current_tmp, b.data, b.len, 0600) < 0) {
		buffer_free(&b);
		fatalf("Could not secure the management state temporary file.");
	}
	buffer_free(&b);
	if (path_exists_l(app.state_file)) {
		char backup[PATH_MAX];
		if (backup_name_for(app.state_file, backup, sizeof(backup)) < 0 ||
			copy_file_preserve(app.state_file, backup) < 0)
			fatalf("Could not back up the existing management state.");
		record_change('O', app.state_file, backup);
	} else {
		record_change('N', app.state_file, "");
	}
	if (rename(app.current_tmp, app.state_file) < 0)
		fatalf("Could not atomically install the management state file.");
	app.current_tmp[0] = '\0';
	log_success("Updated management state: %s", app.state_file);
}

static void require_root(void) {
	if (geteuid() != 0) {
		log_error("This script must be run as root. Re-run it with sudo or from a root shell.");
		exit(1);
	}
}

static void prompt_ipv6_support(void) {
	app.ipv6_enabled = confirm("Enable IPv6 support?", 0);
}

static void ensure_directories(void) {
	const char *names[] = {"conf.d", "certs", "snippets", "stream-conf.d", "backups"};
	for (size_t i = 0; i < ARRAY_LEN(names); ++i) {
		char path[PATH_MAX];
		if (str_printf(path, sizeof(path), "%s/%s", app.nginx_conf_dir, names[i]) < 0)
			fatalf("Nginx directory path is too long.");
		if (!path_is_dir(path)) {
			log_info("Creating %s", path);
			if (mkdir_p(path, 0755) < 0)
				fatalf("Command failed: Creating %s", path);
		}
		log_info("Setting permissions on %s", path);
		if (chmod(path, 0755) < 0)
			fatalf("Command failed: Setting permissions on %s", path);
	}
}

static void validate_existing_managed_runtime(void) {
	if (!command_exists(app.nginx_bin))
		fatalf("The managed Nginx executable is unavailable: %s", app.nginx_bin);
	if (!path_is_file(app.nginx_main_conf))
		fatalf("The managed Nginx configuration is missing: %s", app.nginx_main_conf);
}

static void install_packages(void) {
	if (!command_exists("apt"))
		fatalf("The apt command was not found. This script supports Debian/Ubuntu systems.");
	char *update[] = {"apt", "update", NULL};
	run_or_die("Updating APT package indexes", update);
	char *install[] = {"apt", "install", "-y", "nginx", "openssl",
					   "libnginx-mod-stream", NULL};
	run_or_die("Installing Nginx, OpenSSL, and the stream module", install);
	if (!command_exists("ss") && !command_exists("netstat")) {
		log_warn("Neither ss nor netstat is installed; installing iproute2 to provide ss.");
		char *iproute[] = {"apt", "install", "-y", "iproute2", NULL};
		run_or_die("Installing iproute2", iproute);
	}
}

static void remove_default_site(void) {
	char target[PATH_MAX];
	str_printf(target, sizeof(target), "%s/sites-enabled/default", app.nginx_conf_dir);
	if (!path_exists_l(target)) {
		log_info("The default enabled site is already absent.");
		return;
	}
	log_warn("Nginx's default enabled site exists: %s", target);
	if (!confirm("Disable it by moving it into the transaction backup directory?", 0)) {
		(void)unlink(target);
		return;
	}
	char backup[PATH_MAX];
	str_printf(backup, sizeof(backup), "%s/sites-enabled-default", app.backup_dir);
	if (move_path(target, backup) < 0)
		fatalf("Could not disable the default Nginx site.");
	record_change('M', target, backup);
	log_success("Default site disabled and preserved at %s", backup);
}

#endif
