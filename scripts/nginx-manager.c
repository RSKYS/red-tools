#define NM_AMALGAMATED_BUILD 1

#include "../modules/nm_allfather.h"
#include "../modules/nm_certificates.c"
#include "../modules/nm_core.c"
#include "../modules/nm_filesystem.c"
#include "../modules/nm_generators.c"
#include "../modules/nm_manage.c"
#include "../modules/nm_nginx_parser.c"
#include "../modules/nm_process.c"
#include "../modules/nm_proxy.c"
#include "../modules/nm_runtime.c"
#include "../modules/nm_transaction.c"
#include "../modules/nm_validators.c"

// nginx-manager.c

// Copyright 2026 Pouria Rezaei <Pouria.rz@outlook.com>
// All rights reserved.

// Redistribution and use of this script, with or without modification, is
// permitted provided that the following conditions are met:

// 1. Redistributions of this script must retain the above copyright
//    notice, this list of conditions and the following disclaimer.

//  THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR IMPLIED
//  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
//  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
//  EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
//  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
//  OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
//  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
//  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
//  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

static void prompt_raw_tls_port(void) {
	char input[64];
	for (;;) {
		printf("Desired raw TLS backend port [3000]: ");
		fflush(stdout);
		if (!read_input(input, sizeof(input)))
			fatalf("Input ended before a raw TLS backend port was provided.");
		if (!*input) app.backend_port = 3000;
		else if (!parse_port(input, &app.backend_port)) {
			log_warn("Enter a numeric TCP port from 1 through 65535.");
			continue;
		}
		log_success("Selected raw TLS backend port: %u", app.backend_port);
		return;
	}
}

static void configure_domain_mode(void) {
	strcpy(app.mode, "domain");
	prompt_domain("Enter the public domain to route on ports 80 and 443");
	if (confirm("Use Nginx-managed certificates?", 1)) {
		app.tls_passthrough = 0;
		select_certificate_paths();
		validate_certificate_pair();
		configure_ca_certificate(443);
		client_max_body_size(443);
	} else {
		client_max_body_size(443);
		if (app.body_size_mb) {
			app.tls_passthrough = 0;
			bootstrap_certificate_pair();
			validate_certificate_pair();
			configure_ca_certificate(443);
			log_info("The custom client_max_body_size will be enforced by an internal Nginx HTTPS node on a randomly selected loopback port.");
		} else {
			app.tls_passthrough = 1;
			prompt_raw_tls_port();
		}
	}
	if (!app.tls_passthrough) {
		find_random_port(10000, 19999);
		app.tls_internal_port = app.selected_free_port;
		app.selected_free_port = 0;
		log_success("Selected internal TLS listener port: %u", app.tls_internal_port);
		choose_proxy_layout(443);
	}
	ensure_stream_include();
	write_domain_redirect_80();
	write_stream_sni_config(443);
	if (!app.tls_passthrough) {
		char token[128], target[PATH_MAX], listen[128];
		bounded_domain_token(app.domain, 80, token, sizeof(token));
		str_printf(target, sizeof(target), "%s/conf.d/reverse_%s.conf",
				   app.nginx_conf_dir, token);
		str_printf(listen, sizeof(listen), "listen 127.0.0.1:%u ssl;",
				   app.tls_internal_port);
		while (!write_https_site_config(listen, target)) {
			log_warn("The HTTPS virtual-host candidate was rejected by nginx -t.");
			if (!confirm("Re-enter the proxy layout and location parameters?", 1))
				fatalf("Stopped without installing an invalid HTTPS virtual-host configuration.");
			choose_proxy_layout(443);
		}
		log_success("The HTTPS virtual-host configuration passed validation and was installed.");
	}
	app.location_fragment[0] = app.upstream_fragment[0] = '\0';
}

static void configure_custom_port_mode(void) {
	strcpy(app.mode, "custom");
	prompt_port("Custom HTTP redirect port", 2080, &app.http_port);
	for (;;) {
		prompt_port("Custom HTTPS reverse-proxy port", 2443, &app.https_port);
		if (app.https_port == app.http_port) {
			log_warn("The HTTP and HTTPS ports must differ.");
			app.https_port = 0;
			continue;
		}
		break;
	}
	prompt_domain("Enter the domain clients will use on these custom ports");
	if (confirm("Use Nginx-managed certificates?", 1)) {
		app.tls_passthrough = 0;
		select_certificate_paths();
		validate_certificate_pair();
		configure_ca_certificate(app.https_port);
		client_max_body_size(app.https_port);
	} else {
		client_max_body_size(app.https_port);
		if (app.body_size_mb) {
			app.tls_passthrough = 0;
			bootstrap_certificate_pair();
			validate_certificate_pair();
			configure_ca_certificate(app.https_port);
			log_info("The custom client_max_body_size will be enforced by an Nginx HTTPS reverse-proxy node on port %u.",
					 app.https_port);
		} else {
			app.tls_passthrough = 1;
		}
	}
	if (!app.tls_passthrough) {
		choose_proxy_layout(app.https_port);
		char token[128], target[PATH_MAX];
		bounded_domain_token(app.domain, 72, token, sizeof(token));
		str_printf(target, sizeof(target), "%s/conf.d/proxy_%s_%u_%u.conf",
				   app.nginx_conf_dir, token, app.http_port, app.https_port);
		while (!write_custom_config(target)) {
			log_warn("The custom-port HTTP/HTTPS candidate was rejected by nginx -t.");
			if (!confirm("Re-enter the proxy layout and location parameters?", 1))
				fatalf("Stopped without installing an invalid custom-port configuration.");
			choose_proxy_layout(app.https_port);
		}
		log_success("The custom-port HTTP/HTTPS configuration passed validation and was installed.");
		app.location_fragment[0] = app.upstream_fragment[0] = '\0';
	} else {
		prompt_raw_tls_port();
		ensure_stream_include();
		write_forced_ssl_snippet(app.https_port);
		write_custom_redirect_config();
		write_stream_sni_config(app.https_port);
	}
}

static void add_new_configuration(void) {
	printf("\n\033[1mAdd new configuration\033[0m\n");
	printf("  1) Public ports 80 and 443 with domain and SNI routing\n");
	printf("  2) Isolated custom HTTP and HTTPS ports\n");
	char input[64];
	for (;;) {
		printf("Select the new configuration type [1-2]: ");
		fflush(stdout);
		if (!read_input(input, sizeof(input)))
			fatalf("Input ended before a new configuration type was selected.");
		if (!strcmp(input, "1")) { configure_domain_mode(); return; }
		if (!strcmp(input, "2")) {
			write_redirect_80();
			configure_custom_port_mode();
			return;
		}
		log_warn("Enter 1 or 2.");
	}
}

static void choose_next_action(void) {
	if (!app.managed_existing || !has_managed_configurations()) {
		if (app.managed_existing)
			log_warn("Management state exists, but no script-managed listener or upstream configuration files were found.");
		add_new_configuration();
		return;
	}
	char input[64];
	for (;;) {
		printf("Choose what to do next:\n\n"
			   "  1) Manage existing configurations — select a listener, server, or upstream shown above to modify its port or remove that node\n"
			   "  2) Add new configuration — create a new public 80/443 or custom-port setup without changing an existing node\n\n");
		printf("Select an action [1-2]: ");
		fflush(stdout);
		if (!read_input(input, sizeof(input)))
			fatalf("Input ended before the next action was selected.");
		if (!strcmp(input, "1")) {
			if (manage_existing_confs()) return;
			putchar('\n');
		} else if (!strcmp(input, "2")) {
			add_new_configuration();
			return;
		} else {
			log_warn("Enter 1 or 2.");
		}
	}
}

static int nginx_test(Buffer *output) {
	char *argv[] = {app.nginx_bin, "-t", "-c", app.nginx_main_conf, NULL};
	return run_process(argv, 0, output);
}

static void test_nginx_conf(void) {
	log_info("Testing the complete Nginx configuration...");
	Buffer output;
	buffer_init(&output);
	if (nginx_test(&output) == 0) {
		print_captured(&output, stdout);
		log_success("nginx -t completed successfully.");
		buffer_free(&output);
		return;
	}
	print_captured(&output, stderr);
	buffer_free(&output);
	log_error("The generated Nginx configuration is invalid.");
	rollback_transaction();
	log_info("Testing the restored configuration...");
	buffer_init(&output);
	if (nginx_test(&output) == 0) {
		print_captured(&output, stdout);
		log_success("Rollback restored a valid Nginx configuration.");
	} else {
		print_captured(&output, stderr);
		log_error("The restored configuration also fails nginx -t; inspect existing Nginx files manually.");
	}
	buffer_free(&output);
	exit(1);
}

static void restart_nginx(void) {
	log_info("Restarting Nginx...");
	intS restarted = 0;
	if (command_exists("systemctl")) {
		char *restart[] = {"systemctl", "restart", "nginx", NULL};
		if (run_process(restart, 0, NULL) == 0) {
			restarted = 1;
			log_success("Nginx restarted successfully with systemctl.");
			putchar('\n');
			char *status[] = {"systemctl", "--no-pager", "--full", "status", "nginx", NULL};
			(void)run_process(status, 0, NULL);
		}
	}
	if (!restarted && command_exists("service")) {
		char *restart[] = {"service", "nginx", "restart", NULL};
		if (run_process(restart, 0, NULL) == 0) {
			restarted = 1;
			log_success("Nginx restarted successfully with service.");
			putchar('\n');
			char *status[] = {"service", "nginx", "status", NULL};
			(void)run_process(status, 0, NULL);
		}
	}
	if (restarted) return;
	if (!command_exists("systemctl") && !command_exists("service"))
		fatalf("Neither systemctl nor service is available to restart Nginx.");
	log_error("Nginx failed to restart; rolling back managed changes.");
	rollback_transaction();
	Buffer output;
	buffer_init(&output);
	if (nginx_test(&output) == 0) {
		if (command_exists("systemctl")) {
			char *restart[] = {"systemctl", "restart", "nginx", NULL};
			(void)run_process(restart, 1, NULL);
		} else if (command_exists("service")) {
			char *restart[] = {"service", "nginx", "restart", NULL};
			(void)run_process(restart, 1, NULL);
		}
	}
	buffer_free(&output);
	fatalf("Nginx restart failed. Previous managed files were restored where backups were available.");
}

static void print_summary(void) {
	printf("\n\033[1mConfiguration summary\033[0m\n");
	if (!strcmp(app.mode, "manage")) {
		printf("  %-22s %s\n", "Mode:", "Manage existing configurations");
		printf("  %-22s %s\n", "Action:", app.managed_node_action);
		printf("  %-22s %s\n", "Managed node:", app.managed_node_description);
		printf("  %-22s %s\n\n", "Transaction backups:", app.backup_dir);
		return;
	}
	printf("  %-22s %s\n", "Mode:", app.mode);
	printf("  %-22s %s\n", "domain:", app.domain);
	if (app.tls_passthrough) {
		printf("  %-22s %s\n", "TLS handling:", "Raw TLS passthrough");
		printf("  %-22s %s\n", "Client body limit:",
			   "Backend-controlled (not available in Nginx stream context)");
	} else {
		char bootstrap_cert[PATH_MAX], bootstrap_key[PATH_MAX];
		str_printf(bootstrap_cert, sizeof(bootstrap_cert), "%s/certs/cert.pem", app.nginx_conf_dir);
		str_printf(bootstrap_key, sizeof(bootstrap_key), "%s/certs/cert.key", app.nginx_conf_dir);
		if (!strcmp(app.cert_file, bootstrap_cert) && !strcmp(app.key_file, bootstrap_key))
			printf("  %-22s %s\n", "TLS handling:",
				   "Nginx termination with bootstrap certificate");
		else
			printf("  %-22s %s\n", "TLS handling:", "Nginx-managed certificates");
		if (app.body_size_mb)
			printf("  %-22s %u MB\n", "Client body limit:", app.body_size_mb);
		else
			printf("  %-22s %s\n", "Client body limit:", "Nginx default");
		printf("  %-22s %s\n", "Certificate:", app.cert_file);
		printf("  %-22s %s\n", "Private key:", app.key_file);
		printf("  %-22s %s\n", "CA/intermediate:",
			   app.ca_file[0] ? app.ca_file : "Not configured");
		printf("  %-22s %s\n", "Site configuration:", app.site_config);
	}
	if (!strcmp(app.mode, "domain")) {
		printf("  %-22s %u\n", "Public HTTP:", 80U);
		printf("  %-22s %u\n", "Public HTTPS/SNI:", 443U);
		if (!app.tls_passthrough)
			printf("  %-22s %u\n", "Internal TLS port:", app.tls_internal_port);
		if (app.http_redirect_config[0])
			printf("  %-22s %s\n", "HTTP redirect config:", app.http_redirect_config);
		printf("  %-22s %s\n", "Stream configuration:", app.stream_config);
	} else {
		printf("  %-22s %u\n", "Custom HTTP port:", app.http_port);
		printf("  %-22s %u\n", "Custom HTTPS port:", app.https_port);
		if (app.tls_passthrough) {
			printf("  %-22s %s\n", "Forced-SSL snippet:", app.redirect_snippet);
			printf("  %-22s %s\n", "HTTP redirect config:", app.http_redirect_config);
			printf("  %-22s %s\n", "Stream configuration:", app.stream_config);
		}
	}
	if (app.backend_port) {
		if (app.tls_passthrough) {
			printf("  %-22s tls://127.0.0.1:%u\n", "Selected backend:", app.backend_port);
			log_warn("Start your TLS backend service on 127.0.0.1:%u before sending production traffic.",
					 app.backend_port);
		} else {
			printf("  %-22s http://127.0.0.1:%u\n", "Selected backend:", app.backend_port);
			log_warn("Start your backend service on 127.0.0.1:%u before sending production traffic.",
					 app.backend_port);
		}
	}
	printf("  %-22s %s\n\n", "Transaction backups:", app.backup_dir);
}

static void run_manager(void) {
	require_root();
	memset(&app, 0, sizeof(app));
	initialize_runtime_paths();
	if (load_management_state()) {
		printf("\033[1mNginx Reverse-Proxy Manager\033[0m\n");
		log_success("Detected an existing Nginx instance managed by this script.");
		validate_existing_managed_runtime();
		ensure_directories();
		init_transaction();
		ensure_support_files();
	} else {
		app.managed_existing = 0;
		prompt_ipv6_support();
		printf("\033[1mNginx Reverse-Proxy Manager\033[0m\n");
		printf("This installer will install packages and modify Nginx configuration files.\n\n");
		if (!confirm("Proceed with package installation and transactional Nginx changes?", 1)) {
			log_warn("No changes were made.");
			exit(0);
		}
		install_packages();
		ensure_directories();
		init_transaction();
		remove_default_site();
		bootstrap_certificates();
		write_redirect_snippet();
	}
	display_inventory();
	choose_next_action();
	write_management_state();
	test_nginx_conf();
	restart_nginx();
	app.transaction_active = 0;
	cleanup_temp();
	cleanup_validation_dir();
	print_summary();
	log_success("Nginx reverse-proxy configuration completed.");
}

static void print_help(const char *program) {
	printf(
		"Nginx Reverse-Proxy Manager %s\n\n"
		"Usage:\n"
		"  %s\n"
		"  %s --help\n"
		"  %s --version\n\n"
		"The no-option form starts the interactive manager. It installs or manages\n"
		"Nginx on Debian/Ubuntu, creates transactional backups, configures public\n"
		"80/443 SNI routing or isolated custom ports, supports certificate\n"
		"termination and raw TLS passthrough, creates static/proxy/WebSocket/custom\n"
		"locations, validates every candidate with nginx -t, and rolls back failed\n"
		"changes.\n\n"
		"Options:\n"
		"  -h, --help      Show this help text and compilation instructions.\n"
		"  -V, --version   Show the program version.\n\n"
		"Environment:\n"
		"  NGINX_CONF_DIR          Nginx configuration directory [/etc/nginx]\n"
		"  NGINX_MAIN_CONF         Main configuration [NGINX_CONF_DIR/nginx.conf]\n"
		"  NGINX_BIN               Nginx executable [nginx]\n"
		"  NGINX_RPM_STATE_FILE    Manager state [NGINX_CONF_DIR/.nginx-manager.conf]\n"
		"  NGINX_RPM_NO_MAIN=1     Exit without starting the manager\n\n"
		"Compilation (run from the project root):\n"
		"  gcc scripts/nginx-manager.c -o nginx-manager\n\n"
		"Recommended warning-enabled compilation:\n"
		"  gcc -std=c11 -O2 -Wall -Wextra -Wpedantic scripts/nginx-manager.c -o nginx-manager\n",
		VERSION, program, program, program);
}

int main(int argc, char **argv) {
	install_signal_handlers();
	if (!strcmp(env_default("NGINX_RPM_NO_MAIN", "0"), "1")) return 0;
	if (argc == 1) {
		run_manager();
		return 0;
	}
	if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h") ||
					  !strcmp(argv[1], "help"))) {
		print_help(argv[0]);
		return 0;
	}
	if (argc == 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
		printf("nm %s\n", VERSION);
		return 0;
	}
	fprintf(stderr, "Unknown option: %s\nTry '%s --help' for usage.\n",
			argv[1], argv[0]);
	return 2;
}
