#ifndef PEAK_CXX_UTILS_H
#define PEAK_CXX_UTILS_H

/**
 * @file cxx_utils.h
 * @brief C-callable helpers for C++ symbol names.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Removes a @c +0x-prefixed suffix from a symbol in place.
 *
 * The first @c '+' terminates the string only when it is immediately followed
 * by @c "0x". An empty string or a string without that form is unchanged.
 *
 * @param[in,out] mangled_name_with_offset Writable, null-terminated symbol
 *                 string, for example @c "_Z3fooi+0x01".
 * @pre @p mangled_name_with_offset is not NULL and points to writable storage.
 */
void removeTrailingOffset(char* mangled_name_with_offset);

/**
 * @brief Demangles a C++ ABI symbol name.
 *
 * If ABI demangling fails, the returned allocation contains an unchanged copy
 * of @p name. This includes an empty input string.
 *
 * @param[in] name Null-terminated symbol name, for example @c "_Z3fooi".
 * @return A newly allocated demangled name or input copy, or NULL if allocation
 *         fails. The caller owns the result and must release it with free().
 * @pre @p name is not NULL.
 */
char* cxa_demangle(const char* name);

/**
 * @brief Returns the C++ ABI demangler status for a symbol name.
 *
 * Any temporary demangled string is freed before this function returns.
 *
 * @param[in] mangled_name Null-terminated symbol name to test.
 * @retval 0 Demangling succeeded.
 * @return A nonzero status reported by @c abi::__cxa_demangle otherwise.
 */
 int cxa_demangle_status(const char* mangled_name);

/**
 * @brief Extracts a bare function name from a demangled signature.
 *
 * The implementation heuristically removes the final parameter list, trailing
 * template arguments, text through the final space, and namespace qualification.
 * An empty input produces an allocated empty string.
 *
 * @param[in] demangled Null-terminated demangled C++ signature.
 * @return A newly allocated bare name, for example @c "MyKernel", or NULL if
 *         the final result allocation fails. The caller owns a non-NULL result
 *         and must free it.
 * @pre @p demangled is not NULL.
 * @warning The current implementation assumes every intermediate allocation
 *          succeeds.
 */
char* extract_function_name(const char* demangled);

/**
 * @brief Copies a string, optionally truncating it with an ellipsis.
 *
 * When PEAK name truncation is disabled, this always returns a complete copy.
 * When truncation is enabled and @p s is longer than @p max_len, the result
 * contains the first @c max_len-3 bytes followed by @c "...". The limit counts
 * bytes including the ellipsis but excludes the null terminator.
 * Empty and already-short strings are copied unchanged.
 *
 * @param[in] s Null-terminated string to copy.
 * @param[in] max_len Maximum result length when truncation is enabled.
 * @return A newly allocated null-terminated string, or NULL if a complete-copy
 *         allocation fails. The caller owns a non-NULL result and must release
 *         it with free().
 * @pre @p s is not NULL.
 * @pre When truncation is enabled, @p max_len is at least 3.
 * @warning The truncating allocation path assumes allocation succeeds.
 */
char* truncate_string(const char* s, int max_len);

#ifdef __cplusplus
}
#endif

#endif /* PEAK_CXX_UTILS_H */
