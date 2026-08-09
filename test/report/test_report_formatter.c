#define _GNU_SOURCE

#include "internal/general_listener/report_formatter.h"
#include "internal/general_listener/output_identity.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

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

static void
remove_directory_files(const char* directory)
{
    DIR* stream = opendir(directory);
    struct dirent* entry;

    assert(stream != NULL);
    while ((entry = readdir(stream)) != NULL) {
        char path[PATH_MAX];

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        assert(snprintf(path, sizeof(path), "%s/%s", directory,
                        entry->d_name) < (int)sizeof(path));
        assert(unlink(path) == 0);
    }
    assert(closedir(stream) == 0);
    assert(rmdir(directory) == 0);
}

static size_t
collect_strict_rank_local_files(const char* directory,
                                char paths[][PATH_MAX],
                                size_t capacity)
{
    DIR* stream = opendir(directory);
    struct dirent* entry;
    size_t count = 0;

    assert(stream != NULL);
    while ((entry = readdir(stream)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        assert(strlen(entry->d_name) <= NAME_MAX);
        assert(strstr(entry->d_name, ".tmp.") == NULL);
        assert(strstr(entry->d_name, ".peak-tmp.") == NULL);
        if (strstr(entry->d_name, "-ranklocal-h") == NULL) {
            continue;
        }
        assert(count < capacity);
        assert(snprintf(paths[count], PATH_MAX, "%s/%s", directory,
                        entry->d_name) < PATH_MAX);
        count++;
    }
    assert(closedir(stream) == 0);
    return count;
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
    snapshot->dropped_calls = 7;
    snapshot->dropped_threads = 3;
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
        "total_s,exclusive_s,thread_max_s,thread_min_s,overhead_s,dropped_calls,dropped_threads\n"
        "\"alpha\",5,3,2.5,5.000000000e-01,1.250000000e-01,"
        "1.250000000e+00,1.250000000e+00,7.500000000e-01,"
        "2.500000000e-01,5.000000000e-02,7,3\n"
        "\"PEAK_ACCOUNTING_DIAGNOSTICS\",0,0,0,0,0,0,0,0,0,0,7,3\n";
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
    char strict_hostname_path[1024];
    PeakReportSnapshot* snapshot = create_fixture("rank-local");

    clear_launcher_environment();
    assert(setenv("PMI_SIZE", "4", 1) == 0);
    assert(setenv("PMI_RANK", "3", 1) == 0);
    assert(setenv("PEAK_STATSLOG_PATH", stats_base, 1) == 0);
    peak_report_snapshot_prepare_for_render(snapshot);

    /* Aggregate naming remains unchanged even inside a multi-rank job. */
    assert(setenv("PEAK_STATSLOG_TEMPLATE", aggregate_path, 1) == 0);
    assert(peak_report_formatter_write_csv(snapshot));
    assert(access(aggregate_path, F_OK) == 0);
    assert(unlink(aggregate_path) == 0);

    assert(snprintf(rank_path,
                    sizeof(rank_path),
                    "%s-p%d-r3.csv",
                    stats_base,
                    (int)getpid()) > 0);
    assert(setenv("PEAK_STATSLOG_TEMPLATE", rank_path, 1) == 0);
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
    assert(setenv("PEAK_STATSLOG_TEMPLATE", hostname_path, 1) == 0);
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
    assert(snprintf(strict_hostname_path, sizeof(strict_hostname_path),
                    "%.*s-ranklocal-h%s.csv",
                    (int)(strlen(hostname_path) - 4), hostname_path,
                    hostname) > 0);
    assert(peak_report_formatter_write_rank_local_csv_host_disambiguated(
        snapshot));
    assert(access(hostname_path, F_OK) != 0);
    assert(access(strict_hostname_path, F_OK) == 0);
    assert(unlink(strict_hostname_path) == 0);

    clear_launcher_environment();
    peak_report_snapshot_destroy(snapshot);
}

static void
check_strict_rank_local_bounded_names(void)
{
    char ordinary_directory[] = "/tmp/peak_report_formatter_strict_XXXXXX";
    char long_directory[] = "/tmp/peak_report_formatter_long_XXXXXX";
    char aggregate_path[PATH_MAX];
    char strict_path[PATH_MAX];
    char strict_paths[2][PATH_MAX];
    char long_component[NAME_MAX + 1];
    char long_hostname[255];
    char alternate_long_hostname[255];
    PeakReportSnapshot* snapshot = create_fixture("strict-name-boundary");
    char* contents;

    assert(mkdtemp(ordinary_directory) != NULL);
    peak_report_snapshot_prepare_for_render(snapshot);

    assert(snprintf(aggregate_path, sizeof(aggregate_path), "%s/ordinary.csv",
                    ordinary_directory) < (int)sizeof(aggregate_path));
    assert(snprintf(strict_path, sizeof(strict_path),
                    "%s/ordinary-ranklocal-hnode-1.csv", ordinary_directory) <
           (int)sizeof(strict_path));
    assert(setenv("PEAK_TEST_REPORT_HOSTNAME", "node-1", 1) == 0);
    assert(setenv("PEAK_STATSLOG_TEMPLATE", aggregate_path, 1) == 0);
    assert(peak_report_formatter_write_csv(snapshot));
    assert(peak_report_formatter_write_rank_local_csv_host_disambiguated(
        snapshot));
    assert(access(aggregate_path, F_OK) == 0);
    assert(access(strict_path, F_OK) == 0);
    assert(!directory_has_prefix(ordinary_directory, "ordinary.csv.tmp."));
    assert(unlink(aggregate_path) == 0);
    assert(unlink(strict_path) == 0);

    /* A runtime component limit below the shortest safe strict name fails. */
    assert(setenv("PEAK_TEST_REPORT_NAME_MAX", "20", 1) == 0);
    assert(setenv("PEAK_STATSLOG_TEMPLATE", aggregate_path, 1) == 0);
    assert(!peak_report_formatter_write_rank_local_csv_host_disambiguated(
        snapshot));
    assert(access(aggregate_path, F_OK) != 0);
    assert(unsetenv("PEAK_TEST_REPORT_NAME_MAX") == 0);

    assert(snprintf(aggregate_path, sizeof(aggregate_path), "%s/bare",
                    ordinary_directory) < (int)sizeof(aggregate_path));
    assert(snprintf(strict_path, sizeof(strict_path),
                    "%s/bare-ranklocal-hhost_____", ordinary_directory) <
           (int)sizeof(strict_path));
    assert(setenv("PEAK_TEST_REPORT_HOSTNAME", "host:/\\?*", 1) == 0);
    assert(setenv("PEAK_STATSLOG_TEMPLATE", aggregate_path, 1) == 0);
    assert(peak_report_formatter_write_csv(snapshot));
    assert(peak_report_formatter_write_rank_local_csv_host_disambiguated(
        snapshot));
    assert(access(aggregate_path, F_OK) == 0);
    assert(access(strict_path, F_OK) == 0);
    assert(!directory_has_prefix(ordinary_directory, "bare.tmp."));
    assert(unlink(aggregate_path) == 0);
    assert(unlink(strict_path) == 0);
    assert(rmdir(ordinary_directory) == 0);

    memset(long_component, 'a', NAME_MAX - 4);
    memcpy(long_component + NAME_MAX - 4, ".csv", 5);
    memset(long_hostname, 'h', sizeof(long_hostname) - 1);
    long_hostname[sizeof(long_hostname) - 1] = '\0';
    memcpy(alternate_long_hostname, long_hostname, sizeof(long_hostname));
    alternate_long_hostname[sizeof(alternate_long_hostname) - 2] = 'i';
    assert(mkdtemp(long_directory) != NULL);
    assert(snprintf(aggregate_path, sizeof(aggregate_path), "%s/%s",
                    long_directory, long_component) < (int)sizeof(aggregate_path));
    assert(setenv("PEAK_TEST_REPORT_HOSTNAME", long_hostname, 1) == 0);
    assert(setenv("PEAK_STATSLOG_TEMPLATE", aggregate_path, 1) == 0);
    assert(peak_report_formatter_write_csv(snapshot));
    assert(peak_report_formatter_write_rank_local_csv_host_disambiguated(
        snapshot));
    assert(collect_strict_rank_local_files(long_directory, strict_paths, 2) == 1);
    assert(setenv("PEAK_TEST_REPORT_HOSTNAME", alternate_long_hostname, 1) == 0);
    assert(peak_report_formatter_write_rank_local_csv_host_disambiguated(
        snapshot));
    assert(collect_strict_rank_local_files(long_directory, strict_paths, 2) == 2);
    assert(strcmp(strict_paths[0], strict_paths[1]) != 0);
    assert(access(aggregate_path, F_OK) == 0);
    contents = read_file(aggregate_path);
    assert(strstr(contents, "function,") != NULL);
    free(contents);
    for (size_t index = 0; index < 2; index++) {
        contents = read_file(strict_paths[index]);
        assert(strstr(contents, "function,") != NULL);
        free(contents);
    }
    assert(unsetenv("PEAK_TEST_REPORT_HOSTNAME") == 0);
    clear_launcher_environment();
    peak_report_snapshot_destroy(snapshot);
    remove_directory_files(long_directory);
}

static void
check_csv_permissions(const char* csv_path)
{
    PeakReportSnapshot* snapshot = create_fixture("permissions");
    struct stat attributes;
    mode_t previous_umask;

    assert(setenv("PEAK_STATSLOG_TEMPLATE", csv_path, 1) == 0);
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
    assert(setenv("PEAK_STATSLOG_TEMPLATE", csv_path, 1) == 0);
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
check_dropped_only_output(const char* csv_path)
{
    PeakReportSnapshot* snapshot = peak_report_snapshot_create(1);
    const PeakReportFormatOptions options = {.print_text = true};
    char* text;
    char* csv;

    assert(snapshot != NULL);
    assert(setenv("PEAK_STATSLOG_TEMPLATE", csv_path, 1) == 0);
    assert(peak_report_snapshot_set_program(snapshot, "dropped-only"));
    assert(peak_report_snapshot_set_name(snapshot, 0, "untracked"));
    snapshot->instrumented[0] = 1;
    snapshot->dropped_calls = 9;
    snapshot->dropped_threads = 2;
    assert(peak_report_formatter_write_csv(snapshot));
    csv = read_file(csv_path);
    assert(strstr(csv, "\"PEAK_ACCOUNTING_DIAGNOSTICS\",0,0,0,0,0,0,0,0,0,0,9,2") != NULL);
    free(csv);
    text = capture_text_report(snapshot, &options);
    assert(strstr(text, "dropped_calls=9 dropped_threads=2") != NULL);
    free(text);
    assert(unlink(csv_path) == 0);
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
    assert(setenv("PEAK_STATSLOG_TEMPLATE", csv_path, 1) == 0);

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
    assert(setenv("PEAK_STATSLOG_TEMPLATE", csv_path, 1) == 0);
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

static void
check_output_template_and_no_clobber(const char* temp_directory)
{
    char stats_base[512];
    char template_path[768];
    char written_path[1024] = {0};
    DIR* directory;
    struct dirent* entry;
    PeakReportSnapshot* first = create_fixture("template-first");
    PeakReportSnapshot* second = create_fixture("template-second");
    char* contents;

    assert(snprintf(stats_base, sizeof(stats_base), "%s/template", temp_directory) > 0);
    assert(snprintf(template_path, sizeof(template_path),
                    "%s/{jobid}-{stepid}-{host}-{rank}-{pid}-{session}.csv",
                    temp_directory) > 0);
    assert(setenv("SLURM_JOB_ID", "job-77", 1) == 0);
    assert(setenv("SLURM_STEP_ID", "step-9", 1) == 0);
    assert(setenv("SLURM_PROCID", "6", 1) == 0);
    assert(setenv("PEAK_STATSLOG_PATH", stats_base, 1) == 0);
    assert(setenv("PEAK_STATSLOG_TEMPLATE", template_path, 1) == 0);
    peak_report_snapshot_prepare_for_render(first);
    assert(peak_report_formatter_write_csv(first));

    directory = opendir(temp_directory);
    assert(directory != NULL);
    while ((entry = readdir(directory)) != NULL) {
        if (strstr(entry->d_name, "job-77-step-9-") == entry->d_name) {
            assert(snprintf(written_path, sizeof(written_path), "%s/%s",
                            temp_directory, entry->d_name) > 0);
            break;
        }
    }
    assert(closedir(directory) == 0);
    assert(written_path[0] != '\0');
    assert(strstr(written_path, "-5-") != NULL);
    assert(strstr(written_path, "{session}") == NULL);
    contents = read_file(written_path);
    assert(strstr(contents, "template-first") != NULL);
    free(contents);

    peak_report_snapshot_prepare_for_render(second);
    assert(!peak_report_formatter_write_csv(second));
    contents = read_file(written_path);
    assert(strstr(contents, "template-first") != NULL);
    assert(strstr(contents, "template-second") == NULL);
    free(contents);

    assert(setenv("PEAK_OUTPUT_ALLOW_OVERWRITE", "1", 1) == 0);
    assert(peak_report_formatter_write_csv(second));
    contents = read_file(written_path);
    assert(strstr(contents, "template-first") == NULL);
    assert(strstr(contents, "template-second") != NULL);
    free(contents);
    assert(unsetenv("PEAK_OUTPUT_ALLOW_OVERWRITE") == 0);
    assert(unlink(written_path) == 0);

    assert(setenv("PEAK_STATSLOG_TEMPLATE", "{unknown}", 1) == 0);
    assert(!peak_report_formatter_write_csv(second));
    assert(unsetenv("SLURM_JOB_ID") == 0);
    assert(unsetenv("SLURM_STEP_ID") == 0);
    assert(unsetenv("SLURM_PROCID") == 0);
    peak_report_snapshot_destroy(second);
    peak_report_snapshot_destroy(first);
}

static void
check_concurrent_no_clobber(const char* temp_directory)
{
    char path[PATH_MAX];
    char component[NAME_MAX + 1];
    int start[2];
    pid_t children[32];
    unsigned int winners = 0;
    PeakReportSnapshot* snapshot = create_fixture("concurrent");

    memset(component, 'c', NAME_MAX - 4);
    memcpy(component + NAME_MAX - 4, ".csv", 5);
    assert(snprintf(path, sizeof(path), "%s/%s", temp_directory, component) <
           (int)sizeof(path));
    assert(setenv("PEAK_STATSLOG_TEMPLATE", path, 1) == 0);
    peak_report_snapshot_prepare_for_render(snapshot);
    assert(pipe(start) == 0);
    for (size_t index = 0; index < 32; index++) {
        children[index] = fork();
        assert(children[index] >= 0);
        if (children[index] == 0) {
            char signal;
            (void)close(start[1]);
            if (read(start[0], &signal, 1) != 1) _exit(2);
            _exit(peak_report_formatter_write_csv(snapshot) ? 0 : 1);
        }
    }
    assert(close(start[0]) == 0);
    for (size_t index = 0; index < 32; index++) {
        assert(write(start[1], "x", 1) == 1);
    }
    assert(close(start[1]) == 0);
    for (size_t index = 0; index < 32; index++) {
        int child_status;

        assert(waitpid(children[index], &child_status, 0) == children[index]);
        assert(WIFEXITED(child_status));
        winners += WEXITSTATUS(child_status) == 0;
    }
    assert(winners == 1);
    assert(access(path, F_OK) == 0);
    assert(!directory_has_prefix(temp_directory, ".peak-tmp."));
    assert(unlink(path) == 0);
    peak_report_snapshot_destroy(snapshot);
}

static void
check_template_parent_creation(const char* temp_directory)
{
    char template_path[768];
    char final_path[768];
    char directory_b[640];
    char directory_a[640];
    PeakReportSnapshot* snapshot = create_fixture("nested-template");

    assert(snprintf(directory_a, sizeof(directory_a), "%s/./nested", temp_directory) > 0);
    assert(snprintf(directory_b, sizeof(directory_b), "%s/job", directory_a) > 0);
    assert(snprintf(template_path, sizeof(template_path), "%s/report.csv", directory_b) > 0);
    assert(snprintf(final_path, sizeof(final_path), "%s", template_path) > 0);
    assert(setenv("PEAK_STATSLOG_TEMPLATE", template_path, 1) == 0);
    peak_report_snapshot_prepare_for_render(snapshot);
    assert(peak_report_formatter_write_csv(snapshot));
    assert(access(final_path, F_OK) == 0);
    assert(unlink(final_path) == 0);
    assert(rmdir(directory_b) == 0);
    assert(rmdir(directory_a) == 0);
    peak_report_snapshot_destroy(snapshot);
}

static void
check_near_path_max_destination(const char* temp_directory)
{
    enum { FINAL_LENGTH = PATH_MAX - 7, COMPONENT_LENGTH = 240 };
    static const char final_name[] = "final.csv";
    char directories[32][PATH_MAX];
    char current_directory[PATH_MAX];
    char final_path[PATH_MAX];
    char component[COMPONENT_LENGTH + 1];
    char* strict_name = NULL;
    FILE* direct;
    DIR* stream;
    struct dirent* entry;
    int directory_fd;
    size_t depth = 0;
    PeakReportSnapshot* first = create_fixture("near-path-first");
    PeakReportSnapshot* second = create_fixture("near-path-second");
    char* contents;

    assert(strlen(temp_directory) + 1 + strlen(final_name) < FINAL_LENGTH);
    assert(snprintf(directories[depth], sizeof(directories[depth]), "%s",
                    temp_directory) > 0);
    while (strlen(directories[depth]) + 1 + strlen(final_name) <
           FINAL_LENGTH) {
        size_t remaining = FINAL_LENGTH - strlen(directories[depth]) - 1 -
                           strlen(final_name);
        size_t component_length = remaining > COMPONENT_LENGTH + 1 ?
                                      COMPONENT_LENGTH : remaining - 1;

        assert(component_length != 0);
        assert(depth + 1 < sizeof(directories) / sizeof(directories[0]));
        memset(component, 'n', component_length);
        component[component_length] = '\0';
        assert(snprintf(current_directory, sizeof(current_directory), "%s",
                        directories[depth]) > 0);
        assert(snprintf(directories[depth + 1], PATH_MAX, "%s/%s",
                        current_directory, component) < PATH_MAX);
        assert(mkdir(directories[depth + 1], 0700) == 0);
        depth++;
    }
    assert(snprintf(final_path, sizeof(final_path), "%s/%s", directories[depth],
                    final_name) == FINAL_LENGTH);

    /* A legal final pathname at this length must work before PEAK uses it. */
    direct = fopen(final_path, "wx");
    assert(direct != NULL);
    assert(fputs("direct-final-path-ok\n", direct) >= 0);
    assert(fclose(direct) == 0);
    assert(unlink(final_path) == 0);

    assert(setenv("PEAK_STATSLOG_TEMPLATE", final_path, 1) == 0);
    assert(setenv("PEAK_TEST_REPORT_HOSTNAME", "near-path-host", 1) == 0);
    peak_report_snapshot_prepare_for_render(first);
    assert(peak_report_formatter_write_csv(first));
    contents = read_file(final_path);
    assert(strstr(contents, "near-path-first") != NULL);
    free(contents);

    assert(peak_report_formatter_write_rank_local_csv_host_disambiguated(
        first));
    directory_fd = open(directories[depth], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(directory_fd >= 0);
    stream = fdopendir(dup(directory_fd));
    assert(stream != NULL);
    while ((entry = readdir(stream)) != NULL) {
        if (strstr(entry->d_name, "-ranklocal-h") != NULL) {
            assert(strict_name == NULL);
            assert(strlen(entry->d_name) <= (size_t)fpathconf(directory_fd,
                                                               _PC_NAME_MAX));
            strict_name = strdup(entry->d_name);
            assert(strict_name != NULL);
        }
        assert(strstr(entry->d_name, ".tmp.") == NULL);
        assert(strstr(entry->d_name, ".peak-tmp.") == NULL);
    }
    assert(closedir(stream) == 0);
    assert(strict_name != NULL);
    direct = fdopen(openat(directory_fd, strict_name, O_RDONLY | O_CLOEXEC), "r");
    assert(direct != NULL);
    contents = read_stream(direct);
    assert(fclose(direct) == 0);
    assert(strstr(contents, "near-path-first") != NULL);
    free(contents);

    peak_report_snapshot_prepare_for_render(second);
    assert(!peak_report_formatter_write_csv(second));
    contents = read_file(final_path);
    assert(strstr(contents, "near-path-first") != NULL);
    assert(strstr(contents, "near-path-second") == NULL);
    free(contents);
    assert(setenv("PEAK_OUTPUT_ALLOW_OVERWRITE", "1", 1) == 0);
    assert(peak_report_formatter_write_csv(second));
    assert(unsetenv("PEAK_OUTPUT_ALLOW_OVERWRITE") == 0);
    contents = read_file(final_path);
    assert(strstr(contents, "near-path-first") == NULL);
    assert(strstr(contents, "near-path-second") != NULL);
    free(contents);

    assert(unlinkat(directory_fd, strict_name, 0) == 0);
    assert(unlink(final_path) == 0);
    free(strict_name);
    assert(close(directory_fd) == 0);
    assert(unsetenv("PEAK_TEST_REPORT_HOSTNAME") == 0);
    peak_report_snapshot_destroy(second);
    peak_report_snapshot_destroy(first);
    while (depth != 0) {
        assert(rmdir(directories[depth]) == 0);
        depth--;
    }
}

static void
check_output_identity_metadata(void)
{
    char path[512];

    assert(setenv("SLURM_JOB_ID", "job-77", 1) == 0);
    assert(setenv("SLURM_STEP_ID", "step-9", 1) == 0);
    assert(setenv("PMIX_RANK", "5", 1) == 0);
    peak_output_identity_initialize();
    assert(setenv("SLURM_JOB_ID", "../outside", 1) == 0);
    assert(unsetenv("SLURM_STEP_ID") == 0);
    assert(setenv("PMIX_RANK", "6", 1) == 0);
    assert(peak_output_identity_path(path, sizeof(path), "ignored",
                                     "/tmp/{jobid}/{stepid}/{rank}-{pid}-{session}.csv",
                                     ".csv", -1));
    assert(strstr(path, "/job-77/step-9/5-") != NULL);
    assert(peak_output_identity_path(path, sizeof(path), "ignored",
                                     "/tmp/{jobid}.csv", ".csv", -1));
    assert(strcmp(path, "/tmp/job-77.csv") == 0);
    assert(unsetenv("SLURM_JOB_ID") == 0);
    assert(unsetenv("PMIX_RANK") == 0);
}

int
main(void)
{
    char temp_directory[] = "/tmp/peak-report-formatter-XXXXXX";
    char stats_base[512];
    char csv_path[768];

    assert(mkdtemp(temp_directory) != NULL);
    check_output_identity_metadata();
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
    assert(setenv("PEAK_STATSLOG_TEMPLATE", csv_path, 1) == 0);

    check_csv_golden(csv_path);
    check_csv_quoted_name(csv_path);
    check_per_rank_average_precision(csv_path);
    check_rank_local_csv_names(stats_base, csv_path);
    check_csv_permissions(csv_path);
    check_no_output(csv_path);
    check_dropped_only_output(csv_path);
    check_text_name_policy();
    check_text_flush_failure();
    check_long_stats_path(temp_directory);
    check_failed_csv_never_replaces_final(temp_directory);
    check_output_template_and_no_clobber(temp_directory);
    check_concurrent_no_clobber(temp_directory);
    check_template_parent_creation(temp_directory);
    check_strict_rank_local_bounded_names();
    check_near_path_max_destination(temp_directory);

    assert(unsetenv("PEAK_STATSLOG_PATH") == 0);
    assert(unsetenv("PEAK_STATSLOG_TEMPLATE") == 0);
    assert(rmdir(temp_directory) == 0);
    puts("report_formatter_test_ok");
    return 0;
}
