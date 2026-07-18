#define _POSIX_C_SOURCE 200809L

#include "internal/general_listener/exec_checkpoint_writer.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int
read_file(const char* path, char* buffer, size_t buffer_size)
{
    FILE* file = fopen(path, "rb");
    size_t length;
    int read_failed;
    int close_failed;

    if (file == NULL) {
        return 1;
    }
    length = fread(buffer, 1, buffer_size - 1, file);
    read_failed = ferror(file) || !feof(file);
    close_failed = fclose(file) != 0;
    if (read_failed || close_failed) {
        return 1;
    }
    buffer[length] = '\0';
    return 0;
}

int
main(void)
{
    static const char expected[] =
        "function,"
        "count,per_thread,per_rank,call_max_s,call_min_s,"
        "total_s,exclusive_s,thread_max_s,thread_min_s,overhead_s\n"
        "\"a\"\"b\",5,3,5,4.000000000e+00,1.000000000e+00,"
        "6.000000000e+00,6.000000000e+00,5.000000000e+00,"
        "2.000000000e+00,1.250000000e+00\n";
    PeakExecCheckpointRow rows[] = {
        {
            .name = "a\"b",
            .num_calls = 5,
            .threads_seen = 2,
            .total_time = 6.0,
            .max_total_time = 5.0,
            .min_total_time = 2.0,
            .exclusive_time = 7.0,
            .max_time = 4.0f,
            .min_time = 1.0f,
        },
        {
            .name = "unused",
        },
    };
    char directory[] = "/tmp/peak-checkpoint-writer-XXXXXX";
    char base[PATH_MAX];
    char path_zero[PATH_MAX];
    char path_one[PATH_MAX];
    char contents[1024];
    struct stat attributes;
    int failed = 0;

    if (mkdtemp(directory) == NULL ||
        snprintf(base, sizeof(base), "%s/stats", directory) >=
            (int)sizeof(base) ||
        snprintf(path_zero,
                 sizeof(path_zero),
                 "%s-p%ld-exec0.csv",
                 base,
                 (long)getpid()) >= (int)sizeof(path_zero) ||
        snprintf(path_one,
                 sizeof(path_one),
                 "%s-p%ld-exec1.csv",
                 base,
                 (long)getpid()) >= (int)sizeof(path_one) ||
        setenv("PEAK_STATSLOG_PATH", base, 1) != 0) {
        fputs("exec_checkpoint_writer_test_failed\n", stderr);
        return 1;
    }

    errno = 0;
    if (peak_exec_checkpoint_write_rows(0, NULL, 1, 0.25) ||
        errno != EINVAL ||
        !peak_exec_checkpoint_write_rows(0, rows, 2, 0.25) ||
        !peak_exec_checkpoint_write_rows(0, rows, 2, 0.25) ||
        read_file(path_zero, contents, sizeof(contents)) ||
        strcmp(contents, expected) != 0 ||
        read_file(path_one, contents, sizeof(contents)) ||
        strcmp(contents, expected) != 0 ||
        stat(path_zero, &attributes) != 0 ||
        (attributes.st_mode & 0777) != 0600) {
        failed = 1;
    }

    unsetenv("PEAK_STATSLOG_PATH");
    unlink(path_zero);
    unlink(path_one);
    rmdir(directory);

    if (failed) {
        fputs("exec_checkpoint_writer_test_failed\n", stderr);
        return 1;
    }
    puts("exec_checkpoint_writer_test_ok");
    return 0;
}
