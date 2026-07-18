#ifndef PEAK_LOGGING_H
#define PEAK_LOGGING_H

/**
 * @file logging.h
 * @brief Internal runtime logging interface.
 */

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /** Suppress messages emitted through this logging interface. */
    PEAK_VERBOSITY_SILENT = 0,
    /** Emit requested reports only. */
    PEAK_VERBOSITY_REPORT = 1,
    /** Emit reports and warnings (the default). */
    PEAK_VERBOSITY_WARN = 2,
    /** Emit reports, warnings, and informational messages. */
    PEAK_VERBOSITY_INFO = 3,
    /** Emit all messages, including debug diagnostics. */
    PEAK_VERBOSITY_DEBUG = 4,
} PeakVerbosity;

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_PRINTF_FORMAT(fmt_index, first_arg) \
    __attribute__((format(printf, fmt_index, first_arg)))
#else
#define PEAK_PRINTF_FORMAT(fmt_index, first_arg)
#endif

/**
 * @brief Write a formatted message when its verbosity is enabled.
 *
 * The active level is read once from `PEAK_VERBOSITY` and then cached. Invalid
 * values produce a warning and select `PEAK_VERBOSITY_WARN`. Messages are
 * written to `stderr` without adding a prefix or trailing newline.
 *
 * @param level Message verbosity; emitted when no greater than the cached
 *              active verbosity.
 * @param format `printf`-style format string.
 * @param ... Arguments consumed by @p format.
 */
void peak_log_message(PeakVerbosity level,
                      const char* format,
                      ...) PEAK_PRINTF_FORMAT(2, 3);

#define peak_log_report(...) \
    peak_log_message(PEAK_VERBOSITY_REPORT, __VA_ARGS__)
#define peak_log_warn(...) \
    peak_log_message(PEAK_VERBOSITY_WARN, __VA_ARGS__)
#define peak_log_info(...) \
    peak_log_message(PEAK_VERBOSITY_INFO, __VA_ARGS__)
#define peak_log_debug(...) \
    peak_log_message(PEAK_VERBOSITY_DEBUG, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* PEAK_LOGGING_H */
