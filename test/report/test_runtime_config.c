#define _POSIX_C_SOURCE 200809L

#include "internal/general_listener/runtime_config.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static const char* const peak_test_environment_names[] = {
    "PEAK_DETACH_COUNT",
    "PEAK_TEST_UINT",
    "MPI_LOCALNRANKS",
    "OMPI_COMM_WORLD_LOCAL_SIZE",
    "MV2_COMM_WORLD_LOCAL_SIZE",
    "PMI_LOCAL_SIZE",
    "PMI_SIZE",
    "PMIX_SIZE",
    "OMPI_COMM_WORLD_SIZE",
    "MV2_COMM_WORLD_SIZE",
    "I_MPI_SIZE",
    "SLURM_NTASKS",
    "PMI_RANK",
    "PMIX_RANK",
    "OMPI_COMM_WORLD_RANK",
    "MV2_COMM_WORLD_RANK",
    "I_MPI_RANK",
    "SLURM_PROCID",
    "PEAK_TEXT_OUTPUT",
    "PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK",
};

static void
clear_test_environment(void)
{
    for (size_t i = 0;
         i < sizeof(peak_test_environment_names) /
                 sizeof(peak_test_environment_names[0]);
         i++) {
        unsetenv(peak_test_environment_names[i]);
    }
}

static int
check_truthy_values(void)
{
    return peak_general_listener_env_value_truthy(NULL) ||
           !peak_general_listener_env_value_truthy("1") ||
           !peak_general_listener_env_value_truthy("TRUE") ||
           !peak_general_listener_env_value_truthy("Yes") ||
           !peak_general_listener_env_value_truthy("on") ||
           peak_general_listener_env_value_truthy("0") ||
           peak_general_listener_env_value_truthy("true ");
}

static int
check_unsigned_parser(void)
{
    char negative[64];
    char spaced_negative[66];

    if (peak_general_listener_parse_uint_env_default(
            "PEAK_TEST_UINT", 7U) != 7U) {
        return 1;
    }
    setenv("PEAK_TEST_UINT", "", 1);
    if (peak_general_listener_parse_uint_env_default(
            "PEAK_TEST_UINT", 7U) != 7U) {
        return 1;
    }
    setenv("PEAK_TEST_UINT", "0", 1);
    if (peak_general_listener_parse_uint_env_default(
            "PEAK_TEST_UINT", 7U) != 0U) {
        return 1;
    }
    setenv("PEAK_TEST_UINT", "42", 1);
    if (peak_general_listener_parse_uint_env_default(
            "PEAK_TEST_UINT", 7U) != 42U) {
        return 1;
    }
    setenv("PEAK_TEST_UINT", "42junk", 1);
    if (peak_general_listener_parse_uint_env_default(
            "PEAK_TEST_UINT", 7U) != 7U ||
        snprintf(negative, sizeof(negative), "-%lu", ULONG_MAX) >=
            (int)sizeof(negative) ||
        snprintf(spaced_negative,
                 sizeof(spaced_negative),
                 "  -%lu",
                 ULONG_MAX) >= (int)sizeof(spaced_negative)) {
        return 1;
    }
    setenv("PEAK_TEST_UINT", negative, 1);
    if (peak_general_listener_parse_uint_env_default(
            "PEAK_TEST_UINT", 7U) != 7U) {
        return 1;
    }
    setenv("PEAK_TEST_UINT", spaced_negative, 1);
    if (peak_general_listener_parse_uint_env_default(
            "PEAK_TEST_UINT", 7U) != 7U) {
        return 1;
    }
    setenv("PEAK_TEST_UINT", "-0", 1);
    if (peak_general_listener_parse_uint_env_default(
            "PEAK_TEST_UINT", 7U) != 7U) {
        return 1;
    }
    setenv("PEAK_TEST_UINT", "-1", 1);
    if (peak_general_listener_parse_uint_env_default(
            "PEAK_TEST_UINT", 7U) != 7U) {
        return 1;
    }
    setenv("PEAK_TEST_UINT", "+42", 1);
    if (peak_general_listener_parse_uint_env_default(
            "PEAK_TEST_UINT", 7U) != 42U) {
        return 1;
    }
    setenv("PEAK_TEST_UINT", " 42", 1);
    return peak_general_listener_parse_uint_env_default(
               "PEAK_TEST_UINT", 7U) != 42U;
}

static int
check_detach_override(void)
{
    char negative[64];
    char spaced_negative[66];
    unsigned long count = 99U;

    if (peak_general_listener_parse_detach_count_override(&count) ||
        count != 99U) {
        return 1;
    }
    setenv("PEAK_DETACH_COUNT", "12", 1);
    if (!peak_general_listener_parse_detach_count_override(&count) ||
        count != 12U) {
        return 1;
    }
    setenv("PEAK_DETACH_COUNT", "0", 1);
    count = 99U;
    if (peak_general_listener_parse_detach_count_override(&count) ||
        count != 99U) {
        return 1;
    }
    setenv("PEAK_DETACH_COUNT", "bad", 1);
    if (peak_general_listener_parse_detach_count_override(NULL) ||
        snprintf(negative, sizeof(negative), "-%lu", ULONG_MAX) >=
            (int)sizeof(negative) ||
        snprintf(spaced_negative,
                 sizeof(spaced_negative),
                 "  -%lu",
                 ULONG_MAX) >= (int)sizeof(spaced_negative)) {
        return 1;
    }
    count = 99U;
    setenv("PEAK_DETACH_COUNT", negative, 1);
    if (peak_general_listener_parse_detach_count_override(&count) ||
        count != 99U) {
        return 1;
    }
    setenv("PEAK_DETACH_COUNT", spaced_negative, 1);
    if (peak_general_listener_parse_detach_count_override(&count) ||
        count != 99U) {
        return 1;
    }
    setenv("PEAK_DETACH_COUNT", "-0", 1);
    if (peak_general_listener_parse_detach_count_override(&count) ||
        count != 99U) {
        return 1;
    }
    setenv("PEAK_DETACH_COUNT", "-1", 1);
    if (peak_general_listener_parse_detach_count_override(&count) ||
        count != 99U) {
        return 1;
    }
    setenv("PEAK_DETACH_COUNT", "+1", 1);
    if (!peak_general_listener_parse_detach_count_override(&count) ||
        count != 1U) {
        return 1;
    }
    setenv("PEAK_DETACH_COUNT", " 2", 1);
    return !peak_general_listener_parse_detach_count_override(&count) ||
           count != 2U;
}

static int
check_local_rank_precedence(void)
{
    if (peak_general_listener_local_mpi_ranks() != 1U) {
        return 1;
    }
    setenv("MPI_LOCALNRANKS", "bad", 1);
    setenv("OMPI_COMM_WORLD_LOCAL_SIZE", "6", 1);
    if (peak_general_listener_local_mpi_ranks() != 6U) {
        return 1;
    }
    setenv("MPI_LOCALNRANKS", "4", 1);
    return peak_general_listener_local_mpi_ranks() != 4U;
}

static int
check_world_rank_precedence(void)
{
    char int_overflow[64];

    if (peak_general_listener_mpi_env_size() != -1 ||
        peak_general_listener_mpi_env_rank() != -1) {
        return 1;
    }
    setenv("PMI_SIZE", "2junk", 1);
    setenv("PMIX_SIZE", "8", 1);
    setenv("PMI_RANK", "1junk", 1);
    setenv("PMIX_RANK", "2", 1);
    if (peak_general_listener_mpi_env_size() != 8 ||
        peak_general_listener_mpi_env_rank() != 2) {
        return 1;
    }

    setenv("PMI_SIZE", "999999999999999999999999999999999", 1);
    setenv("PMIX_SIZE", "9", 1);
    setenv("PMI_RANK", "1 ", 1);
    setenv("PMIX_RANK", "3", 1);
    if (peak_general_listener_mpi_env_size() != 9 ||
        peak_general_listener_mpi_env_rank() != 3) {
        return 1;
    }

#if LONG_MAX > INT_MAX
    if (snprintf(int_overflow,
                 sizeof(int_overflow),
                 "%ld",
                 (long)INT_MAX + 1L) >= (int)sizeof(int_overflow)) {
        return 1;
    }
    setenv("PMI_SIZE", int_overflow, 1);
    setenv("PMIX_SIZE", "10", 1);
    setenv("PMI_RANK", int_overflow, 1);
    setenv("PMIX_RANK", "4", 1);
    if (peak_general_listener_mpi_env_size() != 10 ||
        peak_general_listener_mpi_env_rank() != 4) {
        return 1;
    }
#else
    (void)int_overflow;
#endif

    return 0;
}

static int
expect_world_rank_size(long expected_rank, long expected_size)
{
    long rank = 99;
    long size = 99;

    return !peak_general_listener_mpi_env_rank_size(&rank, &size) ||
           rank != expected_rank || size != expected_size;
}

static int
expect_world_rank_size_failure(void)
{
    long rank = 99;
    long size = 99;

    return peak_general_listener_mpi_env_rank_size(&rank, &size) ||
           rank != 99 || size != 99;
}

static int
check_world_rank_size_pairs(void)
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

    if (expect_world_rank_size_failure()) {
        return 1;
    }

    for (size_t i = 0;
         i < sizeof(mpi_pairs) / sizeof(mpi_pairs[0]);
         i++) {
        clear_test_environment();
        setenv(mpi_pairs[i].rank_name, "2", 1);
        setenv(mpi_pairs[i].size_name, "8", 1);
        setenv("SLURM_PROCID", "0", 1);
        setenv("SLURM_NTASKS", "8", 1);
        if (expect_world_rank_size(2, 8)) {
            return 1;
        }

        setenv("SLURM_PROCID", "bad", 1);
        setenv("SLURM_NTASKS", "0", 1);
        if (expect_world_rank_size(2, 8)) {
            return 1;
        }
    }

    clear_test_environment();
    setenv("PMI_RANK", "2", 1);
    if (expect_world_rank_size_failure()) {
        return 1;
    }
    setenv("PMI_SIZE", "8", 1);
    if (expect_world_rank_size(2, 8)) {
        return 1;
    }

    setenv("OMPI_COMM_WORLD_RANK", "2", 1);
    setenv("OMPI_COMM_WORLD_SIZE", "8", 1);
    if (expect_world_rank_size(2, 8)) {
        return 1;
    }
    setenv("OMPI_COMM_WORLD_RANK", "3", 1);
    if (expect_world_rank_size_failure()) {
        return 1;
    }

    clear_test_environment();
    setenv("PMI_RANK", "bad", 1);
    setenv("PMI_SIZE", "8", 1);
    setenv("SLURM_PROCID", "2", 1);
    setenv("SLURM_NTASKS", "8", 1);
    if (expect_world_rank_size_failure()) {
        return 1;
    }

    clear_test_environment();
    setenv("PMI_RANK", "8", 1);
    setenv("PMI_SIZE", "8", 1);
    setenv("SLURM_PROCID", "2", 1);
    setenv("SLURM_NTASKS", "8", 1);
    if (expect_world_rank_size_failure()) {
        return 1;
    }

    clear_test_environment();
    setenv("PMI_RANK", "2", 1);
    setenv("PMI_SIZE", "8", 1);
    setenv("OMPI_COMM_WORLD_RANK", "bad", 1);
    setenv("OMPI_COMM_WORLD_SIZE", "8", 1);
    if (expect_world_rank_size_failure()) {
        return 1;
    }

    clear_test_environment();
    setenv("PMI_RANK", "2", 1);
    setenv("SLURM_PROCID", "3", 1);
    setenv("SLURM_NTASKS", "8", 1);
    if (expect_world_rank_size(3, 8)) {
        return 1;
    }

    clear_test_environment();
    setenv("SLURM_PROCID", "3", 1);
    setenv("SLURM_NTASKS", "8", 1);
    if (expect_world_rank_size(3, 8)) {
        return 1;
    }
    unsetenv("SLURM_NTASKS");
    if (expect_world_rank_size_failure()) {
        return 1;
    }
    setenv("SLURM_NTASKS", "8", 1);
    setenv("SLURM_PROCID", "8", 1);
    if (expect_world_rank_size_failure()) {
        return 1;
    }
    setenv("SLURM_PROCID", "3", 1);
    setenv("SLURM_NTASKS", "0", 1);
    if (expect_world_rank_size_failure()) {
        return 1;
    }

    clear_test_environment();
    setenv("PMI_RANK", "2", 1);
    setenv("PMI_SIZE", "8", 1);
    setenv("OMPI_COMM_WORLD_RANK", "2", 1);
    setenv("OMPI_COMM_WORLD_SIZE", "8", 1);
    if (expect_world_rank_size(2, 8)) {
        return 1;
    }
    setenv("OMPI_COMM_WORLD_RANK", "3", 1);
    return expect_world_rank_size_failure();
}

static int
check_output_policies(void)
{
    if (!peak_general_listener_should_print_text(false) ||
        peak_general_listener_should_print_text(true) ||
        !peak_general_listener_socket_reduce_fallback_enabled()) {
        return 1;
    }
    setenv("PEAK_TEXT_OUTPUT", "yes", 1);
    if (!peak_general_listener_should_print_text(true)) {
        return 1;
    }
    setenv("PEAK_TEXT_OUTPUT", "0", 1);
    if (peak_general_listener_should_print_text(false)) {
        return 1;
    }
    setenv("PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK", "off", 1);
    if (peak_general_listener_socket_reduce_fallback_enabled()) {
        return 1;
    }
    setenv("PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK", "ON", 1);
    if (!peak_general_listener_socket_reduce_fallback_enabled()) {
        return 1;
    }
    setenv("PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK", "invalid", 1);
    return !peak_general_listener_socket_reduce_fallback_enabled();
}

int
main(void)
{
    clear_test_environment();
    if (check_truthy_values() || check_unsigned_parser() ||
        check_detach_override()) {
        fputs("runtime_config_test_failed\n", stderr);
        return 1;
    }

    clear_test_environment();
    if (check_local_rank_precedence()) {
        fputs("runtime_config_test_failed\n", stderr);
        return 1;
    }

    clear_test_environment();
    if (check_world_rank_precedence()) {
        fputs("runtime_config_test_failed\n", stderr);
        return 1;
    }

    clear_test_environment();
    if (check_world_rank_size_pairs()) {
        fputs("runtime_config_test_failed\n", stderr);
        return 1;
    }

    clear_test_environment();
    if (check_output_policies()) {
        fputs("runtime_config_test_failed\n", stderr);
        return 1;
    }

    clear_test_environment();
    puts("runtime_config_test_ok");
    return 0;
}
