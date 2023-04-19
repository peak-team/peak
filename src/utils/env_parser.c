#include "env_parser.h"

int parse_env_w_comma(const char *env_var, char ***result) {
    const char *p = getenv(env_var);
    if (p == NULL) {
        *result = NULL;
        return 0;
    }
    char *str = strdup(p);
    int count = 0;
    char *token = strtok(str, ",");
    *result = malloc(sizeof(char*));
    while (token != NULL) {
        *result = realloc(*result, sizeof(char*) * (count + 1));
        (*result)[count] = malloc(sizeof(char) * (strlen(token) + 1));
        strcpy((*result)[count], token);
        count++;
        token = strtok(NULL, ",");
    }
    free(str);
    return count;
}
