#include "logging.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PEAK_VERBOSITY_ENV "PEAK_VERBOSITY"

static pthread_once_t peak_log_configuration_once = PTHREAD_ONCE_INIT;
static pthread_once_t peak_log_output_once = PTHREAD_ONCE_INIT;
static PeakVerbosity peak_log_cached_verbosity = PEAK_VERBOSITY_WARN;
static FILE* peak_log_output;
static int peak_log_output_initialized;
static int peak_log_force_output_failure;

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

static void
peak_log_configure_once(void)
{
    const char* value = getenv(PEAK_VERBOSITY_ENV);
    int valid = 1;
    PeakVerbosity verbosity = peak_log_parse_verbosity(value, &valid);

    peak_log_cached_verbosity = verbosity;
    if (!valid) {
        fprintf(stderr,
                "[peak] ignoring invalid %s=%s; using warn verbosity\n",
                PEAK_VERBOSITY_ENV,
                value != NULL ? value : "");
    }
}

void
peak_log_configure(void)
{
    (void)pthread_once(&peak_log_configuration_once,
                       peak_log_configure_once);
}

static void
peak_log_initialize_output_once(void)
{
    int output_fd;

    peak_log_output_initialized = 1;
    if (peak_log_force_output_failure) {
        errno = EMFILE;
        output_fd = -1;
    } else {
        output_fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
    }
    if (output_fd < 0) {
        int error = errno;

        fprintf(stderr,
                "[peak] unable to duplicate the report descriptor: %s; disabling PEAK diagnostics and text reports\n",
                strerror(error));
        return;
    }
    peak_log_output = fdopen(output_fd, "w");
    if (peak_log_output == NULL) {
        int error = errno;

        (void)close(output_fd);
        fprintf(stderr,
                "[peak] unable to open the owned report stream: %s; disabling PEAK diagnostics and text reports\n",
                strerror(error));
        return;
    }
    (void)setvbuf(peak_log_output, NULL, _IONBF, 0);
}

void
peak_log_initialize_output(int fail_descriptor_dup_for_test)
{
    peak_log_force_output_failure = fail_descriptor_dup_for_test;
    (void)pthread_once(&peak_log_output_once,
                       peak_log_initialize_output_once);
}

static PeakVerbosity
peak_log_verbosity(void)
{
    peak_log_configure();
    return (PeakVerbosity)peak_log_cached_verbosity;
}

static int
peak_log_enabled(PeakVerbosity level)
{
    return level <= peak_log_verbosity();
}

static void
peak_log_vmessage(PeakVerbosity level, const char* format, va_list args)
{
    FILE* output;

    if (!peak_log_enabled(level)) {
        return;
    }
    output = peak_log_output_initialized ? peak_log_output : stderr;
    if (output != NULL) {
        vfprintf(output, format, args);
    }
}

void
peak_log_message(PeakVerbosity level, const char* format, ...)
{
    va_list args;

    va_start(args, format);
    peak_log_vmessage(level, format, args);
    va_end(args);
}

int
peak_log_flush(void)
{
    FILE* output = peak_log_output_initialized ? peak_log_output : stderr;

    if (output == NULL) {
        errno = EBADF;
        return -1;
    }
    if (fflush(output) != 0 || ferror(output)) {
        if (errno == 0) {
            errno = EIO;
        }
        return -1;
    }
    return 0;
}

void
peak_log_shutdown(void)
{
    FILE* output = peak_log_output;

    peak_log_output = NULL;
    if (output != NULL) {
        (void)fclose(output);
    }
}
