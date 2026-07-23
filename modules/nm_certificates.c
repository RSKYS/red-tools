#ifndef NM_CERTIFICATES_C
#define NM_CERTIFICATES_C

#ifndef NM_AMALGAMATED_BUILD
#define NM_AMALGAMATED_BUILD 1
#endif

#include "nm_certificates.h"

static void bootstrap_certificates(void) {
	char dir[PATH_MAX], cert[PATH_MAX], key[PATH_MAX];
	str_printf(dir, sizeof(dir), "%s/certs", app.nginx_conf_dir);
	str_printf(cert, sizeof(cert), "%s/cert.pem", dir);
	str_printf(key, sizeof(key), "%s/cert.key", dir);
	if (path_is_file(cert) && path_is_file(key)) {
		log_info("Preserving the existing bootstrap certificate pair in %s.", dir);
		return;
	}
	if (path_exists_l(cert) || path_exists_l(key))
		fatalf("The bootstrap certificate pair is incomplete; refusing to overwrite existing material in %s.", dir);
	if (!command_exists("openssl"))
		fatalf("OpenSSL is required to generate the bootstrap certificate pair.");

	char workspace[PATH_MAX], config[PATH_MAX], staged_cert[PATH_MAX], staged_key[PATH_MAX];
	str_printf(workspace, sizeof(workspace), "%s/bootstrap-certificate", app.backup_dir);
	str_printf(config, sizeof(config), "%s/openssl.cnf", workspace);
	str_printf(staged_cert, sizeof(staged_cert), "%s/cert.pem", workspace);
	str_printf(staged_key, sizeof(staged_key), "%s/cert.key", workspace);
	log_info("Creating the bootstrap-certificate workspace");
	if (mkdir_p(workspace, 0700) < 0)
		fatalf("Command failed: Creating the bootstrap-certificate workspace");
	log_info("Securing the bootstrap-certificate workspace");
	if (chmod(workspace, 0700) < 0)
		fatalf("Command failed: Securing the bootstrap-certificate workspace");
	static const char config_text[] =
		"[req]\n"
		"prompt = no\n"
		"distinguished_name = distinguished_name\n"
		"x509_extensions = extensions\n\n"
		"[distinguished_name]\n"
		"CN = localhost\n\n"
		"[extensions]\n"
		"basicConstraints = critical,CA:FALSE\n"
		"keyUsage = critical,digitalSignature,keyEncipherment\n"
		"extendedKeyUsage = serverAuth\n"
		"subjectAltName = @subject_alt_names\n\n"
		"[subject_alt_names]\n"
		"DNS.1 = localhost\n"
		"IP.1 = 127.0.0.1\n"
		"IP.2 = ::1\n";
	if (write_file_mode(config, config_text, sizeof(config_text) - 1, 0600) < 0)
		fatalf("Could not secure the bootstrap OpenSSL configuration.");
	log_info("Generating a 10-year local bootstrap certificate in %s", dir);
	mode_t oldmask = umask(077);
	char *argv[] = {"openssl", "req", "-x509", "-nodes", "-days", "3650",
					"-sha256", "-newkey", "rsa:2048", "-config", config,
					"-keyout", staged_key, "-out", staged_cert, NULL};
	int rc = run_process(argv, 0, NULL);
	umask(oldmask);
	if (rc != 0) fatalf("Could not generate the bootstrap certificate pair.");
	if (chmod(staged_key, 0600) < 0)
		fatalf("Could not secure the generated bootstrap private key.");
	if (chmod(staged_cert, 0644) < 0)
		fatalf("Could not set permissions on the generated bootstrap certificate.");
	record_change('N', key, "");
	if (move_path(staged_key, key) < 0)
		fatalf("Could not install the generated bootstrap private key.");
	record_change('N', cert, "");
	if (move_path(staged_cert, cert) < 0)
		fatalf("Could not install the generated bootstrap certificate.");
	log_success("Bootstrap certificate installed: %s", cert);
	log_success("Bootstrap private key installed: %s", key);
}

static void bootstrap_certificate_pair(void) {
	char cert[PATH_MAX], key[PATH_MAX];
	str_printf(cert, sizeof(cert), "%s/certs/cert.pem", app.nginx_conf_dir);
	str_printf(key, sizeof(key), "%s/certs/cert.key", app.nginx_conf_dir);
	if (!path_is_file(cert) || !path_is_file(key)) {
		log_warn("The local bootstrap certificate pair is missing; generating it now.");
		bootstrap_certificates();
	}
	if (!path_readable_file(cert))
		fatalf("The bootstrap certificate is unavailable or unreadable: %s", cert);
	if (!path_readable_file(key))
		fatalf("The bootstrap private key is unavailable or unreadable: %s", key);
	str_copy(app.cert_file, sizeof(app.cert_file), cert);
	str_copy(app.key_file, sizeof(app.key_file), key);
	log_success("Using the automatically generated bootstrap certificate: %s", app.cert_file);
	log_success("Using the automatically generated bootstrap private key: %s", app.key_file);
	log_warn("This local bootstrap certificate is self-signed. Replace it with a trusted certificate before exposing the HTTPS node to untrusted clients.");
}

static void select_certificate_paths(void) {
	char pairs[10][2][PATH_MAX];
	size_t count = 0;
#define ADD_PAIR(certfmt, keyfmt, ...) do { \
	str_printf(pairs[count][0], PATH_MAX, certfmt, __VA_ARGS__); \
	str_printf(pairs[count][1], PATH_MAX, keyfmt, __VA_ARGS__); \
	++count; \
} while (0)
	ADD_PAIR("%s/../letsencrypt/live/%s/fullchain.pem", "%s/../letsencrypt/live/%s/privkey.pem", app.nginx_conf_dir, app.domain);
	ADD_PAIR("%s/certs/%s/fullchain.pem", "%s/certs/%s/privkey.pem", app.nginx_conf_dir, app.domain);
	ADD_PAIR("%s/certs/%s.crt", "%s/certs/%s.key", app.nginx_conf_dir, app.domain);
	ADD_PAIR("%s/certs/%s.cer", "%s/certs/%s.key", app.nginx_conf_dir, app.domain);
	ADD_PAIR("/etc/certs/%s/fullchain.pem", "/etc/certs/%s/privkey.pem", app.domain);
	ADD_PAIR("/etc/certs/%s/%s.crt", "/etc/certs/%s/%s.key", app.domain, app.domain);
	ADD_PAIR("/etc/certs/%s/%s.cer", "/etc/certs/%s/%s.key", app.domain, app.domain);
	ADD_PAIR("%s/ssl/%s.crt", "%s/ssl/%s.key", app.nginx_conf_dir, app.domain);
	ADD_PAIR("%s/ssl/%s.cer", "%s/ssl/%s.key", app.nginx_conf_dir, app.domain);
	ADD_PAIR("/etc/ssl/%s/%s.crt", "/etc/ssl/%s/%s.key", app.domain, app.domain);
#undef ADD_PAIR
	intS detected = 0;
	for (size_t i = 0; i < count; ++i) {
		if (!path_readable_file(pairs[i][0]) || !path_readable_file(pairs[i][1])) continue;
		detected = 1;
		log_success("Detected certificate: %s", pairs[i][0]);
		log_success("Detected private key: %s", pairs[i][1]);
		if (confirm("Use these detected certificate files?", 1)) {
			str_copy(app.cert_file, sizeof(app.cert_file), pairs[i][0]);
			str_copy(app.key_file, sizeof(app.key_file), pairs[i][1]);
			return;
		}
		break;
	}
	if (!detected)
		log_warn("No complete certificate/key pair was auto-detected for %s.", app.domain);
	prompt_existing_file("Enter the full SSL certificate/full-chain path",
						 app.cert_file, sizeof(app.cert_file));
	prompt_existing_file("Enter the full SSL private-key path",
						 app.key_file, sizeof(app.key_file));
	if (!strcmp(app.cert_file, app.key_file))
		fatalf("The certificate and private key paths must be different files.");
}

static void validate_certificate_pair(void) {
	if (!command_exists("openssl")) {
		log_warn("OpenSSL is unavailable; certificate structure and key matching will be checked later by nginx -t.");
		return;
	}
	Buffer ignored;
	buffer_init(&ignored);
	char *check_cert[] = {"openssl", "x509", "-in", app.cert_file, "-noout", NULL};
	int rc = run_process(check_cert, 0, &ignored);
	buffer_free(&ignored);
	if (rc != 0)
		fatalf("The selected certificate is not a readable X.509 certificate: %s", app.cert_file);

	Buffer cert_pub, key_pub;
	buffer_init(&cert_pub);
	buffer_init(&key_pub);
	char *cert_key[] = {"openssl", "x509", "-in", app.cert_file, "-pubkey", "-noout", NULL};
	if (run_process(cert_key, 0, &cert_pub) != 0) {
		buffer_free(&cert_pub); buffer_free(&key_pub);
		fatalf("Could not extract the public key from %s.", app.cert_file);
	}
	char *private_key[] = {"openssl", "pkey", "-in", app.key_file, "-pubout",
						   "-outform", "PEM", "-passin", "pass:", NULL};
	if (run_process(private_key, 0, &key_pub) != 0) {
		buffer_free(&cert_pub); buffer_free(&key_pub);
		fatalf("The private key is invalid, unreadable, or encrypted: %s", app.key_file);
	}
	if (cert_pub.len != key_pub.len ||
		memcmp(cert_pub.data, key_pub.data, cert_pub.len) != 0) {
		buffer_free(&cert_pub); buffer_free(&key_pub);
		fatalf("The certificate and private key do not match.");
	}
	buffer_free(&cert_pub);
	buffer_free(&key_pub);

	Buffer expiry;
	buffer_init(&expiry);
	char *checkend[] = {"openssl", "x509", "-in", app.cert_file,
						"-checkend", "604800", "-noout", NULL};
	rc = run_process(checkend, 0, &expiry);
	buffer_free(&expiry);
	if (rc != 0) {
		log_warn("The selected certificate is expired or will expire within seven days.");
		if (!confirm("Continue with this certificate anyway?", 0))
			fatalf("Stopped because the certificate is expired or near expiry.");
	}
	log_success("Certificate and private key validation passed.");
}

static void select_ca_certificate(void) {
	app.ca_file[0] = app.ssl_certificate_file[0] = app.generated_chain_file[0] = '\0';
	char cert_dir[PATH_MAX];
	parent_dir(app.cert_file, cert_dir, sizeof(cert_dir));
	char candidates[6][PATH_MAX];
	str_printf(candidates[0], PATH_MAX, "%s/%s.ca", cert_dir, app.domain);
	str_printf(candidates[1], PATH_MAX, "%s/certs/%s.ca", app.nginx_conf_dir, app.domain);
	str_printf(candidates[2], PATH_MAX, "/etc/certs/%s/%s.ca", app.domain, app.domain);
	str_printf(candidates[3], PATH_MAX, "/etc/certs/%s.ca", app.domain);
	str_printf(candidates[4], PATH_MAX, "%s/ssl/%s.ca", app.nginx_conf_dir, app.domain);
	str_printf(candidates[5], PATH_MAX, "/etc/ssl/%s/%s.ca", app.domain, app.domain);
	const char *found = NULL;
	for (size_t i = 0; i < ARRAY_LEN(candidates); ++i) {
		intS duplicate = 0;
		for (size_t j = 0; j < i; ++j)
			if (!strcmp(candidates[i], candidates[j])) duplicate = 1;
		if (!duplicate && path_readable_file(candidates[i])) { found = candidates[i]; break; }
	}
	if (found) {
		log_success("Detected CA/intermediate certificate: %s", found);
		if (confirm("Use this detected CA/intermediate certificate?", 1))
			str_copy(app.ca_file, sizeof(app.ca_file), found);
		else if (confirm("Select a different CA/intermediate certificate?", 0))
			prompt_existing_file("Enter the full CA/intermediate certificate path",
								 app.ca_file, sizeof(app.ca_file));
		else
			log_info("No CA/intermediate certificate will be configured for this HTTPS node.");
	} else if (confirm("Add an optional CA/intermediate certificate for this HTTPS node?", 0)) {
		prompt_existing_file("Enter the full CA/intermediate certificate path",
							 app.ca_file, sizeof(app.ca_file));
	} else {
		log_info("No CA/intermediate certificate will be configured for this HTTPS node.");
	}
	if (app.ca_file[0] && !strcmp(app.ca_file, app.cert_file))
		fatalf("The CA/intermediate certificate must be separate from the leaf certificate file.");
	if (app.ca_file[0] && !strcmp(app.ca_file, app.key_file))
		fatalf("The CA/intermediate certificate path cannot be the private key file.");
}

static void validate_ca_certificate(void) {
	if (!app.ca_file[0]) return;
	if (!command_exists("openssl"))
		fatalf("OpenSSL is required to validate the selected CA/intermediate certificate.");
	if (!file_contains_literal(app.ca_file, "-----BEGIN CERTIFICATE-----"))
		fatalf("The selected CA/intermediate file is not a valid PEM certificate bundle: %s", app.ca_file);
	Buffer ignored;
	buffer_init(&ignored);
	char *argv[] = {"openssl", "crl2pkcs7", "-nocrl", "-certfile",
					app.ca_file, "-outform", "PEM", NULL};
	int rc = run_process(argv, 0, &ignored);
	buffer_free(&ignored);
	if (rc != 0)
		fatalf("The selected CA/intermediate file is not a valid PEM certificate bundle: %s", app.ca_file);
	log_success("CA/intermediate certificate validation passed: %s", app.ca_file);
}

static void configure_ca_certificate(unsigned https_port) {
	select_ca_certificate();
	str_copy(app.ssl_certificate_file, sizeof(app.ssl_certificate_file), app.cert_file);
	if (app.ca_file[0]) {
		validate_ca_certificate();
		log_success("The selected certificate path will be used unchanged for HTTPS port %u: %s",
					https_port, app.cert_file);
	}
}

#endif
