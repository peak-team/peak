#ifndef PEAK_TARGET_CONFIG_H
#define PEAK_TARGET_CONFIG_H

/**
 * @file target_config.h
 * @brief Target-list parsing and built-in target-group loading.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Splits an environment variable into tokens using a delimiter.
 *
 * An unset or empty value stores NULL in @p result and returns zero. Otherwise
 * the environment string is duplicated before tokenization and is not
 * modified. Tokens are trimmed; leading, consecutive, trailing, and
 * whitespace-only fields are ignored.
 *
 * Assuming all required allocations succeed, both the array and every token
 * are newly allocated. The caller owns them and must pass the returned count
 * to free_parsed_result().
 *
 * @param[in] env_var Name of the environment variable to parse.
 * @param[in] a_delim Non-NUL delimiter character.
 * @param[out] result Receives the newly allocated token array, or NULL for an
 *                    unset or empty value.
 * @return The number of successfully appended owned strings. Allocation
 *         failures return the successfully appended prefix.
 * @pre @p env_var and @p result are not NULL.
 * @pre @p a_delim is not @c '\0'.
 */
size_t parse_env_w_delim(const char* env_var, const char a_delim, char*** result);

/**
 * @brief Appends target symbols from an external configuration file.
 *
 * The environment variable named by config_file supplies the file path. Lines
 * are read with getline(), trimmed (including CRLF), and empty lines are
 * ignored. The caller retains ownership of the reallocated array and all
 * strings and must eventually release them with free_parsed_result().
 *
 * @param[in] config_file Name of the environment variable containing the path.
 * @param[in,out] result Address of a heap-allocated string array to extend.
 * @param[in] existing_count Number of initialized entries already in @p result.
 * @return The actual number of successfully appended target names.
 * @pre @p config_file and @p result are not NULL.
 * @pre @p *result is NULL or a realloc-compatible allocation; its first
 *      @p existing_count entries are initialized, heap-owned strings.
 */
size_t load_profiling_symbols(const char* config_file, char*** result, size_t existing_count);

/**
 * @brief Appends symbols from the built-in target groups selected by an environment variable.
 *
 * Selection uses exact, case-sensitive comma-separated group names: BLAS,
 * LAPACK, PBLAS, ScaLAPACK, and FFTW. Tokens are trimmed and each selected
 * symbol is duplicated and appended at @p existing_count.
 *
 * @param[in] env_var Name of the environment variable selecting target groups.
 * @param[in,out] result Address of a heap-allocated string array to extend.
 * @param[in] existing_count Number of initialized entries already in @p result.
 * @return The number of successfully appended symbols.
 * @pre @p env_var and @p result are not NULL.
 * @pre @p *result is NULL or a realloc-compatible allocation; its first
 *      @p existing_count entries are initialized, heap-owned strings.
 */
size_t load_symbols_from_array(const char* env_var, char*** result, size_t existing_count);

/**
 * @brief Frees an owned target-string array and its elements.
 *
 * This accepts arrays created by parse_env_w_delim() and arrays extended by the
 * two loader functions. It frees exactly the first @p count strings and then
 * the array. For an extended array, callers must pass the total initialized
 * count (`existing_count` plus all appended counts), not only the latest
 * loader's return value. Passing NULL with a zero count is supported.
 *
 * @param[in] result Owned string array to release.
 * @param[in] count Number of initialized owned strings in @p result.
 * @pre @p result is NULL and @p count is zero, or @p result contains at least
 *      @p count entries that may each be passed to free().
 */
void free_parsed_result(char** result, size_t count);

#ifdef PEAK_TARGET_CONFIG_TESTING
/** Fail allocations after @p successful_allocations allocation attempts. */
void peak_target_config_test_fail_allocations_after(size_t successful_allocations);
/** Reset the target-parser warning counters used by unit tests. */
void peak_target_config_test_reset_warning_counts(void);
/** Return the number of empty-token warnings emitted since the last reset. */
size_t peak_target_config_test_empty_token_warning_count(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PEAK_TARGET_CONFIG_H */
