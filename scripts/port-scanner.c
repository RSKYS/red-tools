#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <poll.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// port-scanner.c (Scan ports in the ip list)

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

// Warning: Risk of using this tool is all on you.

#define MAX_QUEUE_CAPACITY 4224
#define MAX_WORKERS 384
#define MIN_QUEUE_CAPACITY 128
#define DEFAULT_PARALLEL 5120
#define FIRST_PORT 80
#define SECND_PORT 443
#define TIMEOUT_SECONDS 5
#define PING_TIMEOUT 5

typedef struct {
	char **items;
	size_t capacity;
	size_t head;
	size_t tail;
	size_t count;
	bool closed;
	pthread_mutex_t mutex;
	pthread_cond_t not_empty;
	pthread_cond_t not_full;
} LineQueue;

typedef struct {
	FILE *out;
	pthread_mutex_t io_mutex;
} Context;

typedef struct {
	char *input;
	char *output;
	bool input_owned;
	bool output_owned;
	int parallel;
} Args;

static void die_error(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(2);
}

static void die_perror(const char *msg)
{
	perror(msg);
	exit(1);
}

static void *xmalloc(size_t size)
{
	void *p = malloc(size);
	if (!p)
		die_perror("error: malloc failure");
	return p;
}

static bool valid_ipv4(const char *ip)
{
	struct in_addr addr;
	return inet_pton(AF_INET, ip, &addr) == 1;
}

static bool file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void trim_in_place(char *s, char **start_out, size_t *len_out)
{
	char *start = s;
	while (*start && isspace((unsigned char)*start)) {
		start++;
	}

	char *end = start + strlen(start);
	while (end > start && isspace((unsigned char)end[-1])) {
		end--;
	}

	*start_out = start;
	*len_out = (size_t)(end - start);
}

static char *prompt_path(const char *msg, bool must_exist)
{
	char *line = NULL;
	size_t size = 0;

	for (;;) {
		printf("%s", msg);
		fflush(stdout);

		if (getline(&line, &size, stdin) < 0) {
			free(line);
			exit(1);
		}

		char *trimmed;
		size_t len;
		trim_in_place(line, &trimmed, &len);

		if (len == 0) {
			puts("Path cannot be empty.");
			continue;
		}

		if (trimmed != line) {
			memmove(line, trimmed, len);
		}
		line[len] = '\0';

		if (must_exist && !file_exists(line)) {
			printf("File not found: %s\n", line);
			continue;
		}

		return line;
	}
}

static Args parse_args(int argc, char **argv)
{
	Args args = {
		.input = NULL,
		.output = NULL,
		.input_owned = false,
		.output_owned = false,
		.parallel = DEFAULT_PARALLEL
	};

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--parallel")) {
			if (++i >= argc)
				die_error("error: missing --parallel value");

			char *endptr;
			long val = strtol(argv[i], &endptr, 10);
			if (*endptr != '\0' || val < 1)
				die_error("error: invalid --parallel numerical value");
			args.parallel = (int)val;
		} else if (!strncmp(argv[i], "--parallel=", 11)) {
			char *endptr;
			long val = strtol(argv[i] + 11, &endptr, 10);
			if (*endptr != '\0' || val < 1)
				die_error("error: invalid --parallel numerical value");
			args.parallel = (int)val;
		} else if (argv[i][0] == '-') {
			die_error("error: unrecognized arguments");
		} else if (!args.input) {
			args.input = argv[i];
		} else if (!args.output) {
			args.output = argv[i];
		} else {
			die_error("error: too many arguments");
		}
	}

	if (args.parallel < 1)
		die_error("error: --parallel must be at least 1");

	if (!args.input) {
		args.input = prompt_path("Input file: ", true);
		args.input_owned = true;
	} else if (!file_exists(args.input)) {
		fprintf(stderr, "error: input file not found: %s\n", args.input);
		exit(2);
	}

	if (!args.output) {
		args.output = prompt_path("Output file: ", false);
		args.output_owned = true;
	}

	return args;
}

static bool ping_reachable(const char *ip)
{
	int fd = open("/dev/null", O_WRONLY);
	if (fd < 0)
		return false;

	pid_t pid = fork();
	if (pid < 0) {
		close(fd);
		return false;
	}

	if (pid == 0) {
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		close(fd);

		char timeout[16];
		snprintf(timeout, sizeof(timeout), "%d", PING_TIMEOUT);

		execlp("ping", "ping", "-c", "1", "-W", timeout, ip, NULL);
		_exit(127);
	}

	close(fd);

	int status = 0;
	if (waitpid(pid, &status, 0) == pid) {
		return WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}

	return false;
}

static bool tcp_port_responds(const char *ip, int port)
{
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		return false;

	int flags = fcntl(sock, F_GETFL, 0);
	if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
		close(sock);
		return false;
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);

	if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
		close(sock);
		return false;
	}

	int result = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
	if (result < 0 && errno != EINPROGRESS) {
		close(sock);
		return false;
	}

	struct pollfd pfd = {
		.fd = sock,
		.events = POLLOUT,
		.revents = 0
	};

	result = poll(&pfd, 1, TIMEOUT_SECONDS * 1000);
	if (result <= 0) {
		close(sock);
		return false;
	}

	int error = 0;
	socklen_t len = sizeof(error);
	if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
		close(sock);
		return false;
	}

	close(sock);
	return error == 0;
}

static const char *check_ip(const char *ip)
{
	if (!valid_ipv4(ip))
		return "SKIP invalid";

	if (!ping_reachable(ip))
		return "FAIL ping";

	if (!tcp_port_responds(ip, FIRST_PORT) || !tcp_port_responds(ip, SECND_PORT))
		return "FAIL PORTS";

	return "OK";
}

static void queue_init(LineQueue *q, size_t capacity)
{
	q->items = xmalloc(capacity * sizeof(char *));
	q->capacity = capacity;
	q->head = 0;
	q->tail = 0;
	q->count = 0;
	q->closed = false;

	if (pthread_mutex_init(&q->mutex, NULL) != 0)
		die_perror("error: pthread_mutex_init");

	if (pthread_cond_init(&q->not_empty, NULL) != 0)
		die_perror("error: pthread_cond_init");

	if (pthread_cond_init(&q->not_full, NULL) != 0)
		die_perror("error: pthread_cond_init");
}

static void queue_destroy(LineQueue *q)
{
	free(q->items);
	pthread_mutex_destroy(&q->mutex);
	pthread_cond_destroy(&q->not_empty);
	pthread_cond_destroy(&q->not_full);
}

static void queue_close(LineQueue *q)
{
	pthread_mutex_lock(&q->mutex);
	q->closed = true;
	pthread_cond_broadcast(&q->not_empty);
	pthread_cond_broadcast(&q->not_full);
	pthread_mutex_unlock(&q->mutex);
}

static bool queue_push(LineQueue *q, char *item)
{
	pthread_mutex_lock(&q->mutex);

	while (q->count == q->capacity && !q->closed) {
		pthread_cond_wait(&q->not_full, &q->mutex);
	}

	if (q->closed) {
		pthread_mutex_unlock(&q->mutex);
		return false;
	}

	q->items[q->tail] = item;
	q->tail = (q->tail + 1) % q->capacity;
	q->count++;

	pthread_cond_signal(&q->not_empty);
	pthread_mutex_unlock(&q->mutex);
	return true;
}

static char *queue_pop(LineQueue *q)
{
	pthread_mutex_lock(&q->mutex);

	while (q->count == 0 && !q->closed) {
		pthread_cond_wait(&q->not_empty, &q->mutex);
	}

	if (q->count == 0 && q->closed) {
		pthread_mutex_unlock(&q->mutex);
		return NULL;
	}

	char *item = q->items[q->head];
	q->head = (q->head + 1) % q->capacity;
	q->count--;

	pthread_cond_signal(&q->not_full);
	pthread_mutex_unlock(&q->mutex);
	return item;
}

static char *normalize_line(char *line)
{
	char *hash = strchr(line, '#');
	if (hash)
		*hash = '\0';

	char *trimmed;
	size_t len;
	trim_in_place(line, &trimmed, &len);

	if (len == 0)
		return NULL;

	char *copy = xmalloc(len + 1);
	memcpy(copy, trimmed, len);
	copy[len] = '\0';
	return copy;
}

static void *worker(void *arg)
{
	struct {
		Context *ctx;
		LineQueue *queue;
	} *bundle = arg;

	Context *ctx = bundle->ctx;
	LineQueue *queue = bundle->queue;

	for (;;) {
		char *ip = queue_pop(queue);
		if (!ip)
			break;

		const char *status = check_ip(ip);

		pthread_mutex_lock(&ctx->io_mutex);
		printf("[%s] %s\n", status, ip);

		if (!strcmp(status, "OK")) {
			fprintf(ctx->out, "%s\n", ip);
			fflush(ctx->out);
		}

		pthread_mutex_unlock(&ctx->io_mutex);

		free(ip);
	}

	return NULL;
}

int main(int argc, char **argv)
{
	Args args = parse_args(argc, argv);

	FILE *in = fopen(args.input, "r");
	if (!in) {
		if (args.input_owned) free(args.input);
		if (args.output_owned) free(args.output);
		die_perror("error: input open failed");
	}

	FILE *out = fopen(args.output, "a");
	if (!out) {
		fclose(in);
		if (args.input_owned) free(args.input);
		if (args.output_owned) free(args.output);
		die_perror("error: output open failed");
	}

	Context ctx;
	ctx.out = out;
	if (pthread_mutex_init(&ctx.io_mutex, NULL) != 0)
		die_perror("error: pthread_mutex_init");

	size_t workers = (size_t)args.parallel;
	if (workers > MAX_WORKERS)
		workers = MAX_WORKERS;
	if (workers < 1)
		workers = 1;

	size_t queue_capacity = workers * 4;
	if (queue_capacity < MIN_QUEUE_CAPACITY)
		queue_capacity = MIN_QUEUE_CAPACITY;
	if (queue_capacity > MAX_QUEUE_CAPACITY)
		queue_capacity = MAX_QUEUE_CAPACITY;

	LineQueue queue;
	queue_init(&queue, queue_capacity);

	printf("Checking input with %zu worker threads...\n\n", workers);

	pthread_t *threads = xmalloc(workers * sizeof(pthread_t));

	struct {
		Context *ctx;
		LineQueue *queue;
	} bundle = { .ctx = &ctx, .queue = &queue };

	pthread_attr_t attr;
	if (pthread_attr_init(&attr) != 0)
		die_perror("error: pthread_attr_init");

	if (pthread_attr_setstacksize(&attr, 256 * 1024) != 0)
		die_perror("error: pthread_attr_setstacksize");

	for (size_t i = 0; i < workers; i++) {
		if (pthread_create(&threads[i], &attr, worker, &bundle) != 0) {
			die_perror("error: thread creation failed");
		}
	}

	pthread_attr_destroy(&attr);

	char *line = NULL;
	size_t line_cap = 0;

	while (getline(&line, &line_cap, in) >= 0) {
		char *ip = normalize_line(line);
		if (!ip)
			continue;

		if (!queue_push(&queue, ip)) {
			free(ip);
			break;
		}
	}

	free(line);
	queue_close(&queue);

	for (size_t i = 0; i < workers; i++) {
		pthread_join(threads[i], NULL);
	}

	free(threads);
	queue_destroy(&queue);

	pthread_mutex_destroy(&ctx.io_mutex);
	fclose(out);
	fclose(in);

	if (args.input_owned)
		free(args.input);
	if (args.output_owned)
		free(args.output);

	return 0;
}
