#ifndef PEAK_TARGET_CONFIG_H
#define PEAK_TARGET_CONFIG_H

/**
 * @file target_config.h
 * @brief Target-list parsing and built-in target-group loading.
 */

#include <stddef.h>

/**
 * @brief Splits an environment variable into tokens using a delimiter.
 *
 * The resulting array is dynamically allocated and should be released with
 * free_parsed_result(). If the value does not contain the delimiter, the array
 * has one element containing the complete value.
 *
 * @param env_var The environment variable to parse.
 * @param a_delim The delimiter character.
 * @param result A pointer to an array of strings to be allocated and filled by this function.
 * @return 0 if the env is unset or empty, or the number of strings in the array.
 */
size_t parse_env_w_delim(const char* env_var, const char a_delim, char*** result);

/**
 * @brief Appends target symbols from an external configuration file.
 *
 * The environment variable named by config_file supplies the file path. Each
 * line is appended to the dynamically allocated result array.
 *
 * @param config_file The name of the environment variable containing the path.
 * @param result A pointer to an array of strings to be allocated and filled by this function.
 * @param existing_count The current size of the resulting array.
 * @return 0 if the env is unset or empty, or the number of lines read from the configuration file.
 */
size_t load_profiling_symbols(const char* config_file, char*** result, size_t existing_count);

/**
 * @brief Appends symbols from the built-in target groups selected by an environment variable.
 *
 * Recognized group names include BLAS, LAPACK, PBLAS, ScaLAPACK, and FFTW.
 * Matching group entries are appended to the dynamically allocated result
 * array.
 *
 * @param env_var The name of the environment variable selecting target groups.
 * @param result A pointer to an array of strings to be allocated and filled by this function.
 * @param existing_count The current size of the resulting array.
 * @return 0 if the environment variable is unset, or the number of appended symbols.
 */
size_t load_symbols_from_array(const char* env_var, char*** result, size_t existing_count);

/**
 * @brief Frees a token array returned by parse_env_w_delim().
 *
 * This function frees the memory allocated by the parse_env_w_delim function
 * for the result variable. The function should be called when the result variable
 * is no longer needed to avoid memory leaks.
 *
 * @param result A pointer to the result variable.
 * @param count The number of elements in the result variable.
 */
void free_parsed_result(char** result, size_t count);

#endif /* PEAK_TARGET_CONFIG_H */
