#ifndef PEAK_LOGGING_H
#define PEAK_LOGGING_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PEAK_VERBOSITY_SILENT = 0,
    PEAK_VERBOSITY_REPORT = 1,
    PEAK_VERBOSITY_WARN = 2,
    PEAK_VERBOSITY_INFO = 3,
    PEAK_VERBOSITY_DEBUG = 4,
} PeakVerbosity;

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_PRINTF_FORMAT(fmt_index, first_arg) \
    __attribute__((format(printf, fmt_index, first_arg)))
#else
#define PEAK_PRINTF_FORMAT(fmt_index, first_arg)
#endif

PeakVerbosity peak_log_verbosity(void);
int peak_log_enabled(PeakVerbosity level);
void peak_log_vmessage(PeakVerbosity level,
                       const char* format,
                       va_list args);
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

#endif
