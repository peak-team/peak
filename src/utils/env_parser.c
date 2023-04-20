#include "env_parser.h"

size_t parse_env_w_delim(const char *env_var, const char a_delim, char ***result) {
    char *a_str = getenv(env_var);
    if (a_str == NULL) {
        *result = NULL;
        return 0;
    }

    *result    = 0;
    size_t count     = 0;
    char* tmp        = a_str;
    char* last_comma = 0;
    char delim[2];
    delim[0] = a_delim;
    delim[1] = 0;

    /* Count how many elements will be extracted. */
    while (*tmp)
    {
        if (a_delim == *tmp)
        {
            count++;
            last_comma = tmp;
        }
        tmp++;
    }

    /* Add space for trailing token. */
    count += last_comma < (a_str + strlen(a_str) - 1);

    *result = malloc(sizeof(char*) * count);

    if (*result)
    {
        size_t idx  = 0;
        char* token = strtok(a_str, delim);

        while (token)
        {
            assert(idx < count);
            *(*result + idx++) = strdup(token);
            token = strtok(0, delim);
        }
        assert(idx == count - 1);
    }

    return count;
}
