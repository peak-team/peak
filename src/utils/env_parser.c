#include "env_parser.h"

size_t parse_env_w_delim(const char* env_var, const char a_delim, char*** result)
{
    char* a_str = getenv(env_var);
    if (a_str == NULL) {
        *result = NULL;
        return 0;
    }

    *result = 0;
    size_t count = 0;
    char* tmp = a_str;
    char* last_comma = 0;
    char delim[2];
    delim[0] = a_delim;
    delim[1] = 0;

    /* Count how many elements will be extracted. */
    while (*tmp) {
        if (a_delim == *tmp) {
            count++;
            last_comma = tmp;
        }
        tmp++;
    }

    /* Add space for trailing token. */
    count += last_comma < (a_str + strlen(a_str) - 1);
    char* a_str_dup = strdup(a_str);

    *result = malloc(sizeof(char*) * count);

    if (*result) {
        size_t idx = 0;
        char* token = strtok(a_str_dup, delim);

        while (token) {
            assert(idx < count);
            *(*result + idx++) = strdup(token);
            token = strtok(0, delim);
        }
    }
    free(a_str_dup);

    return count;
}

float parse_env_to_float(const char* env_var)
{
    char* varvalue = getenv(env_var);
    if (varvalue == NULL) {
        // Environment variable is not set or is empty
        return 0.0f;
    }

    // Parse the string as a floating point number
    char* endptr;
    float result = strtof(varvalue, &endptr);

    // Check for errors during parsing
    if (*endptr != '\0') {
        // The string contains invalid characters
        return 0.0f;
    }

    return result;
}

void free_parsed_result(char** result, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(result[i]);
    }
    free(result);
}
