#define _GNU_SOURCE

#include "internal/general_listener/report_formatter.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    peak_report_formatter_write_text(snapshot, options);
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
        "\"alpha\",5,3,2,5.000000000e-01,1.250000000e-01,"
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

    assert(peak_report_formatter_remove_csv());
    errno = 0;
    assert(access(csv_path, F_OK) != 0 && errno == ENOENT);
    assert(peak_report_formatter_remove_csv());
    peak_report_snapshot_destroy(prepared);
    peak_report_snapshot_destroy(snapshot);
}

static void
check_csv_quoted_name(const char* csv_path)
{
    static const char expected_name[] = "\"operator\"\"\"\"_x\",5,3,2,";
    PeakReportSnapshot* snapshot = create_fixture("operator\"\"_x");
    char* actual;

    peak_report_snapshot_prepare_for_render(snapshot);
    assert(peak_report_formatter_write_csv(snapshot));
    actual = read_file(csv_path);
    assert(strstr(actual, expected_name) != NULL);
    free(actual);
    assert(peak_report_formatter_remove_csv());
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
    assert(peak_report_formatter_remove_csv());

    peak_report_snapshot_destroy(snapshot);
    assert(rmdir(directory_c) == 0);
    assert(rmdir(directory_b) == 0);
    assert(rmdir(directory_a) == 0);
}

static void
check_partial_csv_removed(const char* temp_directory)
{
    char stats_base[512];
    char csv_path[768];
    PeakReportSnapshot* snapshot = create_fixture("write-failure");

    assert(snprintf(stats_base, sizeof(stats_base), "%s/full",
                    temp_directory) > 0);
    assert(snprintf(csv_path, sizeof(csv_path), "%s-p%d.csv",
                    stats_base, (int)getpid()) > 0);
    assert(symlink("/dev/full", csv_path) == 0);
    assert(setenv("PEAK_STATSLOG_PATH", stats_base, 1) == 0);
    peak_report_snapshot_prepare_for_render(snapshot);
    assert(!peak_report_formatter_write_csv(snapshot));
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
    check_no_output(csv_path);
    check_text_name_policy();
    check_long_stats_path(temp_directory);
    check_partial_csv_removed(temp_directory);

    assert(unsetenv("PEAK_STATSLOG_PATH") == 0);
    assert(rmdir(temp_directory) == 0);
    puts("report_formatter_test_ok");
    return 0;
}
