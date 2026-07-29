#define _POSIX_C_SOURCE 200809L

#include "source_target.h"
#include "target_config.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
expect_names(char** names, size_t count, const char* const* expected, size_t expected_count)
{
    if (count != expected_count) {
        fprintf(stderr, "count mismatch: got %zu expected %zu\n", count, expected_count);
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], expected[i]) != 0) {
            fprintf(stderr, "name %zu mismatch: got '%s' expected '%s'\n", i, names[i], expected[i]);
            return 0;
        }
    }
    return 1;
}

static int
expect_prefix(char** names, size_t count, const char* const* expected, size_t expected_count)
{
    if (count != expected_count) {
        fprintf(stderr, "prefix count mismatch: got %zu expected %zu\n", count, expected_count);
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], expected[i]) != 0) {
            fprintf(stderr, "prefix name %zu mismatch: got '%s' expected '%s'\n", i, names[i], expected[i]);
            return 0;
        }
    }
    return 1;
}

static int
test_environment_tokens(void)
{
    static const char* const expected[] = {"alpha", "beta"};
    char** names = NULL;
    size_t count;
    int ok;

    peak_target_config_test_reset_warning_counts();
    setenv("PEAK_TEST_TARGETS", ", alpha ,, beta, \t", 1);
    count = parse_env_w_delim("PEAK_TEST_TARGETS", ',', &names);
    ok = expect_names(names, count, expected, 2) &&
         peak_target_config_test_empty_token_warning_count() == 1;
    free_parsed_result(names, count);

    setenv("PEAK_TEST_TARGETS", "duplicate,duplicate", 1);
    names = NULL;
    count = parse_env_w_delim("PEAK_TEST_TARGETS", ',', &names);
    ok &= expect_names(names, count, (const char* const[]){"duplicate", "duplicate"}, 2);
    free_parsed_result(names, count);
    unsetenv("PEAK_TEST_TARGETS");
    return ok;
}

static int
test_target_file(void)
{
    char path[] = "/tmp/peak-target-config-XXXXXX";
    char long_name[4097];
    char** names = NULL;
    int fd;
    FILE* file;
    size_t count;
    int ok;

    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    fd = mkstemp(path);
    if (fd < 0 || (file = fdopen(fd, "w")) == NULL) {
        perror("target config temporary file");
        if (fd >= 0) {
            close(fd);
            unlink(path);
        }
        return 0;
    }
    if (fputs("\r\n  alpha  \r\n\n", file) == EOF ||
        fputs(long_name, file) == EOF || fputs("\r\nalpha\n", file) == EOF ||
        fclose(file) != 0) {
        perror("writing target config temporary file");
        unlink(path);
        return 0;
    }

    setenv("PEAK_TEST_TARGET_FILE", path, 1);
    count = load_profiling_symbols("PEAK_TEST_TARGET_FILE", &names, 0);
    ok = count == 3 && strcmp(names[0], "alpha") == 0 &&
         strlen(names[1]) == strlen(long_name) && strcmp(names[2], "alpha") == 0;
    if (!ok) fprintf(stderr, "file parser did not preserve expected nonempty lines\n");
    free_parsed_result(names, count);
    unsetenv("PEAK_TEST_TARGET_FILE");
    unlink(path);
    return ok;
}

static int
test_exact_groups(void)
{
    char** names = NULL;
    size_t count;
    int ok = 1;

    setenv("PEAK_TEST_TARGET_GROUP", "PBLAS", 1);
    count = load_symbols_from_array("PEAK_TEST_TARGET_GROUP", &names, 0);
    ok &= count == source_count_PBLAS && strcmp(names[0], source_target_array_PBLAS[0]) == 0;
    free_parsed_result(names, count);

    setenv("PEAK_TEST_TARGET_GROUP", "PBLASX, ScaLAPACKX", 1);
    names = NULL;
    count = load_symbols_from_array("PEAK_TEST_TARGET_GROUP", &names, 0);
    ok &= count == 0 && names == NULL;
    free_parsed_result(names, count);

    setenv("PEAK_TEST_TARGET_GROUP", ", PBLAS ,, BLAS ,", 1);
    names = NULL;
    count = load_symbols_from_array("PEAK_TEST_TARGET_GROUP", &names, 0);
    ok &= count == source_count_PBLAS + source_count_BLAS;
    free_parsed_result(names, count);
    unsetenv("PEAK_TEST_TARGET_GROUP");
    if (!ok) fprintf(stderr, "group parser did not use exact comma-separated matching\n");
    return ok;
}

static int
test_allocation_failure(void)
{
    static const char* const env_expected[] = {"alpha", "beta", "gamma"};
    static const size_t env_counts[] = {0, 0, 0, 1, 2, 3, 3};
    static const char* const long_env_expected[] = {
        "a", "b", "c", "d", "e", "f", "g", "h", "i"
    };
    static const size_t file_counts[] = {0, 0, 1, 2, 2};
    static const char* const file_expected[] = {"alpha", "beta"};
    static const size_t group_counts[] = {0, 0, 0, 1, 1, 2};
    char path[] = "/tmp/peak-target-config-fail-XXXXXX";
    char** names = NULL;
    char** original;
    FILE* file;
    int fd;
    size_t count;
    int ok = 1;

    setenv("PEAK_TEST_TARGETS", "alpha,beta,gamma", 1);
    for (size_t allowed = 0; allowed < sizeof(env_counts) / sizeof(env_counts[0]); allowed++) {
        names = NULL;
        peak_target_config_test_fail_allocations_after(allowed);
        count = parse_env_w_delim("PEAK_TEST_TARGETS", ',', &names);
        ok &= expect_prefix(names, count, env_expected, env_counts[allowed]);
        free_parsed_result(names, count);
    }

    setenv("PEAK_TEST_TARGETS", "a,b,c,d,e,f,g,h,i", 1);
    names = NULL;
    peak_target_config_test_fail_allocations_after(10);
    count = parse_env_w_delim("PEAK_TEST_TARGETS", ',', &names);
    ok &= expect_prefix(names, count, long_env_expected, 8);
    free_parsed_result(names, count);
    names = NULL;
    peak_target_config_test_fail_allocations_after(11);
    count = parse_env_w_delim("PEAK_TEST_TARGETS", ',', &names);
    ok &= expect_prefix(names, count, long_env_expected, 8);
    free_parsed_result(names, count);
    names = NULL;
    peak_target_config_test_fail_allocations_after(12);
    count = parse_env_w_delim("PEAK_TEST_TARGETS", ',', &names);
    ok &= expect_prefix(names, count, long_env_expected, 9);
    free_parsed_result(names, count);

    fd = mkstemp(path);
    if (fd < 0 || (file = fdopen(fd, "w")) == NULL) {
        perror("target config failure temporary file");
        if (fd >= 0) {
            close(fd);
            unlink(path);
        }
        return 0;
    }
    if (fputs("alpha\nbeta\n", file) == EOF || fclose(file) != 0) {
        perror("writing target config failure temporary file");
        unlink(path);
        return 0;
    }
    setenv("PEAK_TEST_TARGET_FILE", path, 1);
    for (size_t allowed = 0; allowed < sizeof(file_counts) / sizeof(file_counts[0]); allowed++) {
        names = NULL;
        peak_target_config_test_fail_allocations_after(allowed);
        count = load_profiling_symbols("PEAK_TEST_TARGET_FILE", &names, 0);
        ok &= expect_prefix(names, count, file_expected, file_counts[allowed]);
        free_parsed_result(names, count);
    }

    peak_target_config_test_fail_allocations_after(SIZE_MAX);
    names = malloc(sizeof(*names));
    if (names == NULL) return 0;
    names[0] = strdup("existing");
    if (names[0] == NULL) {
        free(names);
        return 0;
    }
    original = names;
    peak_target_config_test_fail_allocations_after(1);
    count = load_profiling_symbols("PEAK_TEST_TARGET_FILE", &names, 1);
    ok &= count == 0 && names == original && strcmp(names[0], "existing") == 0;
    peak_target_config_test_fail_allocations_after(SIZE_MAX);
    free_parsed_result(names, 1);

    setenv("PEAK_TEST_TARGET_GROUP", "PBLAS", 1);
    for (size_t allowed = 0; allowed < sizeof(group_counts) / sizeof(group_counts[0]); allowed++) {
        peak_target_config_test_fail_allocations_after(SIZE_MAX);
        names = malloc(sizeof(*names));
        if (names == NULL) return 0;
        names[0] = strdup("existing");
        if (names[0] == NULL) {
            free(names);
            return 0;
        }
        original = names;
        peak_target_config_test_fail_allocations_after(allowed);
        count = load_symbols_from_array("PEAK_TEST_TARGET_GROUP", &names, 1);
        ok &= count == group_counts[allowed] && names != NULL &&
              strcmp(names[0], "existing") == 0;
        for (size_t i = 0; i < count; i++) {
            ok &= strcmp(names[i + 1], source_target_array_PBLAS[i]) == 0;
        }
        if (allowed == 2) {
            /* The first grow failure cannot replace or lose the original array. */
            ok &= names == original;
        }
        peak_target_config_test_fail_allocations_after(SIZE_MAX);
        free_parsed_result(names, count + 1);
    }
    peak_target_config_test_fail_allocations_after(SIZE_MAX);
    unsetenv("PEAK_TEST_TARGETS");
    unsetenv("PEAK_TEST_TARGET_GROUP");
    unsetenv("PEAK_TEST_TARGET_FILE");
    unlink(path);
    if (!ok) fprintf(stderr, "allocation failure did not preserve initialized targets\n");
    return ok;
}

int
main(void)
{
    int ok = test_environment_tokens();
    ok &= test_target_file();
    ok &= test_exact_groups();
    ok &= test_allocation_failure();
    if (!ok) return EXIT_FAILURE;
    puts("target_config_ok");
    return EXIT_SUCCESS;
}
