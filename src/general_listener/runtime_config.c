#include "internal/general_listener/runtime_config.h"

#include "logging.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

#define PEAK_TEXT_OUTPUT_ENV "PEAK_TEXT_OUTPUT"
#define PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK_ENV \
    "PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK"

static bool
peak_general_listener_unsigned_text_is_negative(const char* value)
{
    const unsigned char* cursor = (const unsigned char*)value;

    if (cursor == NULL) {
        return false;
    }
    while (*cursor != '\0' && isspace(*cursor)) {
        cursor++;
    }
    return *cursor == '-';
}

bool
peak_general_listener_parse_detach_count_override(unsigned long* count_out)
{
    const char* value = getenv("PEAK_DETACH_COUNT");

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    if (peak_general_listener_unsigned_text_is_negative(value)) {
        peak_log_info("[peak] ignoring invalid PEAK_DETACH_COUNT=%s\n", value);
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
    if (peak_general_listener_unsigned_text_is_negative(value)) {
        peak_log_info("[peak] ignoring invalid %s=%s\n", name, value);
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

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < 0 ||
        parsed > INT_MAX) {
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
peak_general_listener_mpi_env_rank_size(long* rank_out, long* size_out)
{
    static const struct {
        const char* rank_name;
        const char* size_name;
    } mpi_pairs[] = {
        { "PMI_RANK", "PMI_SIZE" },
        { "PMIX_RANK", "PMIX_SIZE" },
        { "OMPI_COMM_WORLD_RANK", "OMPI_COMM_WORLD_SIZE" },
        { "MV2_COMM_WORLD_RANK", "MV2_COMM_WORLD_SIZE" },
        { "I_MPI_RANK", "I_MPI_SIZE" },
    };
    long resolved_rank = -1;
    long resolved_size = -1;

    for (size_t i = 0;
         i < sizeof(mpi_pairs) / sizeof(mpi_pairs[0]);
         i++) {
        const char* rank_text = getenv(mpi_pairs[i].rank_name);
        const char* size_text = getenv(mpi_pairs[i].size_name);
        bool rank_present = rank_text != NULL && rank_text[0] != '\0';
        bool size_present = size_text != NULL && size_text[0] != '\0';

        if (!rank_present || !size_present) {
            continue;
        }

        long rank = peak_general_listener_parse_long_env(
            mpi_pairs[i].rank_name);
        long size = peak_general_listener_parse_long_env(
            mpi_pairs[i].size_name);
        if (rank < 0 || size <= 0 || rank >= size) {
            return false;
        }
        if (resolved_rank >= 0 &&
            (resolved_rank != rank || resolved_size != size)) {
            return false;
        }
        resolved_rank = rank;
        resolved_size = size;
    }

    if (resolved_rank < 0) {
        const char* rank_text = getenv("SLURM_PROCID");
        const char* size_text = getenv("SLURM_NTASKS");
        bool rank_present = rank_text != NULL && rank_text[0] != '\0';
        bool size_present = size_text != NULL && size_text[0] != '\0';

        if (!rank_present || !size_present) {
            return false;
        }
        resolved_rank = peak_general_listener_parse_long_env(
            "SLURM_PROCID");
        resolved_size = peak_general_listener_parse_long_env(
            "SLURM_NTASKS");
        if (resolved_rank < 0 || resolved_size <= 0 ||
            resolved_rank >= resolved_size) {
            return false;
        }
    }
    if (rank_out != NULL) {
        *rank_out = resolved_rank;
    }
    if (size_out != NULL) {
        *size_out = resolved_size;
    }
    return true;
}

bool
peak_general_listener_mpi_env_world_metadata_present(void)
{
    static const char* names[] = {
        "PMI_RANK", "PMI_SIZE",
        "PMIX_RANK", "PMIX_SIZE",
        "OMPI_COMM_WORLD_RANK", "OMPI_COMM_WORLD_SIZE",
        "MV2_COMM_WORLD_RANK", "MV2_COMM_WORLD_SIZE",
        "I_MPI_RANK", "I_MPI_SIZE",
        "SLURM_PROCID", "SLURM_NTASKS",
    };

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char* value = getenv(names[i]);
        if (value != NULL && value[0] != '\0') {
            return true;
        }
    }
    return false;
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
