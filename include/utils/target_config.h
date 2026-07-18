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
 * modified. A single trailing delimiter is ignored.
 *
 * Assuming all required allocations succeed, both the array and every token
 * are newly allocated. The caller owns them and must pass the returned count
 * to free_parsed_result().
 *
 * @param[in] env_var Name of the environment variable to parse.
 * @param[in] a_delim Non-NUL delimiter character.
 * @param[out] result Receives the newly allocated token array, or NULL for an
 *                    unset or empty value.
 * @return Zero for an unset or empty value; otherwise the number of owned
 *         strings in @p result for supported input.
 * @pre @p env_var and @p result are not NULL.
 * @pre @p a_delim is not @c '\0'.
 * @pre A nonempty environment value contains at least one @p a_delim, does not
 *      begin with it, and does not contain adjacent delimiters. Delimiter-free,
 *      leading-delimiter, and adjacent-delimiter forms are outside the current
 *      implementation's supported input.
 * @pre Every required allocation succeeds; allocation failure has no reliable,
 *      recoverable result contract in the current implementation.
 */
size_t parse_env_w_delim(const char* env_var, const char a_delim, char*** result);

/**
 * @brief Appends target symbols from an external configuration file.
 *
 * The environment variable named by config_file supplies the file path. Each
 * line is copied, with its newline removed, and appended at @p existing_count.
 * Assuming all required allocations succeed, the caller retains ownership of
 * the reallocated array and all strings and must eventually release them with
 * free_parsed_result(). An unset or empty path, or a file-open failure, returns
 * zero without appending entries.
 *
 * @param[in] config_file Name of the environment variable containing the path.
 * @param[in,out] result Address of a heap-allocated string array to extend.
 * @param[in] existing_count Number of initialized entries already in @p result.
 * @return Zero when no entries are appended; otherwise the number of appended
 *         file lines for supported input.
 * @pre @p config_file and @p result are not NULL.
 * @pre @p *result is NULL or a realloc-compatible allocation; its first
 *      @p existing_count entries are initialized, heap-owned strings.
 * @pre The configuration file is nonempty, contains no consecutive newline
 *      bytes or embedded NUL bytes, remains unchanged across the counting and
 *      reading passes, and each logical line is consumed by one successful
 *      fgets() call with a 256-byte buffer (at most 254 bytes before a newline,
 *      or 255 bytes for a final line without a newline). fgetc() and rewind()
 *      must also succeed. Other file shapes and I/O failures are outside the
 *      loader's supported input because its two passes can disagree.
 * @pre Size arithmetic does not overflow and every required realloc() and
 *      strdup() succeeds; allocation failure has no reliable recovery contract.
 */
size_t load_profiling_symbols(const char* config_file, char*** result, size_t existing_count);

/**
 * @brief Appends symbols from the built-in target groups selected by an environment variable.
 *
 * Selection uses case-sensitive substring searches in the fixed order BLAS,
 * LAPACK, PBLAS, ScaLAPACK, and FFTW. Consequently @c PBLAS also selects BLAS,
 * and @c ScaLAPACK also selects LAPACK. Each selected symbol is duplicated and
 * appended at @p existing_count. Assuming all allocations succeed, the caller
 * owns the reallocated array and all appended strings and must eventually use
 * free_parsed_result().
 *
 * @param[in] env_var Name of the environment variable selecting target groups.
 * @param[in,out] result Address of a heap-allocated string array to extend.
 * @param[in] existing_count Number of initialized entries already in @p result.
 * @return Zero if the value is unset or contains no recognized substring;
 *         otherwise the number of appended symbols.
 * @pre @p env_var and @p result are not NULL.
 * @pre @p *result is NULL or a realloc-compatible allocation; its first
 *      @p existing_count entries are initialized, heap-owned strings.
 * @pre Size arithmetic does not overflow and every required realloc() and
 *      strdup() succeeds; allocation failure has no reliable recovery contract.
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

#ifdef __cplusplus
}
#endif

#endif /* PEAK_TARGET_CONFIG_H */
