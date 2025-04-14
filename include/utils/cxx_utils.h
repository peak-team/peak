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
 char* cxa_demangle(const char* mangled_name);

/**
 * Extracts the function name only (without return type, namespace, templates, or parameters).
 *
 * @param demangled A demangled C++ function signature.
 * @return The bare function name (e.g., "MyKernel").
 */
char* extract_function_name(const char* demangled);

#ifdef __cplusplus
}
#endif

#endif // CXX_UTILS_H