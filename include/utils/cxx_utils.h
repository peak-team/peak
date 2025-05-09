#ifndef CXX_UTILS_H
#define CXX_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Demangles a C++ mangled symbol name (cxa).
 *
 * @param mangled_name The mangled symbol (e.g., "_Z3fooi").
 * @return A newly allocated string with the demangled name.
 *         Caller must free the returned string using free().
 */
char* cxa_demangle(const char* name);

/**
 * Return demangle status for input string (cxa).
 *
 * @param name
 * @return Status code for demangle operation on input string
 */
 int cxa_demangle_status(const char* mangled_name);

/**
 * Extracts the function name only (without return type, namespace, templates, or parameters).
 *
 * @param demangled A demangled C++ function signature.
 * @return The bare function name (e.g., "MyKernel").
 */
char* extract_function_name(const char* demangled);

/**
 * @brief Truncate a string and append ellipsis if it exceeds the specified length.
 *
 * This function creates a new dynamically allocated string. If the input string
 * `s` is longer than `max_len`, the returned string will contain the first
 * (max_len - 3) characters followed by "...". Otherwise, it returns a copy of the
 * original string.
 *
 * @param s The input null-terminated string to truncate.
 * @param max_len The maximum allowed length of the output string (including ellipsis).
 * @return A newly allocated null-terminated string. The caller is responsible for freeing it.
 */
char* truncate_string(const char* s, int max_len);

#ifdef __cplusplus
}
#endif

#endif // CXX_UTILS_H