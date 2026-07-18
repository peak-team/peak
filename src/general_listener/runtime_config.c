#include "internal/general_listener/runtime_config.h"

#include "logging.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

#define PEAK_TEXT_OUTPUT_ENV "PEAK_TEXT_OUTPUT"
#define PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK_ENV \
    "PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK"

bool
peak_general_listener_parse_detach_count_override(unsigned long* count_out)
{
    const char* value = getenv("PEAK_DETACH_COUNT");

    if (value == NULL || value[0] == '\0') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);

    if (errno == ERANGE || end == value || *end != '\0' || parsed == 0) {
        peak_log_info("[peak] ignoring invalid PEAK_DETACH_COUNT=%s\n", value);
        return false;
    }

    if (count_out != NULL) {
        *count_out = parsed;
    }
    return true;
}

unsigned int
peak_general_listener_parse_uint_env_default(const char* name,
                                             unsigned int default_value)
{
    const char* value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return default_value;
    }

    errno = 0;
    char* end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' ||
        parsed > UINT_MAX) {
        peak_log_info("[peak] ignoring invalid %s=%s\n", name, value);
        return default_value;
    }

    return (unsigned int)parsed;
}

static bool
peak_general_listener_parse_positive_uint_text(const char* value,
                                               unsigned int* parsed_out)
{
    const char* cursor;
    char* end = NULL;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    for (cursor = value; *cursor != '\0'; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' || parsed == 0 ||
        parsed > UINT_MAX) {
        return false;
    }
    if (parsed_out != NULL) {
        *parsed_out = (unsigned int)parsed;
    }
    return true;
}

unsigned int
peak_general_listener_local_mpi_ranks(void)
{
    static const char* local_size_envs[] = {
        "MPI_LOCALNRANKS",
        "OMPI_COMM_WORLD_LOCAL_SIZE",
        "MV2_COMM_WORLD_LOCAL_SIZE",
        "PMI_LOCAL_SIZE",
    };

    for (size_t i = 0;
         i < sizeof(local_size_envs) / sizeof(local_size_envs[0]);
         i++) {
        unsigned int parsed = 0;
        if (peak_general_listener_parse_positive_uint_text(
                getenv(local_size_envs[i]),
                &parsed)) {
            return parsed;
        }
    }
    return 1U;
}

static bool
peak_general_listener_ascii_equal_ci(const char* left, const char* right)
{
    if (left == NULL || right == NULL) {
        return false;
    }

    while (*left != '\0' && *right != '\0') {
        unsigned char left_char = (unsigned char)*left;
        unsigned char right_char = (unsigned char)*right;

        if (left_char >= 'A' && left_char <= 'Z') {
            left_char = (unsigned char)(left_char - 'A' + 'a');
        }
        if (right_char >= 'A' && right_char <= 'Z') {
            right_char = (unsigned char)(right_char - 'A' + 'a');
        }
        if (left_char != right_char) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

bool
peak_general_listener_env_value_truthy(const char* value)
{
    return peak_general_listener_ascii_equal_ci(value, "1") ||
           peak_general_listener_ascii_equal_ci(value, "true") ||
           peak_general_listener_ascii_equal_ci(value, "yes") ||
           peak_general_listener_ascii_equal_ci(value, "on");
}

static bool
peak_general_listener_env_value_falsey(const char* value)
{
    return peak_general_listener_ascii_equal_ci(value, "0") ||
           peak_general_listener_ascii_equal_ci(value, "false") ||
           peak_general_listener_ascii_equal_ci(value, "off") ||
           peak_general_listener_ascii_equal_ci(value, "no");
}

bool
peak_general_listener_socket_reduce_fallback_enabled(void)
{
    const char* value =
        getenv(PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK_ENV);

    if (value == NULL || value[0] == '\0') {
        return true;
    }
    if (peak_general_listener_env_value_falsey(value)) {
        return false;
    }
    if (peak_general_listener_env_value_truthy(value)) {
        return true;
    }

    peak_log_info("[peak] Ignoring invalid %s=%s; using fallback-to-local behavior\n",
                  PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK_ENV,
                  value);
    return true;
}

static long
peak_general_listener_parse_long_env(const char* name)
{
    const char* value = getenv(name);
    char* end = NULL;
    long parsed;

    if (value == NULL || value[0] == '\0') {
        return -1;
    }

    parsed = strtol(value, &end, 10);
    if (end == value || parsed < 0) {
        return -1;
    }

    return parsed;
}

long
peak_general_listener_mpi_env_size(void)
{
    static const char* names[] = {
        "PMI_SIZE",
        "PMIX_SIZE",
        "OMPI_COMM_WORLD_SIZE",
        "SLURM_NTASKS",
        NULL
    };

    for (const char** name = names; *name != NULL; name++) {
        long value = peak_general_listener_parse_long_env(*name);
        if (value > 0) {
            return value;
        }
    }

    return -1;
}

long
peak_general_listener_mpi_env_rank(void)
{
    static const char* names[] = {
        "PMI_RANK",
        "PMIX_RANK",
        "OMPI_COMM_WORLD_RANK",
        "MV2_COMM_WORLD_RANK",
        "SLURM_PROCID",
        NULL
    };

    for (const char** name = names; *name != NULL; name++) {
        long value = peak_general_listener_parse_long_env(*name);
        if (value >= 0) {
            return value;
        }
    }

    return -1;
}

bool
peak_general_listener_should_print_text(bool rank_local_mpi_output)
{
    const char* value = getenv(PEAK_TEXT_OUTPUT_ENV);

    if (value != NULL && value[0] != '\0') {
        return peak_general_listener_env_value_truthy(value);
    }

    return !rank_local_mpi_output;
}
