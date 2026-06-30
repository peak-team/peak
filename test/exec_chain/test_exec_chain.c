#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(PEAK_TEST_HAVE_SYS_CLONE3)
#include <linux/sched.h>
#endif

#if defined(__GNUC__)
#define PEAK_NOINLINE __attribute__((noinline, noclone))
#define PEAK_EXPORT __attribute__((visibility("default"), used, externally_visible))
#else
#define PEAK_NOINLINE
#define PEAK_EXPORT
#endif

extern char** environ;

#define EXTRA_PRELOAD_TOKEN_A "/tmp/peak_exec_chain_extra_preload_a.so"
#define EXTRA_PRELOAD_TOKEN_B "/tmp/peak_exec_chain_extra_preload_b.so"

static volatile unsigned long peak_exec_chain_sink;

typedef struct {
    const char* self;
    char** envp;
    const char* child_arg;
    const char* child_env_name;
} CloneExecArgs;

typedef struct {
    char* name;
    char* value;
} EnvSnapshotEntry;

typedef struct {
    EnvSnapshotEntry entries[128];
    size_t count;
} EnvSnapshot;

static int wait_for_spawned_child(pid_t pid);
static int wait_for_spawned_child_exit(pid_t pid,
                                       int expected_exit,
                                       const char* label);
static char* make_duplicate_preload_value(void);
static char* const* invalid_argv_for_test(void);
static char* const* invalid_envp_for_test(void);

#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0
#endif

void PEAK_EXPORT PEAK_NOINLINE
peak_exec_chain_hot_target(int value)
{
    peak_exec_chain_sink += (unsigned long)value + 1UL;
    asm volatile("" ::: "memory");
}

static void
call_hot_target(int loops)
{
    for (int i = 0; i < loops; i++) {
        peak_exec_chain_hot_target(i);
    }
}

static const char*
env_or_default(const char* name, const char* fallback)
{
    const char* value = getenv(name);
    return (value != NULL && value[0] != '\0') ? value : fallback;
}

static char*
make_env_entry(const char* name, const char* value)
{
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    char* entry = malloc(name_len + 1 + value_len + 1);
    if (entry == NULL) {
        perror("malloc");
        exit(100);
    }
    memcpy(entry, name, name_len);
    entry[name_len] = '=';
    memcpy(entry + name_len + 1, value, value_len + 1);
    return entry;
}

static void
append_current_env_entry_if_present(char** envp, size_t* index, const char* name)
{
    const char* value = getenv(name);
    if (value != NULL && value[0] != '\0') {
        envp[(*index)++] = make_env_entry(name, value);
    }
}

static char**
make_minimal_child_env(void)
{
    char** envp = calloc(4, sizeof(char*));
    size_t index = 0;
    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    envp[index++] = make_env_entry("PATH",
                                   env_or_default("PATH", "/bin:/usr/bin"));
    append_current_env_entry_if_present(envp, &index, "LD_LIBRARY_PATH");
    envp[index++] = make_env_entry("EXEC_CHAIN_TEST_MARKER", "custom-env");
    envp[index] = NULL;
    return envp;
}

static char**
make_bad_path_child_env(void)
{
    char** envp = calloc(4, sizeof(char*));
    size_t index = 0;
    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    envp[index++] = make_env_entry("PATH", "/definitely/not/a/peak/path");
    append_current_env_entry_if_present(envp, &index, "LD_LIBRARY_PATH");
    envp[index++] = make_env_entry("EXEC_CHAIN_TEST_MARKER", "bad-path-env");
    envp[index] = NULL;
    return envp;
}

static char**
make_child_path_env(void)
{
    char** envp = calloc(4, sizeof(char*));
    size_t index = 0;
    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    envp[index++] = make_env_entry(
        "PATH",
        env_or_default("EXEC_CHAIN_TEST_CHILD_PATH", "/bin:/usr/bin"));
    append_current_env_entry_if_present(envp, &index, "LD_LIBRARY_PATH");
    envp[index++] = make_env_entry("EXEC_CHAIN_TEST_MARKER", "child-path-env");
    envp[index] = NULL;
    return envp;
}

static char**
make_child_path_duplicate_preload_env(void)
{
    char* duplicate_preload = make_duplicate_preload_value();
    char** envp = calloc(6, sizeof(char*));
    size_t index = 0;
    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    envp[index++] = make_env_entry(
        "PATH",
        env_or_default("EXEC_CHAIN_TEST_CHILD_PATH", "/bin:/usr/bin"));
    append_current_env_entry_if_present(envp, &index, "LD_LIBRARY_PATH");
    envp[index++] = make_env_entry("LD_PRELOAD", duplicate_preload);
    envp[index++] = make_env_entry("EXEC_CHAIN_TEST_MARKER",
                                   "child-path-duplicate-preload-env");
    envp[index] = NULL;
    free(duplicate_preload);
    return envp;
}

static char**
make_large_child_env(size_t pad_count)
{
    char** envp = calloc(pad_count + 4, sizeof(char*));
    char name[64];
    size_t index = 0;

    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    envp[index++] = make_env_entry("PATH",
                                   env_or_default("PATH", "/bin:/usr/bin"));
    append_current_env_entry_if_present(envp, &index, "LD_LIBRARY_PATH");
    envp[index++] = make_env_entry("EXEC_CHAIN_TEST_MARKER",
                                   "large-child-env");
    for (size_t i = 0; i < pad_count; i++) {
        snprintf(name, sizeof(name), "CHILD_PAD_%04zu", i);
        envp[index++] = make_env_entry(name, "x");
    }
    envp[index] = NULL;
    return envp;
}

static char**
make_child_peak_only_env(void)
{
    char** envp = calloc(7, sizeof(char*));
    size_t index = 0;
    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    envp[index++] = make_env_entry("PATH",
                                   env_or_default("PATH", "/bin:/usr/bin"));
    append_current_env_entry_if_present(envp, &index, "LD_LIBRARY_PATH");
    envp[index++] = make_env_entry("EXEC_CHAIN_TEST_MARKER", "child-peak-env");
    envp[index++] = make_env_entry("PEAK_TARGET",
                                   "peak_exec_chain_hot_target");
    envp[index++] = make_env_entry(
        "PEAK_STATSLOG_PATH",
        env_or_default("EXEC_CHAIN_CHILD_STATS_PREFIX", "./peak_statslog"));
    envp[index++] = make_env_entry("PEAK_HEARTBEAT_INTERVAL", "0");
    envp[index] = NULL;
    return envp;
}

static char**
make_explicit_wrong_peak_target_env(void)
{
    char** envp = calloc(6, sizeof(char*));
    size_t index = 0;
    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    envp[index++] = make_env_entry("PATH",
                                   env_or_default("PATH", "/bin:/usr/bin"));
    append_current_env_entry_if_present(envp, &index, "LD_LIBRARY_PATH");
    envp[index++] = make_env_entry("EXEC_CHAIN_TEST_MARKER", "custom-env");
    envp[index++] = make_env_entry("PEAK_TARGET",
                                   "definitely_not_the_test_target");
    envp[index++] = make_env_entry(
        "PEAK_STATSLOG_PATH",
        env_or_default("PEAK_STATSLOG_PATH", "./peak_statslog"));
    envp[index] = NULL;
    return envp;
}

static char*
make_duplicate_preload_value(void)
{
    const char* preload = env_or_default("LD_PRELOAD", "");
    size_t len = strlen(preload);
    char* value = malloc(len + 1 + len + 1);
    if (value == NULL) {
        perror("malloc");
        exit(100);
    }
    memcpy(value, preload, len);
    value[len] = ':';
    memcpy(value + len + 1, preload, len + 1);
    return value;
}

static char*
make_whitespace_duplicate_preload_value(void)
{
    const char* preload = env_or_default("LD_PRELOAD", "");
    size_t len = strlen(preload);
    char* value = malloc(len + 1 + len + 1);
    if (value == NULL) {
        perror("malloc");
        exit(100);
    }
    memcpy(value, preload, len);
    value[len] = ' ';
    memcpy(value + len + 1, preload, len + 1);
    return value;
}

static char**
make_duplicate_preload_env_with(char* duplicate_preload)
{
    char** envp = calloc(9, sizeof(char*));
    size_t index = 0;
    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    envp[index++] = make_env_entry("PATH",
                                   env_or_default("PATH", "/bin:/usr/bin"));
    append_current_env_entry_if_present(envp, &index, "LD_LIBRARY_PATH");
    envp[index++] = make_env_entry("LD_PRELOAD", duplicate_preload);
    envp[index++] = make_env_entry(
        "PEAK_TARGET",
        env_or_default("PEAK_TARGET", "peak_exec_chain_hot_target"));
    envp[index++] = make_env_entry(
        "PEAK_STATSLOG_PATH",
        env_or_default("PEAK_STATSLOG_PATH", "./peak_statslog"));
    envp[index++] = make_env_entry(
        "PEAK_EXEC_TRACE_PATH",
        env_or_default("PEAK_EXEC_TRACE_PATH", ""));
    envp[index++] = make_env_entry(
        "PEAK_HEARTBEAT_INTERVAL",
        env_or_default("PEAK_HEARTBEAT_INTERVAL", "0"));
    envp[index++] = make_env_entry("PEAK_TEXT_OUTPUT",
                                   env_or_default("PEAK_TEXT_OUTPUT", "0"));
    envp[index] = NULL;
    free(duplicate_preload);
    return envp;
}

static char**
make_duplicate_preload_entry_env(void)
{
    const char* preload = env_or_default("LD_PRELOAD", "");
    char** envp = calloc(11, sizeof(char*));
    size_t index = 0;
    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    envp[index++] = make_env_entry("PATH",
                                   env_or_default("PATH", "/bin:/usr/bin"));
    append_current_env_entry_if_present(envp, &index, "LD_LIBRARY_PATH");
    envp[index++] = make_env_entry("LD_PRELOAD", EXTRA_PRELOAD_TOKEN_A);
    envp[index++] = make_env_entry("LD_PRELOAD", preload);
    envp[index++] = make_env_entry("LD_PRELOAD", EXTRA_PRELOAD_TOKEN_B);
    envp[index++] = make_env_entry(
        "PEAK_TARGET",
        env_or_default("PEAK_TARGET", "peak_exec_chain_hot_target"));
    envp[index++] = make_env_entry(
        "PEAK_STATSLOG_PATH",
        env_or_default("PEAK_STATSLOG_PATH", "./peak_statslog"));
    envp[index++] = make_env_entry(
        "PEAK_EXEC_TRACE_PATH",
        env_or_default("PEAK_EXEC_TRACE_PATH", ""));
    envp[index++] = make_env_entry(
        "PEAK_HEARTBEAT_INTERVAL",
        env_or_default("PEAK_HEARTBEAT_INTERVAL", "0"));
    envp[index++] = make_env_entry("PEAK_TEXT_OUTPUT",
                                   env_or_default("PEAK_TEXT_OUTPUT", "0"));
    envp[index] = NULL;
    return envp;
}

static char**
make_duplicate_preload_env(void)
{
    return make_duplicate_preload_env_with(make_duplicate_preload_value());
}

static char**
make_whitespace_duplicate_preload_env(void)
{
    return make_duplicate_preload_env_with(
        make_whitespace_duplicate_preload_value());
}

static int
count_libpeak_preload_entries(void)
{
    const char* preload = getenv("LD_PRELOAD");
    int count = 0;

    if (preload == NULL) {
        return 0;
    }

    const char* cursor = preload;
    while (*cursor != '\0') {
        while (*cursor == ':' || *cursor == ' ' || *cursor == '\t' ||
               *cursor == '\n') {
            cursor++;
        }
        const char* start = cursor;
        while (*cursor != '\0' && *cursor != ':' && *cursor != ' ' &&
               *cursor != '\t' && *cursor != '\n') {
            cursor++;
        }
        if (cursor > start) {
            size_t token_len = (size_t)(cursor - start);
            char* token = strndup(start, token_len);
            if (token != NULL) {
                if (strstr(token, "libpeak") != NULL) {
                    count++;
                }
                free(token);
            }
        }
    }
    return count;
}

static int
count_extra_preload_entries(void)
{
    const char* preload = getenv("LD_PRELOAD");
    int count = 0;

    if (preload == NULL) {
        return 0;
    }

    while (*preload != '\0') {
        while (*preload == ':' || *preload == ' ' || *preload == '\t' ||
               *preload == '\n') {
            preload++;
        }
        const char* start = preload;
        while (*preload != '\0' && *preload != ':' && *preload != ' ' &&
               *preload != '\t' && *preload != '\n') {
            preload++;
        }
        size_t len = (size_t)(preload - start);
        if ((len == strlen(EXTRA_PRELOAD_TOKEN_A) &&
             strncmp(start, EXTRA_PRELOAD_TOKEN_A, len) == 0) ||
            (len == strlen(EXTRA_PRELOAD_TOKEN_B) &&
             strncmp(start, EXTRA_PRELOAD_TOKEN_B, len) == 0)) {
            count++;
        }
    }
    return count;
}

static int
count_ld_preload_env_entries(void)
{
    int count = 0;
    if (environ == NULL) {
        return 0;
    }
    for (char** scan = environ; *scan != NULL; scan++) {
        if (strncmp(*scan, "LD_PRELOAD=", strlen("LD_PRELOAD=")) == 0) {
            count++;
        }
    }
    return count;
}

static void
unset_peak_and_preload_env(void)
{
    char* names[128];
    size_t count = 0;

    if (environ != NULL) {
        for (char** scan = environ; *scan != NULL && count < 128; scan++) {
            const char* equals = strchr(*scan, '=');
            size_t name_len;
            char* copy;

            if (equals == NULL) {
                continue;
            }
            name_len = (size_t)(equals - *scan);
            if (strncmp(*scan, "PEAK_", 5) != 0 &&
                !(name_len == strlen("LD_PRELOAD") &&
                  strncmp(*scan, "LD_PRELOAD", name_len) == 0)) {
                continue;
            }
            copy = strndup(*scan, name_len);
            if (copy != NULL) {
                names[count++] = copy;
            }
        }
    }

    for (size_t i = 0; i < count; i++) {
        unsetenv(names[i]);
        free(names[i]);
    }
}

static void
snapshot_and_unset_peak_env(EnvSnapshot* snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (environ != NULL) {
        for (char** scan = environ;
             *scan != NULL && snapshot->count < 128;
             scan++) {
            const char* equals = strchr(*scan, '=');
            size_t name_len;

            if (equals == NULL) {
                continue;
            }
            name_len = (size_t)(equals - *scan);
            if (strncmp(*scan, "PEAK_", 5) != 0 &&
                !(name_len == strlen("LD_PRELOAD") &&
                  strncmp(*scan, "LD_PRELOAD", name_len) == 0)) {
                continue;
            }

            snapshot->entries[snapshot->count].name =
                strndup(*scan, name_len);
            snapshot->entries[snapshot->count].value = strdup(equals + 1);
            if (snapshot->entries[snapshot->count].name != NULL &&
                snapshot->entries[snapshot->count].value != NULL) {
                snapshot->count++;
            } else {
                free(snapshot->entries[snapshot->count].name);
                free(snapshot->entries[snapshot->count].value);
                snapshot->entries[snapshot->count].name = NULL;
                snapshot->entries[snapshot->count].value = NULL;
            }
        }
    }

    for (size_t i = 0; i < snapshot->count; i++) {
        unsetenv(snapshot->entries[i].name);
    }
}

static void
restore_peak_env_snapshot(EnvSnapshot* snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    for (size_t i = 0; i < snapshot->count; i++) {
        setenv(snapshot->entries[i].name, snapshot->entries[i].value, 1);
        free(snapshot->entries[i].name);
        free(snapshot->entries[i].value);
        snapshot->entries[i].name = NULL;
        snapshot->entries[i].value = NULL;
    }
    snapshot->count = 0;
}

static int
run_child_basic(void)
{
    call_hot_target(7);
    printf("exec_child_ok sink=%lu\n", peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_child_print_ld(void)
{
    call_hot_target(7);
    printf("ld_preload_libpeak_count=%d ld_preload_env_entries=%d "
           "ld_preload_extra_count=%d peak_target_present=%d "
           "peak_statslog_present=%d\n",
           count_libpeak_preload_entries(),
           count_ld_preload_env_entries(),
           count_extra_preload_entries(),
           getenv("PEAK_TARGET") != NULL || getenv("PEAK_TARGET_FILE") != NULL,
           getenv("PEAK_STATSLOG_PATH") != NULL);
    fflush(stdout);
    return 0;
}

static int
run_child_stderr_sentinel(void)
{
    call_hot_target(7);
    printf("child_stdout_after_stderr_close sink=%lu\n", peak_exec_chain_sink);
    fflush(stdout);
    fprintf(stderr, "child_stderr_sentinel_after_spawn_close\n");
    fflush(stderr);
    return 0;
}

static int
run_child_check_env(const char* name)
{
    const char* value = getenv(name);
    call_hot_target(7);
    printf("child_env_%s=%s\n", name, value != NULL ? value : "<missing>");
    fflush(stdout);
    return value != NULL ? 0 : 1;
}

static int
run_execv_success(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    execv(self, argv);
    perror("execv");
    return 111;
}

static int
run_execv_write_target_success(const char* self)
{
    static const char marker[] = "parent_write_before_exec\n";
    if (write(STDOUT_FILENO, marker, sizeof(marker) - 1) < 0) {
        perror("write-before-exec");
        return 230;
    }
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    execv(self, argv);
    perror("execv-write-target");
    return 231;
}

static int
run_execve_custom_env(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_minimal_child_env();
    execve(self, argv, envp);
    perror("execve");
    return 112;
}

static int
run_execve_child_peak_env_only(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_child_peak_only_env();
    unset_peak_and_preload_env();
    execve(self, argv, envp);
    perror("execve-child-peak-env-only");
    return 141;
}

static int
run_syscall_execve_custom_env(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_minimal_child_env();
#ifdef SYS_execve
    syscall(SYS_execve, self, argv, envp);
#else
    errno = ENOSYS;
#endif
    perror("syscall-execve");
    return 137;
}

static int
run_syscall_execve_failure(void)
{
    char* const argv[] = {(char*)"/definitely/not/found", NULL};

    call_hot_target(2);
    errno = 0;
#ifdef SYS_execve
    long result = syscall(SYS_execve,
                          "/definitely/not/found",
                          argv,
                          environ);
#else
    long result = -1;
    errno = ENOSYS;
#endif
    int saved_errno = errno;
    if (result != -1 || saved_errno != ENOENT) {
        fprintf(stderr,
                "syscall_execve_failure_bad_result result=%ld errno=%d\n",
                result,
                saved_errno);
        return 138;
    }
    call_hot_target(4);
    printf("syscall_execve_failure_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_execve_explicit_peak_env(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_explicit_wrong_peak_target_env();
    execve(self, argv, envp);
    perror("execve-explicit-peak-env");
    return 131;
}

static int
run_syscall_execve_bad_env_failure(void)
{
    char* const argv[] = {(char*)"/bin/true", NULL};

    call_hot_target(3);
    errno = 0;
#ifdef SYS_execve
    long result = syscall(SYS_execve,
                          "/bin/true",
                          argv,
                          (char* const*)1);
#else
    long result = -1;
    errno = ENOSYS;
#endif
    int saved_errno = errno;
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "syscall_execve_bad_env_bad_result result=%ld errno=%d\n",
                result,
                saved_errno);
        return 217;
    }
    call_hot_target(3);
    printf("syscall_execve_bad_env_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_syscall_execve_bad_argv_failure(void)
{
    call_hot_target(3);
    errno = 0;
#ifdef SYS_execve
    long result = syscall(SYS_execve,
                          "/bin/true",
                          invalid_argv_for_test(),
                          environ);
#else
    long result = -1;
    errno = ENOSYS;
#endif
    int saved_errno = errno;
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "syscall_execve_bad_argv_bad_result result=%ld errno=%d\n",
                result,
                saved_errno);
        return 230;
    }
    call_hot_target(3);
    printf("syscall_execve_bad_argv_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_execve_bad_env_failure(void)
{
    char* const argv[] = {(char*)"/bin/true", NULL};

    call_hot_target(3);
    errno = 0;
    int result = execve("/bin/true", argv, invalid_envp_for_test());
    int saved_errno = errno;
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "execve_bad_env_bad_result result=%d errno=%d\n",
                result,
                saved_errno);
        return 223;
    }
    call_hot_target(3);
    printf("execve_bad_env_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_execve_bad_argv_failure(void)
{
    call_hot_target(3);
    errno = 0;
    int result = execve("/bin/true", invalid_argv_for_test(), environ);
    int saved_errno = errno;
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "execve_bad_argv_bad_result result=%d errno=%d\n",
                result,
                saved_errno);
        return 231;
    }
    call_hot_target(3);
    printf("execve_bad_argv_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_execve_null_env(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    execve(self, argv, NULL);
    perror("execve-null-env");
    return 121;
}

static int
run_execv_zero_call_checkpoint(const char* self)
{
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    execv(self, argv);
    perror("execv-zero-call");
    return 122;
}

static int
run_fexecve_custom_env(const char* self)
{
    int fd;

    call_hot_target(5);
    fd = open(self, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 123;
    }

    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_minimal_child_env();
    fexecve(fd, argv, envp);
    perror("fexecve");
    close(fd);
    return 124;
}

static int
run_fexecve_failure(void)
{
    call_hot_target(2);
    char* const argv[] = {(char*)"bad-fd", NULL};
    errno = 0;
    int result = fexecve(INT_MAX, argv, environ);
    int saved_errno = errno;
    if (result != -1 || saved_errno != EBADF) {
        fprintf(stderr,
                "fexecve_failure_bad_result result=%d errno=%d\n",
                result,
                saved_errno);
        return 132;
    }
    call_hot_target(4);
    printf("fexecve_failure_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_fexecve_bad_env_failure(const char* self)
{
    int fd;

    call_hot_target(2);
    fd = open(self, O_RDONLY);
    if (fd < 0) {
        perror("open-fexecve-bad-env");
        return 219;
    }

    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    errno = 0;
    int result = fexecve(fd, argv, invalid_envp_for_test());
    int saved_errno = errno;
    close(fd);
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "fexecve_bad_env_result=%d errno=%d\n",
                result,
                saved_errno);
        return 220;
    }
    call_hot_target(4);
    printf("fexecve_bad_env_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_fexecve_bad_argv_failure(const char* self)
{
    int fd;

    call_hot_target(2);
    fd = open(self, O_RDONLY);
    if (fd < 0) {
        perror("open-fexecve-bad-argv");
        return 232;
    }

    errno = 0;
    int result = fexecve(fd, invalid_argv_for_test(), environ);
    int saved_errno = errno;
    close(fd);
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "fexecve_bad_argv_result=%d errno=%d\n",
                result,
                saved_errno);
        return 233;
    }
    call_hot_target(4);
    printf("fexecve_bad_argv_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_execveat_custom_env(const char* self)
{
    call_hot_target(5);
#if defined(PEAK_TEST_HAVE_EXECVEAT)
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_minimal_child_env();
    execveat(AT_FDCWD, self, argv, envp, 0);
#else
    errno = ENOSYS;
#endif
    perror("execveat");
    return 125;
}

static int
run_execveat_empty_path_custom_env(const char* self)
{
#if defined(PEAK_TEST_HAVE_EXECVEAT) && defined(PEAK_TEST_HAVE_AT_EMPTY_PATH)
    int fd = open(self, O_RDONLY);
    if (fd < 0) {
        perror("open-execveat-empty-path");
        return 126;
    }

    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_minimal_child_env();
    execveat(fd, "", argv, envp, AT_EMPTY_PATH);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
#else
    errno = ENOSYS;
#endif
    perror("execveat-empty-path");
    return 126;
}

static int
run_execveat_empty_path_bad_env_failure(const char* self)
{
#if defined(PEAK_TEST_HAVE_EXECVEAT) && defined(PEAK_TEST_HAVE_AT_EMPTY_PATH)
    int fd = open(self, O_RDONLY);
    if (fd < 0) {
        perror("open-execveat-empty-path-bad-env");
        return 224;
    }

    call_hot_target(3);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    errno = 0;
    int result = execveat(fd, "", argv, invalid_envp_for_test(), AT_EMPTY_PATH);
    int saved_errno = errno;
    close(fd);
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "execveat_empty_path_bad_env_result=%d errno=%d\n",
                result,
                saved_errno);
        return 225;
    }
    call_hot_target(3);
    printf("execveat_empty_path_bad_env_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
#else
    (void)self;
    printf("execveat_unavailable\n");
    fflush(stdout);
#endif
    return 0;
}

static int
run_execveat_empty_path_bad_argv_failure(const char* self)
{
#if defined(PEAK_TEST_HAVE_EXECVEAT) && defined(PEAK_TEST_HAVE_AT_EMPTY_PATH)
    int fd = open(self, O_RDONLY);
    if (fd < 0) {
        perror("open-execveat-empty-path-bad-argv");
        return 234;
    }

    call_hot_target(3);
    errno = 0;
    int result =
        execveat(fd, "", invalid_argv_for_test(), environ, AT_EMPTY_PATH);
    int saved_errno = errno;
    close(fd);
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "execveat_empty_path_bad_argv_result=%d errno=%d\n",
                result,
                saved_errno);
        return 235;
    }
    call_hot_target(3);
    printf("execveat_empty_path_bad_argv_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
#else
    (void)self;
    printf("execveat_unavailable\n");
    fflush(stdout);
#endif
    return 0;
}

static int
run_syscall_execveat_custom_env(const char* self)
{
    call_hot_target(5);
#if defined(PEAK_TEST_HAVE_SYS_EXECVEAT)
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_minimal_child_env();
    syscall(SYS_execveat, AT_FDCWD, self, argv, envp, 0);
#else
    errno = ENOSYS;
#endif
    perror("syscall-execveat");
    return 139;
}

static int
run_execveat_failure(void)
{
    call_hot_target(2);
    errno = 0;
#if defined(PEAK_TEST_HAVE_EXECVEAT)
    char* const argv[] = {(char*)"/definitely/not/found", NULL};
    int result = execveat(AT_FDCWD,
                          "/definitely/not/found",
                          argv,
                          environ,
                          0);
#else
    int result = -1;
    errno = ENOSYS;
#endif
    int saved_errno = errno;
    if (result != -1 || saved_errno != ENOENT) {
        fprintf(stderr,
                "execveat_failure_bad_result result=%d errno=%d\n",
                result,
                saved_errno);
        return 133;
    }
    call_hot_target(4);
    printf("execveat_failure_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_syscall_execveat_failure(void)
{
    call_hot_target(2);
    errno = 0;
#if defined(PEAK_TEST_HAVE_SYS_EXECVEAT)
    char* const argv[] = {(char*)"/definitely/not/found", NULL};
    long result = syscall(SYS_execveat,
                          AT_FDCWD,
                          "/definitely/not/found",
                          argv,
                          environ,
                          0);
#else
    long result = -1;
    errno = ENOSYS;
#endif
    int saved_errno = errno;
    if (result != -1 || saved_errno != ENOENT) {
        fprintf(stderr,
                "syscall_execveat_failure_bad_result result=%ld errno=%d\n",
                result,
                saved_errno);
        return 140;
    }
    call_hot_target(4);
    printf("syscall_execveat_failure_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_execveat_bad_env_failure(void)
{
    char* const argv[] = {(char*)"/bin/true", NULL};

    call_hot_target(3);
    errno = 0;
#if defined(PEAK_TEST_HAVE_EXECVEAT)
    int result = execveat(AT_FDCWD,
                          "/bin/true",
                          argv,
                          invalid_envp_for_test(),
                          0);
#else
    int result = -1;
    errno = ENOSYS;
#endif
    int saved_errno = errno;
#if defined(PEAK_TEST_HAVE_EXECVEAT)
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "execveat_bad_env_result=%d errno=%d\n",
                result,
                saved_errno);
        return 221;
    }
    call_hot_target(3);
    printf("execveat_bad_env_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
#else
    printf("execveat_unavailable\n");
    fflush(stdout);
#endif
    return 0;
}

static int
run_execveat_bad_argv_failure(void)
{
    call_hot_target(3);
    errno = 0;
#if defined(PEAK_TEST_HAVE_EXECVEAT)
    int result = execveat(AT_FDCWD,
                          "/bin/true",
                          invalid_argv_for_test(),
                          environ,
                          0);
#else
    int result = -1;
    errno = ENOSYS;
#endif
    int saved_errno = errno;
#if defined(PEAK_TEST_HAVE_EXECVEAT)
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "execveat_bad_argv_result=%d errno=%d\n",
                result,
                saved_errno);
        return 236;
    }
    call_hot_target(3);
    printf("execveat_bad_argv_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
#else
    printf("execveat_unavailable\n");
    fflush(stdout);
#endif
    return 0;
}

static int
run_syscall_execveat_bad_env_failure(void)
{
    char* const argv[] = {(char*)"/bin/true", NULL};

    call_hot_target(3);
    errno = 0;
#if defined(PEAK_TEST_HAVE_SYS_EXECVEAT)
    long result = syscall(SYS_execveat,
                          AT_FDCWD,
                          "/bin/true",
                          argv,
                          (char* const*)1,
                          0);
#else
    long result = -1;
    errno = ENOSYS;
#endif
    int saved_errno = errno;
#if defined(PEAK_TEST_HAVE_SYS_EXECVEAT)
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "syscall_execveat_bad_env_bad_result result=%ld errno=%d\n",
                result,
                saved_errno);
        return 218;
    }
    call_hot_target(3);
    printf("syscall_execveat_bad_env_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
#else
    printf("execveat_unavailable\n");
    fflush(stdout);
#endif
    return 0;
}

static int
run_syscall_execveat_bad_argv_failure(void)
{
    call_hot_target(3);
    errno = 0;
#if defined(PEAK_TEST_HAVE_SYS_EXECVEAT)
    long result = syscall(SYS_execveat,
                          AT_FDCWD,
                          "/bin/true",
                          invalid_argv_for_test(),
                          environ,
                          0);
#else
    long result = -1;
    errno = ENOSYS;
#endif
    int saved_errno = errno;
#if defined(PEAK_TEST_HAVE_SYS_EXECVEAT)
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "syscall_execveat_bad_argv_bad_result result=%ld errno=%d\n",
                result,
                saved_errno);
        return 237;
    }
    call_hot_target(3);
    printf("syscall_execveat_bad_argv_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
#else
    printf("execveat_unavailable\n");
    fflush(stdout);
#endif
    return 0;
}

static int
run_helper_named_exec(void)
{
    call_hot_target(5);
    char* const argv[] = {
        (char*)"peak_detach_helper",
        (char*)"child-basic",
        NULL
    };
    char** envp = make_minimal_child_env();
    execve("./peak_detach_helper", argv, envp);
    perror("helper-named-exec");
    return 135;
}

static int
run_configured_helper_named_exec(void)
{
    call_hot_target(5);
    char* const argv[] = {
        (char*)"custom_detach_helper",
        (char*)"child-basic",
        NULL
    };
    char** envp = make_minimal_child_env();
    execve("./custom_detach_helper", argv, envp);
    perror("configured-helper-named-exec");
    return 136;
}

static int
run_execl_success(const char* self)
{
    call_hot_target(5);
    execl(self, self, "child-basic", (char*)NULL);
    perror("execl");
    return 128;
}

static int
run_execlp_path_search(void)
{
    call_hot_target(5);
    execlp("test_exec_chain",
           "test_exec_chain",
           "child-basic",
           (char*)NULL);
    perror("execlp");
    return 129;
}

static int
run_execle_custom_env(const char* self)
{
    call_hot_target(5);
    char** envp = make_minimal_child_env();
    execle(self, self, "child-basic", (char*)NULL, envp);
    perror("execle");
    return 130;
}

static int
run_fork_execl_success(const char* self)
{
    call_hot_target(7);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork-execl");
        return 212;
    }
    if (pid == 0) {
        call_hot_target(5);
        execl(self, self, "child-basic", (char*)NULL);
        perror("fork-execl");
        _exit(213);
    }
    return wait_for_spawned_child(pid);
}

static int
run_fork_execlp_path_search(void)
{
    call_hot_target(7);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork-execlp");
        return 214;
    }
    if (pid == 0) {
        call_hot_target(5);
        execlp("test_exec_chain",
               "test_exec_chain",
               "child-basic",
               (char*)NULL);
        perror("fork-execlp");
        _exit(215);
    }
    return wait_for_spawned_child(pid);
}

static int
run_fork_execle_custom_env(const char* self)
{
    char** envp = make_minimal_child_env();
    call_hot_target(7);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork-execle");
        return 216;
    }
    if (pid == 0) {
        call_hot_target(5);
        execle(self, self, "child-basic", (char*)NULL, envp);
        perror("fork-execle");
        _exit(217);
    }
    return wait_for_spawned_child(pid);
}

static int
run_exec_failure(void)
{
    call_hot_target(3);
    char* const argv[] = {(char*)"/definitely/not/found", NULL};
    errno = 0;
    int result = execv("/definitely/not/found", argv);
    int saved_errno = errno;
    if (result != -1 || saved_errno != ENOENT) {
        fprintf(stderr,
                "exec_failure_bad_result result=%d errno=%d\n",
                result,
                saved_errno);
        return 113;
    }
    call_hot_target(5);
    printf("exec_failure_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_execvp_enoent(void)
{
    call_hot_target(2);
    char* const argv[] = {(char*)"definitely-not-found-peak-exec-chain", NULL};
    errno = 0;
    int result = execvp("definitely-not-found-peak-exec-chain", argv);
    int saved_errno = errno;
    if (result != -1 || saved_errno != ENOENT) {
        fprintf(stderr,
                "execvp_enoent_bad_result result=%d errno=%d\n",
                result,
                saved_errno);
        return 114;
    }
    call_hot_target(4);
    printf("execvp_enoent_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_execvp_eacces(void)
{
    call_hot_target(2);
    char* const argv[] = {(char*)"peak-noexec-child", NULL};
    errno = 0;
    int result = execvp("peak-noexec-child", argv);
    int saved_errno = errno;
    if (result != -1 || saved_errno != EACCES) {
        fprintf(stderr,
                "execvp_eacces_bad_result result=%d errno=%d\n",
                result,
                saved_errno);
        return 161;
    }
    call_hot_target(4);
    printf("execvp_eacces_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_execvp_enoexec(void)
{
    call_hot_target(2);
    char* const argv[] = {
        (char*)"peak-enoexec-script",
        (char*)"alpha",
        (char*)"beta",
        NULL
    };
    execvp("peak-enoexec-script", argv);
    perror("execvp-enoexec");
    return 126;
}

static int
run_fork_execvp_enoexec_fallback(void)
{
    call_hot_target(5);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork-execvp-enoexec");
        return 218;
    }
    if (pid == 0) {
        call_hot_target(2);
        char* const argv[] = {
            (char*)"peak-enoexec-script",
            (char*)"alpha",
            (char*)"beta",
            NULL
        };
        execvp("peak-enoexec-script", argv);
        perror("fork-execvp-enoexec");
        _exit(219);
    }
    int result = wait_for_spawned_child_exit(pid, 37, "fork_execvp_enoexec");
    if (result != 0) {
        return result;
    }
    printf("fork_execvp_enoexec_exit=37\n");
    fflush(stdout);
    return 0;
}

static int
run_execvp_path_search(void)
{
    call_hot_target(5);
    char* const argv[] = {(char*)"test_exec_chain", (char*)"child-basic", NULL};
    execvp("test_exec_chain", argv);
    perror("execvp");
    return 115;
}

static int
run_duplicate_preload(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_duplicate_preload_env();
    execve(self, argv, envp);
    perror("execve");
    return 116;
}

static int
run_whitespace_duplicate_preload(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_whitespace_duplicate_preload_env();
    execve(self, argv, envp);
    perror("execve");
    return 127;
}

static int
run_duplicate_preload_entry(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_duplicate_preload_entry_env();
    execve(self, argv, envp);
    perror("execve-duplicate-preload-entry");
    return 142;
}

static int
wait_for_spawned_child(pid_t pid)
{
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 117;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "spawned_child_bad_status=%d\n", status);
        return 118;
    }
    return 0;
}

static int
wait_for_spawned_child_exit(pid_t pid, int expected_exit, const char* label)
{
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 117;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expected_exit) {
        fprintf(stderr,
                "%s_bad_status=%d expected_exit=%d\n",
                label,
                status,
                expected_exit);
        return 118;
    }
    return 0;
}

static char* const*
invalid_envp_for_test(void)
{
    volatile uintptr_t value = 1;
    return (char* const*)(uintptr_t)value;
}

static char* const*
invalid_argv_for_test(void)
{
    volatile uintptr_t value = 1;
    return (char* const*)(uintptr_t)value;
}

static pid_t
raw_clone_fork_like(void)
{
#ifdef SYS_clone
    long raw_pid = syscall(SYS_clone, SIGCHLD, NULL, NULL, NULL, NULL);
    if (raw_pid < 0) {
        return -1;
    }
    return (pid_t)raw_pid;
#else
    errno = ENOSYS;
    return -1;
#endif
}

static int
run_fork_exec_custom_env(const char* self)
{
    call_hot_target(5);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 143;
    }
    if (pid == 0) {
        char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
        char** envp = make_minimal_child_env();
        execve(self, argv, envp);
        perror("fork-execve");
        _exit(144);
    }
    return wait_for_spawned_child(pid);
}

static int
run_fork_child_work_exec(const char* self)
{
    call_hot_target(2);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork-child-work");
        return 177;
    }
    if (pid == 0) {
        call_hot_target(6);
        char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
        char** envp = make_minimal_child_env();
        execve(self, argv, envp);
        perror("fork-child-work-execve");
        return 178;
    }
    return wait_for_spawned_child(pid);
}

static int
run_fork_exec_failure_trace(void)
{
    call_hot_target(2);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork-exec-failure-trace");
        return 179;
    }
    if (pid == 0) {
        call_hot_target(3);
        char* const argv[] = {(char*)"/definitely/not/found", NULL};
        errno = 0;
        int result = execv("/definitely/not/found", argv);
        int saved_errno = errno;
        if (result != -1 || saved_errno != ENOENT) {
            fprintf(stderr,
                    "fork_exec_failure_bad_result result=%d errno=%d\n",
                    result,
                    saved_errno);
            return 180;
        }
        call_hot_target(4);
        printf("fork_exec_failure_errno=%d continued sink=%lu\n",
               saved_errno,
               peak_exec_chain_sink);
        fflush(stdout);
        return 0;
    }
    return wait_for_spawned_child(pid);
}

static int
run_syscall_clone_exec_custom_env(const char* self)
{
    call_hot_target(2);
    pid_t pid = raw_clone_fork_like();
    if (pid < 0) {
        perror("syscall-clone");
        return 186;
    }
    if (pid == 0) {
        call_hot_target(5);
        char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
        char** envp = make_minimal_child_env();
        execve(self, argv, envp);
        perror("syscall-clone-execve");
        _exit(187);
    }
    return wait_for_spawned_child(pid);
}

static int
run_syscall_clone_syscall_exec_custom_env(const char* self)
{
    call_hot_target(2);
    pid_t pid = raw_clone_fork_like();
    if (pid < 0) {
        perror("syscall-clone");
        return 188;
    }
    if (pid == 0) {
        call_hot_target(5);
        char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
        char** envp = make_minimal_child_env();
#ifdef SYS_execve
        syscall(SYS_execve, self, argv, envp);
#else
        errno = ENOSYS;
#endif
        perror("syscall-clone-syscall-execve");
        _exit(189);
    }
    return wait_for_spawned_child(pid);
}

static int
run_syscall_clone_exec_failure_trace(void)
{
    call_hot_target(2);
    pid_t pid = raw_clone_fork_like();
    if (pid < 0) {
        perror("syscall-clone-failure");
        return 190;
    }
    if (pid == 0) {
        call_hot_target(3);
        char* const argv[] = {(char*)"/definitely/not/found", NULL};
        errno = 0;
        int result = execv("/definitely/not/found", argv);
        int saved_errno = errno;
        if (result != -1 || saved_errno != ENOENT) {
            _exit(191);
        }
        call_hot_target(4);
        (void)write(STDOUT_FILENO,
                    "syscall_clone_exec_failure_errno=2\n",
                    strlen("syscall_clone_exec_failure_errno=2\n"));
        _exit(0);
    }
    return wait_for_spawned_child(pid);
}

static int
run_syscall_clone3_exec_custom_env(const char* self)
{
#if defined(PEAK_TEST_HAVE_SYS_CLONE3) && defined(SYS_clone3)
    struct clone_args args;
    memset(&args, 0, sizeof(args));
    args.exit_signal = SIGCHLD;

    call_hot_target(2);
    long raw_pid = syscall(SYS_clone3, &args, sizeof(args));
    if (raw_pid < 0) {
        if (errno == ENOSYS) {
            printf("clone3_unavailable\n");
            fflush(stdout);
            return 0;
        }
        perror("syscall-clone3");
        return 202;
    }
    if (raw_pid == 0) {
        call_hot_target(5);
        char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
        char** envp = make_minimal_child_env();
        execve(self, argv, envp);
        perror("syscall-clone3-execve");
        _exit(203);
    }
    return wait_for_spawned_child((pid_t)raw_pid);
#else
    printf("clone3_unavailable\n");
    fflush(stdout);
    return 0;
#endif
}

static int
run_syscall_clone3_bad_pointer_failure(void)
{
#if defined(PEAK_TEST_HAVE_SYS_CLONE3) && defined(SYS_clone3)
    call_hot_target(3);
    errno = 0;
    long raw_pid = syscall(SYS_clone3, (void*)1, sizeof(struct clone_args));
    int saved_errno = errno;
    if (raw_pid != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "syscall_clone3_bad_pointer_bad_result result=%ld errno=%d\n",
                raw_pid,
                saved_errno);
        return 216;
    }
    call_hot_target(3);
    printf("syscall_clone3_bad_pointer_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
#else
    printf("clone3_unavailable\n");
    fflush(stdout);
    return 0;
#endif
}

static int
run_vfork_exec_custom_env(const char* self)
{
    char** envp = make_minimal_child_env();
    call_hot_target(5);
    pid_t pid = vfork();
    if (pid < 0) {
        perror("vfork");
        return 145;
    }
    if (pid == 0) {
        char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
        execve(self, argv, envp);
        perror("vfork-execve");
        _exit(146);
    }
    return wait_for_spawned_child(pid);
}

static int
run_vfork_exec_failure_trace(void)
{
    call_hot_target(2);
    pid_t pid = vfork();
    if (pid < 0) {
        perror("vfork-exec-failure-trace");
        return 181;
    }
    if (pid == 0) {
        char* const argv[] = {(char*)"/definitely/not/found", NULL};
        errno = 0;
        int result = execv("/definitely/not/found", argv);
        int saved_errno = errno;
        if (result != -1 || saved_errno != ENOENT) {
            _exit(182);
        }
        (void)write(STDOUT_FILENO,
                    "vfork_exec_failure_errno=2\n",
                    strlen("vfork_exec_failure_errno=2\n"));
        _exit(0);
    }
    return wait_for_spawned_child(pid);
}

static int
run_vfork_execvp_enoexec_fallback(void)
{
    call_hot_target(2);
    pid_t pid = vfork();
    if (pid < 0) {
        perror("vfork-execvp-enoexec");
        return 192;
    }
    if (pid == 0) {
        char* const argv[] = {
            (char*)"peak-enoexec-script",
            (char*)"alpha",
            (char*)"beta",
            NULL
        };
        execvp("peak-enoexec-script", argv);
        _exit(193);
    }
    int result =
        wait_for_spawned_child_exit(pid, 37, "vfork_execvp_enoexec");
    if (result != 0) {
        return result;
    }
    printf("vfork_execvp_enoexec_exit=37\n");
    fflush(stdout);
    return 0;
}

static int
run_vfork_execvp_empty_path_component(void)
{
    call_hot_target(5);
    pid_t pid = vfork();
    if (pid < 0) {
        perror("vfork-execvp-empty-path");
        return 194;
    }
    if (pid == 0) {
        char* const argv[] = {
            (char*)"test_exec_chain",
            (char*)"child-basic",
            NULL
        };
        execvp("test_exec_chain", argv);
        _exit(195);
    }
    return wait_for_spawned_child(pid);
}

static int
run_vfork_large_child_env(const char* self)
{
    char** envp = make_large_child_env(9000);
    call_hot_target(5);
    pid_t pid = vfork();
    if (pid < 0) {
        perror("vfork-large-child-env");
        return 163;
    }
    if (pid == 0) {
        char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
        execve(self, argv, envp);
        perror("vfork-large-child-env-execve");
        _exit(164);
    }
    return wait_for_spawned_child(pid);
}

static int
run_vfork_duplicate_preload(const char* self)
{
    char** envp = make_duplicate_preload_entry_env();
    call_hot_target(5);
    pid_t pid = vfork();
    if (pid < 0) {
        perror("vfork-duplicate-preload");
        return 165;
    }
    if (pid == 0) {
        char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
        execve(self, argv, envp);
        perror("vfork-duplicate-preload-execve");
        _exit(166);
    }
    return wait_for_spawned_child(pid);
}

static void
populate_large_peak_env(void)
{
    char name[64];
    for (int i = 0; i < 4200; i++) {
        snprintf(name, sizeof(name), "PEAK_PAD_%04d", i);
        if (setenv(name, "x", 1) != 0) {
            perror("setenv");
            exit(156);
        }
    }
}

static void
populate_huge_peak_env(void)
{
    char name[64];
    for (int i = 0; i < 20000; i++) {
        snprintf(name, sizeof(name), "PEAK_PAD_HUGE_%05d", i);
        if (setenv(name, "x", 1) != 0) {
            perror("setenv");
            exit(169);
        }
    }
}

static void
populate_overflow_peak_env(void)
{
    char name[64];
    for (int i = 0; i < 42000; i++) {
        snprintf(name, sizeof(name), "PEAK_PAD_OVERFLOW_%05d", i);
        if (setenv(name, "x", 1) != 0) {
            perror("setenv");
            exit(202);
        }
    }
    if (setenv("PEAK_MEMLOG_PATH", "exec-overflow-sentinel", 1) != 0) {
        perror("setenv");
        exit(203);
    }
}

static int
run_vfork_native_optout_then_parent_exec(const char* self)
{
    if (setenv("PEAK_EXEC_CHAIN", "0", 1) != 0 ||
        setenv("PEAK_EXEC_CHECKPOINT", "0", 1) != 0) {
        perror("setenv");
        return 157;
    }

    call_hot_target(5);
    char** envp = make_minimal_child_env();
    pid_t pid = vfork();
    if (pid < 0) {
        perror("vfork");
        return 158;
    }
    if (pid == 0) {
        char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
        execve(self, argv, envp);
        perror("native-vfork-execve");
        _exit(159);
    }
    int result = wait_for_spawned_child(pid);
    if (result != 0) {
        return result;
    }

    unsetenv("PEAK_EXEC_CHAIN");
    unsetenv("PEAK_EXEC_CHECKPOINT");
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** parent_envp = make_minimal_child_env();
    execve(self, argv, parent_envp);
    perror("native-vfork-parent-execve");
    return 160;
}

static int
run_fork_large_peak_env(const char* self)
{
    populate_large_peak_env();
    return run_fork_exec_custom_env(self);
}

static int
run_vfork_large_peak_env(const char* self)
{
    populate_large_peak_env();
    return run_vfork_exec_custom_env(self);
}

static int
run_vfork_huge_peak_env(const char* self)
{
    populate_huge_peak_env();
    return run_vfork_exec_custom_env(self);
}

static int
run_vfork_overflow_peak_env(const char* self)
{
    populate_overflow_peak_env();
    char** envp = make_minimal_child_env();
    call_hot_target(5);
    pid_t pid = vfork();
    if (pid < 0) {
        perror("vfork-overflow-peak-env");
        return 204;
    }
    if (pid == 0) {
        char* const argv[] = {
            (char*)self,
            (char*)"child-check-env",
            (char*)"PEAK_MEMLOG_PATH",
            NULL
        };
        execve(self, argv, envp);
        perror("vfork-overflow-peak-env-execve");
        _exit(205);
    }
    return wait_for_spawned_child(pid);
}

static int
clone_exec_child(void* arg)
{
    CloneExecArgs* exec_args = (CloneExecArgs*)arg;
    const char* self = exec_args->self;
    const char* child_arg =
        exec_args->child_arg != NULL ? exec_args->child_arg : "child-basic";
    char* const argv[] = {
        (char*)self,
        (char*)child_arg,
        (char*)exec_args->child_env_name,
        NULL
    };
    execve(self, argv, exec_args->envp);
    perror("clone-vfork-execve");
    _exit(148);
}

static int
clone_child_work_exec_child(void* arg)
{
    call_hot_target(6);
    return clone_exec_child(arg);
}

static int
clone_exec_failure_child(void* arg)
{
    (void)arg;
    char* const argv[] = {(char*)"/definitely/not/found", NULL};
    errno = 0;
    int result = execv("/definitely/not/found", argv);
    int saved_errno = errno;
    if (result != -1 || saved_errno != ENOENT) {
        _exit(183);
    }
    (void)write(STDOUT_FILENO,
                "clone_vfork_exec_failure_errno=2\n",
                strlen("clone_vfork_exec_failure_errno=2\n"));
    _exit(0);
}

static int
clone_private_exec_failure_child(void* arg)
{
    (void)arg;
    char* const argv[] = {(char*)"/definitely/not/found", NULL};

    call_hot_target(5);
    errno = 0;
    int result = execv("/definitely/not/found", argv);
    int saved_errno = errno;
    if (result != -1 || saved_errno != ENOENT) {
        _exit(216);
    }
    call_hot_target(4);
    printf("clone_private_exec_failure_errno=2 continued sink=%lu\n",
           peak_exec_chain_sink);
    exit(0);
}

static int
clone_vm_exec_failure_child(void* arg)
{
    (void)arg;
    char* const argv[] = {(char*)"/definitely/not/found", NULL};
    errno = 0;
    int result = execv("/definitely/not/found", argv);
    int saved_errno = errno;
    if (result != -1 || saved_errno != ENOENT) {
        _exit(211);
    }
    (void)write(STDOUT_FILENO,
                "clone_vm_exec_failure_errno=2\n",
                strlen("clone_vm_exec_failure_errno=2\n"));
    _exit(0);
}

static int
clone_execvp_enoexec_child(void* arg)
{
    (void)arg;
    char* const argv[] = {
        (char*)"peak-enoexec-script",
        (char*)"alpha",
        (char*)"beta",
        NULL
    };
    execvp("peak-enoexec-script", argv);
    _exit(196);
}

static int
clone_execvp_empty_path_child(void* arg)
{
    (void)arg;
    char* const argv[] = {
        (char*)"test_exec_chain",
        (char*)"child-basic",
        NULL
    };
    execvp("test_exec_chain", argv);
    _exit(197);
}

static int
run_clone_vfork_exec_custom_env(const char* self)
{
    enum { STACK_SIZE = 1024 * 1024 };
    void* stack = malloc(STACK_SIZE);
    void* stack_top;
    pid_t pid;
    int result;
    CloneExecArgs exec_args;

    if (stack == NULL) {
        perror("malloc");
        return 147;
    }

    call_hot_target(5);
    exec_args.self = self;
    exec_args.envp = make_minimal_child_env();
    exec_args.child_arg = "child-basic";
    exec_args.child_env_name = NULL;
    stack_top = (char*)stack + STACK_SIZE;
    pid = clone(clone_exec_child,
                stack_top,
                CLONE_VM | CLONE_VFORK | SIGCHLD,
                &exec_args);
    if (pid < 0) {
        perror("clone");
        free(stack);
        return 149;
    }

    result = wait_for_spawned_child(pid);
    free(stack);
    return result;
}

static int
run_clone_vm_exec_custom_env(const char* self)
{
    enum { STACK_SIZE = 1024 * 1024 };
    void* stack = malloc(STACK_SIZE);
    void* stack_top;
    pid_t pid;
    int result;
    CloneExecArgs exec_args;

    if (stack == NULL) {
        perror("malloc");
        return 209;
    }

    call_hot_target(5);
    exec_args.self = self;
    exec_args.envp = make_minimal_child_env();
    exec_args.child_arg = "child-basic";
    exec_args.child_env_name = NULL;
    stack_top = (char*)stack + STACK_SIZE;
    pid = clone(clone_exec_child, stack_top, CLONE_VM | SIGCHLD, &exec_args);
    if (pid < 0) {
        perror("clone-vm");
        free(stack);
        return 210;
    }

    call_hot_target(5);
    result = wait_for_spawned_child(pid);
    free(stack);
    return result;
}

static int
run_clone_private_child_work_exec(const char* self)
{
    enum { STACK_SIZE = 1024 * 1024 };
    void* stack = malloc(STACK_SIZE);
    void* stack_top;
    pid_t pid;
    int result;
    CloneExecArgs exec_args;

    if (stack == NULL) {
        perror("malloc");
        return 214;
    }

    call_hot_target(2);
    exec_args.self = self;
    exec_args.envp = make_minimal_child_env();
    exec_args.child_arg = "child-basic";
    exec_args.child_env_name = NULL;
    stack_top = (char*)stack + STACK_SIZE;
    pid = clone(clone_child_work_exec_child, stack_top, SIGCHLD, &exec_args);
    if (pid < 0) {
        perror("clone-private-child-work");
        free(stack);
        return 215;
    }

    result = wait_for_spawned_child(pid);
    free(stack);
    return result;
}

static int
run_clone_vfork_large_peak_env(const char* self)
{
    populate_large_peak_env();
    return run_clone_vfork_exec_custom_env(self);
}

static int
run_clone_vfork_huge_peak_env(const char* self)
{
    populate_huge_peak_env();
    return run_clone_vfork_exec_custom_env(self);
}

static int
run_clone_vfork_overflow_peak_env(const char* self)
{
    enum { STACK_SIZE = 1024 * 1024 };
    void* stack = malloc(STACK_SIZE);
    void* stack_top;
    pid_t pid;
    int result;
    CloneExecArgs exec_args;

    if (stack == NULL) {
        perror("malloc");
        return 207;
    }

    populate_overflow_peak_env();
    call_hot_target(5);
    exec_args.self = self;
    exec_args.envp = make_minimal_child_env();
    exec_args.child_arg = "child-check-env";
    exec_args.child_env_name = "PEAK_MEMLOG_PATH";
    stack_top = (char*)stack + STACK_SIZE;
    pid = clone(clone_exec_child,
                stack_top,
                CLONE_VM | CLONE_VFORK | SIGCHLD,
                &exec_args);
    if (pid < 0) {
        perror("clone-vfork-overflow-peak-env");
        free(stack);
        return 208;
    }

    result = wait_for_spawned_child(pid);
    free(stack);
    return result;
}

static int
run_clone_vfork_exec_failure_trace(void)
{
    enum { STACK_SIZE = 1024 * 1024 };
    void* stack = malloc(STACK_SIZE);
    void* stack_top;
    pid_t pid;
    int result;

    if (stack == NULL) {
        perror("malloc");
        return 184;
    }

    call_hot_target(2);
    stack_top = (char*)stack + STACK_SIZE;
    pid = clone(clone_exec_failure_child,
                stack_top,
                CLONE_VM | CLONE_VFORK | SIGCHLD,
                NULL);
    if (pid < 0) {
        perror("clone-exec-failure-trace");
        free(stack);
        return 185;
    }

    result = wait_for_spawned_child(pid);
    free(stack);
    return result;
}

static int
run_clone_private_exec_failure_trace(void)
{
    enum { STACK_SIZE = 1024 * 1024 };
    void* stack = malloc(STACK_SIZE);
    void* stack_top;
    pid_t pid;
    int result;

    if (stack == NULL) {
        perror("malloc");
        return 216;
    }

    call_hot_target(2);
    stack_top = (char*)stack + STACK_SIZE;
    pid = clone(clone_private_exec_failure_child, stack_top, SIGCHLD, NULL);
    if (pid < 0) {
        perror("clone-private-exec-failure-trace");
        free(stack);
        return 217;
    }

    result = wait_for_spawned_child(pid);
    free(stack);
    return result;
}

static int
run_clone_vm_exec_failure_trace(void)
{
    enum { STACK_SIZE = 1024 * 1024 };
    void* stack = malloc(STACK_SIZE);
    void* stack_top;
    pid_t pid;
    int result;

    if (stack == NULL) {
        perror("malloc");
        return 212;
    }

    call_hot_target(2);
    stack_top = (char*)stack + STACK_SIZE;
    pid = clone(clone_vm_exec_failure_child, stack_top, CLONE_VM | SIGCHLD, NULL);
    if (pid < 0) {
        perror("clone-vm-exec-failure-trace");
        free(stack);
        return 213;
    }

    call_hot_target(4);
    result = wait_for_spawned_child(pid);
    call_hot_target(2);
    free(stack);
    return result;
}

static int
run_clone_vfork_execvp_enoexec_fallback(void)
{
    enum { STACK_SIZE = 1024 * 1024 };
    void* stack = malloc(STACK_SIZE);
    void* stack_top;
    pid_t pid;
    int result;

    if (stack == NULL) {
        perror("malloc");
        return 198;
    }

    call_hot_target(2);
    stack_top = (char*)stack + STACK_SIZE;
    pid = clone(clone_execvp_enoexec_child,
                stack_top,
                CLONE_VM | CLONE_VFORK | SIGCHLD,
                NULL);
    if (pid < 0) {
        perror("clone-vfork-execvp-enoexec");
        free(stack);
        return 199;
    }

    result = wait_for_spawned_child_exit(pid, 37, "clone_vfork_execvp_enoexec");
    free(stack);
    if (result != 0) {
        return result;
    }
    printf("clone_vfork_execvp_enoexec_exit=37\n");
    fflush(stdout);
    return 0;
}

static int
run_clone_vfork_execvp_empty_path_component(void)
{
    enum { STACK_SIZE = 1024 * 1024 };
    void* stack = malloc(STACK_SIZE);
    void* stack_top;
    pid_t pid;
    int result;

    if (stack == NULL) {
        perror("malloc");
        return 200;
    }

    call_hot_target(5);
    stack_top = (char*)stack + STACK_SIZE;
    pid = clone(clone_execvp_empty_path_child,
                stack_top,
                CLONE_VM | CLONE_VFORK | SIGCHLD,
                NULL);
    if (pid < 0) {
        perror("clone-vfork-execvp-empty-path");
        free(stack);
        return 201;
    }

    result = wait_for_spawned_child(pid);
    free(stack);
    return result;
}

static int
run_clone_vfork_duplicate_preload_env(const char* self)
{
    enum { STACK_SIZE = 1024 * 1024 };
    void* stack = malloc(STACK_SIZE);
    void* stack_top;
    pid_t pid;
    int result;
    CloneExecArgs exec_args;

    if (stack == NULL) {
        perror("malloc");
        return 167;
    }

    call_hot_target(5);
    exec_args.self = self;
    exec_args.envp = make_duplicate_preload_entry_env();
    exec_args.child_arg = "child-print-ld";
    exec_args.child_env_name = NULL;
    stack_top = (char*)stack + STACK_SIZE;
    pid = clone(clone_exec_child,
                stack_top,
                CLONE_VM | CLONE_VFORK | SIGCHLD,
                &exec_args);
    if (pid < 0) {
        perror("clone");
        free(stack);
        return 168;
    }

    result = wait_for_spawned_child(pid);
    free(stack);
    return result;
}

static int
run_clone_vfork_then_parent_exec(const char* self)
{
    enum { STACK_SIZE = 1024 * 1024 };
    void* stack = malloc(STACK_SIZE);
    void* stack_top;
    pid_t pid;
    int result;
    CloneExecArgs exec_args;

    if (stack == NULL) {
        perror("malloc");
        return 150;
    }

    call_hot_target(5);
    exec_args.self = self;
    exec_args.envp = make_minimal_child_env();
    exec_args.child_arg = "child-basic";
    exec_args.child_env_name = NULL;
    stack_top = (char*)stack + STACK_SIZE;
    pid = clone(clone_exec_child,
                stack_top,
                CLONE_VM | CLONE_VFORK | SIGCHLD,
                &exec_args);
    if (pid < 0) {
        perror("clone");
        free(stack);
        return 151;
    }

    result = wait_for_spawned_child(pid);
    free(stack);
    if (result != 0) {
        return result;
    }

    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_minimal_child_env();
    execve(self, argv, envp);
    perror("clone-parent-execve");
    return 152;
}

static int
run_posix_spawn_custom_env(const char* self)
{
    pid_t pid = -1;
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_minimal_child_env();
    int result = posix_spawn(&pid, self, NULL, NULL, argv, envp);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_failed=%d\n", result);
        return 119;
    }
    return wait_for_spawned_child(pid);
}

static int
run_posix_spawn_null_env(const char* self)
{
    pid_t pid = -1;
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    int result = posix_spawn(&pid, self, NULL, NULL, argv, NULL);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_null_env_failed=%d\n", result);
        return 191;
    }
    return wait_for_spawned_child(pid);
}

static int
run_posix_spawn_bad_env(const char* self)
{
    pid_t pid = -1;
    call_hot_target(2);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    int result =
        posix_spawn(&pid, self, NULL, NULL, argv, invalid_envp_for_test());
    if (result != EFAULT) {
        fprintf(stderr, "posix_spawn_bad_env_result=%d\n", result);
        return 192;
    }
    call_hot_target(4);
    printf("posix_spawn_bad_env_result=%d continued sink=%lu\n",
           result,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_posix_spawn_bad_argv(const char* self)
{
    pid_t pid = -1;
    call_hot_target(2);
    int result =
        posix_spawn(&pid, self, NULL, NULL, invalid_argv_for_test(), environ);
    if (result != EFAULT) {
        fprintf(stderr, "posix_spawn_bad_argv_result=%d\n", result);
        return 238;
    }
    call_hot_target(4);
    printf("posix_spawn_bad_argv_result=%d continued sink=%lu\n",
           result,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_posix_spawnp_bad_env(void)
{
    pid_t pid = -1;
    call_hot_target(2);
    char* const argv[] = {(char*)"test_exec_chain", (char*)"child-basic", NULL};
    int result =
        posix_spawnp(&pid, "test_exec_chain", NULL, NULL, argv,
                     invalid_envp_for_test());
    if (result != EFAULT) {
        fprintf(stderr, "posix_spawnp_bad_env_result=%d\n", result);
        return 222;
    }
    call_hot_target(4);
    printf("posix_spawnp_bad_env_result=%d continued sink=%lu\n",
           result,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_posix_spawnp_bad_argv(void)
{
    pid_t pid = -1;
    call_hot_target(2);
    int result = posix_spawnp(&pid,
                              "test_exec_chain",
                              NULL,
                              NULL,
                              invalid_argv_for_test(),
                              environ);
    if (result != EFAULT) {
        fprintf(stderr, "posix_spawnp_bad_argv_result=%d\n", result);
        return 239;
    }
    call_hot_target(4);
    printf("posix_spawnp_bad_argv_result=%d continued sink=%lu\n",
           result,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_posix_spawn_preflight_unavailable(const char* self)
{
    pid_t pid = -1;
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_minimal_child_env();
    int result = posix_spawn(&pid, self, NULL, NULL, argv, envp);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_preflight_unavailable_failed=%d\n", result);
        return 193;
    }
    result = wait_for_spawned_child(pid);
    if (result != 0) {
        return result;
    }
    call_hot_target(4);
    printf("posix_spawn_preflight_unavailable_ok sink=%lu\n",
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_posix_spawn_duplicate_preload(const char* self)
{
    pid_t pid = -1;
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_duplicate_preload_entry_env();
    int result = posix_spawn(&pid, self, NULL, NULL, argv, envp);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_duplicate_preload_failed=%d\n", result);
        return 153;
    }
    return wait_for_spawned_child(pid);
}

static int
run_posix_spawn_actions_attrs(const char* self)
{
    const char* output_path = getenv("EXEC_CHAIN_SPAWN_ACTIONS_OUT");
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attrs;
    sigset_t empty_mask;
    pid_t pid = -1;
    int actions_ready = 0;
    int attrs_ready = 0;
    int result;
    int wait_result;

    if (output_path == NULL || output_path[0] == '\0') {
        fprintf(stderr, "missing EXEC_CHAIN_SPAWN_ACTIONS_OUT\n");
        return 170;
    }

    result = posix_spawn_file_actions_init(&actions);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_file_actions_init_failed=%d\n", result);
        return 171;
    }
    actions_ready = 1;

    result = posix_spawn_file_actions_addopen(&actions,
                                              STDOUT_FILENO,
                                              output_path,
                                              O_CREAT | O_TRUNC | O_WRONLY,
                                              0600);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_file_actions_addopen_failed=%d\n", result);
        posix_spawn_file_actions_destroy(&actions);
        return 172;
    }

    result = posix_spawnattr_init(&attrs);
    if (result != 0) {
        fprintf(stderr, "posix_spawnattr_init_failed=%d\n", result);
        posix_spawn_file_actions_destroy(&actions);
        return 173;
    }
    attrs_ready = 1;

    sigemptyset(&empty_mask);
    result = posix_spawnattr_setsigmask(&attrs, &empty_mask);
    if (result != 0) {
        fprintf(stderr, "posix_spawnattr_setsigmask_failed=%d\n", result);
        posix_spawnattr_destroy(&attrs);
        posix_spawn_file_actions_destroy(&actions);
        return 174;
    }
    result = posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETSIGMASK);
    if (result != 0) {
        fprintf(stderr, "posix_spawnattr_setflags_failed=%d\n", result);
        posix_spawnattr_destroy(&attrs);
        posix_spawn_file_actions_destroy(&actions);
        return 175;
    }

    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_minimal_child_env();
    result = posix_spawn(&pid, self, &actions, &attrs, argv, envp);
    if (attrs_ready) {
        posix_spawnattr_destroy(&attrs);
    }
    if (actions_ready) {
        posix_spawn_file_actions_destroy(&actions);
    }
    if (result != 0) {
        fprintf(stderr, "posix_spawn_actions_attrs_failed=%d\n", result);
        return 176;
    }

    wait_result = wait_for_spawned_child(pid);
    if (wait_result != 0) {
        return wait_result;
    }
    printf("posix_spawn_actions_attrs_ok\n");
    fflush(stdout);
    return 0;
}

static int
run_posix_spawn_actions_close_stderr(const char* self)
{
    const char* output_path = getenv("EXEC_CHAIN_SPAWN_ACTIONS_OUT");
    posix_spawn_file_actions_t actions;
    pid_t pid = -1;
    int actions_ready = 0;
    int result;
    int wait_result;

    if (output_path == NULL || output_path[0] == '\0') {
        fprintf(stderr, "missing EXEC_CHAIN_SPAWN_ACTIONS_OUT\n");
        return 197;
    }

    result = posix_spawn_file_actions_init(&actions);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_close_stderr_actions_init_failed=%d\n",
                result);
        return 198;
    }
    actions_ready = 1;

    result = posix_spawn_file_actions_addopen(&actions,
                                              STDOUT_FILENO,
                                              output_path,
                                              O_CREAT | O_TRUNC | O_WRONLY,
                                              0600);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_close_stderr_addopen_failed=%d\n",
                result);
        posix_spawn_file_actions_destroy(&actions);
        return 199;
    }

    result = posix_spawn_file_actions_addclose(&actions, STDERR_FILENO);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_close_stderr_addclose_failed=%d\n",
                result);
        posix_spawn_file_actions_destroy(&actions);
        return 200;
    }

    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-stderr-sentinel", NULL};
    char** envp = make_minimal_child_env();
    result = posix_spawn(&pid, self, &actions, NULL, argv, envp);
    if (actions_ready) {
        posix_spawn_file_actions_destroy(&actions);
    }
    if (result != 0) {
        fprintf(stderr, "posix_spawn_actions_close_stderr_failed=%d\n",
                result);
        return 201;
    }

    wait_result = wait_for_spawned_child(pid);
    if (wait_result != 0) {
        return wait_result;
    }
    printf("posix_spawn_actions_close_stderr_ok\n");
    fflush(stdout);
    return 0;
}

static int
run_posix_spawn_usevfork(const char* self)
{
#if defined(PEAK_TEST_HAVE_POSIX_SPAWN_USEVFORK)
    posix_spawnattr_t attrs;
    pid_t pid = -1;
    int result;
    int wait_result;

    result = posix_spawnattr_init(&attrs);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_usevfork_attr_init_failed=%d\n", result);
        return 194;
    }

    result = posix_spawnattr_setflags(&attrs, POSIX_SPAWN_USEVFORK);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_usevfork_setflags_failed=%d\n", result);
        posix_spawnattr_destroy(&attrs);
        return 195;
    }

    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_minimal_child_env();
    result = posix_spawn(&pid, self, NULL, &attrs, argv, envp);
    posix_spawnattr_destroy(&attrs);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_usevfork_failed=%d\n", result);
        return 196;
    }

    wait_result = wait_for_spawned_child(pid);
    if (wait_result != 0) {
        return wait_result;
    }
    printf("posix_spawn_usevfork_ok\n");
    fflush(stdout);
    return 0;
#else
    (void)self;
    fprintf(stderr, "posix_spawn_usevfork_unavailable\n");
    return 197;
#endif
}

static int
run_posix_spawn_child_peak_env_only(const char* self)
{
    pid_t pid = -1;
    EnvSnapshot parent_env;
    int result;

    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_child_peak_only_env();
    snapshot_and_unset_peak_env(&parent_env);
    result = posix_spawn(&pid, self, NULL, NULL, argv, envp);
    restore_peak_env_snapshot(&parent_env);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_child_peak_env_only_failed=%d\n", result);
        return 154;
    }
    return wait_for_spawned_child(pid);
}

static int
run_posix_spawn_explicit_peak_env(const char* self)
{
    pid_t pid = -1;
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_explicit_wrong_peak_target_env();
    int result = posix_spawn(&pid, self, NULL, NULL, argv, envp);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_explicit_peak_env_failed=%d\n", result);
        return 155;
    }
    return wait_for_spawned_child(pid);
}

static int
run_posix_spawnp_path_search(void)
{
    pid_t pid = -1;
    call_hot_target(5);
    char* const argv[] = {(char*)"test_exec_chain", (char*)"child-basic", NULL};
    char** envp = make_minimal_child_env();
    int result = posix_spawnp(&pid, "test_exec_chain", NULL, NULL, argv, envp);
    if (result != 0) {
        fprintf(stderr, "posix_spawnp_failed=%d\n", result);
        return 120;
    }
    return wait_for_spawned_child(pid);
}

static int
run_posix_spawnp_child_env_path_ignored(void)
{
    pid_t pid = -1;
    call_hot_target(5);
    char* const argv[] = {(char*)"test_exec_chain", (char*)"child-basic", NULL};
    char** envp = make_bad_path_child_env();
    int result = posix_spawnp(&pid, "test_exec_chain", NULL, NULL, argv, envp);
    if (result != 0) {
        fprintf(stderr, "posix_spawnp_child_env_path_ignored_failed=%d\n", result);
        return 122;
    }
    return wait_for_spawned_child(pid);
}

static int
run_execvpe_child_env_path_used(void)
{
    call_hot_target(5);
    char* const argv[] = {(char*)"test_exec_chain", (char*)"child-basic", NULL};
    char** envp = make_child_path_env();
    execvpe("test_exec_chain", argv, envp);
    perror("execvpe-child-env-path-used");
    return 121;
}

static int
run_execvpe_child_path_duplicate_preload(void)
{
    call_hot_target(5);
    char* const argv[] = {
        (char*)"test_exec_chain",
        (char*)"child-print-ld",
        NULL
    };
    char** envp = make_child_path_duplicate_preload_env();
    execvpe("test_exec_chain", argv, envp);
    perror("execvpe-child-path-duplicate-preload");
    return 226;
}

static int
run_posix_spawn_failure(const char* self)
{
    pid_t pid = -1;
    call_hot_target(2);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    int result = posix_spawn(&pid,
                             "/definitely/not/found",
                             NULL,
                             NULL,
                             argv,
                             environ);
    if (result != ENOENT) {
        fprintf(stderr, "posix_spawn_failure_bad_result=%d\n", result);
        return 134;
    }
    call_hot_target(4);
    printf("posix_spawn_failure_result=%d continued sink=%lu\n",
           result,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

int
main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <mode>\n", argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "child-basic") == 0) {
        return run_child_basic();
    }
    if (strcmp(argv[1], "child-print-ld") == 0) {
        return run_child_print_ld();
    }
    if (strcmp(argv[1], "child-stderr-sentinel") == 0) {
        return run_child_stderr_sentinel();
    }
    if (strcmp(argv[1], "child-check-env") == 0 && argc >= 3) {
        return run_child_check_env(argv[2]);
    }
    if (strcmp(argv[1], "execv-success") == 0) {
        return run_execv_success(argv[0]);
    }
    if (strcmp(argv[1], "execv-write-target-success") == 0) {
        return run_execv_write_target_success(argv[0]);
    }
    if (strcmp(argv[1], "execve-custom-env") == 0) {
        return run_execve_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "execve-child-peak-env-only") == 0) {
        return run_execve_child_peak_env_only(argv[0]);
    }
    if (strcmp(argv[1], "syscall-execve-custom-env") == 0) {
        return run_syscall_execve_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "syscall-execve-failure") == 0) {
        return run_syscall_execve_failure();
    }
    if (strcmp(argv[1], "syscall-execve-bad-env") == 0) {
        return run_syscall_execve_bad_env_failure();
    }
    if (strcmp(argv[1], "syscall-execve-bad-argv") == 0) {
        return run_syscall_execve_bad_argv_failure();
    }
    if (strcmp(argv[1], "execve-bad-env") == 0) {
        return run_execve_bad_env_failure();
    }
    if (strcmp(argv[1], "execve-bad-argv") == 0) {
        return run_execve_bad_argv_failure();
    }
    if (strcmp(argv[1], "execve-explicit-peak-env") == 0) {
        return run_execve_explicit_peak_env(argv[0]);
    }
    if (strcmp(argv[1], "execve-null-env") == 0) {
        return run_execve_null_env(argv[0]);
    }
    if (strcmp(argv[1], "execv-zero-call") == 0) {
        return run_execv_zero_call_checkpoint(argv[0]);
    }
    if (strcmp(argv[1], "fexecve-custom-env") == 0) {
        return run_fexecve_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "fexecve-failure") == 0) {
        return run_fexecve_failure();
    }
    if (strcmp(argv[1], "fexecve-bad-env") == 0) {
        return run_fexecve_bad_env_failure(argv[0]);
    }
    if (strcmp(argv[1], "fexecve-bad-argv") == 0) {
        return run_fexecve_bad_argv_failure(argv[0]);
    }
    if (strcmp(argv[1], "execveat-custom-env") == 0) {
        return run_execveat_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "execveat-empty-path-custom-env") == 0) {
        return run_execveat_empty_path_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "execveat-empty-path-bad-env") == 0) {
        return run_execveat_empty_path_bad_env_failure(argv[0]);
    }
    if (strcmp(argv[1], "execveat-empty-path-bad-argv") == 0) {
        return run_execveat_empty_path_bad_argv_failure(argv[0]);
    }
    if (strcmp(argv[1], "syscall-execveat-custom-env") == 0) {
        return run_syscall_execveat_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "execveat-failure") == 0) {
        return run_execveat_failure();
    }
    if (strcmp(argv[1], "syscall-execveat-failure") == 0) {
        return run_syscall_execveat_failure();
    }
    if (strcmp(argv[1], "execveat-bad-env") == 0) {
        return run_execveat_bad_env_failure();
    }
    if (strcmp(argv[1], "execveat-bad-argv") == 0) {
        return run_execveat_bad_argv_failure();
    }
    if (strcmp(argv[1], "syscall-execveat-bad-env") == 0) {
        return run_syscall_execveat_bad_env_failure();
    }
    if (strcmp(argv[1], "syscall-execveat-bad-argv") == 0) {
        return run_syscall_execveat_bad_argv_failure();
    }
    if (strcmp(argv[1], "helper-named-exec") == 0) {
        return run_helper_named_exec();
    }
    if (strcmp(argv[1], "configured-helper-named-exec") == 0) {
        return run_configured_helper_named_exec();
    }
    if (strcmp(argv[1], "execl-success") == 0) {
        return run_execl_success(argv[0]);
    }
    if (strcmp(argv[1], "execlp-path-search") == 0) {
        return run_execlp_path_search();
    }
    if (strcmp(argv[1], "execle-custom-env") == 0) {
        return run_execle_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "fork-execl-success") == 0) {
        return run_fork_execl_success(argv[0]);
    }
    if (strcmp(argv[1], "fork-execlp-path-search") == 0) {
        return run_fork_execlp_path_search();
    }
    if (strcmp(argv[1], "fork-execle-custom-env") == 0) {
        return run_fork_execle_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "exec-failure") == 0) {
        return run_exec_failure();
    }
    if (strcmp(argv[1], "execvp-path-search") == 0) {
        return run_execvp_path_search();
    }
    if (strcmp(argv[1], "execvp-enoent") == 0) {
        return run_execvp_enoent();
    }
    if (strcmp(argv[1], "execvp-eacces") == 0) {
        return run_execvp_eacces();
    }
    if (strcmp(argv[1], "execvp-enoexec") == 0) {
        return run_execvp_enoexec();
    }
    if (strcmp(argv[1], "fork-execvp-enoexec") == 0) {
        return run_fork_execvp_enoexec_fallback();
    }
    if (strcmp(argv[1], "duplicate-preload") == 0) {
        return run_duplicate_preload(argv[0]);
    }
    if (strcmp(argv[1], "duplicate-preload-whitespace") == 0) {
        return run_whitespace_duplicate_preload(argv[0]);
    }
    if (strcmp(argv[1], "duplicate-preload-entry") == 0) {
        return run_duplicate_preload_entry(argv[0]);
    }
    if (strcmp(argv[1], "fork-execve-custom-env") == 0) {
        return run_fork_exec_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "fork-child-work-exec") == 0) {
        return run_fork_child_work_exec(argv[0]);
    }
    if (strcmp(argv[1], "fork-exec-failure-trace") == 0) {
        return run_fork_exec_failure_trace();
    }
    if (strcmp(argv[1], "syscall-clone-execve-custom-env") == 0) {
        return run_syscall_clone_exec_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "syscall-clone-syscall-execve-custom-env") == 0) {
        return run_syscall_clone_syscall_exec_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "syscall-clone-exec-failure-trace") == 0) {
        return run_syscall_clone_exec_failure_trace();
    }
    if (strcmp(argv[1], "syscall-clone3-execve-custom-env") == 0) {
        return run_syscall_clone3_exec_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "syscall-clone3-bad-pointer") == 0) {
        return run_syscall_clone3_bad_pointer_failure();
    }
    if (strcmp(argv[1], "vfork-execve-custom-env") == 0) {
        return run_vfork_exec_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "vfork-exec-failure-trace") == 0) {
        return run_vfork_exec_failure_trace();
    }
    if (strcmp(argv[1], "vfork-execvp-enoexec") == 0) {
        return run_vfork_execvp_enoexec_fallback();
    }
    if (strcmp(argv[1], "vfork-execvp-empty-path") == 0) {
        return run_vfork_execvp_empty_path_component();
    }
    if (strcmp(argv[1], "vfork-large-child-env") == 0) {
        return run_vfork_large_child_env(argv[0]);
    }
    if (strcmp(argv[1], "vfork-duplicate-preload") == 0) {
        return run_vfork_duplicate_preload(argv[0]);
    }
    if (strcmp(argv[1], "vfork-native-optout-parent-exec") == 0) {
        return run_vfork_native_optout_then_parent_exec(argv[0]);
    }
    if (strcmp(argv[1], "fork-large-peak-env") == 0) {
        return run_fork_large_peak_env(argv[0]);
    }
    if (strcmp(argv[1], "vfork-large-peak-env") == 0) {
        return run_vfork_large_peak_env(argv[0]);
    }
    if (strcmp(argv[1], "clone-vfork-execve-custom-env") == 0) {
        return run_clone_vfork_exec_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "clone-vm-execve-custom-env") == 0) {
        return run_clone_vm_exec_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "clone-private-child-work-exec") == 0) {
        return run_clone_private_child_work_exec(argv[0]);
    }
    if (strcmp(argv[1], "clone-vfork-large-peak-env") == 0) {
        return run_clone_vfork_large_peak_env(argv[0]);
    }
    if (strcmp(argv[1], "vfork-huge-peak-env") == 0) {
        return run_vfork_huge_peak_env(argv[0]);
    }
    if (strcmp(argv[1], "vfork-overflow-peak-env") == 0) {
        return run_vfork_overflow_peak_env(argv[0]);
    }
    if (strcmp(argv[1], "clone-vfork-huge-peak-env") == 0) {
        return run_clone_vfork_huge_peak_env(argv[0]);
    }
    if (strcmp(argv[1], "clone-vfork-overflow-peak-env") == 0) {
        return run_clone_vfork_overflow_peak_env(argv[0]);
    }
    if (strcmp(argv[1], "clone-vfork-exec-failure-trace") == 0) {
        return run_clone_vfork_exec_failure_trace();
    }
    if (strcmp(argv[1], "clone-private-exec-failure-trace") == 0) {
        return run_clone_private_exec_failure_trace();
    }
    if (strcmp(argv[1], "clone-vm-exec-failure-trace") == 0) {
        return run_clone_vm_exec_failure_trace();
    }
    if (strcmp(argv[1], "clone-vfork-execvp-enoexec") == 0) {
        return run_clone_vfork_execvp_enoexec_fallback();
    }
    if (strcmp(argv[1], "clone-vfork-execvp-empty-path") == 0) {
        return run_clone_vfork_execvp_empty_path_component();
    }
    if (strcmp(argv[1], "clone-vfork-duplicate-preload") == 0) {
        return run_clone_vfork_duplicate_preload_env(argv[0]);
    }
    if (strcmp(argv[1], "clone-vfork-then-parent-exec") == 0) {
        return run_clone_vfork_then_parent_exec(argv[0]);
    }
    if (strcmp(argv[1], "posix-spawn-custom-env") == 0) {
        return run_posix_spawn_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "posix-spawn-null-env") == 0) {
        return run_posix_spawn_null_env(argv[0]);
    }
    if (strcmp(argv[1], "posix-spawn-bad-env") == 0) {
        return run_posix_spawn_bad_env(argv[0]);
    }
    if (strcmp(argv[1], "posix-spawn-bad-argv") == 0) {
        return run_posix_spawn_bad_argv(argv[0]);
    }
    if (strcmp(argv[1], "posix-spawnp-bad-env") == 0) {
        return run_posix_spawnp_bad_env();
    }
    if (strcmp(argv[1], "posix-spawnp-bad-argv") == 0) {
        return run_posix_spawnp_bad_argv();
    }
    if (strcmp(argv[1], "posix-spawn-preflight-unavailable") == 0) {
        return run_posix_spawn_preflight_unavailable(argv[0]);
    }
    if (strcmp(argv[1], "posix-spawn-duplicate-preload") == 0) {
        return run_posix_spawn_duplicate_preload(argv[0]);
    }
    if (strcmp(argv[1], "posix-spawn-actions-attrs") == 0) {
        return run_posix_spawn_actions_attrs(argv[0]);
    }
    if (strcmp(argv[1], "posix-spawn-actions-close-stderr") == 0) {
        return run_posix_spawn_actions_close_stderr(argv[0]);
    }
    if (strcmp(argv[1], "posix-spawn-usevfork-custom-env") == 0) {
        return run_posix_spawn_usevfork(argv[0]);
    }
    if (strcmp(argv[1], "posix-spawn-child-peak-env-only") == 0) {
        return run_posix_spawn_child_peak_env_only(argv[0]);
    }
    if (strcmp(argv[1], "posix-spawn-explicit-peak-env") == 0) {
        return run_posix_spawn_explicit_peak_env(argv[0]);
    }
    if (strcmp(argv[1], "posix-spawnp-path-search") == 0) {
        return run_posix_spawnp_path_search();
    }
    if (strcmp(argv[1], "posix-spawnp-child-env-path-ignored") == 0) {
        return run_posix_spawnp_child_env_path_ignored();
    }
    if (strcmp(argv[1], "execvpe-child-env-path-used") == 0) {
        return run_execvpe_child_env_path_used();
    }
    if (strcmp(argv[1], "execvpe-child-path-duplicate-preload") == 0) {
        return run_execvpe_child_path_duplicate_preload();
    }
    if (strcmp(argv[1], "posix-spawn-failure") == 0) {
        return run_posix_spawn_failure(argv[0]);
    }

    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
