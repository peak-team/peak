#ifndef __ENV_PARSER_H
#define __ENV_PARSER_H

/**
 * @file env_parser.h
 * @brief Header file for PEAK's environment variables related functions.
 */

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>

/**
 * @brief Splits a string into an array of substrings based on a given delimiter.
 *
 * This function splits a given string into an array of substrings based on the specified delimiter character.
 * The resulting array is dynamically allocated and should be freed by the caller using `free()`.
 * If the string does not contain the delimiter, the resulting array will have a single element containing the entire string.
 *
 * @param env_var The environment variable to parse.
 * @param a_delim A pointer to a char to be used as the delimiter.
 * @param result A pointer to an array of strings to be allocated and filled by this function.
 * @return 0 if the env does not exist, or the number of strings in the array.
 */
size_t parse_env_w_delim(const char* env_var, const char a_delim, char*** result);

/**
 * @brief Read lines from a outside configuration file.
 *
 * This function reads lines from a outside configuration file by lines and add them to the resulting array.
 * The resulting array is dynamically allocated and should be freed by the caller using `free()`.
 *
 * @param config_file The environment variable to parse.
 * @param result A pointer to an array of strings to be allocated and filled by this function.
 * @param existing_count The current size of the resulting array.
 * @return 0 if the env does not exist, or the number of lines read from the configuration file.
 */
size_t load_profiling_symbols(const char* config_file, char*** result, size_t existing_count);

/**
 * @brief Read strings from a array.
 *
 * This function reads strings from a array and add them to the resulting array.
 * The resulting array is dynamically allocated and should be freed by the caller using `free()`.
 *
 * @param source_array The array to read from.
 * @param source_array The count of the strings in the array.
 * @param result A pointer to an array of strings to be allocated and filled by this function.
 * @param existing_count The current size of the resulting array.
 * @return 0 if the env does not exist, or the number of strings read from the array.
 */
size_t load_symbols_from_array(const char* env_var, char*** result, size_t existing_count);
/**
 * @brief Parses a floating point number from an environment variable.
 *
 * This function retrieves the value of an environment variable as a string
 * and parses it as a floating point number using the standard library
 * function strtof().
 *
 * @param env_var The name of the environment variable to parse.
 * @return The parsed floating point value, or 0.0 if the environment
 *         variable is not set, is empty, or contains invalid characters.
 */
float parse_env_to_float(const char* env_var);

/**
 * @brief Parses an unsigned integer time value from an environment variable.
 *
 * This function retrieves the value of an environment variable as a string
 * and attempts to parse it as an unsigned integer using the standard library
 * function strtoul(). If the environment variable is not set, is empty, or 
 * contains invalid characters, a default value of 1000000 is returned.
 *
 * @param env_var The name of the environment variable to parse.
 * @return The parsed unsigned integer value, or 1000000 if parsing fails.
 */
unsigned int parse_env_to_time(const char* env_var);

/**
 * @brief Parses an unsigned integer interval value from an environment variable.
 *
 * This function retrieves the value of an environment variable as a string
 * and attempts to parse it as an unsigned integer using the standard library
 * function strtoul(). If the environment variable is not set, is empty, or 
 * contains invalid characters, a default value of 5 is returned.
 *
 * @param env_var The name of the environment variable to parse.
 * @return The parsed unsigned integer value, or 5 if parsing fails.
 */
unsigned int parse_env_to_interval(const char* env_var);

/**
 * @brief Parses an unsigned integer interval value (in nanoseconds) from an environment variable.
 *
 * This function retrieves the value of an environment variable as a string
 * and attempts to parse it as an unsigned integer using the standard library
 * function strtoul(). If the environment variable is not set, is empty, or 
 * contains invalid characters, a default value of 10000000 is returned.
 *
 * @param env_var The name of the environment variable to parse.
 * @return The parsed unsigned integer value, or 10000000 if parsing fails.
 */
unsigned int parse_env_to_post_interval(const char* env_var);

/**
 * @brief Parses a boolean value from an environment variable.
 *
 * This function retrieves the value of an environment variable as a string
 * and interprets it as a boolean. The function returns true if the value 
 * is "true" (case-insensitive) or "1", and false otherwise. If the environment 
 * variable is not set, false is returned by default.
 *
 * @param env_var The name of the environment variable to parse.
 * @return The parsed boolean value, or false if parsing fails.
 */
bool parse_env_to_bool(const char* env_var);

/**
 * @brief Frees the memory allocated by parse_env_w_delim function.
 *
 * This function frees the memory allocated by the parse_env_w_delim function
 * for the result variable. The function should be called when the result variable
 * is no longer needed to avoid memory leaks.
 *
 * @param result A pointer to the result variable.
 * @param count The number of elements in the result variable.
 * @return void
 */
void free_parsed_result(char** result, size_t count);

#endif /* __ENV_PARSER_H */
