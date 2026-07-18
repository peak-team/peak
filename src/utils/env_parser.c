#include "env_parser.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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


float parse_env_to_float_ratio(const char* env_var)
{
    char* varvalue = getenv(env_var);
    if (varvalue == NULL) {
        // Environment variable is not set or is empty
        return 0.1f;
    }

    // Parse the string as a floating point number
    char* endptr;
    float result = strtof(varvalue, &endptr);

    // Check for errors during parsing
    if (*endptr != '\0') {
        // The string contains invalid characters
        return 0.1f;
    }

    return result;
}


float parse_env_to_float_detach_factor(const char* env_var)
{
    char* varvalue = getenv(env_var);
    if (varvalue == NULL) {
        // Environment variable is not set or is empty
        return 1.2f;
    }

    // Parse the string as a floating point number
    char* endptr;
    float result = strtof(varvalue, &endptr);

    // Check for errors during parsing
    if (*endptr != '\0') {
        // The string contains invalid characters
        return 1.2f;
    }

    return result;
}

float parse_env_to_float_reattach_factor(const char* env_var)
{
    char* varvalue = getenv(env_var);
    if (varvalue == NULL) {
        // Environment variable is not set or is empty
        return 0.85f;
    }

    // Parse the string as a floating point number
    char* endptr;
    float result = strtof(varvalue, &endptr);

    // Check for errors during parsing
    if (*endptr != '\0') {
        // The string contains invalid characters
        return 0.85f;
    }

    return result;
}


unsigned int parse_env_to_time(const char* env_var) {
    const double max_heartbeat_seconds = (double)UINT_MAX / 1e6;
    char* varvalue = getenv(env_var);
    if (varvalue == NULL) {
        return 100000;
    }

    char* endptr;
    errno = 0;
    double seconds = strtod(varvalue, &endptr);

    if (errno == ERANGE || seconds < 0 || endptr == varvalue || *endptr != '\0') {
        return 100000;
    }

    if (seconds > max_heartbeat_seconds) {
        return UINT_MAX;
    }

    return (unsigned int)(seconds * 1e6);
}

unsigned int parse_env_to_interval(const char* env_var) {
    char* varvalue = getenv(env_var);
    if (varvalue == NULL) {
        return 50;
    }

    char* endptr;
    errno = 0; 
    unsigned int result = strtoul(varvalue, &endptr, 10);
    if (errno == ERANGE || result > UINT_MAX || *endptr != '\0') {
        return 50;
    }
    return (unsigned int)result;
}

unsigned int parse_env_to_uint_default(const char* env_var, unsigned int default_value) {
    char* varvalue = getenv(env_var);
    if (varvalue == NULL) {
        return default_value;
    }

    char* endptr;
    errno = 0;
    unsigned long result = strtoul(varvalue, &endptr, 10);
    if (errno == ERANGE || result > UINT_MAX || endptr == varvalue || *endptr != '\0') {
        return default_value;
    }
    return (unsigned int)result;
}

double parse_env_to_double_default(const char* env_var, double default_value) {
    char* varvalue = getenv(env_var);
    if (varvalue == NULL) {
        return default_value;
    }

    char* endptr;
    errno = 0;
    double result = strtod(varvalue, &endptr);
    if (errno == ERANGE || endptr == varvalue || *endptr != '\0') {
        return default_value;
    }
    return result;
}

unsigned long long parse_env_to_post_interval(const char* env_var) {
    char* varvalue = getenv(env_var);
    if (varvalue == NULL) {
        return 10000000ULL;
    }

    char* endptr;
    errno = 0;
    double seconds = strtod(varvalue, &endptr);

    if (errno == ERANGE || seconds < 0 || endptr == varvalue || *endptr != '\0') {
        return 10000000ULL;
    }

    return (unsigned long long)(seconds * 1e9);
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
