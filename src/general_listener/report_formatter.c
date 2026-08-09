#define _POSIX_C_SOURCE 200809L

#include "internal/general_listener/report_formatter.h"

#include "internal/general_listener/output_identity.h"
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

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

enum {
    PEAK_TEXT_REPORT_FUNCTION_WIDTH = 32,
    PEAK_TEXT_REPORT_COLUMN_WIDTH = 12,
    PEAK_TEXT_REPORT_ROW_WIDTH =
        PEAK_TEXT_REPORT_FUNCTION_WIDTH +
        PEAK_TEXT_REPORT_COLUMN_WIDTH * 5 + 7,
    PEAK_REPORT_TEMP_CREATE_ATTEMPTS = 128,
    PEAK_REPORT_HOST_CAPACITY = 256,
    PEAK_REPORT_HOST_PREVIEW_LENGTH = 32,
    PEAK_REPORT_SUMMARY_HEX_LENGTH = 16,
};

static _Atomic unsigned long peak_report_formatter_temp_counter;

static long
peak_report_formatter_name_max(int dirfd)
{
    long limit;

#ifdef PEAK_ENABLE_TEST_HOOKS
    const char* override = getenv("PEAK_TEST_REPORT_NAME_MAX");
    char* end = NULL;

    if (override != NULL && override[0] != '\0') {
        errno = 0;
        limit = strtol(override, &end, 10);
        if (errno != 0 || end == override || *end != '\0' || limit <= 0) {
            return -1;
        }
        return limit;
    }
#endif
    errno = 0;
    limit = fpathconf(dirfd, _PC_NAME_MAX);
    return limit;
}

static size_t
peak_report_formatter_path_name_max(const char* path)
{
    const char* basename = strrchr(path, '/');
    char* directory = basename == NULL ? strdup(".") :
        strndup(path, (size_t)(basename - path) + 1);
    long limit;
    int dirfd;

    if (directory == NULL) {
        return 0;
    }
    dirfd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(directory);
    if (dirfd < 0) {
        return 0;
    }
    limit = peak_report_formatter_name_max(dirfd);
    (void)close(dirfd);
    if (limit < 0) {
        return 0;
    }
    return (size_t)limit;
}

static void
peak_report_formatter_sanitized_hostname(
    char hostname[PEAK_REPORT_HOST_CAPACITY])
{
    char local_hostname[PEAK_REPORT_HOST_CAPACITY] = {0};
    const char* source = local_hostname;
    size_t host_length = 0;

#ifdef PEAK_ENABLE_TEST_HOOKS
    const char* override = getenv("PEAK_TEST_REPORT_HOSTNAME");

    if (override != NULL && override[0] != '\0') {
        source = override;
    } else
#endif
    if (gethostname(local_hostname, sizeof(local_hostname) - 1) != 0 ||
        local_hostname[0] == '\0') {
        (void)snprintf(local_hostname, sizeof(local_hostname), "unknown");
    }
    for (size_t index = 0;
         source[index] != '\0' && host_length + 1 < PEAK_REPORT_HOST_CAPACITY;
         index++) {
        unsigned char byte = (unsigned char)source[index];

        hostname[host_length++] =
            (byte >= 'a' && byte <= 'z') ||
            (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' ||
            byte == '_' || byte == '.' ? (char)byte : '_';
    }
    if (host_length == 0) {
        (void)snprintf(hostname, PEAK_REPORT_HOST_CAPACITY, "unknown");
    } else {
        hostname[host_length] = '\0';
    }
}

static uint64_t
peak_report_formatter_summary_update(uint64_t summary, const char* text)
{
    for (; *text != '\0'; text++) {
        summary ^= (unsigned char)*text;
        summary *= UINT64_C(1099511628211);
    }
    summary ^= UINT64_C(0xff);
    return summary * UINT64_C(1099511628211);
}

static bool
peak_report_formatter_strict_rank_local_path(char out[PATH_MAX],
                                             const char* path,
                                             long world_rank)
{
    static const char marker[] = "-ranklocal-h";
    const char* basename = strrchr(path, '/');
    const char* extension;
    char hostname[PEAK_REPORT_HOST_CAPACITY] = {0};
    char rank_text[32];
    char summary[PEAK_REPORT_SUMMARY_HEX_LENGTH + 1];
    size_t directory_length;
    size_t basename_length;
    size_t stem_length;
    size_t extension_length;
    size_t component_limit;
    size_t hostname_length;
    size_t marker_length = sizeof(marker) - 1;
    size_t final_component_length;
    uint64_t hash = UINT64_C(1469598103934665603);

    if (basename == NULL) {
        basename = path;
        directory_length = 0;
    } else {
        directory_length = (size_t)(basename - path) + 1;
        basename++;
    }
    basename_length = strlen(basename);
    if (basename_length == 0 || directory_length >= PATH_MAX) {
        return false;
    }
    extension = basename_length >= 4 &&
                        strcmp(basename + basename_length - 4, ".csv") == 0 ?
                    ".csv" : "";
    extension_length = strlen(extension);
    stem_length = basename_length - extension_length;
    component_limit = peak_report_formatter_path_name_max(path);
    if (component_limit == 0) {
        return false;
    }
    if (component_limit > PATH_MAX - directory_length - 1) {
        component_limit = PATH_MAX - directory_length - 1;
    }
    peak_report_formatter_sanitized_hostname(hostname);
    hostname_length = strlen(hostname);
    if (snprintf(rank_text, sizeof(rank_text), "%ld", world_rank) < 0) {
        return false;
    }
    hash = peak_report_formatter_summary_update(hash, path);
    hash = peak_report_formatter_summary_update(hash, hostname);
    hash = peak_report_formatter_summary_update(hash, rank_text);
    if (snprintf(summary, sizeof(summary), "%016llx",
                 (unsigned long long)hash) !=
        PEAK_REPORT_SUMMARY_HEX_LENGTH) {
        return false;
    }
    final_component_length = stem_length + marker_length + hostname_length +
                             extension_length;
    if (final_component_length <= component_limit) {
        memcpy(out, path, directory_length);
        memcpy(out + directory_length, basename, stem_length);
        memcpy(out + directory_length + stem_length, marker, marker_length);
        memcpy(out + directory_length + stem_length + marker_length,
               hostname,
               hostname_length);
        memcpy(out + directory_length + stem_length + marker_length +
                   hostname_length,
               extension,
               extension_length + 1);
        return true;
    }

    /* Preserve a bounded preview, but digest all omitted path/host/rank bytes. */
    final_component_length = marker_length + 2 +
                             PEAK_REPORT_SUMMARY_HEX_LENGTH + extension_length;
    if (final_component_length > component_limit) {
        return false;
    }
    hostname_length = hostname_length > PEAK_REPORT_HOST_PREVIEW_LENGTH ?
                          PEAK_REPORT_HOST_PREVIEW_LENGTH : hostname_length;
    if (hostname_length > component_limit - final_component_length) {
        hostname_length = component_limit - final_component_length;
    }
    final_component_length += hostname_length;
    stem_length = stem_length > component_limit - final_component_length ?
                      component_limit - final_component_length : stem_length;
    memcpy(out, path, directory_length);
    memcpy(out + directory_length, basename, stem_length);
    memcpy(out + directory_length + stem_length, marker, marker_length);
    memcpy(out + directory_length + stem_length + marker_length,
           hostname,
           hostname_length);
    memcpy(out + directory_length + stem_length + marker_length +
               hostname_length,
           "-q",
           2);
    memcpy(out + directory_length + stem_length + marker_length +
               hostname_length + 2,
           summary,
           PEAK_REPORT_SUMMARY_HEX_LENGTH);
    memcpy(out + directory_length + stem_length + marker_length +
               hostname_length + 2 + PEAK_REPORT_SUMMARY_HEX_LENGTH,
           extension,
           extension_length + 1);
    return true;
}

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
    long current_rank = -1;
    long current_size = -1;
    int signal_number;

    (void)peak_general_listener_mpi_env_rank_size(
        &current_rank,
        &current_size);
    (void)current_size;

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
    } else if (strcmp(signal_text, "INT") == 0 ||
               strcmp(signal_text, "SIGINT") == 0 ||
               strcmp(signal_text, "2") == 0) {
        signal_number = SIGINT;
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

static long double
peak_report_formatter_calls_per_rank(unsigned long calls, int rank_count)
{
    int effective_rank_count = rank_count > 0 ? rank_count : 1;

    return (long double)calls / (long double)effective_rank_count;
}

static char*
peak_report_formatter_csv_path(bool rank_local,
                               bool require_host_suffix)
{
    const char* env_path = getenv("PEAK_STATSLOG_PATH");
    const char* template_value = getenv("PEAK_STATSLOG_TEMPLATE");
    const char* base = env_path != NULL && env_path[0] != '\0' ?
                           env_path : "./peak_statslog";
    long world_size = -1;
    long world_rank = -1;
    char path[PATH_MAX];

    if (rank_local) {
        bool have_pair = peak_general_listener_mpi_env_rank_size(
            &world_rank,
            &world_size);
        if (!have_pair || world_size <= 1) {
            world_rank = -1;
        }
    }
    if (!peak_output_identity_path(path,
                                   sizeof(path),
                                   base,
                                   template_value,
                                   ".csv",
                                   world_rank)) {
        return NULL;
    }
    if (require_host_suffix) {
        char suffixed[PATH_MAX];
        if (!peak_report_formatter_strict_rank_local_path(
                suffixed, path, world_rank)) {
            return NULL;
        }
        (void)snprintf(path, sizeof(path), "%s", suffixed);
    }
    if (template_value != NULL && template_value[0] != '\0' &&
        !peak_output_identity_make_parent(path)) {
        return NULL;
    }
    return strdup(path);
}

static int
peak_report_formatter_create_csv_temp(const char* out_csv,
                                      int* dirfd_out,
                                      char** final_name_out,
                                      char** temp_name_out)
{
    const char* basename;
    char* directory;
    char* final_name;
    char* temp_name;
    size_t basename_length;
    long name_max;
    bool compact_name;
    int length;
    int normal_suffix_length;

    if (out_csv == NULL || dirfd_out == NULL || final_name_out == NULL ||
        temp_name_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *dirfd_out = -1;
    *final_name_out = NULL;
    *temp_name_out = NULL;
    basename = strrchr(out_csv, '/');
    if (basename == NULL) {
        basename = out_csv;
        directory = strdup(".");
    } else {
        directory = strndup(out_csv, (size_t)(basename - out_csv) + 1);
        basename++;
    }
    if (directory == NULL) {
        return -1;
    }
    int dirfd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(directory);
    if (dirfd < 0) {
        return -1;
    }
    name_max = peak_report_formatter_name_max(dirfd);
    if (name_max < 0) {
        int saved_errno = errno;
        (void)close(dirfd);
        errno = saved_errno != 0 ? saved_errno : ENAMETOOLONG;
        return -1;
    }
    basename_length = strlen(basename);
    normal_suffix_length =
        snprintf(NULL, 0, ".tmp.p%d.%lu", (int)getpid(), ULONG_MAX);
    if (normal_suffix_length < 0) {
        (void)close(dirfd);
        errno = EINVAL;
        return -1;
    }
    compact_name = basename_length + (size_t)normal_suffix_length >
                   (size_t)name_max;
    length = compact_name ?
                 snprintf(NULL,
                          0,
                          ".peak-tmp.p%d.%lu",
                          (int)getpid(),
                          ULONG_MAX) :
                 snprintf(NULL,
                          0,
                          "%s.tmp.p%d.%lu",
                          basename,
                          (int)getpid(),
                          ULONG_MAX);
    if (length < 0 || length > name_max) {
        (void)close(dirfd);
        errno = EINVAL;
        return -1;
    }
    final_name = strdup(basename);
    temp_name = malloc((size_t)length + 1);
    if (final_name == NULL || temp_name == NULL) {
        free(final_name);
        free(temp_name);
        (void)close(dirfd);
        return -1;
    }

    for (unsigned int attempt = 0;
         attempt < PEAK_REPORT_TEMP_CREATE_ATTEMPTS;
         attempt++) {
        unsigned long ticket = atomic_fetch_add_explicit(
            &peak_report_formatter_temp_counter, 1UL, memory_order_relaxed);
        int fd;

        if (compact_name) {
            (void)snprintf(temp_name,
                           (size_t)length + 1,
                           ".peak-tmp.p%d.%lu",
                           (int)getpid(),
                           ticket);
        } else {
            (void)snprintf(temp_name,
                           (size_t)length + 1,
                           "%s.tmp.p%d.%lu",
                           basename,
                           (int)getpid(),
                           ticket);
        }
        fd = openat(dirfd, temp_name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                    0666);
        if (fd >= 0) {
            *dirfd_out = dirfd;
            *final_name_out = final_name;
            *temp_name_out = temp_name;
            return fd;
        }
        if (errno != EEXIST) {
            free(final_name);
            free(temp_name);
            (void)close(dirfd);
            return -1;
        }
    }

    free(final_name);
    free(temp_name);
    (void)close(dirfd);
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
    if (snapshot->dropped_calls != 0 || snapshot->dropped_threads != 0) {
        return true;
    }
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
                              snapshot->dropped_calls != 0 ||
                              snapshot->dropped_threads != 0 ||
                              peak_report_formatter_positive_finite(
                                  summary.stop_window_seconds);
    }
    return summary;
}

static bool
peak_report_formatter_write_csv_scoped(const PeakReportSnapshot* snapshot,
                                       bool rank_local,
                                       bool require_host_suffix)
{
    static const char header[] =
        "function,"
        "count,per_thread,per_rank,call_max_s,call_min_s,"
        "total_s,exclusive_s,thread_max_s,thread_min_s,overhead_s,"
        "dropped_calls,dropped_threads\n";
    char* out_csv;
    char* temp_csv;
    char* final_name;
    FILE* csv;
    bool success;
    int csv_fd;
    int dirfd;
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

    out_csv = peak_report_formatter_csv_path(
        rank_local,
        require_host_suffix);
    if (out_csv == NULL) {
        peak_log_warn("[peak] failed to allocate stats csv path\n");
        return false;
    }
    csv_fd = peak_report_formatter_create_csv_temp(out_csv,
                                                    &dirfd,
                                                    &final_name,
                                                    &temp_csv);
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
        (void)unlinkat(dirfd, temp_csv, 0);
        (void)close(dirfd);
        peak_log_warn("[peak] failed to open temporary stats csv for '%s': %s\n",
                      out_csv,
                      strerror(failure_errno));
        free(temp_csv);
        free(final_name);
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
                      ",%lu,%lu,%.12Lg,%.9e,%.9e,%.9e,%.9e,%.9e,%.9e,%.9e,"
                      "%llu,%llu\n",
                      snapshot->num_calls[i],
                      peak_report_calls_per_active_thread(
                          snapshot->num_calls[i], snapshot->thread_count[i]),
                      peak_report_formatter_calls_per_rank(
                          snapshot->num_calls[i], rank_count),
                      (double)snapshot->max_time[i],
                      (double)snapshot->min_time[i],
                      snapshot->total_time[i],
                      snapshot->exclusive_time[i],
                      snapshot->max_total_time[i],
                      snapshot->min_total_time[i],
                      hook_profile_overhead,
                      (unsigned long long)snapshot->dropped_calls,
                      (unsigned long long)snapshot->dropped_threads) >= 0;
    }
    if (success && (snapshot->dropped_calls != 0 ||
                    snapshot->dropped_threads != 0)) {
        success = peak_report_formatter_write_csv_name(
                      csv, "PEAK_ACCOUNTING_DIAGNOSTICS") &&
                  fprintf(csv,
                          ",0,0,0,0,0,0,0,0,0,0,%llu,%llu\n",
                          (unsigned long long)snapshot->dropped_calls,
                          (unsigned long long)snapshot->dropped_threads) >= 0;
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
        bool allow_overwrite = peak_general_listener_env_value_truthy(
            getenv("PEAK_OUTPUT_ALLOW_OVERWRITE"));

        if ((allow_overwrite ? renameat(dirfd, temp_csv, dirfd, final_name) :
                               linkat(dirfd, temp_csv, dirfd, final_name, 0)) != 0) {
            success = false;
            failure_errno = errno;
        } else if (!allow_overwrite) {
            (void)unlinkat(dirfd, temp_csv, 0);
        }
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (success) {
        peak_report_formatter_test_signal_publication_phase(
            "after-publish");
    }
#endif
    if (!success) {
        (void)unlinkat(dirfd, temp_csv, 0);
        peak_log_warn("[peak] failed to publish complete stats csv '%s': %s; "
                      "any existing completed csv was left unchanged\n",
                      out_csv,
                      strerror(failure_errno != 0 ? failure_errno : EIO));
    }
    free(temp_csv);
    free(final_name);
    (void)close(dirfd);
    free(out_csv);
    return success;
}

bool
peak_report_formatter_write_csv(const PeakReportSnapshot* snapshot)
{
    return peak_report_formatter_write_csv_scoped(snapshot, false, false);
}

bool
peak_report_formatter_write_rank_local_csv(
    const PeakReportSnapshot* snapshot)
{
    return peak_report_formatter_write_csv_scoped(snapshot, true, false);
}

bool
peak_report_formatter_write_rank_local_csv_host_disambiguated(
    const PeakReportSnapshot* snapshot)
{
    return peak_report_formatter_write_csv_scoped(snapshot, true, true);
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
    if (snapshot->degraded_mask != PEAK_PROFILER_DEGRADED_NONE) {
        char reasons[128];

        peak_report_snapshot_format_degraded_mask(snapshot->degraded_mask,
                                                  reasons,
                                                  sizeof(reasons));
        peak_log_report("Profiler degraded mode: enabled reasons=%s\n",
                        reasons[0] != '\0' ? reasons : "unknown");
    }
    if (snapshot->dropped_calls != 0 || snapshot->dropped_threads != 0) {
        peak_log_report("Accounting diagnostics: dropped_calls=%llu dropped_threads=%llu "
                        "(untracked, overflow, or unowned callers excluded)\n",
                        (unsigned long long)snapshot->dropped_calls,
                        (unsigned long long)snapshot->dropped_threads);
    }

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
                    "avg/rank",
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
        peak_log_report("|%*s%s|%*lu|%*lu|%*.6Lg|%*.3e|%*.3e|\n",
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
                        peak_report_formatter_calls_per_rank(
                            snapshot->num_calls[i], rank_count),
                        max_col_width,
                        snapshot->max_time[i],
                        max_col_width,
                        snapshot->min_time[i]);
        free(truncated_name);
    }
    peak_log_report("%s\n", row_separator);
    peak_log_report("Count semantics: calls is exact; per thread is the ceiling over active threads; avg/rank is the arithmetic mean over all %d report rank%s.\n",
                    rank_count,
                    rank_count == 1 ? "" : "s");
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
