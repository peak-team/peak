#include "logging.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PEAK_VERBOSITY_ENV "PEAK_VERBOSITY"

static int peak_log_cached_verbosity = -1;

static int
peak_log_streq_ci(const char* left, const char* right)
{
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) !=
            tolower((unsigned char)*right)) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static int
peak_log_parse_numeric_verbosity(const char* value, PeakVerbosity* out)
{
    char* end = NULL;
    long parsed;

    if (value == NULL || value[0] == '\0') {
        return 0;
    }

    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' ||
        parsed < PEAK_VERBOSITY_SILENT ||
        parsed > PEAK_VERBOSITY_DEBUG) {
        return 0;
    }

    *out = (PeakVerbosity)parsed;
    return 1;
}

static PeakVerbosity
peak_log_parse_verbosity(const char* value, int* valid)
{
    PeakVerbosity parsed;

    *valid = 1;
    if (value == NULL || value[0] == '\0' ||
        peak_log_streq_ci(value, "default") ||
        peak_log_streq_ci(value, "warn") ||
        peak_log_streq_ci(value, "warning") ||
        peak_log_streq_ci(value, "warnings")) {
        return PEAK_VERBOSITY_WARN;
    }
    if (peak_log_streq_ci(value, "silent") ||
        peak_log_streq_ci(value, "none") ||
        peak_log_streq_ci(value, "off")) {
        return PEAK_VERBOSITY_SILENT;
    }
    if (peak_log_streq_ci(value, "quiet") ||
        peak_log_streq_ci(value, "report") ||
        peak_log_streq_ci(value, "reports")) {
        return PEAK_VERBOSITY_REPORT;
    }
    if (peak_log_streq_ci(value, "info") ||
        peak_log_streq_ci(value, "normal") ||
        peak_log_streq_ci(value, "verbose")) {
        return PEAK_VERBOSITY_INFO;
    }
    if (peak_log_streq_ci(value, "debug") ||
        peak_log_streq_ci(value, "trace")) {
        return PEAK_VERBOSITY_DEBUG;
    }
    if (peak_log_parse_numeric_verbosity(value, &parsed)) {
        return parsed;
    }

    *valid = 0;
    return PEAK_VERBOSITY_WARN;
}

PeakVerbosity
peak_log_verbosity(void)
{
    if (peak_log_cached_verbosity < 0) {
        const char* value = getenv(PEAK_VERBOSITY_ENV);
        int valid = 1;
        PeakVerbosity verbosity = peak_log_parse_verbosity(value, &valid);

        peak_log_cached_verbosity = (int)verbosity;
        if (!valid) {
            fprintf(stderr,
                    "[peak] ignoring invalid %s=%s; using warn verbosity\n",
                    PEAK_VERBOSITY_ENV,
                    value != NULL ? value : "");
        }
    }

    return (PeakVerbosity)peak_log_cached_verbosity;
}

int
peak_log_enabled(PeakVerbosity level)
{
    return level <= peak_log_verbosity();
}

void
peak_log_vmessage(PeakVerbosity level, const char* format, va_list args)
{
    if (!peak_log_enabled(level)) {
        return;
    }
    vfprintf(stderr, format, args);
}

void
peak_log_message(PeakVerbosity level, const char* format, ...)
{
    va_list args;

    va_start(args, format);
    peak_log_vmessage(level, format, args);
    va_end(args);
}
