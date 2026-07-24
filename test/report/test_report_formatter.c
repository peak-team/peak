#define _GNU_SOURCE

#include "internal/general_listener/report_formatter.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

enum { TEST_HOSTNAME_CAPACITY = 256 };

static const char* const launcher_environment[] = {
    "PMI_SIZE",
    "PMIX_SIZE",
    "OMPI_COMM_WORLD_SIZE",
    "MV2_COMM_WORLD_SIZE",
    "I_MPI_SIZE",
    "SLURM_NTASKS",
    "PMI_RANK",
    "PMIX_RANK",
    "OMPI_COMM_WORLD_RANK",
    "MV2_COMM_WORLD_RANK",
    "I_MPI_RANK",
    "SLURM_PROCID",
    NULL,
};

static void
clear_launcher_environment(void)
{
    for (const char* const* name = launcher_environment;
         *name != NULL;
         name++) {
        assert(unsetenv(*name) == 0);
    }
}

static void
sanitized_hostname(char sanitized[TEST_HOSTNAME_CAPACITY])
{
    char hostname[TEST_HOSTNAME_CAPACITY] = {0};
    size_t output_length = 0;

    if (gethostname(hostname, sizeof(hostname) - 1) != 0 ||
        hostname[0] == '\0') {
        assert(snprintf(hostname, sizeof(hostname), "unknown") > 0);
    }
    hostname[sizeof(hostname) - 1] = '\0';
    for (size_t i = 0;
         hostname[i] != '\0' &&
         output_length + 1 < TEST_HOSTNAME_CAPACITY;
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
        assert(snprintf(sanitized,
                        TEST_HOSTNAME_CAPACITY,
                        "unknown") > 0);
        return;
    }
    sanitized[output_length] = '\0';
}

static bool
directory_has_prefix(const char* directory, const char* prefix)
{
    DIR* stream = opendir(directory);
    struct dirent* entry;
    bool found = false;

    assert(stream != NULL);
    while ((entry = readdir(stream)) != NULL) {
        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0) {
            found = true;
            break;
        }
    }
    assert(closedir(stream) == 0);
    return found;
}

static char*
read_stream(FILE* stream)
{
    long length;
    char* contents;

    assert(fflush(stream) == 0);
    assert(fseek(stream, 0, SEEK_END) == 0);
    length = ftell(stream);
    assert(length >= 0);
    assert(fseek(stream, 0, SEEK_SET) == 0);
    contents = malloc((size_t)length + 1);
    assert(contents != NULL);
    assert(fread(contents, 1, (size_t)length, stream) == (size_t)length);
    contents[length] = '\0';
    return contents;
}

static char*
read_file(const char* path)
{
    FILE* file = fopen(path, "r");
    char* contents;

    assert(file != NULL);
    contents = read_stream(file);
    assert(fclose(file) == 0);
    return contents;
}

static char*
capture_text_report(const PeakReportSnapshot* snapshot,
                    const PeakReportFormatOptions* options)
{
    FILE* capture = tmpfile();
    int saved_stderr;
    char* contents;

    assert(capture != NULL);
    assert(fflush(stderr) == 0);
    saved_stderr = dup(STDERR_FILENO);
    assert(saved_stderr >= 0);
    assert(dup2(fileno(capture), STDERR_FILENO) >= 0);
    assert(peak_report_formatter_write_text(snapshot, options));
    assert(fflush(stderr) == 0);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    assert(close(saved_stderr) == 0);

    contents = read_stream(capture);
    assert(fclose(capture) == 0);
    return contents;
}

static PeakReportSnapshot*
create_fixture(const char* name)
{
    PeakReportSnapshot* snapshot = peak_report_snapshot_create(2);

    assert(snapshot != NULL);
    assert(peak_report_snapshot_set_program(snapshot, "milc -i input"));
    assert(peak_report_snapshot_set_name(snapshot, 0, name));
    assert(peak_report_snapshot_set_name(snapshot, 1, "ignored"));
    snapshot->instrumented[0] = 1;
    snapshot->num_calls[0] = 5;
    snapshot->thread_count[0] = 2;
    snapshot->max_time[0] = 0.5F;
    snapshot->min_time[0] = 0.125F;
    snapshot->total_time[0] = 1.25;
    snapshot->exclusive_time[0] = 2.5;
    snapshot->max_total_time[0] = 0.75;
    snapshot->min_total_time[0] = 0.25;
    snapshot->overhead_per_call = 0.01;
    snapshot->rank_count = 2;

    snapshot->instrumented[1] = 0;
    snapshot->num_calls[1] = 99;

    snapshot->overhead.valid = true;
    snapshot->overhead.accounting_valid = true;
    snapshot->overhead.local_ranks = 2;
    snapshot->overhead.stop_window_count = 3;
    snapshot->overhead.failed_stop_window_count = 1;
    snapshot->overhead.elapsed_seconds = 10.0;
    snapshot->overhead.elapsed_min_seconds = 9.0;
    snapshot->overhead.elapsed_max_seconds = 11.0;
    snapshot->overhead.profile_seconds = 0.05;
    snapshot->overhead.control_seconds = 0.25;
    snapshot->overhead.management_seconds = 0.125;
    snapshot->overhead.control_risk_seconds = 0.5;
    snapshot->overhead.profile_control_risk_seconds = 0.55;
    snapshot->overhead.profile_ratio = 0.005;
    snapshot->overhead.control_ratio = 0.025;
    snapshot->overhead.profile_control_risk_ratio = 0.055;
    snapshot->overhead.control_risk_ratio = 0.05;
    snapshot->overhead.management_ratio = 0.0125;
    snapshot->overhead.ratio = 0.03;
    return snapshot;
}

static void
check_csv_golden(const char* csv_path)
{
    static const char expected[] =
        "function,count,per_thread,per_rank,call_max_s,call_min_s,"
        "total_s,exclusive_s,thread_max_s,thread_min_s,overhead_s\n"
        "\"alpha\",5,3,2.5,5.000000000e-01,1.250000000e-01,"
        "1.250000000e+00,1.250000000e+00,7.500000000e-01,"
        "2.500000000e-01,5.000000000e-02\n";
    PeakReportSnapshot* snapshot = create_fixture("alpha");
    PeakReportSnapshot* prepared = peak_report_snapshot_clone(snapshot);
    char* actual;

    assert(prepared != NULL);
    peak_report_snapshot_prepare_for_render(prepared);
    assert(peak_report_formatter_write_csv(prepared));
    assert(snapshot->exclusive_time[0] == 2.5);
    assert(strcmp(snapshot->program, "milc -i input") == 0);
    actual = read_file(csv_path);
    assert(strcmp(actual, expected) == 0);
    free(actual);

    assert(unlink(csv_path) == 0);
    errno = 0;
    assert(access(csv_path, F_OK) != 0 && errno == ENOENT);
    errno = 0;
    assert(unlink(csv_path) != 0 && errno == ENOENT);
    peak_report_snapshot_destroy(prepared);
    peak_report_snapshot_destroy(snapshot);
}

static void
check_csv_quoted_name(const char* csv_path)
{
    static const char expected_name[] = "\"operator\"\"\"\"_x\",5,3,2.5,";
    PeakReportSnapshot* snapshot = create_fixture("operator\"\"_x");
    char* actual;

    peak_report_snapshot_prepare_for_render(snapshot);
    assert(peak_report_formatter_write_csv(snapshot));
    actual = read_file(csv_path);
    assert(strstr(actual, expected_name) != NULL);
    free(actual);
    assert(unlink(csv_path) == 0);
    peak_report_snapshot_destroy(snapshot);
}

static void
check_per_rank_average_precision(const char* csv_path)
{
    const PeakReportFormatOptions options = {.print_text = true};
    PeakReportSnapshot* snapshot = create_fixture("sparse-rank-calls");
    char* actual;
    char* text;

    snapshot->num_calls[0] = 1;
    snapshot->thread_count[0] = 1;
    snapshot->rank_count = 4096;
    peak_report_snapshot_prepare_for_render(snapshot);
    assert(peak_report_formatter_write_csv(snapshot));
    actual = read_file(csv_path);
    assert(strstr(actual,
                  "\"sparse-rank-calls\",1,1,0.000244140625,") != NULL);
    free(actual);
    assert(unlink(csv_path) == 0);

    text = capture_text_report(snapshot, &options);
    assert(strstr(text, "avg/rank") != NULL);
    assert(strstr(text, "0.000244141") != NULL);
    assert(strstr(text,
                  "calls is exact; per thread is the ceiling over active "
                  "threads; avg/rank is the arithmetic mean over all 4096 "
                  "report ranks.") != NULL);
    free(text);

    snapshot->num_calls[0] = 8192;
    peak_report_snapshot_prepare_for_render(snapshot);
    assert(peak_report_formatter_write_csv(snapshot));
    actual = read_file(csv_path);
    assert(strstr(actual, "\"sparse-rank-calls\",8192,8192,2,") != NULL);
    free(actual);
    assert(unlink(csv_path) == 0);

    snapshot->num_calls[0] = 1;
    snapshot->rank_count = 0;
    peak_report_snapshot_prepare_for_render(snapshot);
    assert(peak_report_formatter_write_csv(snapshot));
    actual = read_file(csv_path);
    assert(strstr(actual, "\"sparse-rank-calls\",1,1,1,") != NULL);
    free(actual);
    assert(unlink(csv_path) == 0);
    peak_report_snapshot_destroy(snapshot);
}

static void
check_rank_local_csv_names(const char* stats_base,
                           const char* aggregate_path)
{
    char rank_path[768];
    char hostname[TEST_HOSTNAME_CAPACITY] = {0};
    char hostname_path[1024];
    PeakReportSnapshot* snapshot = create_fixture("rank-local");

    clear_launcher_environment();
    assert(setenv("PMI_SIZE", "4", 1) == 0);
    assert(setenv("PMI_RANK", "3", 1) == 0);
    assert(setenv("PEAK_STATSLOG_PATH", stats_base, 1) == 0);
    peak_report_snapshot_prepare_for_render(snapshot);

    /* Aggregate naming remains unchanged even inside a multi-rank job. */
    assert(peak_report_formatter_write_csv(snapshot));
    assert(access(aggregate_path, F_OK) == 0);
    assert(unlink(aggregate_path) == 0);

    assert(snprintf(rank_path,
                    sizeof(rank_path),
                    "%s-p%d-r3.csv",
                    stats_base,
                    (int)getpid()) > 0);
    assert(peak_report_formatter_write_rank_local_csv(snapshot));
    assert(access(rank_path, F_OK) == 0);
    assert(access(aggregate_path, F_OK) != 0);
    assert(unlink(rank_path) == 0);

    /* An out-of-range launcher rank must not become part of a pathname. */
    assert(setenv("PMI_RANK", "4", 1) == 0);
    sanitized_hostname(hostname);
    assert(snprintf(hostname_path,
                    sizeof(hostname_path),
                    "%s-p%d-h%s.csv",
                    stats_base,
                    (int)getpid(),
                    hostname) > 0);
    assert(peak_report_formatter_write_rank_local_csv(snapshot));
    assert(access(hostname_path, F_OK) == 0);
    assert(unlink(hostname_path) == 0);

    /* Complete launcher namespaces must agree; conflicts use host naming. */
    assert(setenv("PMI_RANK", "3", 1) == 0);
    assert(setenv("OMPI_COMM_WORLD_RANK", "1", 1) == 0);
    assert(setenv("OMPI_COMM_WORLD_SIZE", "4", 1) == 0);
    assert(peak_report_formatter_write_rank_local_csv(snapshot));
    assert(access(hostname_path, F_OK) == 0);
    assert(unlink(hostname_path) == 0);

    /* Strict MPI fallback naming reduces cross-node PID collision risk. */
    clear_launcher_environment();
    assert(peak_report_formatter_write_rank_local_csv_host_disambiguated(
        snapshot));
    assert(access(hostname_path, F_OK) == 0);
    assert(unlink(hostname_path) == 0);

    clear_launcher_environment();
    peak_report_snapshot_destroy(snapshot);
}

static void
check_csv_permissions(const char* csv_path)
{
    PeakReportSnapshot* snapshot = create_fixture("permissions");
    struct stat attributes;
    mode_t previous_umask;

    peak_report_snapshot_prepare_for_render(snapshot);
    previous_umask = umask(0027);
    assert(peak_report_formatter_write_csv(snapshot));
    (void)umask(previous_umask);
    assert(stat(csv_path, &attributes) == 0);
    assert((attributes.st_mode & 0777) == 0640);
    assert(unlink(csv_path) == 0);
    peak_report_snapshot_destroy(snapshot);
}

static void
check_no_output(const char* csv_path)
{
    PeakReportSnapshot* snapshot = peak_report_snapshot_create(1);
    const PeakReportFormatOptions options = {.print_text = true};
    char* text;

    assert(snapshot != NULL);
    assert(peak_report_snapshot_set_program(snapshot, "idle"));
    assert(peak_report_snapshot_set_name(snapshot, 0, "idle_hook"));
    snapshot->instrumented[0] = 1;
    assert(peak_report_formatter_write_csv(snapshot));
    text = capture_text_report(snapshot, &options);
    assert(text[0] == '\0');
    assert(access(csv_path, F_OK) != 0);
    free(text);
    peak_report_snapshot_destroy(snapshot);
}

static void
check_text_name_policy(void)
{
    static const char long_name[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char truncated_name[] =
        "abcdefghijklmnopqrstuvwxyzABC...";
    PeakReportSnapshot* snapshot = create_fixture(long_name);
    PeakReportFormatOptions options = {.print_text = true};
    char* text;

    peak_report_snapshot_prepare_for_render(snapshot);
    text = capture_text_report(snapshot, &options);
    assert(strstr(text, "PEAK done with: milc -i input\n") != NULL);
    assert(strstr(text, "Report scope: aggregate (2 MPI ranks)\n") != NULL);
    assert(strstr(text, long_name) != NULL);
    free(text);

    options.print_text = false;
    text = capture_text_report(snapshot, &options);
    assert(text[0] == '\0');
    free(text);

    options.print_text = true;
    options.truncate_names = true;
    snapshot->detached[0] = 1;
    snapshot->reattached[0] = 1;
    text = capture_text_report(snapshot, &options);
    assert(strstr(text, truncated_name) != NULL);
    assert(strstr(text, long_name) == NULL);
    assert(strstr(text, "abcdefghijklmnopqrstuvwxyzA...**|") != NULL);
    free(text);
    peak_report_snapshot_destroy(snapshot);
}

static void
check_text_flush_failure(void)
{
    PeakReportSnapshot* snapshot = create_fixture("flush-failure");
    const PeakReportFormatOptions options = {.print_text = true};
    int full_fd;
    int saved_stderr;

    peak_report_snapshot_prepare_for_render(snapshot);
    assert(fflush(stderr) == 0);
    saved_stderr = dup(STDERR_FILENO);
    assert(saved_stderr >= 0);
    full_fd = open("/dev/full", O_WRONLY);
    assert(full_fd >= 0);
    assert(dup2(full_fd, STDERR_FILENO) >= 0);
    assert(close(full_fd) == 0);
    clearerr(stderr);
    assert(!peak_report_formatter_write_text(snapshot, &options));
    clearerr(stderr);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    assert(close(saved_stderr) == 0);
    peak_report_snapshot_destroy(snapshot);
}

static void
check_long_stats_path(const char* temp_directory)
{
    char segment_a[91];
    char segment_b[91];
    char segment_c[91];
    char directory_a[1024];
    char directory_b[1024];
    char directory_c[1024];
    char stats_base[1024];
    char csv_path[1200];
    PeakReportSnapshot* snapshot = create_fixture("long-path");

    memset(segment_a, 'a', sizeof(segment_a) - 1);
    memset(segment_b, 'b', sizeof(segment_b) - 1);
    memset(segment_c, 'c', sizeof(segment_c) - 1);
    segment_a[sizeof(segment_a) - 1] = '\0';
    segment_b[sizeof(segment_b) - 1] = '\0';
    segment_c[sizeof(segment_c) - 1] = '\0';
    assert(snprintf(directory_a, sizeof(directory_a), "%s/%s",
                    temp_directory, segment_a) > 0);
    assert(snprintf(directory_b, sizeof(directory_b), "%s/%s",
                    directory_a, segment_b) > 0);
    assert(snprintf(directory_c, sizeof(directory_c), "%s/%s",
                    directory_b, segment_c) > 0);
    assert(mkdir(directory_a, 0700) == 0);
    assert(mkdir(directory_b, 0700) == 0);
    assert(mkdir(directory_c, 0700) == 0);
    assert(snprintf(stats_base, sizeof(stats_base), "%s/stats",
                    directory_c) > 255);
    assert(snprintf(csv_path, sizeof(csv_path), "%s-p%d.csv",
                    stats_base, (int)getpid()) > 0);
    assert(setenv("PEAK_STATSLOG_PATH", stats_base, 1) == 0);

    peak_report_snapshot_prepare_for_render(snapshot);
    assert(peak_report_formatter_write_csv(snapshot));
    assert(access(csv_path, F_OK) == 0);
    assert(unlink(csv_path) == 0);

    peak_report_snapshot_destroy(snapshot);
    assert(rmdir(directory_c) == 0);
    assert(rmdir(directory_b) == 0);
    assert(rmdir(directory_a) == 0);
}

static void
check_failed_csv_never_replaces_final(const char* temp_directory)
{
    static const char preserved[] = "previous-complete-report\n";
    char stats_base[512];
    char csv_path[768];
    char temp_prefix[256];
    char* actual;
    const char* csv_name;
    PeakReportSnapshot* snapshot = create_fixture("write-failure");
    FILE* existing;
    struct rlimit previous_limit;
    struct rlimit limited;
    struct sigaction ignore_signal = {0};
    struct sigaction previous_signal;

    assert(snprintf(stats_base, sizeof(stats_base), "%s/close-failure",
                    temp_directory) > 0);
    assert(snprintf(csv_path, sizeof(csv_path), "%s-p%d.csv",
                    stats_base, (int)getpid()) > 0);
    assert(setenv("PEAK_STATSLOG_PATH", stats_base, 1) == 0);
    existing = fopen(csv_path, "w");
    assert(existing != NULL);
    assert(fputs(preserved, existing) >= 0);
    assert(fclose(existing) == 0);

    assert(getrlimit(RLIMIT_FSIZE, &previous_limit) == 0);
    assert(previous_limit.rlim_cur == RLIM_INFINITY ||
           previous_limit.rlim_cur > 1);
    limited = previous_limit;
    limited.rlim_cur = 1;
    ignore_signal.sa_handler = SIG_IGN;
    assert(sigemptyset(&ignore_signal.sa_mask) == 0);
    assert(sigaction(SIGXFSZ, &ignore_signal, &previous_signal) == 0);
    assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);

    peak_report_snapshot_prepare_for_render(snapshot);
    assert(!peak_report_formatter_write_csv(snapshot));
    actual = read_file(csv_path);
    assert(strcmp(actual, preserved) == 0);
    free(actual);

    assert(unlink(csv_path) == 0);
    assert(!peak_report_formatter_write_csv(snapshot));
    errno = 0;
    assert(access(csv_path, F_OK) != 0 && errno == ENOENT);

    assert(setrlimit(RLIMIT_FSIZE, &previous_limit) == 0);
    assert(sigaction(SIGXFSZ, &previous_signal, NULL) == 0);

    csv_name = strrchr(csv_path, '/');
    assert(csv_name != NULL);
    csv_name++;
    assert(snprintf(temp_prefix,
                    sizeof(temp_prefix),
                    "%s.tmp.",
                    csv_name) > 0);
    assert(!directory_has_prefix(temp_directory, temp_prefix));
    errno = 0;
    assert(access(csv_path, F_OK) != 0 && errno == ENOENT);
    peak_report_snapshot_destroy(snapshot);
}

int
main(void)
{
    char temp_directory[] = "/tmp/peak-report-formatter-XXXXXX";
    char stats_base[512];
    char csv_path[768];

    assert(mkdtemp(temp_directory) != NULL);
    clear_launcher_environment();
    assert(snprintf(stats_base,
                    sizeof(stats_base),
                    "%s/stats",
                    temp_directory) > 0);
    assert(snprintf(csv_path,
                    sizeof(csv_path),
                    "%s-p%d.csv",
                    stats_base,
                    (int)getpid()) > 0);
    assert(setenv("PEAK_STATSLOG_PATH", stats_base, 1) == 0);

    check_csv_golden(csv_path);
    check_csv_quoted_name(csv_path);
    check_per_rank_average_precision(csv_path);
    check_rank_local_csv_names(stats_base, csv_path);
    check_csv_permissions(csv_path);
    check_no_output(csv_path);
    check_text_name_policy();
    check_text_flush_failure();
    check_long_stats_path(temp_directory);
    check_failed_csv_never_replaces_final(temp_directory);

    assert(unsetenv("PEAK_STATSLOG_PATH") == 0);
    assert(rmdir(temp_directory) == 0);
    puts("report_formatter_test_ok");
    return 0;
}
