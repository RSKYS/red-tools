#ifndef NM_PROCESS_C
#define NM_PROCESS_C

#ifndef NM_AMALGAMATED_BUILD
#define NM_AMALGAMATED_BUILD 1
#endif

#include "nm_process.h"

extern char **environ;

static intS executable_file(const char *path) {
	if (!path || !*path) return 0;
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

static intS command_path(const char *name, char *out, size_t cap) {
	if (out && cap != 0) out[0] = '\0';
	if (!name || !*name || !out || cap == 0) {
		errno = EINVAL;
		return 0;
	}
	if (strchr(name, '/')) {
		if (!executable_file(name)) return 0;
		return str_copy(out, cap, name) == 0;
	}
	const char *path = env_default(
		"PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
	const char *p = path;
	while (*p) {
		const char *colon = strchr(p, ':');
		size_t n = colon ? (size_t)(colon - p) : strlen(p);
		char candidate[PATH_MAX];
		if (n == 0) {
			if (str_printf(candidate, sizeof(candidate), "./%s", name) < 0) return 0;
		} else if (n > SIZE_MAX - strlen(name) - 2 ||
				   n + 1 + strlen(name) >= sizeof(candidate)) {
			if (!colon) break;
			p = colon + 1;
			continue;
		} else {
			memcpy(candidate, p, n);
			candidate[n] = '/';
			memcpy(candidate + n + 1, name, strlen(name) + 1);
		}
		if (executable_file(candidate)) return str_copy(out, cap, candidate) == 0;
		if (!colon) break;
		p = colon + 1;
	}
	return 0;
}

static intS command_exists(const char *name) {
	if (!name || !*name) return 0;
	char path[PATH_MAX];
	return command_path(name, path, sizeof(path));
}

static int run_process(char *const argv[], intS quiet, Buffer *capture) {
	if (!argv || !argv[0] || !*argv[0]) {
		errno = EINVAL;
		return -1;
	}
	check_interrupted();

	int pipefd[2] = {-1, -1};
	if (capture && pipe(pipefd) < 0) return -1;
	pid_t pid = fork();
	if (pid < 0) {
		if (capture) {
			NM_CLOSE_FD(pipefd[0]);
			NM_CLOSE_FD(pipefd[1]);
		}
		return -1;
	}
	if (pid == 0) {
		if (capture) {
			(void)close(pipefd[0]);
			if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
				dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
			(void)close(pipefd[1]);
		} else if (quiet) {
			int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
			if (fd >= 0) {
				if (dup2(fd, STDOUT_FILENO) < 0 ||
					dup2(fd, STDERR_FILENO) < 0) _exit(126);
				if (fd > STDERR_FILENO) (void)close(fd);
			}
		}
		execvp(argv[0], argv);
		_exit(errno == ENOENT ? 127 : 126);
	}

	intS capture_failed = 0;
	if (capture) {
		(void)close(pipefd[1]);
		pipefd[1] = -1;
		char chunk[4096];
		for (;;) {
			ssize_t n = read(pipefd[0], chunk, sizeof(chunk));
			if (n > 0) {
				if (!capture_failed &&
					buffer_append_n(capture, chunk, (size_t)n) < 0)
					capture_failed = 1;
				continue;
			}
			if (n == 0) break;
			if (errno == EINTR) {
				if (nm_pending_signal) {
					(void)kill(pid, SIGTERM);
					break;
				}
				continue;
			}
			capture_failed = 1;
			break;
		}
		NM_CLOSE_FD(pipefd[0]);
	}

	int status = 0;
	for (;;) {
		pid_t waited = waitpid(pid, &status, 0);
		if (waited == pid) break;
		if (waited < 0 && errno == EINTR) {
			if (nm_pending_signal) (void)kill(pid, SIGTERM);
			continue;
		}
		return -1;
	}

	check_interrupted();
	if (capture_failed) return -1;
	if (WIFEXITED(status)) return WEXITSTATUS(status);
	if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
	return -1;
}

static void run_or_die(const char *description, char *const argv[]) {
	log_info("%s", description);
	if (run_process(argv, 0, NULL) != 0)
		fatalf("Command failed: %s", description);
}

static intS read_proc_listen_table(const char *path, unsigned port, StrVec *inodes) {
	FILE *fp = fopen(path, "r");
	if (!fp) return 0;
	char line[1024];
	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return 0;
	}
	intS found = 0;
	while (fgets(line, sizeof(line), fp)) {
		char local[128], remote[128], state[8], inode[64];
		unsigned long ignored[7];
		int count = sscanf(line,
			" %*d: %127s %127s %7s %lx:%lx %lx:%lx %lx %lu %lu %63s",
			local, remote, state, &ignored[0], &ignored[1], &ignored[2],
			&ignored[3], &ignored[4], &ignored[5], &ignored[6], inode);
		if (count < 10 || strcmp(state, "0A") != 0) continue;
		char *colon = strrchr(local, ':');
		if (!colon) continue;
		unsigned long p = strtoul(colon + 1, NULL, 16);
		if (p == port) {
			found = 1;
			if (inodes && !strvec_contains(inodes, inode)) strvec_push(inodes, inode);
		}
	}
	fclose(fp);
	return found;
}

static intS port_in_use(unsigned port) {
	return read_proc_listen_table("/proc/net/tcp", port, NULL) ||
		   read_proc_listen_table("/proc/net/tcp6", port, NULL);
}

static intS pid_is_nginx(const char *pid_name) {
	if (!pid_name || !*pid_name) return 0;
	for (const char *p = pid_name; *p; ++p)
		if (!isdigit((unsigned char)*p)) return 0;
	char path[PATH_MAX], buf[512];
	if (str_printf(path, sizeof(path), "/proc/%s/comm", pid_name) < 0) return 0;
	FILE *fp = fopen(path, "r");
	if (fp) {
		intS ok = fgets(buf, sizeof(buf), fp) != NULL && starts_with(buf, "nginx");
		fclose(fp);
		if (ok) return 1;
	}
	if (str_printf(path, sizeof(path), "/proc/%s/cmdline", pid_name) < 0) return 0;
	fp = fopen(path, "rb");
	if (!fp) return 0;
	size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
	intS failed = ferror(fp) != 0;
	fclose(fp);
	if (failed) return 0;
	buf[n] = '\0';
	return strstr(buf, "nginx") != NULL;
}

static intS port_owned_by_nginx(unsigned port) {
	StrVec inodes;
	strvec_init(&inodes);
	read_proc_listen_table("/proc/net/tcp", port, &inodes);
	read_proc_listen_table("/proc/net/tcp6", port, &inodes);
	if (!inodes.len) {
		strvec_free(&inodes);
		return 0;
	}
	DIR *proc = opendir("/proc");
	if (!proc) {
		strvec_free(&inodes);
		return 0;
	}
	struct dirent *de;
	intS found = 0;
	while (!found && (de = readdir(proc))) {
		if (!pid_is_nginx(de->d_name)) continue;
		char fd_dir[PATH_MAX];
		if (str_printf(fd_dir, sizeof(fd_dir), "/proc/%s/fd", de->d_name) < 0) continue;
		DIR *fds = opendir(fd_dir);
		if (!fds) continue;
		struct dirent *fde;
		while (!found && (fde = readdir(fds))) {
			if (fde->d_name[0] == '.') continue;
			char linkpath[PATH_MAX], target[256];
			if (str_printf(linkpath, sizeof(linkpath), "%s/%s", fd_dir, fde->d_name) < 0) continue;
			ssize_t n = readlink(linkpath, target, sizeof(target) - 1);
			if (n < 0) continue;
			target[n] = '\0';
			if (!starts_with(target, "socket:[") || !ends_with(target, "]")) continue;
			target[n - 1] = '\0';
			const char *inode = target + 8;
			if (strvec_contains(&inodes, inode)) found = 1;
		}
		closedir(fds);
	}
	closedir(proc);
	strvec_free(&inodes);
	return found;
}

static uint32_t random_u32(void) {
	uint32_t value = 0;
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		unsigned char *cursor = (unsigned char *)&value;
		size_t remaining = sizeof(value);
		while (remaining != 0) {
			ssize_t n = read(fd, cursor, remaining);
			if (n > 0) {
				cursor += (size_t)n;
				remaining -= (size_t)n;
			} else if (n < 0 && errno == EINTR) {
				continue;
			} else {
				break;
			}
		}
		(void)close(fd);
		if (remaining == 0) return value;
	}
	struct timespec ts = {0, 0};
	(void)clock_gettime(CLOCK_REALTIME, &ts);
	uint64_t mix = (uint64_t)ts.tv_nsec ^ (uint64_t)ts.tv_sec ^
				   (uint64_t)(uintptr_t)&value ^ (uint64_t)(unsigned)getpid();
	value = (uint32_t)(mix ^ (mix >> 32));
	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	return value;
}

#endif
