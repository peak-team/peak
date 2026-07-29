#define _POSIX_C_SOURCE 200809L

#include "target_config.h"

#include "logging.h"
#include "source_target.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char** items;
    size_t count;
    size_t capacity;
    bool allocation_failed;
} TargetBuilder;

#ifdef PEAK_TARGET_CONFIG_TESTING
static size_t target_config_successful_allocations = SIZE_MAX;
static size_t target_config_empty_token_warning_count;

void
peak_target_config_test_fail_allocations_after(size_t successful_allocations)
{
    target_config_successful_allocations = successful_allocations;
}

void
peak_target_config_test_reset_warning_counts(void)
{
    target_config_empty_token_warning_count = 0;
}

size_t
peak_target_config_test_empty_token_warning_count(void)
{
    return target_config_empty_token_warning_count;
}

static bool
target_config_allocation_allowed(void)
{
    if (target_config_successful_allocations == 0) {
        return false;
    }
    if (target_config_successful_allocations != SIZE_MAX) {
        target_config_successful_allocations--;
    }
    return true;
}
#else
static bool
target_config_allocation_allowed(void)
{
    return true;
}
#endif

static void*
target_config_malloc(size_t bytes)
{
    return target_config_allocation_allowed() ? malloc(bytes) : NULL;
}

static void*
target_config_realloc(void* pointer, size_t bytes)
{
    return target_config_allocation_allowed() ? realloc(pointer, bytes) : NULL;
}

static bool
checked_add_size(size_t left, size_t right, size_t* result)
{
    if (right > SIZE_MAX - left) {
        return false;
    }
    *result = left + right;
    return true;
}

static bool
checked_mul_size(size_t left, size_t right, size_t* result)
{
    if (left != 0 && right > SIZE_MAX / left) {
        return false;
    }
    *result = left * right;
    return true;
}

static void
target_config_warn_empty_once(bool* warned)
{
    if (!*warned) {
        peak_log_warn("[peak] warning: ignoring empty target token\n");
#ifdef PEAK_TARGET_CONFIG_TESTING
        target_config_empty_token_warning_count++;
#endif
        *warned = true;
    }
}

static void
target_config_warn_allocation_once(TargetBuilder* builder)
{
    if (!builder->allocation_failed) {
        peak_log_warn("[peak] warning: unable to append target; out of memory or target list is too large\n");
        builder->allocation_failed = true;
    }
}

static char*
trim_token(char* token)
{
    char* end;

    while (isspace((unsigned char)*token)) {
        token++;
    }
    end = token + strlen(token);
    while (end != token && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return token;
}

static bool
target_builder_grow(TargetBuilder* builder, size_t required)
{
    size_t capacity;
    size_t bytes;
    char** expanded;

    if (required <= builder->capacity) {
        return true;
    }

    capacity = builder->capacity == 0 ? 8 : builder->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    if (!checked_mul_size(capacity, sizeof(*builder->items), &bytes)) {
        target_config_warn_allocation_once(builder);
        return false;
    }

    expanded = target_config_realloc(builder->items, bytes);
    if (expanded == NULL) {
        target_config_warn_allocation_once(builder);
        return false;
    }
    builder->items = expanded;
    builder->capacity = capacity;
    return true;
}

static bool
target_builder_append(TargetBuilder* builder, const char* token)
{
    size_t length;
    size_t bytes;
    size_t required;
    char* copy;

    length = strlen(token);
    if (!checked_add_size(length, 1, &bytes) ||
        !checked_add_size(builder->count, 1, &required)) {
        target_config_warn_allocation_once(builder);
        return false;
    }
    copy = target_config_malloc(bytes);
    if (copy == NULL) {
        target_config_warn_allocation_once(builder);
        return false;
    }
    memcpy(copy, token, bytes);

    if (!target_builder_grow(builder, required)) {
        free(copy);
        return false;
    }
    builder->items[builder->count++] = copy;
    return true;
}

static char*
target_config_duplicate(TargetBuilder* builder, const char* value)
{
    size_t bytes;
    char* copy;

    if (!checked_add_size(strlen(value), 1, &bytes)) {
        target_config_warn_allocation_once(builder);
        return NULL;
    }
    copy = target_config_malloc(bytes);
    if (copy == NULL) {
        target_config_warn_allocation_once(builder);
        return NULL;
    }
    memcpy(copy, value, bytes);
    return copy;
}

static TargetBuilder
target_builder_from_result(char*** result, size_t existing_count)
{
    TargetBuilder builder = {
        .items = *result,
        .count = existing_count,
        .capacity = existing_count,
        .allocation_failed = false,
    };
    return builder;
}

static size_t
target_builder_finish(TargetBuilder* builder, char*** result, size_t initial_count)
{
    *result = builder->items;
    return builder->count - initial_count;
}

static bool
append_trimmed_token(TargetBuilder* builder, char* token, bool* warned_empty)
{
    token = trim_token(token);
    if (*token == '\0') {
        target_config_warn_empty_once(warned_empty);
        return true;
    }
    return target_builder_append(builder, token);
}

size_t
parse_env_w_delim(const char* env_var, const char a_delim, char*** result)
{
    const char* value;
    TargetBuilder builder;
    char* copy;
    char* token;
    char* next;
    bool warned_empty = false;

    if (result == NULL || env_var == NULL || a_delim == '\0') {
        return 0;
    }
    *result = NULL;
    value = getenv(env_var);
    if (value == NULL || *value == '\0') {
        return 0;
    }

    /* Duplicate once so delimiters can be replaced without modifying getenv(). */
    builder = target_builder_from_result(result, 0);
    copy = target_config_duplicate(&builder, value);
    if (copy == NULL) {
        return target_builder_finish(&builder, result, 0);
    }
    token = copy;
    do {
        next = strchr(token, a_delim);
        if (next != NULL) {
            *next++ = '\0';
        }
        if (!append_trimmed_token(&builder, token, &warned_empty)) {
            break;
        }
        token = next;
    } while (token != NULL);
    free(copy);
    return target_builder_finish(&builder, result, 0);
}

size_t
load_profiling_symbols(const char* config_file, char*** result, size_t existing_count)
{
    const char* path;
    FILE* file;
    TargetBuilder builder;
    char* line = NULL;
    size_t line_capacity = 0;
    ssize_t line_length;
    bool warned_empty = false;

    if (result == NULL || config_file == NULL) {
        return 0;
    }
    path = getenv(config_file);
    if (path == NULL || *path == '\0') {
        return 0;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        peak_log_warn("[peak] warning: cannot open target configuration file '%s'\n", path);
        return 0;
    }

    builder = target_builder_from_result(result, existing_count);
    while ((line_length = getline(&line, &line_capacity, file)) != -1) {
        (void)line_length;
        if (!append_trimmed_token(&builder, line, &warned_empty)) {
            break;
        }
    }
    if (ferror(file)) {
        peak_log_warn("[peak] warning: failed while reading target configuration file '%s'\n", path);
    }
    free(line);
    fclose(file);
    return target_builder_finish(&builder, result, existing_count);
}

typedef struct {
    const char* name;
    char** symbols;
    size_t* count;
} TargetGroup;

static const TargetGroup target_groups[] = {
    {"BLAS", source_target_array_BLAS, &source_count_BLAS},
    {"LAPACK", source_target_array_LAPACK, &source_count_LAPACK},
    {"PBLAS", source_target_array_PBLAS, &source_count_PBLAS},
    {"ScaLAPACK", source_target_array_ScaLAPACK, &source_count_ScaLAPACK},
    {"FFTW", source_target_array_FFTW, &source_count_FFTW},
};

static const TargetGroup*
find_target_group(const char* name)
{
    for (size_t i = 0; i < sizeof(target_groups) / sizeof(target_groups[0]); i++) {
        if (strcmp(name, target_groups[i].name) == 0) {
            return &target_groups[i];
        }
    }
    return NULL;
}

size_t
load_symbols_from_array(const char* env_var, char*** result, size_t existing_count)
{
    const char* value;
    TargetBuilder builder;
    char* copy;
    char* token;
    char* next;
    bool warned_empty = false;

    if (result == NULL || env_var == NULL) {
        return 0;
    }
    value = getenv(env_var);
    if (value == NULL || *value == '\0') {
        return 0;
    }
    builder = target_builder_from_result(result, existing_count);
    copy = target_config_duplicate(&builder, value);
    if (copy == NULL) {
        return target_builder_finish(&builder, result, existing_count);
    }

    token = copy;
    do {
        const TargetGroup* group;

        next = strchr(token, ',');
        if (next != NULL) {
            *next++ = '\0';
        }
        token = trim_token(token);
        if (*token == '\0') {
            target_config_warn_empty_once(&warned_empty);
        } else if ((group = find_target_group(token)) != NULL) {
            for (size_t i = 0; i < *group->count; i++) {
                if (!target_builder_append(&builder, group->symbols[i])) {
                    break;
                }
            }
        } else {
            peak_log_warn("[peak] warning: ignoring unknown target group '%s'\n", token);
        }
        if (builder.allocation_failed) {
            break;
        }
        token = next;
    } while (token != NULL);

    free(copy);
    return target_builder_finish(&builder, result, existing_count);
}

void
free_parsed_result(char** result, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(result[i]);
    }
    free(result);
}
