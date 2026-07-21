#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// atomic-scanner.c (Scan every file in pieces)

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

#define OUTPUT_FILE "all_files_here.txt"
#define TERMINAL_LINE_LIMIT 500

typedef short int intS;

typedef struct {
	char **items;
	size_t count;
	size_t capacity;
} PathList;

static const char *program_name = "";
static intS had_error = 0;
static intS fatal_error = 0;

static unsigned char fold_ascii(unsigned char character)
{
	if (character >= 'a' && character <= 'z') {
		return (unsigned char)(character - ('a' - 'A'));
	}

	return character;
}

static intS compare_case_insensitive(const char *left, const char *right)
{
	while (*left != '\0' && *right != '\0') {
		unsigned char left_character = fold_ascii((unsigned char)*left);
		unsigned char right_character = fold_ascii((unsigned char)*right);

		if (left_character < right_character) {
			return -1;
		}

		if (left_character > right_character) {
			return 1;
		}

		left++;
		right++;
	}

	if (*left == '\0' && *right == '\0') {
		return 0;
	}

	return *left == '\0' ? -1 : 1;
}

/* qsort requires an exact int-returning comparator callback. */
static int compare_paths(const void *left_pointer, const void *right_pointer)
{
	const char *left = *(const char *const *)left_pointer;
	const char *right = *(const char *const *)right_pointer;
	intS result = compare_case_insensitive(left, right);

	if (result != 0) {
		return result;
	}

	return strcmp(left, right);
}

static const char *get_filename(const char *path)
{
	const char *forward_slash;
	const char *backward_slash;
	const char *filename;

	if (path == NULL || path[0] == '\0') {
		return "";
	}

	forward_slash = strrchr(path, '/');
	backward_slash = strrchr(path, '\\');
	filename = path;

	if (forward_slash != NULL) {
		filename = forward_slash + 1;
	}

	if (backward_slash != NULL && backward_slash + 1 > filename) {
		filename = backward_slash + 1;
	}

	return filename;
}

static char *join_path(const char *base, const char *name)
{
	size_t base_length = strlen(base);
	size_t name_length = strlen(name);
	intS needs_separator =
		base_length > 0 &&
		base[base_length - 1U] != '/' &&
		base[base_length - 1U] != '\\';
	size_t separator_length = needs_separator ? 1U : 0U;
	size_t total_length;
	char *path;

	if (base_length > (size_t)-1 - separator_length ||
		base_length + separator_length > (size_t)-1 - name_length ||
		base_length + separator_length + name_length > (size_t)-1 - 1U) {
		fprintf(stderr, "Path is too long.\n");
		fatal_error = 1;
		return NULL;
	}

	total_length = base_length + separator_length + name_length + 1U;
	path = malloc(total_length);

	if (path == NULL) {
		fprintf(stderr, "Memory allocation failed.\n");
		fatal_error = 1;
		return NULL;
	}

	if (base_length > 0) {
		memcpy(path, base, base_length);
	}

	if (needs_separator) {
		path[base_length] = '/';
	}

	memcpy(path + base_length + separator_length, name, name_length);
	path[total_length - 1U] = '\0';

	return path;
}

static char *read_directory_path(void)
{
	char chunk[4096];
	char *path = NULL;
	size_t path_length = 0;

	printf("Enter the directory path to scan: ");

	if (fflush(stdout) != 0) {
		fprintf(stderr, "Failed to display the input prompt.\n");
		return NULL;
	}

	while (fgets(chunk, sizeof(chunk), stdin) != NULL) {
		char *newline = strchr(chunk, '\n');
		size_t chunk_length;
		char *resized_path;

		if (newline != NULL) {
			*newline = '\0';
		}

		chunk_length = strlen(chunk);

		if (path_length > (size_t)-1 - chunk_length - 1U) {
			fprintf(stderr, "Input path is too long.\n");
			free(path);
			return NULL;
		}

		resized_path = realloc(path, path_length + chunk_length + 1U);

		if (resized_path == NULL) {
			fprintf(stderr, "Memory allocation failed.\n");
			free(path);
			return NULL;
		}

		path = resized_path;
		memcpy(path + path_length, chunk, chunk_length);
		path_length += chunk_length;
		path[path_length] = '\0';

		if (newline != NULL) {
			break;
		}
	}

	if (path == NULL) {
		fprintf(stderr, "No directory path was provided.\n");
		return NULL;
	}

	if (path_length > 0 && path[path_length - 1U] == '\r') {
		path[path_length - 1U] = '\0';
		path_length--;
	}

	if (path_length == 0) {
		fprintf(stderr, "Directory path cannot be empty.\n");
		free(path);
		return NULL;
	}

	return path;
}

static intS append_path(PathList *list, char *path)
{
	char **resized_items;
	size_t new_capacity;

	if (list->count == list->capacity) {
		if (list->capacity == 0) {
			new_capacity = 64U;
		} else {
			if (list->capacity > ((size_t)-1) / 2U) {
				fprintf(stderr, "Path list is too large.\n");
				fatal_error = 1;
				return 0;
			}

			new_capacity = list->capacity * 2U;
		}

		if (new_capacity > ((size_t)-1) / sizeof(*list->items)) {
			fprintf(stderr, "Path list is too large.\n");
			fatal_error = 1;
			return 0;
		}

		resized_items = realloc(
			list->items,
			new_capacity * sizeof(*list->items));

		if (resized_items == NULL) {
			fprintf(stderr, "Memory allocation failed.\n");
			fatal_error = 1;
			return 0;
		}

		list->items = resized_items;
		list->capacity = new_capacity;
	}

	list->items[list->count] = path;
	list->count++;

	return 1;
}

static void free_path_list(PathList *list)
{
	size_t index;

	if (list == NULL) {
		return;
	}

	for (index = 0; index < list->count; index++) {
		free(list->items[index]);
		list->items[index] = NULL;
	}

	free(list->items);
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
}

static intS should_exclude_root_entry(
	const char *relative_path,
	const char *entry_name)
{
	if (relative_path[0] != '\0') {
		return 0;
	}

	if (strcmp(entry_name, OUTPUT_FILE) == 0) {
		return 1;
	}

	if (program_name[0] != '\0' && strcmp(entry_name, program_name) == 0) {
		return 1;
	}

	return 0;
}

static void traverse_directory(
	DIR *directory,
	const char *filesystem_path,
	const char *relative_path,
	PathList *list)
{
	struct dirent *entry;

	while (!fatal_error) {
		char *child_filesystem_path = NULL;
		char *child_relative_path = NULL;
		DIR *child_directory = NULL;
		intS is_directory = 0;

		entry = readdir(directory);

		if (entry == NULL) {
			break;
		}

		if (strcmp(entry->d_name, ".") == 0 ||
			strcmp(entry->d_name, "..") == 0) {
			continue;
		}

		if (should_exclude_root_entry(relative_path, entry->d_name)) {
			continue;
		}

		child_filesystem_path = join_path(filesystem_path, entry->d_name);

		if (child_filesystem_path == NULL) {
			break;
		}

		child_relative_path = join_path(relative_path, entry->d_name);

		if (child_relative_path == NULL) {
			free(child_filesystem_path);
			break;
		}

#if defined(_DIRENT_HAVE_D_TYPE) || defined(__APPLE__)
		if (entry->d_type == DT_DIR) {
			is_directory = 1;
		} else if (entry->d_type == DT_UNKNOWN) {
			child_directory = opendir(child_filesystem_path);

			if (child_directory != NULL) {
				is_directory = 1;
			}
		}
#else
		child_directory = opendir(child_filesystem_path);

		if (child_directory != NULL) {
			is_directory = 1;
		}
#endif

		if (is_directory) {
			if (child_directory == NULL) {
				child_directory = opendir(child_filesystem_path);
			}

			if (child_directory == NULL) {
				fprintf(
					stderr,
					"Cannot access directory: %s\n",
					child_filesystem_path);
				had_error = 1;
			} else {
				traverse_directory(
					child_directory,
					child_filesystem_path,
					child_relative_path,
					list);

				if (closedir(child_directory) != 0) {
					fprintf(
						stderr,
						"Failed to close directory: %s\n",
						child_filesystem_path);
					had_error = 1;
				}

				child_directory = NULL;
			}

			free(child_filesystem_path);
			free(child_relative_path);
		} else {
			free(child_filesystem_path);
			child_filesystem_path = NULL;

			if (!append_path(list, child_relative_path)) {
				free(child_relative_path);
				break;
			}

			child_relative_path = NULL;
		}
	}
}

static size_t count_unique_paths(const PathList *list)
{
	size_t index;
	size_t unique_count = 0;

	for (index = 0; index < list->count; index++) {
		if (index == 0 ||
			compare_case_insensitive(
				list->items[index - 1U],
				list->items[index]) != 0) {
			unique_count++;
		}
	}

	return unique_count;
}

static intS write_sorted_unique_paths(
	PathList *list,
	const char *destination_path)
{
	FILE *output_file;
	char *output_file_path;
	size_t index;
	size_t unique_count;
	intS print_to_terminal;

	if (list->count > 1U) {
		qsort(
			list->items,
			list->count,
			sizeof(*list->items),
			compare_paths);
	}

	unique_count = count_unique_paths(list);
	print_to_terminal = unique_count <= TERMINAL_LINE_LIMIT;

	output_file_path = join_path(destination_path, OUTPUT_FILE);

	if (output_file_path == NULL) {
		return 0;
	}

	output_file = fopen(output_file_path, "w");

	if (output_file == NULL) {
		fprintf(
			stderr,
			"Cannot create or open %s for writing.\n",
			output_file_path);
		free(output_file_path);
		return 0;
	}

	for (index = 0; index < list->count; index++) {
		if (index > 0 &&
			compare_case_insensitive(
				list->items[index - 1U],
				list->items[index]) == 0) {
			continue;
		}

		if (print_to_terminal && printf("%s\n", list->items[index]) < 0) {
			fprintf(stderr, "Failed to write to standard output.\n");
			fatal_error = 1;
			break;
		}

		if (fprintf(output_file, "%s\n", list->items[index]) < 0) {
			fprintf(stderr, "Failed to write to %s.\n", output_file_path);
			fatal_error = 1;
			break;
		}
	}

	if (fflush(output_file) != 0) {
		fprintf(stderr, "Failed to flush %s.\n", output_file_path);
		fatal_error = 1;
	}

	if (fclose(output_file) != 0) {
		fprintf(stderr, "Failed to close %s.\n", output_file_path);
		fatal_error = 1;
	}

	if (fflush(stdout) != 0) {
		fprintf(stderr, "Failed to flush standard output.\n");
		fatal_error = 1;
	}

	free(output_file_path);
	output_file_path = NULL;

	return fatal_error ? 0 : 1;
}

/* Hosted C requires main to return int and receive argc as int. */
int main(int argc, char *argv[])
{
	char *root_path = NULL;
	DIR *root_directory = NULL;
	PathList list = {NULL, 0U, 0U};
	intS exit_status = EXIT_SUCCESS;

	if (argc > 0 && argv != NULL && argv[0] != NULL) {
		program_name = get_filename(argv[0]);
	}

	root_path = read_directory_path();

	if (root_path == NULL) {
		return EXIT_FAILURE;
	}

	root_directory = opendir(root_path);

	if (root_directory == NULL) {
		fprintf(stderr, "Cannot access directory: %s\n", root_path);
		free(root_path);
		return EXIT_FAILURE;
	}

	traverse_directory(root_directory, root_path, "", &list);

	if (closedir(root_directory) != 0) {
		fprintf(stderr, "Failed to close directory: %s\n", root_path);
		had_error = 1;
	}

	root_directory = NULL;

	if (!fatal_error) {
		write_sorted_unique_paths(&list, root_path);
	}

	free_path_list(&list);
	free(root_path);

	if (fatal_error || had_error) {
		exit_status = EXIT_FAILURE;
	}

	return exit_status;
}
