#define _POSIX_C_SOURCE 200809L

#include "internal/general_listener/output_identity.h"
#include "internal/general_listener/exec_checkpoint_writer.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum { THREADS = 12 };

struct identity_thread {
    char path[PATH_MAX];
    int ok;
};

static void *
render_identity(void *opaque)
{
    struct identity_thread *thread = opaque;

    thread->ok = peak_output_identity_path(thread->path, sizeof(thread->path),
                                           getenv("PEAK_STATSLOG_PATH"),
                                           getenv("PEAK_STATSLOG_TEMPLATE"),
                                           ".csv", -1);
    return NULL;
}

static int
checkpoint(char path[PATH_MAX])
{
    return peak_output_identity_checkpoint_path(path, PATH_MAX, 7);
}

static int
write_cached_checkpoint(void)
{
    PeakExecCheckpointRow row = {
        .name = "identity",
        .num_calls = 1,
        .threads_seen = 1,
        .total_time = 1.0,
        .max_total_time = 1.0,
        .min_total_time = 1.0,
        .exclusive_time = 1.0,
        .max_time = 1.0f,
        .min_time = 1.0f,
    };
    char path[PATH_MAX];

    if (peak_output_identity_checkpoint_path(path, sizeof(path), 0) != PEAK_OUTPUT_CHECKPOINT_READY ||
        !peak_exec_checkpoint_write_rows(0, &row, 1, 0.0)) {
        return 1;
    }
    return access(path, F_OK) == 0 ? 0 : 1;
}

static void
set_common(const char *base)
{
    assert(setenv("PEAK_STATSLOG_PATH", base, 1) == 0);
    assert(setenv("SLURM_JOB_ID", "job-exec", 1) == 0);
    assert(setenv("SLURM_STEP_ID", "step", 1) == 0);
    assert(setenv("PMI_RANK", "3", 1) == 0);
}

static int
run_preinit(void)
{
    char path[PATH_MAX];

    return checkpoint(path) == PEAK_OUTPUT_CHECKPOINT_PREINIT ? 0 : 1;
}

static int
run_default(const char *directory)
{
    char base[PATH_MAX];
    char path[PATH_MAX];

    assert(snprintf(base, sizeof(base), "%s/stats", directory) < (int)sizeof(base));
    set_common(base);
    peak_output_identity_initialize();
    if (checkpoint(path) != PEAK_OUTPUT_CHECKPOINT_READY || strstr(path, "-jjob-exec-sstep-") == NULL ||
        strstr(path, "-r3-p") == NULL || strstr(path, "-exec7.csv") == NULL) {
        return 1;
    }
    return write_cached_checkpoint();
}

static int
run_template(const char *directory, int with_extension)
{
    char base[PATH_MAX];
    char template_path[PATH_MAX];
    char path[PATH_MAX];
    char parent[PATH_MAX];
    char *slash;
    struct stat status;

    assert(snprintf(base, sizeof(base), "%s/stats", directory) < (int)sizeof(base));
    assert(snprintf(template_path, sizeof(template_path),
                    "%s/nested/{jobid}/report%s", directory,
                    with_extension ? ".csv" : "") < (int)sizeof(template_path));
    set_common(base);
    assert(setenv("PEAK_STATSLOG_TEMPLATE", template_path, 1) == 0);
    peak_output_identity_initialize();
    if (checkpoint(path) != PEAK_OUTPUT_CHECKPOINT_READY ||
        strstr(path, "/nested/job-exec/report-exec7.csv") == NULL) {
        return 1;
    }
    assert(snprintf(parent, sizeof(parent), "%s", path) < (int)sizeof(parent));
    slash = strrchr(parent, '/');
    assert(slash != NULL);
    *slash = '\0';
    if (stat(parent, &status) != 0 || !S_ISDIR(status.st_mode)) return 1;
    return write_cached_checkpoint();
}

static int
run_invalid_template(const char *directory, const char *template_path)
{
    char base[PATH_MAX];
    char path[PATH_MAX];
    char legacy[PATH_MAX];
    PeakExecCheckpointRow row = {.name = "identity", .threads_seen = 1};

    assert(snprintf(base, sizeof(base), "%s/stats", directory) < (int)sizeof(base));
    set_common(base);
    assert(setenv("PEAK_STATSLOG_TEMPLATE", template_path, 1) == 0);
    peak_output_identity_initialize();
    if (checkpoint(path) != PEAK_OUTPUT_CHECKPOINT_UNAVAILABLE) return 1;
    errno = 0;
    if (peak_exec_checkpoint_write_rows(0, &row, 1, 0.0) || errno != EINVAL) return 1;
    assert(snprintf(legacy, sizeof(legacy), "%s-p%ld-exec0.csv", base,
                    (long)getpid()) < (int)sizeof(legacy));
    return access(legacy, F_OK) != 0 ? 0 : 1;
}

static int
run_invalid(const char *directory)
{
    return run_invalid_template(directory, "/dev/null/no-cwd-leak.csv");
}

static int
run_template_overlong(const char *directory, int expanded)
{
    char template_path[PATH_MAX + 32];
    size_t used = 0;

    if (!expanded) {
        memset(template_path, 'x', sizeof(template_path) - 1);
        template_path[sizeof(template_path) - 1] = '\0';
    } else {
        while (used + sizeof("{session}") < sizeof(template_path) - 1) {
            memcpy(template_path + used, "{session}", sizeof("{session}") - 1);
            used += sizeof("{session}") - 1;
        }
        template_path[used] = '\0';
    }
    return run_invalid_template(directory, template_path);
}

static int
run_concurrent(const char *directory)
{
    char base[PATH_MAX];
    pthread_t threads[THREADS];
    struct identity_thread results[THREADS] = {{0}};

    assert(snprintf(base, sizeof(base), "%s/stats", directory) < (int)sizeof(base));
    set_common(base);
    for (int i = 0; i < THREADS; i++)
        assert(pthread_create(&threads[i], NULL, render_identity, &results[i]) == 0);
    for (int i = 0; i < THREADS; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
        if (!results[i].ok || strcmp(results[0].path, results[i].path) != 0) return 1;
    }
    return 0;
}

static int
run_snapshot(const char *directory)
{
    char base[PATH_MAX];
    char path[PATH_MAX];

    assert(snprintf(base, sizeof(base), "%s/stats", directory) < (int)sizeof(base));
    set_common(base);
    peak_output_identity_initialize();
    assert(setenv("SLURM_JOB_ID", "mutated", 1) == 0);
    assert(unsetenv("PMI_RANK") == 0);
    return peak_output_identity_path(path, sizeof(path), base, NULL, ".csv", -1) &&
                   strstr(path, "-jjob-exec-") != NULL && strstr(path, "-r3-") != NULL
               ? 0 : 1;
}

static int
run_entropy_fallback(const char *directory)
{
    char base[PATH_MAX];
    int pipes[2][2];
    char paths[2][PATH_MAX] = {{0}};

    assert(snprintf(base, sizeof(base), "%s/stats", directory) < (int)sizeof(base));
    set_common(base);
    assert(setenv("PEAK_TEST_OUTPUT_IDENTITY_ENTROPY_FAIL", "1", 1) == 0);
    for (int index = 0; index < 2; index++) {
        pid_t child;

        assert(pipe(pipes[index]) == 0);
        child = fork();
        assert(child >= 0);
        if (child == 0) {
            char path[PATH_MAX];
            (void)close(pipes[index][0]);
            peak_output_identity_initialize();
            if (checkpoint(path) != PEAK_OUTPUT_CHECKPOINT_READY ||
                write(pipes[index][1], path, sizeof(path)) != (ssize_t)sizeof(path)) {
                _exit(1);
            }
            _exit(0);
        }
        (void)close(pipes[index][1]);
        if (read(pipes[index][0], paths[index], sizeof(paths[index])) !=
            (ssize_t)sizeof(paths[index]) || waitpid(child, NULL, 0) != child) {
            return 1;
        }
        (void)close(pipes[index][0]);
    }
    return strstr(paths[0], "-q0000000000000000") == NULL &&
                   strcmp(paths[0], paths[1]) != 0
               ? 0 : 1;
}

int
main(int argc, char **argv)
{
    char directory[] = "/tmp/peak-output-identity-XXXXXX";
    int rc;

    if (argc != 2) return 2;
    if (strcmp(argv[1], "preinit") == 0) rc = run_preinit();
    else {
        if (mkdtemp(directory) == NULL) return 2;
        if (strcmp(argv[1], "default") == 0) rc = run_default(directory);
        else if (strcmp(argv[1], "template-csv") == 0) rc = run_template(directory, 1);
        else if (strcmp(argv[1], "template-bare") == 0) rc = run_template(directory, 0);
        else if (strcmp(argv[1], "invalid") == 0) rc = run_invalid(directory);
        else if (strcmp(argv[1], "template-overlong") == 0)
            rc = run_template_overlong(directory, 0);
        else if (strcmp(argv[1], "template-expanded-overlong") == 0)
            rc = run_template_overlong(directory, 1);
        else if (strcmp(argv[1], "concurrent") == 0) rc = run_concurrent(directory);
        else if (strcmp(argv[1], "snapshot") == 0) rc = run_snapshot(directory);
        else if (strcmp(argv[1], "entropy-fallback") == 0) rc = run_entropy_fallback(directory);
        else return 2;
    }
    if (rc != 0) return rc;
    puts("output_identity_test_ok");
    return 0;
}
