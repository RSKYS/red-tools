#ifndef NM_TRANSACTION_C
#define NM_TRANSACTION_C

#ifndef NM_AMALGAMATED_BUILD
#define NM_AMALGAMATED_BUILD 1
#endif

#include "nm_transaction.h"

static volatile sig_atomic_t nm_pending_signal;

static void rollback_transaction(void) {
	if (!app.transaction_active || app.rollback_done) return;
	app.rollback_done = 1;
	log_warn("Rolling back managed configuration changes...");
	FILE *fp = fopen(app.manifest, "r");
	if (fp) {
		char line[PATH_MAX * 2 + 16];
		while (fgets(line, sizeof(line), fp)) {
			char *nl = strchr(line, '\n');
			if (nl) *nl = '\0';
			char *first = strchr(line, '|');
			if (!first) continue;
			*first++ = '\0';
			char *second = strchr(first, '|');
			if (!second) continue;
			*second++ = '\0';
			char type = line[0];
			const char *target = first;
			const char *backup = second;
			if (type == 'O') {
				if (path_exists_l(backup)) {
					(void)unlink(target);
					if (copy_file_preserve(backup, target) == 0)
						log_warn("Restored: %s", target);
					else
						log_error("Failed to restore: %s", target);
				}
			} else if (type == 'N') {
				if (unlink(target) == 0 || errno == ENOENT)
					log_warn("Removed newly created file: %s", target);
				else
					log_error("Failed to remove new file: %s", target);
			} else if (type == 'M') {
				if (path_exists_l(backup)) {
					(void)unlink(target);
					if (move_path(backup, target) == 0)
						log_warn("Restored moved item: %s", target);
					else
						log_error("Failed to restore moved item: %s", target);
				}
			} else if (type == 'X') {
				log_error("Cannot automatically restore unbacked file: %s", target);
			}
		}
		fclose(fp);
	}
	app.transaction_active = 0;
}

static void signal_handler(int signo) {
	static const char text[] = "\n\033[31m\033[1m[ERROR]\033[0m Interrupted.\n";
	nm_pending_signal = signo;
	ssize_t ignored = write(STDERR_FILENO, text, sizeof(text) - 1);
	(void)ignored;
}

static void check_interrupted(void) {
	if (!nm_pending_signal) return;
	sigset_t blocked, previous;
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGHUP);
	sigaddset(&blocked, SIGINT);
	sigaddset(&blocked, SIGTERM);
	(void)sigprocmask(SIG_BLOCK, &blocked, &previous);
	nm_pending_signal = 0;
	cleanup_temp();
	cleanup_validation_dir();
	rollback_transaction();
	(void)sigprocmask(SIG_SETMASK, &previous, NULL);
	exit(130);
}

static void install_signal_handlers(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGHUP, &sa, NULL) < 0 ||
		sigaction(SIGINT, &sa, NULL) < 0 ||
		sigaction(SIGTERM, &sa, NULL) < 0)
		fatalf("Could not install signal handlers: %s", strerror(errno));
}

static void format_time_utc(char *out, size_t cap) {
	time_t now = time(NULL);
	struct tm tmv;
	gmtime_r(&now, &tmv);
	strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static void init_transaction(void) {
	time_t now = time(NULL);
	struct tm tmv;
	localtime_r(&now, &tmv);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
	if (str_printf(app.backup_dir, sizeof(app.backup_dir),
				   "%s/backups/reverse-proxy-%s", app.nginx_conf_dir, stamp) < 0 ||
		str_printf(app.manifest, sizeof(app.manifest), "%s/manifest.txt", app.backup_dir) < 0)
		fatalf("Could not initialize transaction paths.");
	log_info("Creating transaction backup directory");
	if (mkdir_p(app.backup_dir, 0700) < 0)
		fatalf("Command failed: Creating transaction backup directory");
	log_info("Securing transaction backup directory");
	if (chmod(app.backup_dir, 0700) < 0)
		fatalf("Command failed: Securing transaction backup directory");
	if (write_file_mode(app.manifest, "", 0, 0600) < 0)
		fatalf("Could not create %s.", app.manifest);
	app.rollback_done = 0;
	app.transaction_active = 1;
}

#endif
