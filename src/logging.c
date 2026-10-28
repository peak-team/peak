#define _POSIX_C_SOURCE 200809L
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

typedef struct {
    FILE* memory;
    char* bytes;
    size_t size;
    size_t published;
    pid_t owner;
    int error;
} PeakLogReportBatch;

/* The memstream's output pointers live on the heap rather than in TLS: a
 * canceled thread or fork child's libc exit cannot dereference retired TLS.
 * An inherited memstream only flushes memory, never the report descriptor. */
static _Thread_local PeakLogReportBatch* peak_log_report_batch;

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

static FILE*
peak_log_output_stream(void)
{
    return peak_log_output_initialized ? peak_log_output : stderr;
}

static PeakLogReportBatch*
peak_log_current_batch(void)
{
    PeakLogReportBatch* batch = peak_log_report_batch;
    return batch != NULL && batch->owner == getpid() ? batch : NULL;
}

static int
peak_log_batch_publish(PeakLogReportBatch* batch)
{
    FILE* output = peak_log_output_stream();

    if (output == NULL) {
        batch->error = EBADF;
    } else if (batch->size > batch->published) {
        size_t remaining = batch->size - batch->published;
        size_t written = fwrite(batch->bytes + batch->published, 1,
                                remaining, output);
        batch->published += written;
        if (written != remaining)
            batch->error = errno != 0 ? errno : EIO;
    }
    if (output != NULL && (fflush(output) != 0 || ferror(output)))
        batch->error = errno != 0 ? errno : EIO;
    if (batch->error != 0) {
        errno = batch->error;
        return -1;
    }
    return 0;
}

int
peak_log_report_batch_begin(void)
{
    if (peak_log_current_batch() != NULL) return 0;
    PeakLogReportBatch* batch = calloc(1, sizeof(*batch));
    if (batch == NULL) return 0;
    batch->memory = open_memstream(&batch->bytes, &batch->size);
    if (batch->memory == NULL) {
        free(batch);
        return 0;
    }
    batch->owner = getpid();
    /* A fork child may replace its inherited, parent-owned TLS pointer. The
     * inherited FILE remains memory-only and is left to libc process exit. */
    peak_log_report_batch = batch;
    return 1;
}

static void
peak_log_batch_destroy(void* data)
{
    PeakLogReportBatch* batch = data;
    if (batch->memory != NULL) (void)fclose(batch->memory);
    free(batch->bytes);
    free(batch);
}

void
peak_log_report_batch_abort(void)
{
    PeakLogReportBatch* batch = peak_log_current_batch();
    if (batch == NULL) return;
    peak_log_report_batch = NULL;
    peak_log_batch_destroy(batch);
}

int
peak_log_report_batch_end(void)
{
    PeakLogReportBatch* batch = peak_log_current_batch();
    if (batch == NULL) return 0;
    int result, error;
    peak_log_report_batch = NULL;
    pthread_cleanup_push(peak_log_batch_destroy, batch);
    FILE* memory = batch->memory;
    batch->memory = NULL;
    if (memory != NULL && fclose(memory) != 0)
        batch->error = errno != 0 ? errno : EIO;
    result = peak_log_batch_publish(batch);
    error = errno;
    pthread_cleanup_pop(1);
    errno = error;
    return result;
}

static void
peak_log_vwrite(const char* format, va_list args)
{
    FILE* output = peak_log_output_stream();
    PeakLogReportBatch* batch = peak_log_current_batch();

    if (batch != NULL && batch->memory != NULL) {
        off_t before = ftello(batch->memory);
        va_list replay;
        va_copy(replay, args);
        int result = vfprintf(batch->memory, format, args);
        if (result < 0) {
            /* Preserve complete earlier messages, discard only a partial
             * current format, then replay that format on the direct stream.
             * Subsequent messages retain the old immediate-output behavior. */
            (void)fclose(batch->memory);
            batch->memory = NULL;
            if (before < 0 || (size_t)before > batch->size)
                batch->error = EIO;
            else
                batch->size = (size_t)before;
            (void)peak_log_batch_publish(batch);
            free(batch->bytes);
            batch->bytes = NULL;
            batch->size = batch->published = 0;
            if (output != NULL && vfprintf(output, format, replay) < 0)
                batch->error = errno != 0 ? errno : EIO;
        }
        va_end(replay);
        return;
    }

    if (output != NULL) {
        if (vfprintf(output, format, args) < 0 && batch != NULL)
            batch->error = errno != 0 ? errno : EIO;
    }
}

static void
peak_log_vmessage(PeakVerbosity level, const char* format, va_list args)
{
    if (peak_log_enabled(level)) {
        peak_log_vwrite(format, args);
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

void
peak_log_message_always(const char* format, ...)
{
    va_list args;

    va_start(args, format);
    peak_log_vwrite(format, args);
    va_end(args);
}

int
peak_log_flush(void)
{
    FILE* output = peak_log_output_stream();
    PeakLogReportBatch* batch = peak_log_current_batch();

    if (batch != NULL) {
        if (batch->memory != NULL && fflush(batch->memory) != 0)
            batch->error = errno != 0 ? errno : EIO;
        return peak_log_batch_publish(batch);
    }

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
    (void)peak_log_report_batch_end();
    FILE* output = peak_log_output;

    peak_log_output = NULL;
    if (output != NULL) {
        (void)fclose(output);
    }
}
