#include "env_parser.h"
#include "source_target.h"

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

size_t count_lines_in_file(FILE* file) {
    size_t lines = 0;
    int ch;
    int prev_ch = '\0';
    while ((ch = fgetc(file)) != EOF) {
        if (ch == '\n' && prev_ch != '\n') {
            lines++;
        }
        prev_ch = ch;
    }

    // If the last character read wasn't a newline, then we have one last line
    if (prev_ch != '\n' && prev_ch != EOF) {
        lines++;
    }
    rewind(file);
    return lines;
}

size_t load_profiling_symbols(const char* config_file, char*** result, size_t existing_count) {
    char* a_str = getenv(config_file);
    if (a_str == NULL) {
        return 0;
    }
    FILE* file = fopen(a_str, "r");
    if (!file) {
        printf("Can't find the configuration file!\n");
        return 0;
    }
    char line[256];
    size_t count = 0;
    size_t capacity = count_lines_in_file(file);
    *result = realloc(*result, sizeof(char*) * (existing_count + capacity));
    if (*result) {
        while (fgets(line, sizeof(line), file)) {
            line[strcspn(line, "\n")] = 0;
            (*result)[count + existing_count] = strdup(line);
            count ++;
        }
        fclose(file);
        return capacity;
    }
    return 0;
}

size_t load_symbols_from_array(const char* env_var, char*** result, size_t existing_count) {
    char* a_str = getenv(env_var);
    if (a_str == NULL) {
        return 0;
    }

    size_t source_count = 0;
    if (strstr(a_str, "BLAS")) {
        *result = realloc(*result, sizeof(char*) * (existing_count + source_count_BLAS));
        for (size_t i = 0; i < source_count_BLAS; i++) {
            (*result)[i + existing_count] = strdup(source_target_array_BLAS[i]);
            if ((*result)[existing_count + i] == NULL) {
                printf("Failed to duplicate string!\n");
                return i + source_count;
            }
        }
        source_count += source_count_BLAS;
        existing_count += source_count_BLAS;
    }

    if (strstr(a_str, "LAPACK")) {
        *result = realloc(*result, sizeof(char*) * (existing_count + source_count_LAPACK));
        for (size_t i = 0; i < source_count_LAPACK; i++) {
            (*result)[i + existing_count] = strdup(source_target_array_LAPACK[i]);
            if ((*result)[existing_count + i] == NULL) {
                printf("Failed to duplicate string!\n");
                return i + source_count;
            }
        }
        source_count += source_count_LAPACK;
        existing_count += source_count_LAPACK;
    }

    if (strstr(a_str, "PBLAS")) {
        *result = realloc(*result, sizeof(char*) * (existing_count + source_count_PBLAS));
        for (size_t i = 0; i < source_count_PBLAS; i++) {
            (*result)[i + existing_count] = strdup(source_target_array_PBLAS[i]);
            if ((*result)[existing_count + i] == NULL) {
                printf("Failed to duplicate string!\n");
                return i + source_count;
            }
        }
        source_count += source_count_PBLAS;
        existing_count += source_count_PBLAS;
    }

    if (strstr(a_str, "ScaLAPACK")) {
        *result = realloc(*result, sizeof(char*) * (existing_count + source_count_ScaLAPACK));
        for (size_t i = 0; i < source_count_ScaLAPACK; i++) {
            (*result)[i + existing_count] = strdup(source_target_array_ScaLAPACK[i]);
            if ((*result)[existing_count + i] == NULL) {
                printf("Failed to duplicate string!\n");
                return i + source_count;
            }
        }
        source_count += source_count_ScaLAPACK;
        existing_count += source_count_ScaLAPACK;
    }

    if (strstr(a_str, "FFTW")) {
        *result = realloc(*result, sizeof(char*) * (existing_count + source_count_FFTW));
        for (size_t i = 0; i < source_count_FFTW; i++) {
            (*result)[i + existing_count] = strdup(source_target_array_FFTW[i]);
            if ((*result)[existing_count + i] == NULL) {
                printf("Failed to duplicate string!\n");
                return i + source_count;
            }
        }
        source_count += source_count_FFTW;
        existing_count += source_count_FFTW;
    }

    return source_count;
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

unsigned int parse_env_to_time(const char* env_var) {
    char* varvalue = getenv(env_var);
    if (varvalue == NULL) {
        return 1000000;
    }

    char* endptr;
    errno = 0; 
    unsigned int result = strtoul(varvalue, &endptr, 10);\
    if (errno == ERANGE || result > UINT_MAX || *endptr != '\0') {
        return 1000000;
    }
    return (unsigned int)result;
}

unsigned int parse_env_to_interval(const char* env_var) {
    char* varvalue = getenv(env_var);
    if (varvalue == NULL) {
        return 5;
    }

    char* endptr;
    errno = 0; 
    unsigned int result = strtoul(varvalue, &endptr, 10);
    if (errno == ERANGE || result > UINT_MAX || *endptr != '\0') {
        return 5;
    }
    return (unsigned int)result;
}

unsigned int parse_env_to_post_interval(const char* env_var) {
    char* varvalue = getenv(env_var);
    if (varvalue == NULL) {
        return 1;
    }

    char* endptr;
    errno = 0; 
    unsigned int result = strtoul(varvalue, &endptr, 10);
    if (errno == ERANGE || result > UINT_MAX || *endptr != '\0') {
        return 1;
    }
    return (unsigned int)result;
}

bool parse_env_to_bool(const char* env_var) {
    char* varvalue = getenv(env_var);
    if (varvalue == NULL) {
        return false;
    }
    
    if (strcasecmp(varvalue, "true") == 0 || strcmp(varvalue, "1") == 0) {
        return true;
    }
    return false;
}

void free_parsed_result(char** result, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(result[i]);
    }
    free(result);
}
