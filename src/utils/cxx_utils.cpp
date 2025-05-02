#include "cxx_utils.h"
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>

/**
 * Removes the function parameter list from the symbol.
 * Properly handles nested parentheses.
 *
 * @param s Demangled symbol.
 * @return Newly allocated string without parameters.
 */
char* removeParameterList(const char* s) {
    const char* end = strrchr(s, ')');
    if (!end) return strdup(s);

    int depth = 1;
    const char* ptr = end - 1;
    while (ptr >= s && depth > 0) {
        if (*ptr == '(') --depth;
        else if (*ptr == ')') ++depth;
        --ptr;
    }
    size_t len = (size_t)(ptr - s + 1);
    return strndup(s, len);
}

/**
 * Removes template arguments at the end of the string, only if the string ends with '>'.
 *
 * @param s Symbol string without parameters.
 * @return Newly allocated string without trailing template arguments.
 */
 char* removeTrailingTemplateArgs(const char* s) {
    size_t len = strlen(s);
    if (len == 0 || s[len - 1] != '>') {
        return strdup(s); // does not end with '>', nothing to remove
    }

    int depth = 1;
    const char* ptr = s + len - 2;
    while (ptr >= s && depth > 0) {
        if (*ptr == '<') --depth;
        else if (*ptr == '>') ++depth;
        --ptr;
    }
    if (depth == 0) {
        size_t result_len = (size_t)(ptr - s + 1);
        return strndup(s, result_len);
    } else {
        return strdup(s);
    }
}

/**
 * Removes return type if it exists, based on the last space.
 * Assumes space separates return type and qualified function name.
 *
 * @param s Input string.
 * @return Newly allocated string without return type.
 */
 char* removeReturnTypeIfExist(const char* s) {
    const char* last_space = strrchr(s, ' ');
    return last_space ? strdup(last_space + 1) : strdup(s);
}

/**
 * Extracts the final function name (last component after namespace).
 *
 * @param s Input string (after stripping params/templates/return).
 * @return Newly allocated string with only the function name.
 */
char* extractFinalFunctionName(const char* s) {
    const char* lastColon = strrchr(s, ':');
    return lastColon ? strdup(lastColon + 1) : strdup(s);
}

extern "C" char* cxa_demangle(const char* name) {
    int status = 0;
    char* result = abi::__cxa_demangle(name, 0, 0, &status);
    return (status == 0) ? result : strdup(name);
}

extern "C" int cxa_demangle_status(const char* mangled_name) {
    int status = 0;
    char* result = abi::__cxa_demangle(mangled_name, 0, 0, &status);
    free(result);
    return status;
}

extern "C" char* extract_function_name(const char* demangled) {
    char* noParams = removeParameterList(demangled);
    char* noTemplate = removeTrailingTemplateArgs(noParams);
    char* noReturn = removeReturnTypeIfExist(noTemplate);
    char* nameOnly = extractFinalFunctionName(noReturn);

    free(noParams);
    free(noTemplate);
    free(noReturn);
    return nameOnly;
}

char* truncate_string(const char* s, int max_len) {
    int len = strlen(s);
    if (len <= max_len) return strdup(s);

    char* result = (char*)malloc(max_len + 1);
    strncpy(result, s, max_len - 3);
    strcpy(result + max_len - 3, "...");
    return result;
}

