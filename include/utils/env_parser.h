#ifndef ENV_PARSER_H
#define ENV_PARSER_H

/**
 * @file env_parser.h
 * @brief Header file for the env_parser functions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Parses a comma-separated environment variable into an array of strings.
 *
 * This function takes a comma-separated environment variable as input and returns an array
 * of strings that correspond to the comma-separated values in the input. The array is
 * dynamically allocated and must be freed by the caller when it is no longer needed.
 *
 * @param env_var The environment variable to parse.
 * @param result A pointer to an array of strings to be allocated and filled by this function.
 * @param size A pointer to an integer that will be set to the number of strings in the array.
 * @return 0 if the env does not exist, or the number of strings in the array.
 */
int parse_env_w_comma(const char *env_var, char ***result);

#endif /* ENV_PARSER_H */
