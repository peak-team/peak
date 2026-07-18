#ifndef PEAK_ENV_PARSER_H
#define PEAK_ENV_PARSER_H

/**
 * @file env_parser.h
 * @brief Scalar environment-value parsing for PEAK.
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "target_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parses a single-precision value from an environment variable.
 *
 * Parsing uses strtof() and requires the complete string to be consumed. The
 * implementation does not check @c errno or clamp the result. An empty string
 * therefore produces 0.0; fully consumed NaN and infinity spellings, including
 * overflow results, are returned unchanged.
 *
 * @param[in] env_var Name of the environment variable.
 * @return The parsed value, or 0.0 if the variable is unset or has unconsumed
 *         characters.
 * @pre @p env_var is a non-NULL, null-terminated environment-variable name.
 */
float parse_env_to_float(const char* env_var);

/**
 * @brief Parses a single-precision ratio from an environment variable.
 *
 * Parsing uses strtof() and requires the complete string to be consumed. The
 * result is not clamped and @c errno is not checked. Because the implementation
 * does not require at least one digit, an empty value returns 0.0 rather than
 * the fallback. Fully consumed NaN and infinity spellings, including overflow
 * results, are returned unchanged.
 *
 * @param[in] env_var Name of the environment variable.
 * @return The parsed ratio, or 0.1 if the variable is unset or has unconsumed
 *         characters.
 * @pre @p env_var is a non-NULL, null-terminated environment-variable name.
 */
float parse_env_to_float_ratio(const char* env_var);

/**
 * @brief Parses the single-precision detach factor from an environment variable.
 *
 * Parsing uses strtof() and requires the complete string to be consumed. The
 * result is not clamped and @c errno is not checked. An empty value returns
 * 0.0 because the implementation does not require at least one digit. Fully
 * consumed NaN and infinity spellings, including overflow results, are returned.
 *
 * @param[in] env_var Name of the environment variable.
 * @return The parsed factor, or 1.2 if the variable is unset or has unconsumed
 *         characters.
 * @pre @p env_var is a non-NULL, null-terminated environment-variable name.
 */
float parse_env_to_float_detach_factor(const char* env_var);

/**
 * @brief Parses the single-precision reattach factor from an environment variable.
 *
 * Parsing uses strtof() and requires the complete string to be consumed. The
 * result is not clamped and @c errno is not checked. An empty value returns
 * 0.0 because the implementation does not require at least one digit. Fully
 * consumed NaN and infinity spellings, including overflow results, are returned.
 *
 * @param[in] env_var Name of the environment variable.
 * @return The parsed factor, or 0.85 if the variable is unset or has unconsumed
 *         characters.
 * @pre @p env_var is a non-NULL, null-terminated environment-variable name.
 */
float parse_env_to_float_reattach_factor(const char* env_var);

/**
 * @brief Parses seconds and converts them to microseconds.
 *
 * A missing, empty, negative, partially parsed, or strtod() range-error value
 * returns 100000 microseconds. Valid values above the representable range are
 * clamped to UINT_MAX; this includes positive infinity. Negative infinity
 * falls back. Other valid values are multiplied by 1e6 and truncated toward
 * zero when converted to unsigned int.
 *
 * @param[in] env_var Name of the environment variable.
 * @return The interval in microseconds, or 100000 microseconds (0.1 seconds)
 *         when parsing fails.
 * @pre @p env_var is a non-NULL, null-terminated environment-variable name.
 * @pre The environment value does not parse as NaN; the current implementation
 *      would otherwise perform an undefined floating-to-unsigned conversion.
 */
unsigned int parse_env_to_time(const char* env_var);

/**
 * @brief Parses an unsigned integer interval from an environment variable.
 *
 * Parsing uses strtoul() with base 10. An unset value, a range error, or
 * unconsumed characters returns 50. An empty value returns 0 because this
 * function does not require at least one digit. Otherwise strtoul() semantics,
 * including optional sign, apply; the converted result is not clamped before
 * it is first narrowed to unsigned int. Values between UINT_MAX and ULONG_MAX,
 * and accepted negative spellings, therefore follow unsigned narrowing rather
 * than selecting the fallback.
 *
 * @param[in] env_var Name of the environment variable.
 * @return The converted interval, or 50 for the fallback cases above.
 * @pre @p env_var is a non-NULL, null-terminated environment-variable name.
 */
unsigned int parse_env_to_interval(const char* env_var);

/**
 * @brief Parses an unsigned integer with a caller-supplied fallback.
 *
 * Parsing uses strtoul() with base 10 and requires at least one character and
 * complete consumption. Range errors and values above UINT_MAX return the
 * fallback rather than being clamped.
 *
 * @param[in] env_var Name of the environment variable.
 * @param[in] default_value Value returned for an unset or invalid value.
 * @return The parsed unsigned integer, or @p default_value on failure.
 * @pre @p env_var is a non-NULL, null-terminated environment-variable name.
 */
unsigned int parse_env_to_uint_default(const char* env_var, unsigned int default_value);

/**
 * @brief Parses a double with a caller-supplied fallback.
 *
 * Parsing uses strtod(), requires at least one character and complete
 * consumption, and rejects range errors. Accepted values are not clamped;
 * literal NaN and positive or negative infinity are returned, while overflow
 * or underflow that sets @c ERANGE selects the fallback.
 *
 * @param[in] env_var Name of the environment variable.
 * @param[in] default_value Value returned for an unset or invalid value.
 * @return The parsed value, or @p default_value on failure.
 * @pre @p env_var is a non-NULL, null-terminated environment-variable name.
 */
double parse_env_to_double_default(const char* env_var, double default_value);

/**
 * @brief Parses seconds and converts them to nanoseconds.
 *
 * A missing, empty, negative, partially parsed, or strtod() range-error value
 * returns 10000000 nanoseconds. Valid values are multiplied by 1e9 and
 * truncated toward zero. Unlike parse_env_to_time(), this function does not
 * clamp large values. Negative infinity falls back. NaN, positive infinity,
 * and finite products outside the unsigned long long range are unsupported.
 *
 * @param[in] env_var Name of the environment variable.
 * @return The interval in nanoseconds, or 10000000 nanoseconds (0.01 seconds)
 *         when parsing fails.
 * @pre @p env_var is a non-NULL, null-terminated environment-variable name.
 * @pre A successfully parsed value is nonnegative and finite, and its product
 *      with 1e9 is representable as an unsigned long long; otherwise the
 *      current implementation may perform an undefined conversion.
 */
unsigned long long parse_env_to_post_interval(const char* env_var);

/**
 * @brief Parses a boolean environment variable.
 *
 * Only @c "true" (case-insensitive) and the exact string @c "1" produce true.
 * Unset, empty, and all other values produce false.
 *
 * @param[in] env_var Name of the environment variable.
 * @return true for the two accepted forms; false otherwise.
 * @pre @p env_var is a non-NULL, null-terminated environment-variable name.
 */
bool parse_env_to_bool(const char* env_var);

#ifdef __cplusplus
}
#endif

#endif /* PEAK_ENV_PARSER_H */
