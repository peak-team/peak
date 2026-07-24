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
#define PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS_ENV \
    "PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"
#define PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS_ENV \
    "PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS"
#define PEAK_TEST_OUTPUT_AGGREGATION_WAVE_BUDGET_MS_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_WAVE_BUDGET_MS"
#define PEAK_MPI_REPORT_RELEASE_TIMEOUT_MS_ENV \
    "PEAK_MPI_REPORT_RELEASE_TIMEOUT_MS"
#define PEAK_SOCKET_PHASE_TIMEOUT_MS_DEFAULT 60000U
#define PEAK_MPI_REPORT_RELEASE_TIMEOUT_MS_DEFAULT 180000U
#define PEAK_SOCKET_POST_GATHER_PHASE_COUNT 2U
#define PEAK_MPI_RELEASE_MARGIN_PHASE_COUNT 2U

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

static bool
peak_general_listener_parse_positive_uint_bounded(const char* name,
                                                  unsigned int maximum,
                                                  unsigned int* value_out)
{
    const char* value = getenv(name);
    char* end = NULL;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0' ||
        peak_general_listener_unsigned_text_is_negative(value)) {
        return false;
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' || parsed == 0 ||
        parsed > maximum) {
        return false;
    }
    if (value_out != NULL) {
        *value_out = (unsigned int)parsed;
    }
    return true;
}

static unsigned int
peak_general_listener_multiply_saturated(unsigned int value,
                                         unsigned int multiplier,
                                         unsigned int maximum)
{
    if (value > maximum / multiplier) {
        return maximum;
    }
    return value * multiplier;
}

static unsigned int
peak_general_listener_add_saturated(unsigned int left,
                                    unsigned int right)
{
    if (left > UINT_MAX - right) {
        return UINT_MAX;
    }
    return left + right;
}

PeakReportTimeoutBudget
peak_general_listener_report_timeout_budget_for_rank_count(
    unsigned int rank_count)
{
    PeakReportTimeoutBudget budget = {
        .socket_phase_timeout_ms =
            PEAK_SOCKET_PHASE_TIMEOUT_MS_DEFAULT,
        .socket_gather_hard_timeout_ms = 0,
        .socket_release_timeout_ms = 0,
        .mpi_report_release_timeout_ms =
            PEAK_MPI_REPORT_RELEASE_TIMEOUT_MS_DEFAULT,
        .socket_gather_timeout_was_scaled = false,
        .socket_combined_release_minimum_ms = 0,
        .socket_release_was_raised = false,
    };
    unsigned int configured;
    unsigned int peer_count = rank_count > 0 ? rank_count - 1U : 0U;
    unsigned int waves =
        peer_count / PEAK_SOCKET_GATHER_ACTIVE_MAX +
        (peer_count % PEAK_SOCKET_GATHER_ACTIVE_MAX != 0U);
    unsigned int wave_budget_ms =
        PEAK_SOCKET_GATHER_WAVE_BUDGET_MS;
    unsigned int adaptive_margin;
    unsigned int adaptive_hard;
    bool phase_was_configured;
    unsigned int minimum_socket_release;
    unsigned int mpi_margin;
    unsigned int minimum_mpi_release;

    phase_was_configured =
        peak_general_listener_parse_positive_uint_bounded(
            PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS_ENV,
            (unsigned int)INT_MAX,
            &configured);
    if (phase_was_configured) {
        budget.socket_phase_timeout_ms = configured;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (peak_general_listener_parse_positive_uint_bounded(
            PEAK_TEST_OUTPUT_AGGREGATION_WAVE_BUDGET_MS_ENV,
            PEAK_SOCKET_GATHER_ADAPTIVE_MARGIN_MAX_MS,
            &configured)) {
        wave_budget_ms = configured;
    }
#endif
    adaptive_margin = peak_general_listener_multiply_saturated(
        waves,
        wave_budget_ms,
        PEAK_SOCKET_GATHER_ADAPTIVE_MARGIN_MAX_MS);
    adaptive_hard =
        budget.socket_phase_timeout_ms >
                (unsigned int)INT_MAX - adaptive_margin
            ? (unsigned int)INT_MAX
            : budget.socket_phase_timeout_ms + adaptive_margin;
    budget.socket_gather_hard_timeout_ms = adaptive_hard;
    budget.socket_gather_timeout_was_scaled =
        adaptive_hard > budget.socket_phase_timeout_ms;

    minimum_socket_release =
        peak_general_listener_add_saturated(
            budget.socket_gather_hard_timeout_ms,
            peak_general_listener_multiply_saturated(
            budget.socket_phase_timeout_ms,
            PEAK_SOCKET_POST_GATHER_PHASE_COUNT,
            (unsigned int)INT_MAX));
    if (minimum_socket_release > (unsigned int)INT_MAX) {
        minimum_socket_release = (unsigned int)INT_MAX;
    }
    budget.socket_release_timeout_ms = minimum_socket_release;
    if (peak_general_listener_parse_positive_uint_bounded(
            PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS_ENV,
            (unsigned int)INT_MAX,
            &configured)) {
        budget.socket_release_timeout_ms = configured;
        if (configured < minimum_socket_release) {
            budget.socket_release_timeout_ms = minimum_socket_release;
            budget.socket_release_was_raised = true;
        }
    }

    if (peak_general_listener_parse_positive_uint_bounded(
            PEAK_MPI_REPORT_RELEASE_TIMEOUT_MS_ENV,
            UINT_MAX,
            &configured)) {
        budget.mpi_report_release_timeout_ms = configured;
    }
    mpi_margin = peak_general_listener_multiply_saturated(
        budget.socket_phase_timeout_ms,
        PEAK_MPI_RELEASE_MARGIN_PHASE_COUNT,
        UINT_MAX);
    minimum_mpi_release = peak_general_listener_add_saturated(
        budget.socket_release_timeout_ms, mpi_margin);
    budget.socket_combined_release_minimum_ms = minimum_mpi_release;
    return budget;
}

PeakReportTimeoutBudget
peak_general_listener_report_timeout_budget(void)
{
    long rank_count = -1;

    (void)peak_general_listener_mpi_env_rank_size(NULL, &rank_count);

    return peak_general_listener_report_timeout_budget_for_rank_count(
        rank_count > 0 && (unsigned long)rank_count <= UINT_MAX
            ? (unsigned int)rank_count
            : 1U);
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
        if (rank < 0 || size <= 0 || size > INT_MAX || rank >= size) {
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
            resolved_size > INT_MAX ||
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
