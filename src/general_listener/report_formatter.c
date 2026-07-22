#define _POSIX_C_SOURCE 200809L

#include "internal/general_listener/report_formatter.h"

#include "internal/general_listener/runtime_config.h"

#include "logging.h"

#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    PEAK_TEXT_REPORT_FUNCTION_WIDTH = 32,
    PEAK_TEXT_REPORT_COLUMN_WIDTH = 10,
    PEAK_TEXT_REPORT_ROW_WIDTH =
        PEAK_TEXT_REPORT_FUNCTION_WIDTH +
        PEAK_TEXT_REPORT_COLUMN_WIDTH * 5 + 7,
    PEAK_REPORT_HOSTNAME_CAPACITY = 256,
    PEAK_REPORT_TEMP_CREATE_ATTEMPTS = 128,
};

static _Atomic unsigned long peak_report_formatter_temp_counter;

#ifdef PEAK_ENABLE_TEST_HOOKS
static void
peak_report_formatter_test_delay_aggregate_write(void)
{
    const char* value = getenv("PEAK_TEST_REPORT_ROOT_WRITE_DELAY_MS");
    char* end = NULL;
    unsigned long delay_ms;
    struct timespec remaining;

    if (value == NULL || value[0] == '\0') {
        return;
    }
    errno = 0;
    delay_ms = strtoul(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' ||
        delay_ms == 0 || delay_ms > 60000UL) {
        return;
    }

    remaining.tv_sec = (time_t)(delay_ms / 1000UL);
    remaining.tv_nsec = (long)(delay_ms % 1000UL) * 1000000L;
    peak_log_info("[peak] Test hook delaying aggregate CSV publication by %lu ms\n",
                  delay_ms);
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
    }
}

static void
peak_report_formatter_test_signal_publication_phase(const char* phase)
{
    const char* configured_phase =
        getenv("PEAK_TEST_REPORT_SIGNAL_PHASE");
    const char* signal_text = getenv("PEAK_TEST_REPORT_SIGNAL");
    const char* rank_text = getenv("PEAK_TEST_REPORT_SIGNAL_RANK");
    char* end = NULL;
    long configured_rank = 0;
    long current_rank = peak_general_listener_mpi_env_rank();
    int signal_number;

    if (configured_phase == NULL || signal_text == NULL ||
        strcmp(configured_phase, phase) != 0) {
        return;
    }
    if (rank_text != NULL && rank_text[0] != '\0') {
        errno = 0;
        configured_rank = strtol(rank_text, &end, 10);
        if (errno != 0 || end == rank_text || *end != '\0' ||
            configured_rank < 0) {
            return;
        }
    }
    if (current_rank < 0) {
        current_rank = 0;
    }
    if (current_rank != configured_rank) {
        return;
    }
    if (strcmp(signal_text, "TERM") == 0 ||
        strcmp(signal_text, "SIGTERM") == 0 ||
        strcmp(signal_text, "15") == 0) {
        signal_number = SIGTERM;
    } else if (strcmp(signal_text, "KILL") == 0 ||
               strcmp(signal_text, "SIGKILL") == 0 ||
               strcmp(signal_text, "9") == 0) {
        signal_number = SIGKILL;
    } else {
        return;
    }

    peak_log_info("[peak] Test hook delivering signal %d on rank %ld at report phase %s\n",
                  signal_number,
                  current_rank,
                  phase);
    (void)fflush(stderr);
    (void)kill(getpid(), signal_number);
}
#endif

typedef struct {
    double total_overhead;
    uint64_t stop_window_count;
    uint64_t failed_stop_window_count;
    double stop_window_seconds;
    double stop_window_ratio;
    double elapsed_seconds;
    int stop_window_owner_rank;
    size_t detached_targets;
    size_t reattached_targets;
    size_t revisited_targets;
    size_t instrumented_targets;
    size_t profiled_targets;
    uint64_t total_calls;
    bool total_calls_saturated;
    bool have_output;
} PeakReportTextSummary;

static bool
peak_report_formatter_positive_finite(double value)
{
    return value > 0.0 && value == value && value <= DBL_MAX;
}

static const char*
peak_report_formatter_name(const PeakReportSnapshot* snapshot, size_t hook_id)
{
    return snapshot->names != NULL && snapshot->names[hook_id] != NULL ?
               snapshot->names[hook_id] :
               "";
}

static bool
peak_report_formatter_slot_is_instrumented(
    const PeakReportSnapshot* snapshot,
    size_t hook_id)
{
    return snapshot->instrumented != NULL &&
           snapshot->instrumented[hook_id] != 0;
}

static int
peak_report_formatter_rank_count(const PeakReportSnapshot* snapshot)
{
    return snapshot->rank_count > 0 ? snapshot->rank_count : 1;
}

static void
peak_report_formatter_sanitized_hostname(
    char sanitized[PEAK_REPORT_HOSTNAME_CAPACITY])
{
    char hostname[PEAK_REPORT_HOSTNAME_CAPACITY] = {0};
    size_t output_length = 0;

    if (gethostname(hostname, sizeof(hostname) - 1) != 0 ||
        hostname[0] == '\0') {
        (void)snprintf(hostname, sizeof(hostname), "unknown");
    }
    hostname[sizeof(hostname) - 1] = '\0';

    for (size_t i = 0;
         hostname[i] != '\0' && output_length + 1 <
                                      PEAK_REPORT_HOSTNAME_CAPACITY;
         i++) {
        const unsigned char byte = (unsigned char)hostname[i];

        if ((byte >= (unsigned char)'a' && byte <= (unsigned char)'z') ||
            (byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') ||
            (byte >= (unsigned char)'0' && byte <= (unsigned char)'9') ||
            byte == (unsigned char)'-' || byte == (unsigned char)'_' ||
            byte == (unsigned char)'.') {
            sanitized[output_length++] = (char)byte;
        } else {
            sanitized[output_length++] = '_';
        }
    }
    if (output_length == 0) {
        (void)snprintf(sanitized,
                       PEAK_REPORT_HOSTNAME_CAPACITY,
                       "unknown");
        return;
    }
    sanitized[output_length] = '\0';
}

static char*
peak_report_formatter_csv_path(bool rank_local)
{
    const char* env_path = getenv("PEAK_STATSLOG_PATH");
    const char* base = env_path != NULL && env_path[0] != '\0' ?
                           env_path : "./peak_statslog";
    long world_size = -1;
    long world_rank = -1;
    char hostname[PEAK_REPORT_HOSTNAME_CAPACITY] = {0};
    enum {
        PEAK_REPORT_SUFFIX_NONE = 0,
        PEAK_REPORT_SUFFIX_RANK,
        PEAK_REPORT_SUFFIX_HOSTNAME,
    } suffix = PEAK_REPORT_SUFFIX_NONE;
    int length;
    char* path;

    if (rank_local) {
        world_size = peak_general_listener_mpi_env_size();
        if (world_size > 1) {
            world_rank = peak_general_listener_mpi_env_rank();
            if (world_rank >= 0 && world_rank < world_size) {
                suffix = PEAK_REPORT_SUFFIX_RANK;
            } else {
                peak_report_formatter_sanitized_hostname(hostname);
                suffix = PEAK_REPORT_SUFFIX_HOSTNAME;
            }
        }
    }

    if (suffix == PEAK_REPORT_SUFFIX_RANK) {
        length = snprintf(NULL,
                          0,
                          "%s-p%d-r%ld.csv",
                          base,
                          (int)getpid(),
                          world_rank);
    } else if (suffix == PEAK_REPORT_SUFFIX_HOSTNAME) {
        length = snprintf(NULL,
                          0,
                          "%s-p%d-h%s.csv",
                          base,
                          (int)getpid(),
                          hostname);
    } else {
        length = snprintf(NULL, 0, "%s-p%d.csv", base, (int)getpid());
    }
    if (length < 0) {
        return NULL;
    }
    path = malloc((size_t)length + 1);
    if (path == NULL) {
        return NULL;
    }
    if (suffix == PEAK_REPORT_SUFFIX_RANK) {
        (void)snprintf(path,
                       (size_t)length + 1,
                       "%s-p%d-r%ld.csv",
                       base,
                       (int)getpid(),
                       world_rank);
    } else if (suffix == PEAK_REPORT_SUFFIX_HOSTNAME) {
        (void)snprintf(path,
                       (size_t)length + 1,
                       "%s-p%d-h%s.csv",
                       base,
                       (int)getpid(),
                       hostname);
    } else {
        (void)snprintf(path,
                       (size_t)length + 1,
                       "%s-p%d.csv",
                       base,
                       (int)getpid());
    }
    return path;
}

static int
peak_report_formatter_create_csv_temp(const char* out_csv,
                                      char** temp_path_out)
{
    int length;
    char* temp_path;

    if (out_csv == NULL || temp_path_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *temp_path_out = NULL;
    length = snprintf(NULL,
                      0,
                      "%s.tmp.p%d.%lu",
                      out_csv,
                      (int)getpid(),
                      ULONG_MAX);
    if (length < 0) {
        errno = EINVAL;
        return -1;
    }
    temp_path = malloc((size_t)length + 1);
    if (temp_path == NULL) {
        return -1;
    }

    for (unsigned int attempt = 0;
         attempt < PEAK_REPORT_TEMP_CREATE_ATTEMPTS;
         attempt++) {
        unsigned long ticket = atomic_fetch_add_explicit(
            &peak_report_formatter_temp_counter, 1UL, memory_order_relaxed);
        int fd;

        (void)snprintf(temp_path,
                       (size_t)length + 1,
                       "%s.tmp.p%d.%lu",
                       out_csv,
                       (int)getpid(),
                       ticket);
        fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0) {
            if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
                int saved_errno = errno;

                (void)close(fd);
                (void)unlink(temp_path);
                free(temp_path);
                errno = saved_errno;
                return -1;
            }
            *temp_path_out = temp_path;
            return fd;
        }
        if (errno != EEXIST) {
            free(temp_path);
            return -1;
        }
    }

    free(temp_path);
    errno = EEXIST;
    return -1;
}

static bool
peak_report_formatter_write_csv_name(FILE* csv, const char* name)
{
    if (fputc('"', csv) == EOF) {
        return false;
    }
    for (const unsigned char* cursor = (const unsigned char*)name;
         *cursor != '\0'; cursor++) {
        if (*cursor == '"' && fputc('"', csv) == EOF) {
            return false;
        }
        if (fputc((int)*cursor, csv) == EOF) {
            return false;
        }
    }
    return fputc('"', csv) != EOF;
}

static bool
peak_report_formatter_has_csv_output(const PeakReportSnapshot* snapshot)
{
    for (size_t i = 0; i < snapshot->hook_count; i++) {
        if (peak_report_formatter_slot_is_instrumented(snapshot, i) &&
            snapshot->num_calls[i] != 0) {
            return true;
        }
    }
    return false;
}

static char*
peak_report_formatter_truncate_name(const char* name,
                                    int max_length,
                                    bool truncate)
{
    size_t length = strlen(name);
    size_t result_length = length;
    char* result;

    if (truncate && length > (size_t)max_length) {
        result_length = (size_t)max_length;
    }
    result = malloc(result_length + 1);
    if (result == NULL) {
        return NULL;
    }
    if (!truncate || length <= (size_t)max_length) {
        memcpy(result, name, length + 1);
        return result;
    }

    memcpy(result, name, (size_t)max_length - 3);
    memcpy(result + max_length - 3, "...", 4);
    return result;
}

static void
peak_report_formatter_print_text_section(const char* title,
                                         const char* separator)
{
    peak_log_report("\n%s\n", title);
    peak_log_report("%s\n", separator);
}

void
peak_report_formatter_write_rank_maxima(
    const PeakReportRankTuple maximum[PEAK_REPORT_METRIC_COUNT],
    const int owner_rank[PEAK_REPORT_METRIC_COUNT])
{
    peak_log_report("[peak] per-rank maximum profile+control overhead: owner_rank=%d profile_seconds=%.9f control_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f\n",
                    owner_rank[PEAK_REPORT_METRIC_COMBINED],
                    maximum[PEAK_REPORT_METRIC_COMBINED].profile_seconds,
                    maximum[PEAK_REPORT_METRIC_COMBINED].control_seconds,
                    maximum[PEAK_REPORT_METRIC_COMBINED].elapsed_seconds,
                    maximum[PEAK_REPORT_METRIC_COMBINED].ratio);
    peak_log_report("[peak] per-rank maximum profile overhead: owner_rank=%d profile_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f\n",
                    owner_rank[PEAK_REPORT_METRIC_PROFILE],
                    maximum[PEAK_REPORT_METRIC_PROFILE].profile_seconds,
                    maximum[PEAK_REPORT_METRIC_PROFILE].elapsed_seconds,
                    maximum[PEAK_REPORT_METRIC_PROFILE].profile_ratio);
    peak_log_report("[peak] per-rank maximum control overhead: owner_rank=%d control_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f\n",
                    owner_rank[PEAK_REPORT_METRIC_CONTROL],
                    maximum[PEAK_REPORT_METRIC_CONTROL].control_seconds,
                    maximum[PEAK_REPORT_METRIC_CONTROL].elapsed_seconds,
                    maximum[PEAK_REPORT_METRIC_CONTROL].control_ratio);
    peak_log_report("[peak] per-rank maximum profile+control risk overhead: owner_rank=%d profile_seconds=%.9f raw_control_seconds=%.9f local_ranks=%u control_risk_seconds=%.9f risk_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f\n",
                    owner_rank[PEAK_REPORT_METRIC_PROFILE_CONTROL_RISK],
                    maximum[PEAK_REPORT_METRIC_PROFILE_CONTROL_RISK]
                        .profile_seconds,
                    maximum[PEAK_REPORT_METRIC_PROFILE_CONTROL_RISK]
                        .control_seconds,
                    maximum[PEAK_REPORT_METRIC_PROFILE_CONTROL_RISK]
                        .local_ranks,
                    maximum[PEAK_REPORT_METRIC_PROFILE_CONTROL_RISK]
                        .control_risk_seconds,
                    maximum[PEAK_REPORT_METRIC_PROFILE_CONTROL_RISK]
                        .profile_control_risk_seconds,
                    maximum[PEAK_REPORT_METRIC_PROFILE_CONTROL_RISK]
                        .elapsed_seconds,
                    maximum[PEAK_REPORT_METRIC_PROFILE_CONTROL_RISK]
                        .profile_control_risk_ratio);
    peak_log_report("[peak] per-rank maximum control risk overhead: owner_rank=%d raw_control_seconds=%.9f local_ranks=%u control_risk_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f\n",
                    owner_rank[PEAK_REPORT_METRIC_CONTROL_RISK],
                    maximum[PEAK_REPORT_METRIC_CONTROL_RISK].control_seconds,
                    maximum[PEAK_REPORT_METRIC_CONTROL_RISK].local_ranks,
                    maximum[PEAK_REPORT_METRIC_CONTROL_RISK]
                        .control_risk_seconds,
                    maximum[PEAK_REPORT_METRIC_CONTROL_RISK].elapsed_seconds,
                    maximum[PEAK_REPORT_METRIC_CONTROL_RISK]
                        .control_risk_ratio);
    peak_log_report("[peak] per-rank maximum heartbeat management overhead: owner_rank=%d cpu_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f\n",
                    owner_rank[PEAK_REPORT_METRIC_MANAGEMENT],
                    maximum[PEAK_REPORT_METRIC_MANAGEMENT].management_seconds,
                    maximum[PEAK_REPORT_METRIC_MANAGEMENT].elapsed_seconds,
                    maximum[PEAK_REPORT_METRIC_MANAGEMENT].management_ratio);
}

static PeakReportTextSummary
peak_report_formatter_summarize(const PeakReportSnapshot* snapshot)
{
    const PeakReportOverhead* overhead = &snapshot->overhead;
    PeakReportTextSummary summary = {0};

    summary.stop_window_count = overhead->stop_window_count;
    summary.failed_stop_window_count = overhead->failed_stop_window_count;
    summary.stop_window_seconds = overhead->control_seconds;
    summary.elapsed_seconds = overhead->elapsed_seconds;
    summary.stop_window_owner_rank = -1;
    if (overhead->valid && overhead->per_rank_max) {
        summary.stop_window_owner_rank =
            overhead->per_rank_maxima
                .owner_ranks[PEAK_REPORT_METRIC_COMBINED];
    }
    if (summary.elapsed_seconds > 0.0) {
        summary.stop_window_ratio =
            summary.stop_window_seconds / summary.elapsed_seconds;
    }

    for (size_t i = 0; i < snapshot->hook_count; i++) {
        uint64_t calls;

        if (snapshot->detached != NULL && snapshot->detached[i] != 0) {
            summary.detached_targets++;
        }
        if (snapshot->reattached != NULL && snapshot->reattached[i] != 0) {
            summary.reattached_targets++;
        }
        if (snapshot->revisited != NULL && snapshot->revisited[i] != 0) {
            summary.revisited_targets++;
        }
        if (peak_report_formatter_slot_is_instrumented(snapshot, i)) {
            summary.instrumented_targets++;
        }
        if (!peak_report_formatter_slot_is_instrumented(snapshot, i) ||
            snapshot->num_calls[i] == 0) {
            continue;
        }

        calls = (uint64_t)snapshot->num_calls[i];
        summary.profiled_targets++;
        if (calls > UINT64_MAX - summary.total_calls) {
            summary.total_calls = UINT64_MAX;
            summary.total_calls_saturated = true;
        } else {
            summary.total_calls += calls;
        }
        summary.total_overhead +=
            (double)snapshot->num_calls[i] * snapshot->overhead_per_call;
        summary.have_output = true;
    }

    if (overhead->valid &&
        peak_report_formatter_positive_finite(overhead->profile_seconds)) {
        summary.total_overhead = overhead->profile_seconds;
    }
    if (!summary.have_output) {
        summary.have_output = overhead->valid ||
                              summary.detached_targets > 0 ||
                              summary.reattached_targets > 0 ||
                              peak_report_formatter_positive_finite(
                                  summary.stop_window_seconds);
    }
    return summary;
}

static bool
peak_report_formatter_write_csv_scoped(const PeakReportSnapshot* snapshot,
                                       bool rank_local)
{
    static const char header[] =
        "function,"
        "count,per_thread,per_rank,call_max_s,call_min_s,"
        "total_s,exclusive_s,thread_max_s,thread_min_s,overhead_s\n";
    char* out_csv;
    char* temp_csv;
    FILE* csv;
    bool success;
    int csv_fd;
    int failure_errno = 0;
    int rank_count;

    if (snapshot == NULL) {
        return false;
    }
    if (!peak_report_formatter_has_csv_output(snapshot)) {
        return true;
    }

#ifdef PEAK_ENABLE_TEST_HOOKS
    if (!rank_local) {
        /*
         * Keep the aggregate writer behind its peer long enough for the
         * lifecycle integration test to exercise clean MPI finalization while
         * the root report is still pending. Test builds are unchanged unless
         * the hook environment is set.
         */
        peak_report_formatter_test_delay_aggregate_write();
    }
#endif

    out_csv = peak_report_formatter_csv_path(rank_local);
    if (out_csv == NULL) {
        peak_log_warn("[peak] failed to allocate stats csv path\n");
        return false;
    }
    csv_fd = peak_report_formatter_create_csv_temp(out_csv, &temp_csv);
    if (csv_fd < 0) {
        peak_log_warn("[peak] failed to create temporary stats csv for '%s': %s\n",
                      out_csv,
                      strerror(errno));
        free(out_csv);
        return false;
    }
    csv = fdopen(csv_fd, "w");
    if (csv == NULL) {
        failure_errno = errno;
        (void)close(csv_fd);
        (void)unlink(temp_csv);
        peak_log_warn("[peak] failed to open temporary stats csv for '%s': %s\n",
                      out_csv,
                      strerror(failure_errno));
        free(temp_csv);
        free(out_csv);
        return false;
    }

    success = fputs(header, csv) >= 0;
    rank_count = peak_report_formatter_rank_count(snapshot);
    for (size_t i = 0; success && i < snapshot->hook_count; i++) {
        double hook_profile_overhead;

        if (!peak_report_formatter_slot_is_instrumented(snapshot, i) ||
            snapshot->num_calls[i] == 0) {
            continue;
        }
        hook_profile_overhead =
            (double)snapshot->num_calls[i] * snapshot->overhead_per_call;
        success = peak_report_formatter_write_csv_name(
                      csv, peak_report_formatter_name(snapshot, i)) &&
                  fprintf(
                      csv,
                      ",%lu,%lu,%lu,%.9e,%.9e,%.9e,%.9e,%.9e,%.9e,%.9e\n",
                      snapshot->num_calls[i],
                      peak_report_calls_per_active_thread(
                          snapshot->num_calls[i], snapshot->thread_count[i]),
                      snapshot->num_calls[i] / (unsigned long)rank_count,
                      (double)snapshot->max_time[i],
                      (double)snapshot->min_time[i],
                      snapshot->total_time[i],
                      snapshot->exclusive_time[i],
                      snapshot->max_total_time[i],
                      snapshot->min_total_time[i],
                      hook_profile_overhead) >= 0;
    }
    if (!success || ferror(csv)) {
        success = false;
        failure_errno = errno;
    }
    if (fclose(csv) != 0) {
        success = false;
        failure_errno = errno;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (success) {
        peak_report_formatter_test_signal_publication_phase(
            "before-publish");
    }
#endif
    if (success) {
        if (rename(temp_csv, out_csv) != 0) {
            success = false;
            failure_errno = errno;
        }
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (success) {
        peak_report_formatter_test_signal_publication_phase(
            "after-publish");
    }
#endif
    if (!success) {
        (void)unlink(temp_csv);
        peak_log_warn("[peak] failed to publish complete stats csv '%s': %s; "
                      "any existing completed csv was left unchanged\n",
                      out_csv,
                      strerror(failure_errno != 0 ? failure_errno : EIO));
    }
    free(temp_csv);
    free(out_csv);
    return success;
}

bool
peak_report_formatter_write_csv(const PeakReportSnapshot* snapshot)
{
    return peak_report_formatter_write_csv_scoped(snapshot, false);
}

bool
peak_report_formatter_write_rank_local_csv(
    const PeakReportSnapshot* snapshot)
{
    return peak_report_formatter_write_csv_scoped(snapshot, true);
}

bool
peak_report_formatter_write_text(
    const PeakReportSnapshot* snapshot,
    const PeakReportFormatOptions* options)
{
    const PeakReportOverhead* overhead;
    PeakReportTextSummary summary;
    const int max_function_width = PEAK_TEXT_REPORT_FUNCTION_WIDTH;
    const int max_col_width = PEAK_TEXT_REPORT_COLUMN_WIDTH;
    const int row_width = PEAK_TEXT_REPORT_ROW_WIDTH;
    char row_separator[PEAK_TEXT_REPORT_ROW_WIDTH + 1];
    char report_separator[PEAK_TEXT_REPORT_ROW_WIDTH + 1];
    int rank_count;

    if (snapshot == NULL || options == NULL) {
        return false;
    }
    overhead = &snapshot->overhead;
    summary = peak_report_formatter_summarize(snapshot);
    if (!summary.have_output || !options->print_text) {
        return true;
    }

    memset(row_separator, '-', (size_t)row_width);
    row_separator[row_width] = '\0';
    memset(report_separator, '=', (size_t)row_width);
    report_separator[row_width] = '\0';
    rank_count = peak_report_formatter_rank_count(snapshot);

    {
        const char* report_title = "PEAK Library Performance Report";
        int report_title_field =
            (int)(row_width + strlen(report_title)) / 2;

        peak_log_report("%s\n", report_separator);
        peak_log_report("%*s\n", report_title_field, report_title);
        peak_log_report("%s\n", report_separator);
    }

    peak_report_formatter_print_text_section("Application", row_separator);
    peak_log_report("Time: %f\n", summary.elapsed_seconds);
    if (overhead->valid) {
        peak_log_report("[peak] per-rank elapsed range: min_seconds=%.9f max_seconds=%.9f\n",
                        overhead->elapsed_min_seconds,
                        overhead->elapsed_max_seconds);
    }
    peak_log_report("PEAK done with: %s\n",
                    snapshot->program != NULL ? snapshot->program : "");
    if (rank_count > 1) {
        peak_log_report("Report scope: aggregate (%d MPI ranks)\n", rank_count);
    } else {
        peak_log_report("Report scope: local (1 process)\n");
    }
    peak_log_report("Instrumented targets: %zu\n",
                    summary.instrumented_targets);
    peak_log_report("Profiled targets: %zu\n", summary.profiled_targets);
    peak_log_report("Recorded calls: %s%llu\n",
                    summary.total_calls_saturated ? ">=" : "",
                    (unsigned long long)summary.total_calls);

    peak_report_formatter_print_text_section("Overhead summary",
                                             row_separator);
    peak_log_report("Estimated overhead: %.3es per call and %.3es total\n",
                    snapshot->overhead_per_call,
                    summary.total_overhead);
    if (overhead->valid) {
        if (overhead->per_rank_max) {
            peak_log_report("Aggregated profile estimate: %.9f s\n",
                            overhead->profile_seconds);
        } else {
            peak_log_report("Profile estimate: %.9f s (%8.4f%% of runtime)\n",
                            overhead->profile_seconds,
                            overhead->profile_ratio * 100.0);
            peak_log_report("Control stop windows: %.9f s (%8.4f%% of runtime)\n",
                            overhead->control_seconds,
                            overhead->control_ratio * 100.0);
            peak_log_report("Profile + control: %8.4f%% of runtime\n",
                            overhead->ratio * 100.0);
            peak_log_report("Local-rank risk estimate: %.9f s (%8.4f%% of runtime)\n",
                            overhead->profile_control_risk_seconds,
                            overhead->profile_control_risk_ratio * 100.0);
            peak_log_report("Heartbeat management CPU: %.9f s (%8.4f%% of runtime)\n",
                            overhead->management_seconds,
                            overhead->management_ratio * 100.0);
        }
    }

    peak_report_formatter_print_text_section("Controller accounting",
                                             row_separator);
    if (summary.stop_window_owner_rank >= 0) {
        peak_log_report("Control windows: %llu, %.9f s (%8.4f%%), owner rank %d\n",
                        (unsigned long long)summary.stop_window_count,
                        summary.stop_window_seconds,
                        summary.stop_window_ratio * 100.0,
                        summary.stop_window_owner_rank);
    } else {
        peak_log_report("Control windows: %llu, %.9f s (%8.4f%%), %s\n",
                        (unsigned long long)summary.stop_window_count,
                        summary.stop_window_seconds,
                        summary.stop_window_ratio * 100.0,
                        rank_count > 1 ? "rank-0 local" : "local");
    }
    peak_log_report("Failed control windows: %llu (%s scope)\n",
                    (unsigned long long)summary.failed_stop_window_count,
                    rank_count > 1 ? "aggregate" : "local");
    peak_log_report("Accounting snapshot: %s\n",
                    overhead->accounting_valid ? "valid" : "unavailable");
    peak_log_report("Transition coverage (%s, ever observed): %zu detached, %zu reattached, %zu revisited\n",
                    rank_count > 1 ? "aggregate" : "local",
                    summary.detached_targets,
                    summary.reattached_targets,
                    summary.revisited_targets);

    peak_report_formatter_print_text_section(
        "Detailed metrics (stable key=value)", row_separator);
    if (overhead->valid) {
        if (overhead->per_rank_max) {
            peak_report_formatter_write_rank_maxima(
                overhead->per_rank_maxima.tuples,
                overhead->per_rank_maxima.owner_ranks);
        } else {
            peak_log_report("[peak] local profile+control overhead: profile_seconds=%.9f control_seconds=%.9f profile_ratio=%.9f control_ratio=%.9f ratio=%.9f\n",
                            overhead->profile_seconds,
                            overhead->control_seconds,
                            overhead->profile_ratio,
                            overhead->control_ratio,
                            overhead->ratio);
            peak_log_report("[peak] local profile+local-rank-control risk: profile_seconds=%.9f raw_control_seconds=%.9f local_ranks=%u risk_control_seconds=%.9f ratio=%.9f\n",
                            overhead->profile_seconds,
                            overhead->control_seconds,
                            overhead->local_ranks,
                            overhead->control_risk_seconds,
                            overhead->profile_control_risk_ratio);
            peak_log_report("[peak] local profile+control risk overhead ratio: %.9f\n",
                            overhead->profile_control_risk_ratio);
            peak_log_report("[peak] local control risk overhead ratio: %.9f\n",
                            overhead->control_risk_ratio);
            peak_log_report("[peak] heartbeat management overhead: cpu_seconds=%.9f ratio=%.9f\n",
                            overhead->management_seconds,
                            overhead->management_ratio);
        }
    }
    if (summary.stop_window_owner_rank >= 0) {
        peak_log_report("[peak] owner/local control stop-window overhead: owner_rank=%d windows=%llu wall_seconds=%.9f ratio=%.9f\n",
                        summary.stop_window_owner_rank,
                        (unsigned long long)summary.stop_window_count,
                        summary.stop_window_seconds,
                        summary.stop_window_ratio);
    } else {
        peak_log_report("[peak] %s control stop-window overhead: windows=%llu wall_seconds=%.9f ratio=%.9f\n",
                        rank_count > 1 ? "rank-0/local" : "local",
                        (unsigned long long)summary.stop_window_count,
                        summary.stop_window_seconds,
                        summary.stop_window_ratio);
    }
    peak_log_report("[peak] %s failed control windows: windows=%llu snapshot_valid=%d\n",
                    rank_count > 1 ? "aggregate" : "local",
                    (unsigned long long)summary.failed_stop_window_count,
                    overhead->accounting_valid ? 1 : 0);
    peak_log_report("[peak] %s final transition coverage: detached_targets=%zu reattached_targets=%zu revisited_targets=%zu\n",
                    rank_count > 1 ? "aggregate" : "local",
                    summary.detached_targets,
                    summary.reattached_targets,
                    summary.revisited_targets);

    peak_report_formatter_print_text_section("Function call statistics",
                                             row_separator);
    peak_log_report("Detailed function statistics (call): counts and per-call timing in seconds\n");
    peak_log_report("|%*s|%*s|%*s|%*s|%*s|%*s|\n",
                    max_function_width,
                    "function",
                    max_col_width,
                    "calls",
                    max_col_width,
                    "per thread",
                    max_col_width,
                    "per rank",
                    max_col_width,
                    "max (s)",
                    max_col_width,
                    "min (s)");
    peak_log_report("%s\n", row_separator);
    for (size_t i = 0; i < snapshot->hook_count; i++) {
        const char* marker;
        int function_field_width;
        char* truncated_name;

        if (!peak_report_formatter_slot_is_instrumented(snapshot, i) ||
            snapshot->num_calls[i] == 0) {
            continue;
        }
        marker = "";
        if (snapshot->detached != NULL && snapshot->detached[i] != 0) {
            marker = snapshot->reattached != NULL &&
                             snapshot->reattached[i] != 0 ?
                         "**" :
                         "*";
        }
        function_field_width =
            max_function_width - (int)strlen(marker);
        truncated_name = peak_report_formatter_truncate_name(
            peak_report_formatter_name(snapshot, i),
            function_field_width,
            options->truncate_names);
        peak_log_report("|%*s%s|%*lu|%*lu|%*lu|%*.3e|%*.3e|\n",
                        function_field_width,
                        truncated_name != NULL ? truncated_name :
                                                 peak_report_formatter_name(
                                                     snapshot, i),
                        marker,
                        max_col_width,
                        snapshot->num_calls[i],
                        max_col_width,
                        peak_report_calls_per_active_thread(
                            snapshot->num_calls[i],
                            snapshot->thread_count[i]),
                        max_col_width,
                        snapshot->num_calls[i] / (unsigned long)rank_count,
                        max_col_width,
                        snapshot->max_time[i],
                        max_col_width,
                        snapshot->min_time[i]);
        free(truncated_name);
    }
    peak_log_report("%s\n", row_separator);
    peak_log_report("Markers: * ever detached; ** ever reattached after detachment.\n");
    peak_log_report("Revisited targets are summarized in Controller accounting.\n");

    peak_report_formatter_print_text_section("Function timing statistics",
                                             row_separator);
    peak_log_report("Detailed function statistics (thread): aggregate timing in seconds\n");
    peak_log_report("|%*s|%*s|%*s|%*s|%*s|%*s|\n",
                    max_function_width,
                    "function",
                    max_col_width,
                    "total (s)",
                    max_col_width,
                    "exclusive",
                    max_col_width,
                    "max (s)",
                    max_col_width,
                    "min (s)",
                    max_col_width,
                    "est. cost");
    peak_log_report("%s\n", row_separator);
    for (size_t i = 0; i < snapshot->hook_count; i++) {
        char* truncated_name;

        if (!peak_report_formatter_slot_is_instrumented(snapshot, i) ||
            snapshot->num_calls[i] == 0) {
            continue;
        }
        truncated_name = peak_report_formatter_truncate_name(
            peak_report_formatter_name(snapshot, i),
            max_function_width,
            options->truncate_names);
        peak_log_report("|%*s|%*.3f|%*.3f|%*.3f|%*.3f|%*.3e|\n",
                        max_function_width,
                        truncated_name != NULL ? truncated_name :
                                                 peak_report_formatter_name(
                                                     snapshot, i),
                        max_col_width,
                        snapshot->total_time[i],
                        max_col_width,
                        snapshot->exclusive_time[i],
                        max_col_width,
                        snapshot->max_total_time[i],
                        max_col_width,
                        snapshot->min_total_time[i],
                        max_col_width,
                        peak_report_calls_per_active_thread(
                            snapshot->num_calls[i],
                            snapshot->thread_count[i]) *
                            snapshot->overhead_per_call);
        free(truncated_name);
    }
    peak_log_report("%s\n", row_separator);
    errno = 0;
    if (fflush(stderr) != 0 || ferror(stderr)) {
        peak_log_warn("[peak] failed to flush the complete text report: %s\n",
                      strerror(errno != 0 ? errno : EIO));
        return false;
    }
    return true;
}
