#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// atomic-merger.c (Subnet regulation with advanced calculation)

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

typedef struct {
	uint32_t start;
	uint32_t end;
} IPRange;

typedef struct {
	IPRange *items;
	size_t count;
	size_t capacity;
} RangeList;

typedef struct {
	char **items;
	size_t count;
	size_t capacity;
} StringList;

static void die(const char *message) {
	fprintf(stderr, "Error: %s\n", message);
	exit(EXIT_FAILURE);
}

static void die_errno(const char *context) {
	fprintf(stderr, "Error: %s: %s\n", context, strerror(errno));
	exit(EXIT_FAILURE);
}

static void *xmalloc(size_t size) {
	void *ptr = malloc(size == 0u ? 1u : size);
	if (ptr == NULL) {
		die("out of memory");
	}
	return ptr;
}

static void *xrealloc(void *ptr, size_t size) {
	void *new_ptr = realloc(ptr, size == 0u ? 1u : size);
	if (new_ptr == NULL) {
		free(ptr);
		die("out of memory");
	}
	return new_ptr;
}

static char *xstrdup(const char *text) {
	char *copy = strdup(text);
	if (copy == NULL) {
		die("out of memory");
	}
	return copy;
}

static int range_cmp(const void *a, const void *b) {
	const IPRange *ra = (const IPRange *)a;
	const IPRange *rb = (const IPRange *)b;
	if (ra->start < rb->start) return -1;
	if (ra->start > rb->start) return 1;
	if (ra->end < rb->end) return -1;
	if (ra->end > rb->end) return 1;
	return 0;
}

static int ci_char(int c) {
	return tolower((unsigned char)c);
}

static int ci_cmp(const void *a, const void *b) {
	const char *sa = *(const char * const *)a;
	const char *sb = *(const char * const *)b;

	while (*sa != '\0' && *sb != '\0') {
		int ca = ci_char((unsigned char)*sa);
		int cb = ci_char((unsigned char)*sb);
		if (ca < cb) return -1;
		if (ca > cb) return 1;
		++sa;
		++sb;
	}

	if (*sa == '\0' && *sb == '\0') {
		return 0;
	}
	return (*sa == '\0') ? -1 : 1;
}

static void merge_ranges(IPRange *ranges, size_t count,
						 IPRange **merged_out, size_t *merged_count_out) {
	if (count == 0u) {
		*merged_out = NULL;
		*merged_count_out = 0u;
		return;
	}

	qsort(ranges, count, sizeof(*ranges), range_cmp);

	IPRange *merged = (IPRange *)xmalloc(count * sizeof(*merged));
	size_t merged_count = 0u;

	merged[merged_count++] = ranges[0];

	for (size_t i = 1u; i < count; ++i) {
		IPRange current = ranges[i];
		IPRange *last = &merged[merged_count - 1u];

		if ((uint64_t)current.start <= (uint64_t)last->end + 1u) {
			if (current.end > last->end) {
				last->end = current.end;
			}
		} else {
			merged[merged_count++] = current;
		}
	}

	*merged_out = merged;
	*merged_count_out = merged_count;
}

static uint64_t count_addresses(const IPRange *ranges, size_t count) {
	uint64_t total = 0u;
	for (size_t i = 0u; i < count; ++i) {
		total += (uint64_t)ranges[i].end -
				 (uint64_t)ranges[i].start + 1u;
	}
	return total;
}

static void range_list_append(RangeList *list, IPRange range) {
	if (list->count == list->capacity) {
		size_t new_capacity = list->capacity == 0u ? 64u : list->capacity * 2u;
		if (new_capacity < list->capacity ||
			new_capacity > SIZE_MAX / sizeof(*list->items)) {
			die("too many input ranges");
		}
		list->items = (IPRange *)xrealloc(
			list->items, new_capacity * sizeof(*list->items));
		list->capacity = new_capacity;
	}
	list->items[list->count++] = range;
}

static void string_list_append(StringList *list, const char *text) {
	if (list->count == list->capacity) {
		size_t new_capacity = list->capacity == 0u ? 16u : list->capacity * 2u;
		if (new_capacity < list->capacity ||
			new_capacity > SIZE_MAX / sizeof(*list->items)) {
			die("too many rejected entries");
		}
		list->items = (char **)xrealloc(
			list->items, new_capacity * sizeof(*list->items));
		list->capacity = new_capacity;
	}
	list->items[list->count++] = xstrdup(text);
}

static void string_list_free(StringList *list) {
	for (size_t i = 0u; i < list->count; ++i) {
		free(list->items[i]);
	}
	free(list->items);
	list->items = NULL;
	list->count = 0u;
	list->capacity = 0u;
}

static char *trim(char *text) {
	while (isspace((unsigned char)*text)) {
		++text;
	}

	char *end = text + strlen(text);
	while (end > text && isspace((unsigned char)end[-1])) {
		--end;
	}
	*end = '\0';
	return text;
}

static void strip_matching_quotes(char *path) {
	size_t length = strlen(path);
	if (length >= 2u &&
		((path[0] == '"' && path[length - 1u] == '"') ||
		 (path[0] == '\'' && path[length - 1u] == '\''))) {
		memmove(path, path + 1, length - 2u);
		path[length - 2u] = '\0';
	}
}

static int read_path(const char *prompt, char *buffer, size_t buffer_size) {
	fputs(prompt, stdout);
	fflush(stdout);

	if (fgets(buffer, (int)buffer_size, stdin) == NULL) {
		return 0;
	}

	size_t length = strlen(buffer);
	if (length > 0u && buffer[length - 1u] == '\n') {
		buffer[--length] = '\0';
	} else if (length == buffer_size - 1u) {
		int ch;
		while ((ch = getchar()) != '\n' && ch != EOF) {
		}
		fprintf(stderr, "Error: path is too long.\n");
		return 0;
	}

	char *clean = trim(buffer);
	if (clean != buffer) {
		memmove(buffer, clean, strlen(clean) + 1u);
	}
	strip_matching_quotes(buffer);

	if (buffer[0] == '\0') {
		fprintf(stderr, "Error: path cannot be empty.\n");
		return 0;
	}
	return 1;
}

static int parse_ipv4(const char *text, uint32_t *value_out) {
	struct in_addr address;
	if (inet_pton(AF_INET, text, &address) != 1) {
		return 0;
	}
	*value_out = ntohl(address.s_addr);
	return 1;
}

static int parse_prefix(const char *text, unsigned int *prefix_out) {
	if (text == NULL || *text == '\0') {
		return 0;
	}

	errno = 0;
	char *end = NULL;
	unsigned long value = strtoul(text, &end, 10);

	if (errno != 0 || end == text || *trim(end) != '\0' || value > 32ul) {
		return 0;
	}

	*prefix_out = (unsigned int)value;
	return 1;
}

static int netmask_to_prefix(uint32_t mask, unsigned int *prefix_out) {
	unsigned int prefix = 0u;
	int zero_seen = 0;

	for (int bit = 31; bit >= 0; --bit) {
		int is_one = (mask & (UINT32_C(1) << (unsigned int)bit)) != 0u;
		if (is_one) {
			if (zero_seen) {
				return 0;
			}
			++prefix;
		} else {
			zero_seen = 1;
		}
	}

	*prefix_out = prefix;
	return 1;
}

static IPRange cidr_to_range(uint32_t address, unsigned int prefix) {
	uint32_t mask = prefix == 0u
		? UINT32_C(0)
		: UINT32_MAX << (32u - prefix);

	IPRange result;
	result.start = address & mask;
	result.end = result.start | ~mask;
	return result;
}

static int parse_cidr(char *text, IPRange *range_out) {
	char *slash = strchr(text, '/');
	if (slash == NULL || strchr(slash + 1, '/') != NULL) {
		return 0;
	}

	*slash = '\0';
	char *address_text = trim(text);
	char *prefix_text = trim(slash + 1);

	uint32_t address;
	unsigned int prefix;
	if (!parse_ipv4(address_text, &address) ||
		!parse_prefix(prefix_text, &prefix)) {
		return 0;
	}

	*range_out = cidr_to_range(address, prefix);
	return 1;
}

static int parse_explicit_range(char *text, IPRange *range_out) {
	char *dash = strchr(text, '-');
	if (dash == NULL || strchr(dash + 1, '-') != NULL) {
		return 0;
	}

	*dash = '\0';
	char *start_text = trim(text);
	char *end_text = trim(dash + 1);

	uint32_t start;
	uint32_t end;
	if (!parse_ipv4(start_text, &start) || !parse_ipv4(end_text, &end)) {
		return 0;
	}

	if (start > end) {
		uint32_t temporary = start;
		start = end;
		end = temporary;
	}

	range_out->start = start;
	range_out->end = end;
	return 1;
}

static int parse_address_with_mask(char *text, IPRange *range_out) {
	char *first = trim(text);
	char *separator = first;

	while (*separator != '\0' && !isspace((unsigned char)*separator)) {
		++separator;
	}
	if (*separator == '\0') {
		return 0;
	}

	*separator++ = '\0';
	char *second = trim(separator);
	if (*second == '\0') {
		return 0;
	}

	char *second_end = second;
	while (*second_end != '\0' && !isspace((unsigned char)*second_end)) {
		++second_end;
	}
	if (*trim(second_end) != '\0') {
		return 0;
	}
	*second_end = '\0';

	uint32_t address;
	if (!parse_ipv4(first, &address)) {
		return 0;
	}

	unsigned int prefix;
	if (second[0] == '/') {
		if (!parse_prefix(second + 1, &prefix)) {
			return 0;
		}
	} else if (strchr(second, '.') != NULL) {
		uint32_t mask;
		if (!parse_ipv4(second, &mask) ||
			!netmask_to_prefix(mask, &prefix)) {
			return 0;
		}
	} else if (!parse_prefix(second, &prefix)) {
		return 0;
	}

	*range_out = cidr_to_range(address, prefix);
	return 1;
}

static int parse_entry(char *text, IPRange *range_out) {
	text = trim(text);
	if (*text == '\0') {
		return 0;
	}

	if ((text[0] == '*' || text[0] == '+' || text[0] == '-') &&
		isspace((unsigned char)text[1])) {
		text = trim(text + 1);
	}

	if (strchr(text, '-') != NULL) {
		return parse_explicit_range(text, range_out);
	}

	if (strchr(text, '/') != NULL) {
		return parse_cidr(text, range_out);
	}

	if (strpbrk(text, " \t\r\v\f") != NULL) {
		return parse_address_with_mask(text, range_out);
	}

	uint32_t address;
	if (!parse_ipv4(text, &address)) {
		return 0;
	}

	range_out->start = address;
	range_out->end = address;
	return 1;
}

static void remove_inline_comment(char *line) {
	char *cut = NULL;

	char *hash = strchr(line, '#');
	if (hash != NULL) {
		cut = hash;
	}

	char *semicolon = strchr(line, ';');
	if (semicolon != NULL && (cut == NULL || semicolon < cut)) {
		cut = semicolon;
	}

	char *double_slash = strstr(line, "//");
	if (double_slash != NULL && (cut == NULL || double_slash < cut)) {
		cut = double_slash;
	}

	if (cut != NULL) {
		*cut = '\0';
	}
}

static void format_rejected(char *buffer, size_t buffer_size,
							size_t line_number, const char *entry) {
	int written = snprintf(
		buffer, buffer_size, "line %zu: %s", line_number, entry);
	if (written < 0) {
		buffer[0] = '\0';
	} else if ((size_t)written >= buffer_size && buffer_size >= 4u) {
		buffer[buffer_size - 4u] = '.';
		buffer[buffer_size - 3u] = '.';
		buffer[buffer_size - 2u] = '.';
		buffer[buffer_size - 1u] = '\0';
	}
}

static int same_file(FILE *input, const char *output_path) {
	struct stat input_stat;
	struct stat output_stat;

	if (fstat(fileno(input), &input_stat) != 0) {
		die_errno("cannot inspect input file");
	}

	if (stat(output_path, &output_stat) != 0) {
		if (errno == ENOENT) {
			return 0;
		}
		die_errno("cannot inspect output path");
	}

	return input_stat.st_dev == output_stat.st_dev &&
		   input_stat.st_ino == output_stat.st_ino;
}

static void ipv4_to_text(uint32_t address, char output[INET_ADDRSTRLEN]) {
	struct in_addr network_address;
	network_address.s_addr = htonl(address);

	if (inet_ntop(AF_INET, &network_address,
				  output, INET_ADDRSTRLEN) == NULL) {
		die_errno("inet_ntop failed");
	}
}

static unsigned int covering_prefix(uint32_t start, uint32_t end) {
	uint32_t differing_bits = start ^ end;
	unsigned int prefix = 32u;

	while (differing_bits != 0u) {
		differing_bits >>= 1u;
		--prefix;
	}

	return prefix;
}

static uint32_t prefix_mask(unsigned int prefix) {
	if (prefix == 0u) {
		return UINT32_C(0);
	}

	return UINT32_MAX << (32u - prefix);
}

static void write_range_as_single_cidr(FILE *output, IPRange range) {
	const unsigned int prefix = covering_prefix(range.start, range.end);
	const uint32_t network = range.start & prefix_mask(prefix);
	char address_text[INET_ADDRSTRLEN];

	ipv4_to_text(network, address_text);

	if (fprintf(output, "%s/%u\n", address_text, prefix) < 0) {
		die_errno("cannot write output file");
	}
}

int main(void) {
	char input_path[PATH_MAX];
	char output_path[PATH_MAX];

	if (!read_path("Input file path: ", input_path, sizeof(input_path)) ||
		!read_path("Output file path: ", output_path, sizeof(output_path))) {
		return EXIT_FAILURE;
	}

	FILE *input = fopen(input_path, "r");
	if (input == NULL) {
		die_errno("cannot open input file");
	}

	if (same_file(input, output_path)) {
		fclose(input);
		die("input and output paths refer to the same file");
	}

	RangeList ranges = {0};
	StringList rejected = {0};

	char *line = NULL;
	size_t line_capacity = 0u;
	size_t line_number = 0u;

	while (getline(&line, &line_capacity, input) != -1) {
		++line_number;

		if (line_number == 1u && strlen(line) >= 3u &&
			(unsigned char)line[0] == 0xEFu &&
			(unsigned char)line[1] == 0xBBu &&
			(unsigned char)line[2] == 0xBFu) {
			memmove(line, line + 3, strlen(line + 3) + 1u);
		}

		remove_inline_comment(line);
		char *clean_line = trim(line);
		if (*clean_line == '\0') {
			continue;
		}

		char *save_pointer = NULL;
		char *entry = strtok_r(clean_line, ",", &save_pointer);

		while (entry != NULL) {
			entry = trim(entry);
			if (*entry != '\0') {
				char *entry_copy = xstrdup(entry);
				IPRange parsed_range;

				if (parse_entry(entry_copy, &parsed_range)) {
					range_list_append(&ranges, parsed_range);
				} else {
					char rejected_text[1024];
					format_rejected(rejected_text, sizeof(rejected_text),
									line_number, entry);
					string_list_append(&rejected, rejected_text);
				}

				free(entry_copy);
			}
			entry = strtok_r(NULL, ",", &save_pointer);
		}
	}

	if (ferror(input)) {
		free(line);
		fclose(input);
		free(ranges.items);
		string_list_free(&rejected);
		die_errno("cannot read input file");
	}

	free(line);
	if (fclose(input) != 0) {
		free(ranges.items);
		string_list_free(&rejected);
		die_errno("cannot close input file");
	}

	IPRange *merged = NULL;
	size_t merged_count = 0u;
	merge_ranges(ranges.items, ranges.count, &merged, &merged_count);
	uint64_t address_count = count_addresses(merged, merged_count);

	FILE *output = fopen(output_path, "w");
	if (output == NULL) {
		free(ranges.items);
		free(merged);
		string_list_free(&rejected);
		die_errno("cannot open output file");
	}

	for (size_t i = 0u; i < merged_count; ++i) {
		write_range_as_single_cidr(output, merged[i]);
	}

	const size_t cidr_count = merged_count;

	if (fflush(output) != 0) {
		fclose(output);
		free(ranges.items);
		free(merged);
		string_list_free(&rejected);
		die_errno("cannot flush output file");
	}

	if (fclose(output) != 0) {
		free(ranges.items);
		free(merged);
		string_list_free(&rejected);
		die_errno("cannot close output file");
	}

	printf("\nCompleted successfully.\n");
	printf("Parsed entries : %zu\n", ranges.count);
	printf("Merged ranges  : %zu\n", merged_count);
	printf("Output CIDRs   : %zu\n", cidr_count);
	printf("IPv4 addresses : %" PRIu64 "\n", address_count);
	printf("Rejected items : %zu\n", rejected.count);
	printf("Output written : %s\n", output_path);

	if (rejected.count > 0u) {
		qsort(rejected.items, rejected.count,
			  sizeof(*rejected.items), ci_cmp);
		fputs("\nRejected input entries:\n", stderr);
		for (size_t i = 0u; i < rejected.count; ++i) {
			fprintf(stderr, "  %s\n", rejected.items[i]);
		}
	}

	free(ranges.items);
	free(merged);
	string_list_free(&rejected);
	return EXIT_SUCCESS;
}
