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

#endif /* ENV_PARSER_H */
