#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/sched.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

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
#define LOADER_OBSERVER_ENV "EXEC_CHAIN_TEST_LOADER_OBSERVER"
#define CONTROLLER_QUIESCE_ARG "--quiesce-controller"

static volatile unsigned long peak_exec_chain_sink;
static const char* peak_signal_exec_path;
static const char* peak_signal_exec_command;
static char* const peak_signal_exec_argv[] = {(char*)"missing-exec", NULL};
static char* const peak_signal_exec_envp[] = {NULL};
static unsigned char peak_signal_stack[16384];
static volatile sig_atomic_t peak_signal_exec_seen;
static volatile sig_atomic_t peak_signal_exec_result;
static volatile sig_atomic_t peak_signal_exec_errno;

static int wait_for_child(pid_t pid);

static void
print_default_bypass_case(const char* name, int result, int error_number)
{
    printf("default_bypass_case=%s result=%d errno=%d\n",
           name, result, error_number);
}

static int
run_default_disabled_family_bypass(void)
{
    static const char missing_path[] =
        "/definitely/missing/peak-default-bypass";
    static const char missing_command[] =
        "peak-default-bypass-command-does-not-exist";
    char* const argv[] = {(char*)missing_command, NULL};
    char* const opt_in_env[] = {
        (char*)"PEAK_EXEC_CHAIN=1",
        (char*)"PEAK_EXEC_CHECKPOINT=1",
        NULL,
    };
    pid_t pid = -1;
    int result;

#define RUN_DEFAULT_BYPASS_CASE(name, expression) \
    do { \
        errno = EALREADY; \
        result = (expression); \
        print_default_bypass_case((name), result, errno); \
    } while (0)

    RUN_DEFAULT_BYPASS_CASE("execv", execv(missing_path, argv));
    RUN_DEFAULT_BYPASS_CASE(
        "execvpe", execvpe(missing_command, argv, opt_in_env));
    RUN_DEFAULT_BYPASS_CASE(
        "execl", execl(missing_path, missing_command, (char*)NULL));
    RUN_DEFAULT_BYPASS_CASE(
        "execle",
        execle(missing_path,
               missing_command,
               (char*)NULL,
               opt_in_env));
    RUN_DEFAULT_BYPASS_CASE(
        "fexecve", fexecve(INT_MAX, argv, opt_in_env));
#if defined(PEAK_TEST_HAVE_EXECVEAT)
    RUN_DEFAULT_BYPASS_CASE(
        "execveat",
        execveat(AT_FDCWD, missing_path, argv, opt_in_env, 0));
#endif
    RUN_DEFAULT_BYPASS_CASE(
        "posix_spawnp",
        posix_spawnp(&pid,
                     missing_command,
                     NULL,
                     NULL,
                     argv,
                     opt_in_env));
#if defined(PEAK_TEST_EXEC_RAW_SYSCALL_SUPPORTED)
    RUN_DEFAULT_BYPASS_CASE(
        "raw_execve",
        (int)syscall(SYS_execve, missing_path, argv, opt_in_env));
#if defined(PEAK_TEST_HAVE_EXECVEAT)
    RUN_DEFAULT_BYPASS_CASE(
        "raw_execveat",
        (int)syscall(SYS_execveat,
                     AT_FDCWD,
                     missing_path,
                     argv,
                     opt_in_env,
                     0));
#endif
#endif
#undef RUN_DEFAULT_BYPASS_CASE
    puts("default_disabled_family_bypass=1");
    return 0;
}

typedef struct {
    char* name;
    char* value;
} EnvSnapshotEntry;

typedef struct {
    EnvSnapshotEntry entries[128];
    size_t count;
} EnvSnapshot;

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

static void
signal_handler_execve_missing(int signal_number)
{
    int saved_errno = errno;
    int result;
    int exec_errno;

    (void)signal_number;
    result = execve(peak_signal_exec_path,
                    peak_signal_exec_argv,
                    peak_signal_exec_envp);
    exec_errno = errno;
    if (result == -1 && exec_errno == ENOENT) {
        result = execl(peak_signal_exec_path,
                       peak_signal_exec_path,
                       (char*)NULL);
        exec_errno = errno;
    }
    if (result == -1 && exec_errno == ENOENT) {
        result = execlp(peak_signal_exec_command,
                        peak_signal_exec_command,
                        (char*)NULL);
        exec_errno = errno;
    }
    if (result == -1 && exec_errno == ENOENT) {
        result = execle(peak_signal_exec_path,
                        peak_signal_exec_path,
                        (char*)NULL,
                        peak_signal_exec_envp);
        exec_errno = errno;
    }
    peak_signal_exec_result = (sig_atomic_t)result;
    peak_signal_exec_errno = (sig_atomic_t)exec_errno;
    peak_signal_exec_seen = 1;
    errno = saved_errno;
}

static int
run_signal_handler_execve_default_bypass(void)
{
    struct sigaction action;
    struct sigaction old_action;
    sigset_t blocked;
    sigset_t old_mask;
    sigset_t wait_mask;
    int signal_error;
    int result = 0;
    stack_t signal_stack;
    stack_t old_signal_stack;

    peak_signal_exec_path = "/definitely/missing/peak-signal-exec";
    peak_signal_exec_command = "peak-signal-exec-command-does-not-exist";
    peak_signal_exec_seen = 0;
    peak_signal_exec_result = 0;
    peak_signal_exec_errno = 0;
    signal_stack.ss_sp = peak_signal_stack;
    signal_stack.ss_size = sizeof(peak_signal_stack);
    signal_stack.ss_flags = 0;
    if (syscall(SYS_sigaltstack, &signal_stack, &old_signal_stack) != 0) {
        perror("sigaltstack");
        return 219;
    }
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &blocked, &old_mask) != 0) {
        perror("sigprocmask-block");
        (void)syscall(SYS_sigaltstack, &old_signal_stack, NULL);
        return 220;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler_execve_missing;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART | SA_ONSTACK;
    if (sigaction(SIGUSR1, &action, &old_action) != 0) {
        perror("sigaction");
        (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
        (void)syscall(SYS_sigaltstack, &old_signal_stack, NULL);
        return 221;
    }
    signal_error = pthread_kill(pthread_self(), SIGUSR1);
    if (signal_error != 0) {
        errno = signal_error;
        perror("pthread-kill-self");
        result = 222;
        goto out;
    }
    wait_mask = old_mask;
    sigdelset(&wait_mask, SIGUSR1);
    while (!peak_signal_exec_seen) {
        (void)sigsuspend(&wait_mask);
    }
    if (peak_signal_exec_result != -1 ||
        peak_signal_exec_errno != ENOENT) {
        fprintf(stderr,
                "signal_handler_execve_result=%d errno=%d\n",
                (int)peak_signal_exec_result,
                (int)peak_signal_exec_errno);
        result = 223;
        goto out;
    }
    printf("signal_handler_execve_default_bypass=1\n");

out:
    if (sigaction(SIGUSR1, &old_action, NULL) != 0 && result == 0) {
        perror("sigaction-restore");
        result = 224;
    }
    if (sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0 && result == 0) {
        perror("sigprocmask-restore");
        result = 225;
    }
    if (syscall(SYS_sigaltstack, &old_signal_stack, NULL) != 0 && result == 0) {
        perror("sigaltstack-restore");
        result = 226;
    }
    return result;
}

static void
quiesce_controller_for_required_checkpoint(void)
{
    void (*controller_stop)(void);

    controller_stop = (void (*)(void))dlsym(
        RTLD_DEFAULT, "peak_general_listener_controller_stop");
    if (controller_stop == NULL) {
        fprintf(stderr, "general controller stop API unavailable\n");
        exit(101);
    }
    controller_stop();
}

static const char*
env_or_default(const char* name, const char* fallback)
{
    const char* value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : fallback;
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
make_minimal_child_env(const char* marker)
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
    envp[index++] = make_env_entry("EXEC_CHAIN_TEST_MARKER", marker);
    envp[index] = NULL;
    return envp;
}

static char**
make_minimal_child_env_without_loader(const char* marker)
{
    char** envp = calloc(6, sizeof(char*));

    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    envp[0] = make_env_entry("PATH", env_or_default("PATH", "/bin:/usr/bin"));
    envp[1] = make_env_entry("EXEC_CHAIN_TEST_MARKER", marker);
    return envp;
}

static char**
make_loader_path_child_env(const char* marker,
                           const char* value,
                           const char* second_value)
{
    char** envp = make_minimal_child_env_without_loader(marker);

    if (value != NULL) {
        envp[2] = make_env_entry("LD_LIBRARY_PATH", value);
    }
    if (second_value != NULL) {
        envp[3] = make_env_entry("LD_LIBRARY_PATH", second_value);
    }
    return envp;
}

static char**
make_preloaded_child_env(const char* marker)
{
    const char* preload = env_or_default("LD_PRELOAD", "");
    size_t value_size = strlen(EXTRA_PRELOAD_TOKEN_A) + 1 + strlen(preload) + 1;
    char* value = malloc(value_size);
    char** envp = make_minimal_child_env_without_loader(marker);

    if (value == NULL) {
        perror("malloc");
        exit(100);
    }
    snprintf(value, value_size, "%s:%s", EXTRA_PRELOAD_TOKEN_A, preload);
    envp[2] = make_env_entry("LD_PRELOAD", value);
    free(value);
    return envp;
}

static char**
make_child_control_env(const char* marker)
{
    char** envp = make_minimal_child_env_without_loader(marker);

    envp[2] = make_env_entry("PEAK_EXEC_CHAIN", "1");
    envp[3] = make_env_entry("PEAK_EXEC_PROPAGATE_PEAK_ENV", "1");
    return envp;
}

static char**
make_call_specific_optin_env(const char* marker)
{
    char** envp = make_minimal_child_env_without_loader(marker);

    envp[2] = make_env_entry("PEAK_EXEC_CHAIN", "1");
    envp[3] = make_env_entry("PEAK_EXEC_CHECKPOINT", "1");
    return envp;
}

static char**
make_unterminated_nonpeak_env(void)
{
    const size_t entry_count = 256;
    char** envp = calloc(entry_count + 1, sizeof(char*));
    char name[64];

    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    for (size_t index = 0; index < entry_count; index++) {
        snprintf(name, sizeof(name), "PARENT_PAD_%04zu", index);
        envp[index] = make_env_entry(name, "x");
    }
    return envp;
}

static char**
make_preload_entries_without_terminator(void)
{
    const size_t entry_count = 256;
    char** envp = calloc(entry_count + 1, sizeof(char*));

    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    for (size_t index = 0; index < entry_count; index++) {
        envp[index] = make_env_entry("LD_PRELOAD", "");
    }
    return envp;
}

static char**
make_bad_path_child_env(void)
{
    char** envp = calloc(5, sizeof(char*));
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
make_large_child_env(void)
{
    const size_t pad_count = 512;
    char** envp = calloc(pad_count + 5, sizeof(char*));
    char name[64];
    size_t index = 0;

    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    envp[index++] = make_env_entry("PATH",
                                   env_or_default("PATH", "/bin:/usr/bin"));
    envp[index++] = make_env_entry("EXEC_CHAIN_TEST_MARKER", "large-env");
    for (size_t i = 0; i < pad_count; i++) {
        snprintf(name, sizeof(name), "CHILD_PAD_%04zu", i);
        envp[index++] = make_env_entry(name, "x");
    }
    envp[index] = NULL;
    return envp;
}

static char**
make_long_preload_child_env(void)
{
    const size_t value_len = PATH_MAX * 2U;
    char* value = malloc(value_len + 1);
    char** envp = calloc(5, sizeof(char*));

    if (value == NULL || envp == NULL) {
        perror("malloc");
        exit(100);
    }
    memset(value, 'x', value_len);
    value[value_len] = '\0';
    envp[0] = make_env_entry("PATH", env_or_default("PATH", "/bin:/usr/bin"));
    envp[1] = make_env_entry("EXEC_CHAIN_TEST_MARKER", "postfork-long-preload");
    envp[2] = make_env_entry("LD_PRELOAD", value);
    free(value);
    return envp;
}

static char*
make_duplicate_preload_value(void)
{
    const char* preload = env_or_default("LD_PRELOAD", "");
    size_t preload_len = strlen(preload);
    size_t extra_a_len = strlen(EXTRA_PRELOAD_TOKEN_A);
    size_t extra_b_len = strlen(EXTRA_PRELOAD_TOKEN_B);
    char* value = malloc(preload_len * 2 + extra_a_len + extra_b_len + 4);

    if (value == NULL) {
        perror("malloc");
        exit(100);
    }
    sprintf(value,
            "%s:%s:%s:%s",
            EXTRA_PRELOAD_TOKEN_A,
            preload,
            EXTRA_PRELOAD_TOKEN_B,
            preload);
    return value;
}

static char*
make_whitespace_duplicate_preload_value(void)
{
    const char* preload = env_or_default("LD_PRELOAD", "");
    size_t preload_len = strlen(preload);
    size_t extra_a_len = strlen(EXTRA_PRELOAD_TOKEN_A);
    size_t extra_b_len = strlen(EXTRA_PRELOAD_TOKEN_B);
    char* value = malloc(preload_len * 2 + extra_a_len + extra_b_len + 5);

    if (value == NULL) {
        perror("malloc");
        exit(100);
    }
    sprintf(value,
            "%s %s\t%s\n%s",
            EXTRA_PRELOAD_TOKEN_A,
            preload,
            EXTRA_PRELOAD_TOKEN_B,
            preload);
    return value;
}

static char**
make_duplicate_preload_env_with(char* duplicate_preload,
                                const char* marker)
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
    envp[index++] = make_env_entry("PEAK_TARGET",
                                   env_or_default("PEAK_TARGET",
                                                  "peak_exec_chain_hot_target"));
    envp[index++] = make_env_entry("PEAK_STATSLOG_PATH",
                                   env_or_default("PEAK_STATSLOG_PATH",
                                                  "./peak_statslog"));
    envp[index++] = make_env_entry("PEAK_HEARTBEAT_INTERVAL", "0");
    envp[index++] = make_env_entry("EXEC_CHAIN_TEST_MARKER", marker);
    envp[index] = NULL;
    free(duplicate_preload);
    return envp;
}

static char**
make_duplicate_preload_env(void)
{
    return make_duplicate_preload_env_with(make_duplicate_preload_value(),
                                           "duplicate-preload");
}

static char**
make_whitespace_duplicate_preload_env(void)
{
    return make_duplicate_preload_env_with(
        make_whitespace_duplicate_preload_value(),
        "duplicate-preload-whitespace");
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
    envp[index++] = make_env_entry("PEAK_TARGET",
                                   env_or_default("PEAK_TARGET",
                                                  "peak_exec_chain_hot_target"));
    envp[index++] = make_env_entry("PEAK_STATSLOG_PATH",
                                   env_or_default("PEAK_STATSLOG_PATH",
                                                  "./peak_statslog"));
    envp[index++] = make_env_entry("PEAK_HEARTBEAT_INTERVAL", "0");
    envp[index++] = make_env_entry("EXEC_CHAIN_TEST_MARKER",
                                   "duplicate-preload-entry");
    envp[index] = NULL;
    return envp;
}

static char**
make_child_path_env(void)
{
    char** envp = calloc(5, sizeof(char*));
    size_t index = 0;

    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    envp[index++] = make_env_entry(
        "PATH",
        env_or_default("EXEC_CHAIN_TEST_CHILD_PATH", "/bin:/usr/bin"));
    append_current_env_entry_if_present(envp, &index, "LD_LIBRARY_PATH");
    envp[index++] = make_env_entry("EXEC_CHAIN_TEST_MARKER",
                                   "child-path-env");
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
    envp[index++] = make_env_entry("EXEC_CHAIN_TEST_MARKER",
                                   "child-peak-env");
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
make_explicit_peak_env(void)
{
    char** envp = calloc(8, sizeof(char*));
    size_t index = 0;

    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    envp[index++] = make_env_entry("PATH",
                                   env_or_default("PATH", "/bin:/usr/bin"));
    append_current_env_entry_if_present(envp, &index, "LD_LIBRARY_PATH");
    envp[index++] = make_env_entry("PEAK_TARGET", "explicit_child_target");
    envp[index++] = make_env_entry("PEAK_STATSLOG_PATH",
                                   env_or_default("PEAK_STATSLOG_PATH",
                                                  "./peak_statslog"));
    envp[index++] = make_env_entry("PEAK_HEARTBEAT_INTERVAL", "0");
    envp[index++] = make_env_entry("EXEC_CHAIN_TEST_MARKER",
                                   "explicit-peak-env");
    envp[index] = NULL;
    return envp;
}

static char**
make_child_peak_control_env(const char* name, const char* value)
{
    char** envp = calloc(10, sizeof(char*));
    size_t index = 0;

    if (envp == NULL) {
        perror("calloc");
        exit(100);
    }
    envp[index++] = make_env_entry("PATH",
                                   env_or_default("PATH", "/bin:/usr/bin"));
    append_current_env_entry_if_present(envp, &index, "LD_LIBRARY_PATH");
    envp[index++] = make_env_entry("EXEC_CHAIN_TEST_MARKER", name);
    envp[index++] = make_env_entry(name, value);
    envp[index] = NULL;
    return envp;
}

static char* const*
invalid_envp_for_test(void)
{
    return (char* const*)(uintptr_t)1;
}

static char* const*
invalid_argv_for_test(void)
{
    static char* const argv[] = {(char*)(uintptr_t)1, NULL};

    return argv;
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
wait_for_child(pid_t pid)
{
    int status;

    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        perror("waitpid");
        return 125;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 126;
}

static int
count_libpeak_preload_entries(void)
{
    const char* preload = getenv("LD_PRELOAD");
    int count = 0;

    if (preload == NULL) {
        return 0;
    }
    while (*preload != '\0') {
        const char* start;
        size_t len;

        while (*preload == ':' || *preload == ' ' ||
               *preload == '\t' || *preload == '\n') {
            preload++;
        }
        start = preload;
        while (*preload != '\0' && *preload != ':' &&
               *preload != ' ' && *preload != '\t' &&
               *preload != '\n') {
            preload++;
        }
        len = (size_t)(preload - start);
        if (len != 0 && memmem(start, len, "libpeak", strlen("libpeak")) != NULL) {
            count++;
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
        const char* start;
        size_t len;

        while (*preload == ':' || *preload == ' ' ||
               *preload == '\t' || *preload == '\n') {
            preload++;
        }
        start = preload;
        while (*preload != '\0' && *preload != ':' &&
               *preload != ' ' && *preload != '\t' &&
               *preload != '\n') {
            preload++;
        }
        len = (size_t)(preload - start);
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

static int
count_ld_library_path_env_entries(void)
{
    int count = 0;

    if (environ == NULL) {
        return 0;
    }
    for (char** scan = environ; *scan != NULL; scan++) {
        if (strncmp(*scan, "LD_LIBRARY_PATH=", strlen("LD_LIBRARY_PATH=")) == 0) {
            count++;
        }
    }
    return count;
}

static const char*
ld_library_path_env_value(size_t wanted)
{
    size_t found = 0;

    if (environ == NULL) {
        return NULL;
    }
    for (char** scan = environ; *scan != NULL; scan++) {
        if (strncmp(*scan, "LD_LIBRARY_PATH=", strlen("LD_LIBRARY_PATH=")) == 0 &&
            found++ == wanted) {
            return *scan + strlen("LD_LIBRARY_PATH=");
        }
    }
    return NULL;
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
    const char* marker = getenv("EXEC_CHAIN_TEST_MARKER");
    const char* peak_target = getenv("PEAK_TARGET");
    const char* peak_stats = getenv("PEAK_STATSLOG_PATH");
    const char* peak_exec_chain = getenv("PEAK_EXEC_CHAIN");
    const char* peak_exec_checkpoint = getenv("PEAK_EXEC_CHECKPOINT");
    const char* peak_exec_propagate = getenv("PEAK_EXEC_PROPAGATE_PEAK_ENV");
    const char* path = getenv("PATH");
    const char* loader_observer = getenv(LOADER_OBSERVER_ENV);
    const char* secure_test_hook = getenv("PEAK_TEST_EXEC_AT_SECURE");
    const char* loader_path_0 = ld_library_path_env_value(0);
    const char* loader_path_1 = ld_library_path_env_value(1);

    call_hot_target(7);
    printf("ld_preload_libpeak_count=%d ld_preload_env_entries=%d "
           "ld_preload_extra_count=%d peak_target=%s peak_statslog=%s "
           "marker=%s peak_exec_chain=%s peak_exec_checkpoint=%s "
           "peak_exec_propagate=%s ld_library_path_env_entries=%d "
           "ld_library_path_0=%s ld_library_path_1=%s path=%s "
           "loader_observer=%s secure_test_hook=%s\n",
           count_libpeak_preload_entries(),
           count_ld_preload_env_entries(),
           count_extra_preload_entries(),
           peak_target != NULL ? peak_target : "<missing>",
           peak_stats != NULL ? peak_stats : "<missing>",
           marker != NULL ? marker : "<missing>",
           peak_exec_chain != NULL ? peak_exec_chain : "<missing>",
           peak_exec_checkpoint != NULL ? peak_exec_checkpoint : "<missing>",
           peak_exec_propagate != NULL ? peak_exec_propagate : "<missing>",
           count_ld_library_path_env_entries(),
           loader_path_0 != NULL ? loader_path_0 : "<missing>",
           loader_path_1 != NULL ? loader_path_1 : "<missing>",
           path != NULL ? path : "<missing>",
           loader_observer != NULL ? loader_observer : "<missing>",
           secure_test_hook != NULL ? secure_test_hook : "<missing>");
    fflush(stdout);
    return 0;
}

static int
run_child_check_env(const char* name)
{
    const char* value = getenv(name);

    call_hot_target(7);
    printf("child_env_%s=%s\n", name, value != NULL ? value : "<missing>");
    fflush(stdout);
    return value != NULL ? 0 : 3;
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
run_execv_success(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    execv(self, argv);
    perror("execv-success");
    return 111;
}

static int
run_execv_zero_call_checkpoint(const char* self)
{
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    execv(self, argv);
    perror("execv-zero-call");
    return 112;
}

static int
run_execv_write_target_success(const char* self)
{
    static const char marker[] = "parent_write_before_exec\n";
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};

    if (write(STDOUT_FILENO, marker, sizeof(marker) - 1) < 0) {
        perror("write-before-exec");
        return 142;
    }
    execv(self, argv);
    perror("execv-write-target");
    return 143;
}

static int
run_execve_custom_env(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_minimal_child_env("custom-env");
    execve(self, argv, envp);
    perror("execve-custom-env");
    return 113;
}

static int
run_execve_call_env_optin_default_bypass(const char* self)
{
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_call_specific_optin_env("call-env-optin-execve");

    execve(self, argv, envp);
    perror("execve-call-env-optin-default-bypass");
    return 226;
}

static int
run_raw_syscall_execve_custom_env(const char* self)
{
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_minimal_child_env("raw-syscall-execve-env");

    call_hot_target(5);
    syscall(SYS_execve, self, argv, envp);
    perror("raw-syscall-execve-custom-env");
    return 215;
}

static int
run_raw_syscall_execve_failure(void)
{
    char* const argv[] = {(char*)"/definitely/not/found", NULL};
    int saved_errno;

    call_hot_target(2);
    errno = 0;
    if (syscall(SYS_execve, "/definitely/not/found", argv, environ) != -1) {
        return 216;
    }
    saved_errno = errno;
    call_hot_target(4);
    printf("raw_syscall_execve_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return saved_errno == ENOENT ? 0 : 217;
}

static int
run_raw_syscall_execve_observer_bypass(void)
{
    static const char missing_path[] =
        "/definitely/missing/peak-raw-observer-exec";
    char* const argv[] = {(char*)missing_path, NULL};
    typedef int (*observer_count_fn)(void);
    observer_count_fn observer_count;
    void* observer_symbol;
    long result;
    int saved_errno;
    int observed_calls;

    errno = EALREADY;
    result = syscall(SYS_execve, missing_path, argv, environ);
    saved_errno = errno;
    observer_symbol = dlsym(RTLD_DEFAULT,
                            "peak_test_execve_observer_count");
    if (observer_symbol == NULL) {
        fputs("missing execve observer\n", stderr);
        return 230;
    }
    memcpy(&observer_count, &observer_symbol, sizeof(observer_count));
    observed_calls = observer_count();
    printf("raw_execve_observer_bypass=%d result=%ld errno=%d "
           "observed_execve_calls=%d\n",
           result == -1 && saved_errno == ENOENT && observed_calls == 0,
           result,
           saved_errno,
           observed_calls);
    return result == -1 && saved_errno == ENOENT && observed_calls == 0 ?
               0 :
               231;
}

static int
run_raw_syscall_nonexec_passthrough(void)
{
    long pid;
    long close_rc;
    const char marker[] = "raw_syscall_write=ok\n";
    long write_rc;

    errno = EALREADY;
    pid = syscall(SYS_getpid);
    errno = 0;
    close_rc = syscall(SYS_close, -1);
    if (close_rc != -1 || errno != EBADF) {
        return 218;
    }
    errno = EALREADY;
    write_rc = syscall(SYS_write, STDOUT_FILENO, marker, sizeof(marker) - 1);
    printf("raw_syscall_getpid=%ld expected=%ld errno=%d\n",
           pid,
           (long)getpid(),
           errno);
    fflush(stdout);
    return pid == (long)getpid() && write_rc == (long)(sizeof(marker) - 1) ?
               0 :
               219;
}

static int
run_execve_child_peak_env_only(const char* self)
{
    EnvSnapshot parent_env;

    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_child_peak_only_env();
    snapshot_and_unset_peak_env(&parent_env);
    execve(self, argv, envp);
    perror("execve-child-peak-env-only");
    return 145;
}

static int
run_execve_null_env(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    execve(self, argv, NULL);
    perror("execve-null-env");
    return 114;
}

static int
run_execve_loader_path_env(const char* self,
                           const char* marker,
                           const char* value,
                           const char* second_value,
                           int disable_chain)
{
    const char* observer = getenv(LOADER_OBSERVER_ENV);
    const char* child = observer != NULL && observer[0] != '\0' ? observer : self;
    char* argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_loader_path_child_env(marker, value, second_value);
    size_t index = 0;

    while (envp[index] != NULL) {
        index++;
    }
    argv[0] = (char*)child;
    if (disable_chain) {
        envp[index++] = make_env_entry("PEAK_EXEC_CHAIN", "0");
        envp[index] = NULL;
    }
    call_hot_target(5);
    execve(child, argv, envp);
    perror("execve-loader-path-env");
    return 114;
}

static int
run_execve_preloaded_env(const char* self)
{
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_preloaded_child_env("loader-path-preload-present");

    call_hot_target(5);
    execve(self, argv, envp);
    perror("execve-preloaded-env");
    return 114;
}

static int
run_execve_large_env(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_large_child_env();
    execve(self, argv, envp);
    perror("execve-large-env");
    return 115;
}

static int
run_execve_bad_env_failure(void)
{
    char* const argv[] = {(char*)"/bin/true", NULL};
    int result;
    int saved_errno;

    call_hot_target(3);
    errno = 0;
    result = execve("/bin/true", argv, invalid_envp_for_test());
    saved_errno = errno;
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "execve_bad_env_bad_result result=%d errno=%d\n",
                result,
                saved_errno);
        return 146;
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
    int result;
    int saved_errno;

    call_hot_target(3);
    errno = 0;
    result = execve("/bin/true", invalid_argv_for_test(), environ);
    saved_errno = errno;
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "execve_bad_argv_bad_result result=%d errno=%d\n",
                result,
                saved_errno);
        return 147;
    }
    call_hot_target(3);
    printf("execve_bad_argv_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_execve_explicit_peak_env(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {
        (char*)self,
        (char*)"child-check-env",
        (char*)"PEAK_TARGET",
        NULL
    };
    char** envp = make_explicit_peak_env();
    execve(self, argv, envp);
    perror("execve-explicit-peak-env");
    return 116;
}

static int
run_execve_child_peak_control(const char* self,
                              const char* name,
                              const char* value)
{
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_child_peak_control_env(name, value);

    call_hot_target(5);
    execve(self, argv, envp);
    perror("execve-child-peak-control");
    return 173;
}

static int
run_exec_failure(void)
{
    char* const argv[] = {(char*)"/definitely/not/found", NULL};
    int saved_errno;

    call_hot_target(2);
    errno = 0;
    execv("/definitely/not/found", argv);
    saved_errno = errno;
    call_hot_target(4);
    printf("exec_failure_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return saved_errno == ENOENT ? 0 : 117;
}

static int
run_fexecve_custom_env(const char* self)
{
    int fd = open(self, O_RDONLY);
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_minimal_child_env("fexecve-env");

    if (fd < 0) {
        perror("open-self");
        return 118;
    }
    call_hot_target(5);
    fexecve(fd, argv, envp);
    perror("fexecve-custom-env");
    close(fd);
    return 119;
}

static int
run_fexecve_bad_env_failure(const char* self)
{
    int fd = open(self, O_RDONLY);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    int result;
    int saved_errno;

    if (fd < 0) {
        perror("open-fexecve-bad-env");
        return 148;
    }
    call_hot_target(2);
    errno = 0;
    result = fexecve(fd, argv, invalid_envp_for_test());
    saved_errno = errno;
    close(fd);
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "fexecve_bad_env_result=%d errno=%d\n",
                result,
                saved_errno);
        return 149;
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
    int fd = open(self, O_RDONLY);
    int result;
    int saved_errno;

    if (fd < 0) {
        perror("open-fexecve-bad-argv");
        return 150;
    }
    call_hot_target(2);
    errno = 0;
    result = fexecve(fd, invalid_argv_for_test(), environ);
    saved_errno = errno;
    close(fd);
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "fexecve_bad_argv_result=%d errno=%d\n",
                result,
                saved_errno);
        return 151;
    }
    call_hot_target(4);
    printf("fexecve_bad_argv_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

#if defined(PEAK_TEST_HAVE_EXECVEAT)
static int
run_execveat_custom_env(const char* self)
{
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_minimal_child_env("execveat-env");

    call_hot_target(5);
    execveat(AT_FDCWD, self, argv, envp, 0);
    perror("execveat-custom-env");
    return 120;
}

static int
run_raw_syscall_execveat_custom_env(const char* self)
{
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_minimal_child_env("raw-syscall-execveat-env");

    call_hot_target(5);
    syscall(SYS_execveat, AT_FDCWD, self, argv, envp, 0);
    perror("raw-syscall-execveat-custom-env");
    return 219;
}

static int
run_execveat_bad_env_failure(void)
{
    char* const argv[] = {(char*)"/bin/true", NULL};
    int result;
    int saved_errno;

    call_hot_target(3);
    errno = 0;
    result = execveat(AT_FDCWD,
                      "/bin/true",
                      argv,
                      invalid_envp_for_test(),
                      0);
    saved_errno = errno;
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "execveat_bad_env_result=%d errno=%d\n",
                result,
                saved_errno);
        return 152;
    }
    call_hot_target(3);
    printf("execveat_bad_env_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_execveat_bad_argv_failure(void)
{
    int result;
    int saved_errno;

    call_hot_target(3);
    errno = 0;
    result = execveat(AT_FDCWD,
                      "/bin/true",
                      invalid_argv_for_test(),
                      environ,
                      0);
    saved_errno = errno;
    if (result != -1 || saved_errno != EFAULT) {
        fprintf(stderr,
                "execveat_bad_argv_result=%d errno=%d\n",
                result,
                saved_errno);
        return 153;
    }
    call_hot_target(3);
    printf("execveat_bad_argv_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}
#endif

static int
run_execl_success(const char* self)
{
    call_hot_target(5);
    execl(self, self, "child-basic", NULL);
    perror("execl-success");
    return 121;
}

static int
run_execlp_path_search(void)
{
    call_hot_target(5);
    execlp("test_exec_chain", "test_exec_chain", "child-basic", NULL);
    perror("execlp-path-search");
    return 122;
}

static int
run_execle_custom_env(const char* self)
{
    char** envp = make_minimal_child_env("execle-env");

    call_hot_target(5);
    execle(self, self, "child-print-ld", NULL, envp);
    perror("execle-custom-env");
    return 123;
}

static int
run_execvp_path_search(void)
{
    call_hot_target(5);
    char* const argv[] = {
        (char*)"test_exec_chain",
        (char*)"child-basic",
        NULL
    };
    execvp("test_exec_chain", argv);
    perror("execvp-path-search");
    return 124;
}

static int
run_helper_named_exec(void)
{
    char* const argv[] = {
        (char*)"peak_detach_helper",
        (char*)"child-basic",
        NULL
    };

    call_hot_target(5);
    execvp("peak_detach_helper", argv);
    perror("helper-named-exec");
    return 144;
}

static int
run_execvpe_child_env_path_ignored(void)
{
    call_hot_target(5);
    char* const argv[] = {
        (char*)"test_exec_chain",
        (char*)"child-basic",
        NULL
    };
    char** envp = make_bad_path_child_env();
    execvpe("test_exec_chain", argv, envp);
    perror("execvpe-child-env-path-ignored");
    return 125;
}

static int
run_execvpe_child_env_path_used(void)
{
    call_hot_target(5);
    char* const argv[] = {
        (char*)"test_exec_chain",
        (char*)"child-basic",
        NULL
    };
    char** envp = make_child_path_env();
    execvpe("test_exec_chain", argv, envp);
    perror("execvpe-child-env-path-used");
    return 154;
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
    return 155;
}

static int
run_execvp_enoent(void)
{
    char* const argv[] = {(char*)"definitely_missing_peak_exec_chain", NULL};
    int saved_errno;

    call_hot_target(2);
    errno = 0;
    execvp("definitely_missing_peak_exec_chain", argv);
    saved_errno = errno;
    call_hot_target(4);
    printf("execvp_enoent_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return saved_errno == ENOENT ? 0 : 126;
}

static int
run_execvp_eacces(void)
{
    char* const argv[] = {(char*)"blocked-exec", NULL};
    int saved_errno;

    call_hot_target(2);
    errno = 0;
    execvp("blocked-exec", argv);
    saved_errno = errno;
    call_hot_target(4);
    printf("execvp_eacces_errno=%d continued sink=%lu\n",
           saved_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return saved_errno == EACCES ? 0 : 127;
}

static int
run_execvp_enoexec(void)
{
    char* const argv[] = {(char*)"enoexec-script", NULL};

    call_hot_target(5);
    execvp("enoexec-script", argv);
    perror("execvp-enoexec");
    return 128;
}

static int
run_duplicate_preload(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_duplicate_preload_env();
    execve(self, argv, envp);
    perror("duplicate-preload");
    return 129;
}

static int
run_whitespace_duplicate_preload(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_whitespace_duplicate_preload_env();
    execve(self, argv, envp);
    perror("duplicate-preload-whitespace");
    return 156;
}

static int
run_duplicate_preload_entry(const char* self)
{
    call_hot_target(5);
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_duplicate_preload_entry_env();
    execve(self, argv, envp);
    perror("duplicate-preload-entry");
    return 157;
}

static int
run_posix_spawn_custom_env(const char* self)
{
    pid_t pid = -1;
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_minimal_child_env("posix-spawn-env");
    int result;

    call_hot_target(5);
    errno = EDOM;
    result = posix_spawn(&pid, self, NULL, NULL, argv, envp);
    if (getenv("PEAK_TARGET") != NULL && errno != EDOM) {
        fprintf(stderr, "posix_spawn_custom_env_errno=%d\n", errno);
        return 129;
    }
    if (result != 0) {
        fprintf(stderr, "posix_spawn_custom_env_failed=%d\n", result);
        return 130;
    }
    return wait_for_child(pid);
}

static int
run_posix_spawn_call_env_optin_default_bypass(const char* self)
{
    pid_t pid = -1;
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_call_specific_optin_env("call-env-optin-spawn");
    int result;
    int child_status;

    errno = EDOM;
    result = posix_spawn(&pid, self, NULL, NULL, argv, envp);
    if (result != 0) {
        fprintf(stderr,
                "posix_spawn_call_env_optin_default_bypass_failed=%d\n",
                result);
        return 227;
    }
    if (errno != EDOM) {
        fprintf(stderr,
                "posix_spawn_call_env_optin_default_bypass_errno=%d\n",
                errno);
        return 228;
    }
    child_status = wait_for_child(pid);
    if (child_status != 0) {
        fprintf(stderr,
                "posix_spawn_call_env_optin_default_bypass_status=%d\n",
                child_status);
        return 229;
    }
    printf("posix_spawn_call_env_optin_default_bypass=1\n");
    return 0;
}

static int
run_posix_spawn_null_env(const char* self)
{
    pid_t pid = -1;
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    int result;

    call_hot_target(5);
    result = posix_spawn(&pid, self, NULL, NULL, argv, NULL);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_null_env_failed=%d\n", result);
        return 131;
    }
    return wait_for_child(pid);
}

static int
finish_spawn_observation(const char* operation,
                         int result,
                         int saved_errno,
                         pid_t pid)
{
    int status = -1;
    int wait_errno = 0;
    int child_created = result == 0;
    int invalid_success = 0;
    int child_exit = -1;
    int child_signal = 0;

    if (child_created && pid <= 0) {
        invalid_success = 1;
    } else if (child_created) {
        while (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR) {
                continue;
            }
            wait_errno = errno;
            status = -1;
            break;
        }
        if (status >= 0) {
            if (WIFEXITED(status)) {
                child_exit = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                child_signal = WTERMSIG(status);
            }
        }
    }

    call_hot_target(4);
    printf("spawn_observation operation=%s result=%d errno=%d "
           "pid_created=%d invalid_success=%d wait_status=%d "
           "child_exit=%d child_signal=%d "
           "wait_errno=%d continued_sink=%lu\n",
           operation,
           result,
           saved_errno,
           child_created,
           invalid_success,
           status,
           child_exit,
           child_signal,
           wait_errno,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_posix_spawn_bad_env(const char* self)
{
    pid_t pid = -1;
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    int result;

    call_hot_target(2);
    errno = EDOM;
    result = posix_spawn(&pid,
                         self,
                         NULL,
                         NULL,
                         argv,
                         invalid_envp_for_test());
    return finish_spawn_observation("posix_spawn_bad_env", result, errno, pid);
}

static int
run_posix_spawn_bad_argv(const char* self)
{
    pid_t pid = -1;
    int result;

    call_hot_target(2);
    errno = EDOM;
    result = posix_spawn(&pid,
                         self,
                         NULL,
                         NULL,
                         invalid_argv_for_test(),
                         environ);
    return finish_spawn_observation("posix_spawn_bad_argv", result, errno, pid);
}

static int
run_posix_spawn_preflight_unavailable(const char* self)
{
    pid_t pid = -1;
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_minimal_child_env("posix-spawn-preflight");
    int result;
    int wait_result;

    call_hot_target(5);
    result = posix_spawn(&pid, self, NULL, NULL, argv, envp);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_preflight_unavailable_failed=%d\n",
                result);
        return 160;
    }
    wait_result = wait_for_child(pid);
    if (wait_result != 0) {
        return wait_result;
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
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_duplicate_preload_entry_env();
    int result;

    call_hot_target(5);
    result = posix_spawn(&pid, self, NULL, NULL, argv, envp);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_duplicate_preload_failed=%d\n", result);
        return 132;
    }
    return wait_for_child(pid);
}

static int
run_posix_spawn_actions_attrs(const char* self)
{
    const char* output_path = getenv("EXEC_CHAIN_SPAWN_ACTIONS_OUT");
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attrs;
    sigset_t empty_mask;
    pid_t pid = -1;
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_minimal_child_env("posix-spawn-actions");
    int result;
    int wait_result;

    if (output_path == NULL || output_path[0] == '\0') {
        fprintf(stderr, "missing EXEC_CHAIN_SPAWN_ACTIONS_OUT\n");
        return 133;
    }
    result = posix_spawn_file_actions_init(&actions);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_file_actions_init_failed=%d\n", result);
        return 134;
    }
    result = posix_spawn_file_actions_addopen(&actions,
                                              STDOUT_FILENO,
                                              output_path,
                                              O_CREAT | O_TRUNC | O_WRONLY,
                                              0600);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_file_actions_addopen_failed=%d\n", result);
        posix_spawn_file_actions_destroy(&actions);
        return 135;
    }
    result = posix_spawnattr_init(&attrs);
    if (result != 0) {
        fprintf(stderr, "posix_spawnattr_init_failed=%d\n", result);
        posix_spawn_file_actions_destroy(&actions);
        return 136;
    }
    sigemptyset(&empty_mask);
    posix_spawnattr_setsigmask(&attrs, &empty_mask);
    posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETSIGMASK);

    call_hot_target(5);
    result = posix_spawn(&pid, self, &actions, &attrs, argv, envp);
    posix_spawnattr_destroy(&attrs);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_actions_attrs_failed=%d\n", result);
        return 137;
    }
    wait_result = wait_for_child(pid);
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
    char* const argv[] = {(char*)self, (char*)"child-stderr-sentinel", NULL};
    char** envp = make_minimal_child_env("posix-spawn-close-stderr");
    int result;
    int wait_result;

    if (output_path == NULL || output_path[0] == '\0') {
        fprintf(stderr, "missing EXEC_CHAIN_SPAWN_ACTIONS_OUT\n");
        return 161;
    }
    result = posix_spawn_file_actions_init(&actions);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_close_stderr_actions_init_failed=%d\n",
                result);
        return 162;
    }
    result = posix_spawn_file_actions_addopen(&actions,
                                              STDOUT_FILENO,
                                              output_path,
                                              O_CREAT | O_TRUNC | O_WRONLY,
                                              0600);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_close_stderr_addopen_failed=%d\n",
                result);
        posix_spawn_file_actions_destroy(&actions);
        return 163;
    }
    result = posix_spawn_file_actions_addclose(&actions, STDERR_FILENO);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_close_stderr_addclose_failed=%d\n",
                result);
        posix_spawn_file_actions_destroy(&actions);
        return 164;
    }

    call_hot_target(5);
    result = posix_spawn(&pid, self, &actions, NULL, argv, envp);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_actions_close_stderr_failed=%d\n",
                result);
        return 165;
    }
    wait_result = wait_for_child(pid);
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
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_minimal_child_env("posix-spawn-usevfork");
    int result;
    int wait_result;

    result = posix_spawnattr_init(&attrs);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_usevfork_attr_init_failed=%d\n", result);
        return 166;
    }
    result = posix_spawnattr_setflags(&attrs, POSIX_SPAWN_USEVFORK);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_usevfork_setflags_failed=%d\n", result);
        posix_spawnattr_destroy(&attrs);
        return 167;
    }

    call_hot_target(5);
    result = posix_spawn(&pid, self, NULL, &attrs, argv, envp);
    posix_spawnattr_destroy(&attrs);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_usevfork_failed=%d\n", result);
        return 168;
    }
    wait_result = wait_for_child(pid);
    if (wait_result != 0) {
        return wait_result;
    }
    printf("posix_spawn_usevfork_ok\n");
    fflush(stdout);
    return 0;
#else
    (void)self;
    fprintf(stderr, "posix_spawn_usevfork_unavailable\n");
    return 169;
#endif
}

static int
run_posix_spawn_child_peak_env_only(const char* self)
{
    EnvSnapshot parent_env;
    pid_t pid = -1;
    char* const argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char** envp = make_child_peak_only_env();
    int result;

    call_hot_target(5);
    snapshot_and_unset_peak_env(&parent_env);
    result = posix_spawn(&pid, self, NULL, NULL, argv, envp);
    restore_peak_env_snapshot(&parent_env);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_child_peak_env_only_failed=%d\n", result);
        return 170;
    }
    return wait_for_child(pid);
}

static int
run_posix_spawn_failure(const char* self)
{
    pid_t pid = -1;
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    int result;

    call_hot_target(2);
    errno = EDOM;
    result = posix_spawn(&pid,
                         "/definitely/not/found",
                         NULL,
                         NULL,
                         argv,
                         environ);
    return finish_spawn_observation("posix_spawn_failure", result, errno, pid);
}

static int
run_posix_spawnp_path_search(void)
{
    pid_t pid = -1;
    char* const argv[] = {
        (char*)"test_exec_chain",
        (char*)"child-basic",
        NULL
    };
    char** envp = make_minimal_child_env("posix-spawnp-env");
    int result;

    call_hot_target(5);
    result = posix_spawnp(&pid, "test_exec_chain", NULL, NULL, argv, envp);
    if (result != 0) {
        fprintf(stderr, "posix_spawnp_path_search_failed=%d\n", result);
        return 139;
    }
    return wait_for_child(pid);
}

static int
run_posix_spawnp_custom_env(void)
{
    pid_t pid = -1;
    char* const argv[] = {
        (char*)"test_exec_chain",
        (char*)"child-print-ld",
        NULL
    };
    char** envp = make_minimal_child_env("posix-spawnp-env");
    int result;

    call_hot_target(5);
    errno = EDOM;
    result = posix_spawnp(&pid, "test_exec_chain", NULL, NULL, argv, envp);
    if (getenv("PEAK_TARGET") != NULL && errno != EDOM) {
        fprintf(stderr, "posix_spawnp_custom_env_errno=%d\n", errno);
        return 173;
    }
    if (result != 0) {
        fprintf(stderr, "posix_spawnp_custom_env_failed=%d\n", result);
        return 174;
    }
    return wait_for_child(pid);
}

static int
run_posix_spawn_resolver_null(const char* self, int use_path_search)
{
    pid_t pid = -1;
    char* const spawn_argv[] = {(char*)self, (char*)"child-basic", NULL};
    char* const spawnp_argv[] = {
        (char*)"test_exec_chain",
        (char*)"child-basic",
        NULL
    };
    int result;

    call_hot_target(2);
    errno = EDOM;
    result = use_path_search ?
        posix_spawnp(&pid,
                     "test_exec_chain",
                     NULL,
                     NULL,
                     spawnp_argv,
                     environ) :
        posix_spawn(&pid, self, NULL, NULL, spawn_argv, environ);
    if (result != ENOSYS || errno != EDOM || pid != -1) {
        fprintf(stderr,
                "posix_spawn_resolver_null_bad result=%d errno=%d pid=%ld "
                "path_search=%d\n",
                result,
                errno,
                (long)pid,
                use_path_search);
        return 175;
    }
    call_hot_target(4);
    printf("posix_spawn_resolver_null_ok path_search=%d sink=%lu\n",
           use_path_search,
           peak_exec_chain_sink);
    fflush(stdout);
    return 0;
}

static int
run_posix_spawnp_child_env_path_ignored(void)
{
    pid_t pid = -1;
    char* const argv[] = {
        (char*)"test_exec_chain",
        (char*)"child-basic",
        NULL
    };
    char** envp = make_bad_path_child_env();
    int result;

    call_hot_target(5);
    result = posix_spawnp(&pid, "test_exec_chain", NULL, NULL, argv, envp);
    if (result != 0) {
        fprintf(stderr, "posix_spawnp_child_env_path_ignored_failed=%d\n",
                result);
        return 140;
    }
    return wait_for_child(pid);
}

static int
run_posix_spawnp_bad_env(void)
{
    pid_t pid = -1;
    char* const argv[] = {
        (char*)"test_exec_chain",
        (char*)"child-basic",
        NULL
    };
    int result;

    call_hot_target(2);
    errno = EDOM;
    result = posix_spawnp(&pid,
                          "test_exec_chain",
                          NULL,
                          NULL,
                          argv,
                          invalid_envp_for_test());
    return finish_spawn_observation("posix_spawnp_bad_env", result, errno, pid);
}

static int
run_posix_spawnp_bad_argv(void)
{
    pid_t pid = -1;
    int result;

    call_hot_target(2);
    errno = EDOM;
    result = posix_spawnp(&pid,
                          "test_exec_chain",
                          NULL,
                          NULL,
                          invalid_argv_for_test(),
                          environ);
    return finish_spawn_observation("posix_spawnp_bad_argv", result, errno, pid);
}

static int
run_posix_spawn_explicit_peak_env(const char* self)
{
    pid_t pid = -1;
    char* const argv[] = {
        (char*)self,
        (char*)"child-check-env",
        (char*)"PEAK_TARGET",
        NULL
    };
    char** envp = make_explicit_peak_env();
    int result;

    call_hot_target(5);
    result = posix_spawn(&pid, self, NULL, NULL, argv, envp);
    if (result != 0) {
        fprintf(stderr, "posix_spawn_explicit_peak_env_failed=%d\n", result);
        return 141;
    }
    return wait_for_child(pid);
}

static int
run_fork_child_exec_parent_exec(const char* self)
{
    pid_t pid = fork();
    int child_status;

    if (pid < 0) {
        perror("fork-child-exec-parent-exec");
        return 174;
    }
    if (pid == 0) {
        char* const child_argv[] = {
            (char*)self,
            (char*)"child-basic",
            NULL
        };
        /*
         * The fork child inherits a multithreaded runtime with arbitrary
         * library locks held.  Until exec, do not call instrumented ordinary
         * functions: entering Gum may acquire an inherited allocator lock and
         * deadlock permanently.
         */
        execv(self, child_argv);
        _exit(175);
    }

    child_status = wait_for_child(pid);
    if (child_status != 0) {
        fprintf(stderr, "fork_child_exec_status=%d\n", child_status);
        return 176;
    }
    call_hot_target(5);
    char* const parent_argv[] = {(char*)self, (char*)"child-basic", NULL};
    execv(self, parent_argv);
    perror("fork-parent-exec");
    return 177;
}

static int
run_vfork_child_exec_parent_exec(const char* self)
{
    char* const child_argv[] = {
        (char*)self,
        (char*)"child-basic",
        NULL
    };
    char* const parent_argv[] = {(char*)self, (char*)"child-basic", NULL};
    pid_t pid = vfork();
    int child_status;

    if (pid < 0) {
        perror("vfork-child-exec-parent-exec");
        return 178;
    }
    if (pid == 0) {
        execv(self, child_argv);
        _exit(179);
    }

    child_status = wait_for_child(pid);
    if (child_status != 0) {
        fprintf(stderr, "vfork_child_exec_status=%d\n", child_status);
        return 180;
    }
    call_hot_target(5);
    execv(self, parent_argv);
    perror("vfork-parent-exec");
    return 181;
}

static int
run_vfork_child_failed_exec_parent_exec(const char* self)
{
    char* const child_argv[] = {
        (char*)"/definitely/not/found",
        NULL
    };
    char* const parent_argv[] = {(char*)self, (char*)"child-basic", NULL};
    pid_t pid = vfork();
    int child_status;

    if (pid < 0) {
        perror("vfork-child-failed-exec-parent-exec");
        return 182;
    }
    if (pid == 0) {
        execv("/definitely/not/found", child_argv);
        _exit(errno == ENOENT ? 0 : 183);
    }

    child_status = wait_for_child(pid);
    if (child_status != 0) {
        fprintf(stderr, "vfork_child_failed_exec_status=%d\n", child_status);
        return 184;
    }
    call_hot_target(5);
    execv(self, parent_argv);
    perror("vfork-failed-child-parent-exec");
    return 185;
}

typedef struct {
    const char* self;
} CloneExecContext;

typedef struct {
    char** environ_pointer;
    char* entry_bytes;
    size_t entry_count;
    size_t byte_count;
    uint64_t fingerprint;
} ParentEnvironmentSnapshot;

static int
snapshot_parent_environment(ParentEnvironmentSnapshot* snapshot)
{
    static const uint64_t fnv_offset = UINT64_C(1469598103934665603);
    static const uint64_t fnv_prime = UINT64_C(1099511628211);
    char* output;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->environ_pointer = environ;
    snapshot->fingerprint = fnv_offset;
    if (environ == NULL) {
        return 0;
    }
    while (environ[snapshot->entry_count] != NULL) {
        size_t length = strlen(environ[snapshot->entry_count]) + 1;

        if (snapshot->byte_count > SIZE_MAX - length) {
            errno = EOVERFLOW;
            return -1;
        }
        snapshot->byte_count += length;
        snapshot->entry_count++;
    }
    snapshot->entry_bytes = malloc(snapshot->byte_count == 0 ?
                                       1 : snapshot->byte_count);
    if (snapshot->entry_bytes == NULL) {
        return -1;
    }
    output = snapshot->entry_bytes;
    for (size_t index = 0; index < snapshot->entry_count; index++) {
        size_t length = strlen(environ[index]) + 1;

        memcpy(output, environ[index], length);
        for (size_t byte = 0; byte < length; byte++) {
            snapshot->fingerprint ^= (unsigned char)output[byte];
            snapshot->fingerprint *= fnv_prime;
        }
        output += length;
    }
    return 0;
}

static int
parent_environment_matches(const ParentEnvironmentSnapshot* snapshot)
{
    const char* expected = snapshot->entry_bytes;

    if (environ != snapshot->environ_pointer) {
        return 0;
    }
    if (environ == NULL) {
        return snapshot->entry_count == 0;
    }
    for (size_t index = 0; index < snapshot->entry_count; index++) {
        size_t length;

        if (environ[index] == NULL) {
            return 0;
        }
        length = strlen(expected) + 1;
        if (strlen(environ[index]) + 1 != length ||
            memcmp(environ[index], expected, length) != 0) {
            return 0;
        }
        expected += length;
    }
    return environ[snapshot->entry_count] == NULL &&
           (size_t)(expected - snapshot->entry_bytes) == snapshot->byte_count;
}

static void
destroy_parent_environment_snapshot(ParentEnvironmentSnapshot* snapshot)
{
    free(snapshot->entry_bytes);
    snapshot->entry_bytes = NULL;
}

static int
clone_private_child_exec(void* data)
{
    const CloneExecContext* context = data;
    char* const missing_argv[] = {
        (char*)"/definitely/not/found/peak-clone-exec",
        NULL
    };
    char* const success_argv[] = {
        (char*)context->self,
        (char*)"child-clone-private-success",
        NULL
    };

    if (execv(missing_argv[0], missing_argv) != -1 || errno != ENOENT) {
        _exit(241);
    }
    execv(context->self, success_argv);
    _exit(242);
}

static int
clone_shared_vm_child_raw_exec(void* data)
{
    const CloneExecContext* context = data;
    char* const argv[] = {(char*)context->self, (char*)"child-basic", NULL};

    (void)syscall(SYS_execve, context->self, argv, environ);
    (void)syscall(SYS_exit, 243);
    __builtin_unreachable();
}

static int
finish_clone_parent(const char* self,
                    pid_t pid,
                    const ParentEnvironmentSnapshot* snapshot,
                    const char* mode)
{
    char* const parent_argv[] = {(char*)self, (char*)"child-basic", NULL};
    int child_status = wait_for_child(pid);

    if (child_status != 0) {
        fprintf(stderr, "clone_child_status=%d mode=%s\n", child_status, mode);
        return 244;
    }
    if (!parent_environment_matches(snapshot)) {
        fprintf(stderr,
                "clone_parent_environment_invariant=0 mode=%s\n",
                mode);
        return 245;
    }
    printf("clone_parent_environment_invariant=1 "
           "environ_pointer_unchanged=1 entries=%zu bytes=%zu "
           "fingerprint=%016llx mode=%s parent_pid=%ld\n",
           snapshot->entry_count,
           snapshot->byte_count,
           (unsigned long long)snapshot->fingerprint,
           mode,
           (long)getpid());
    fflush(stdout);
    call_hot_target(5);
    execv(self, parent_argv);
    perror("clone-parent-exec");
    return 246;
}

static int
run_libc_clone_exec(const char* self, int flags)
{
    enum { clone_stack_size = 64 * 1024 };
    _Alignas(16) char stack[clone_stack_size];
    ParentEnvironmentSnapshot snapshot;
    CloneExecContext context = {self};
    int shared_vm = (flags & CLONE_VM) != 0;
    pid_t pid;
    int result;

    if (snapshot_parent_environment(&snapshot) != 0) {
        perror("clone-parent-environment-snapshot");
        return 247;
    }
    pid = clone(shared_vm ? clone_shared_vm_child_raw_exec :
                            clone_private_child_exec,
                stack + sizeof(stack), flags | SIGCHLD, &context);
    if (pid < 0) {
        perror("libc-clone");
        destroy_parent_environment_snapshot(&snapshot);
        return 248;
    }
    result = finish_clone_parent(self,
                                 pid,
                                 &snapshot,
                                 (flags & CLONE_VFORK) != 0 ? "clone-vfork" :
                                     (shared_vm ? "clone-vm" :
                                                  "clone-private-vm"));
    destroy_parent_environment_snapshot(&snapshot);
    return result;
}

#if defined(SYS_clone)
static int
run_raw_clone_exec(const char* self, int raw_exec)
{
    ParentEnvironmentSnapshot snapshot;
    char* const missing_argv[] = {
        (char*)"/definitely/not/found/peak-raw-clone-exec",
        NULL
    };
    char* const success_argv[] = {
        (char*)self,
        (char*)"child-clone-private-success",
        NULL
    };
    long result;
    int finish_result;

    if (snapshot_parent_environment(&snapshot) != 0) {
        perror("raw-clone-parent-environment-snapshot");
        return 251;
    }
    result = syscall(SYS_clone, SIGCHLD, NULL, NULL, NULL, 0);
    if (result < 0) {
        perror("raw-clone");
        destroy_parent_environment_snapshot(&snapshot);
        return 252;
    }
    if (result == 0) {
        if (raw_exec) {
            if (syscall(SYS_execve,
                        missing_argv[0],
                        missing_argv,
                        environ) != -1 || errno != ENOENT) {
                _exit(253);
            }
            (void)syscall(SYS_execve, self, success_argv, environ);
            (void)syscall(SYS_exit, 254);
        } else {
            if (execv(missing_argv[0], missing_argv) != -1 ||
                errno != ENOENT) {
                _exit(255);
            }
            execv(self, success_argv);
            _exit(256);
        }
        __builtin_unreachable();
    }
    finish_result = finish_clone_parent(
        self,
        (pid_t)result,
        &snapshot,
        raw_exec ? "raw-clone-raw-exec" : "raw-clone-libc-exec");
    destroy_parent_environment_snapshot(&snapshot);
    return finish_result;
}
#endif

#if defined(PEAK_TEST_HAVE_SYS_CLONE3) && defined(SYS_clone3)
static int
run_clone3_exec(const char* self)
{
    struct clone_args args;
    ParentEnvironmentSnapshot snapshot;
    char* const missing_argv[] = {
        (char*)"/definitely/not/found/peak-clone3-exec",
        NULL
    };
    char* const success_argv[] = {
        (char*)self,
        (char*)"child-clone-private-success",
        NULL
    };
    long result;
    int finish_result;

    if (snapshot_parent_environment(&snapshot) != 0) {
        perror("clone3-parent-environment-snapshot");
        return 257;
    }
    memset(&args, 0, sizeof(args));
    args.exit_signal = SIGCHLD;
    result = syscall(SYS_clone3, &args, sizeof(args));
    if (result < 0 && errno == ENOSYS) {
        puts("clone3_skip_enosys=1");
        destroy_parent_environment_snapshot(&snapshot);
        return 0;
    }
    if (result < 0) {
        perror("clone3");
        destroy_parent_environment_snapshot(&snapshot);
        return 258;
    }
    if (result == 0) {
        if (syscall(SYS_execve,
                    missing_argv[0],
                    missing_argv,
                    environ) != -1 || errno != ENOENT) {
            _exit(259);
        }
        (void)syscall(SYS_execve, self, success_argv, environ);
        (void)syscall(SYS_exit, 260);
        __builtin_unreachable();
    }
    finish_result = finish_clone_parent(self,
                                        (pid_t)result,
                                        &snapshot,
                                        "clone3");
    destroy_parent_environment_snapshot(&snapshot);
    return finish_result;
}
#endif

static int
run_vfork_varargs_exec_parent_exec(const char* self, int mode)
{
    char* const parent_argv[] = {(char*)self, (char*)"child-basic", NULL};
    pid_t pid = vfork();
    int child_status;

    if (pid < 0) {
        perror("vfork-varargs-parent-exec");
        return 187;
    }
    if (pid == 0) {
        if (mode == 0) {
            execl(self, self, "child-basic", (char*)NULL);
        } else if (mode == 1) {
            execlp(self, self, "child-basic", (char*)NULL);
        } else {
            execle(self,
                   self,
                   "child-basic",
                   (char*)NULL,
                   environ);
        }
        _exit(188);
    }

    child_status = wait_for_child(pid);
    if (child_status != 0) {
        fprintf(stderr, "vfork_varargs_child_status=%d mode=%d\n",
                child_status,
                mode);
        return 189;
    }
    call_hot_target(5);
    execv(self, parent_argv);
    perror("vfork-varargs-parent-exec");
    return 190;
}

static int
run_postfork_custom_env(const char* self,
                        int use_vfork,
                        int use_execle,
                        int disable_chain,
                        int use_raw_syscall,
                        int omit_loader_path)
{
    const char* observer = getenv(LOADER_OBSERVER_ENV);
    const char* child = observer != NULL && observer[0] != '\0' ? observer : self;
    char* const child_argv[] = {
        (char*)child,
        (char*)"child-print-ld",
        NULL
    };
    char* const parent_argv[] = {(char*)self, (char*)"child-basic", NULL};
    const char* parent_preload = getenv("LD_PRELOAD");
    char* parent_preload_copy = parent_preload != NULL ? strdup(parent_preload) : NULL;
    char** child_env = omit_loader_path ?
        make_minimal_child_env_without_loader("postfork-loader-path") :
        make_minimal_child_env(disable_chain ? "postfork-custom-disabled" :
                                             "postfork-custom");
    size_t child_env_count = 0;
    pid_t pid;
    int child_status;

    if (parent_preload != NULL && parent_preload_copy == NULL) {
        perror("postfork-custom-strdup");
        return 191;
    }
    while (child_env[child_env_count] != NULL) {
        child_env_count++;
    }
    if (disable_chain) {
        child_env[child_env_count++] = make_env_entry("PEAK_EXEC_CHAIN", "0");
        child_env[child_env_count] = NULL;
    }

    pid = use_vfork ? vfork() : fork();
    if (pid < 0) {
        perror("postfork-custom-env");
        free(parent_preload_copy);
        return 192;
    }
    if (pid == 0) {
        if (use_execle) {
            execle(child,
                   child,
                   "child-print-ld",
                   (char*)NULL,
                   child_env);
        } else if (use_raw_syscall) {
            (void)syscall(SYS_execve, child, child_argv, child_env);
        } else {
            execve(child, child_argv, child_env);
        }
        _exit(193);
    }

    child_status = wait_for_child(pid);
    if (child_status != 0) {
        fprintf(stderr, "postfork_custom_child_status=%d\n", child_status);
        free(parent_preload_copy);
        return 194;
    }
    if ((parent_preload == NULL && getenv("LD_PRELOAD") != NULL) ||
        (parent_preload != NULL &&
         strcmp(parent_preload_copy, getenv("LD_PRELOAD")) != 0)) {
        fprintf(stderr, "parent_env_unchanged=0\n");
        free(parent_preload_copy);
        return 195;
    }
    printf("parent_env_unchanged=1\n");
    fflush(stdout);
    free(parent_preload_copy);
    call_hot_target(5);
    execv(self, parent_argv);
    perror("postfork-custom-parent-exec");
    return 196;
}

static int
run_vfork_preloaded_env(const char* self)
{
    char* const child_argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char* const parent_argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** child_env = make_preloaded_child_env("loader-path-preload-present");
    pid_t pid = vfork();
    int child_status;

    if (pid < 0) {
        perror("vfork-preloaded-env");
        return 227;
    }
    if (pid == 0) {
        execve(self, child_argv, child_env);
        _exit(228);
    }
    child_status = wait_for_child(pid);
    if (child_status != 0) {
        fprintf(stderr, "vfork_preloaded_child_status=%d\n", child_status);
        return 229;
    }
    printf("vfork_preloaded_parent_ok=1\n");
    fflush(stdout);
    call_hot_target(5);
    execv(self, parent_argv);
    perror("vfork-preloaded-parent-exec");
    return 230;
}

static int
run_fork_parent_env_exhaustion(const char* self)
{
    const char* observer = getenv(LOADER_OBSERVER_ENV);
    const char* child = observer != NULL && observer[0] != '\0' ? observer : self;
    char* const child_argv[] = {(char*)child, (char*)"child-print-ld", NULL};
    char* const parent_argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** child_env = make_child_control_env("parent-env-exhaustion");
    pid_t pid = fork();
    int child_status;

    if (pid < 0) {
        perror("fork-parent-env-exhaustion");
        return 231;
    }
    if (pid == 0) {
        environ = make_unterminated_nonpeak_env();
        execve(child, child_argv, child_env);
        _exit(232);
    }
    child_status = wait_for_child(pid);
    if (child_status != 0) {
        fprintf(stderr, "parent_env_exhaustion_child_status=%d\n", child_status);
        return 233;
    }
    printf("parent_env_exhaustion_passthrough=1\n");
    fflush(stdout);
    call_hot_target(5);
    execv(self, parent_argv);
    perror("parent-env-exhaustion-parent-exec");
    return 234;
}

enum PostforkExecApi {
    POSTFORK_EXECVPE,
    POSTFORK_EXECVP,
    POSTFORK_EXECLP,
    POSTFORK_FEXECVE,
    POSTFORK_EXECVEAT,
    POSTFORK_RAW_EXECVEAT,
};

static int
run_vfork_custom_env_api(const char* self, enum PostforkExecApi api)
{
    char* const child_argv[] = {(char*)self, (char*)"child-print-ld", NULL};
    char* const parent_argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** child_env = make_minimal_child_env("postfork-api-custom");
    char** saved_environ = environ;
    int fd = -1;
    pid_t pid;
    int child_status;

    for (size_t index = 0; child_env[index] != NULL; index++) {
        if (child_env[index + 1] == NULL) {
            child_env[index + 1] = make_env_entry("PEAK_TARGET",
                                                  "explicit_child_target");
            child_env[index + 2] = NULL;
            break;
        }
    }

    if (api == POSTFORK_FEXECVE) {
        fd = open(self, O_RDONLY);
        if (fd < 0) {
            perror("postfork-api-open");
            return 210;
        }
    }
    if (api == POSTFORK_EXECVP || api == POSTFORK_EXECLP) {
        environ = child_env;
    }
    pid = vfork();
    if (pid < 0) {
        environ = saved_environ;
        if (fd >= 0) {
            close(fd);
        }
        perror("postfork-api-vfork");
        return 211;
    }
    if (pid == 0) {
        switch (api) {
        case POSTFORK_EXECVPE:
            execvpe(self, child_argv, child_env);
            break;
        case POSTFORK_EXECVP:
            execvp(self, child_argv);
            break;
        case POSTFORK_EXECLP:
            execlp(self, self, "child-print-ld", (char*)NULL);
            break;
        case POSTFORK_FEXECVE:
            fexecve(fd, child_argv, child_env);
            break;
        case POSTFORK_EXECVEAT:
#if defined(PEAK_TEST_HAVE_EXECVEAT)
            execveat(AT_FDCWD, self, child_argv, child_env, 0);
#endif
            break;
        case POSTFORK_RAW_EXECVEAT:
#if defined(PEAK_TEST_HAVE_EXECVEAT)
            (void)syscall(SYS_execveat,
                          AT_FDCWD,
                          self,
                          child_argv,
                          child_env,
                          0);
#endif
            break;
        }
        _exit(212);
    }
    environ = saved_environ;
    if (fd >= 0) {
        close(fd);
    }
    child_status = wait_for_child(pid);
    if (child_status != 0) {
        fprintf(stderr, "postfork_api_child_status=%d api=%d\n",
                child_status,
                (int)api);
        return 213;
    }
    printf("postfork_api_parent_env_unchanged=%d\n", environ == saved_environ);
    fflush(stdout);
    call_hot_target(5);
    execv(self, parent_argv);
    perror("postfork-api-parent-exec");
    return 214;
}

static int
run_postfork_bad_env(const char* self, int use_vfork, int bad_string)
{
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    char* bad_string_env[] = {(char*)(uintptr_t)1, NULL};
    char* const* envp = bad_string ? bad_string_env : invalid_envp_for_test();
    char* const parent_argv[] = {(char*)self, (char*)"child-basic", NULL};
    pid_t pid = use_vfork ? vfork() : fork();
    int child_status;

    if (pid < 0) {
        perror("postfork-bad-env");
        return 215;
    }
    if (pid == 0) {
        execve(self, argv, (char* const*)envp);
        _exit(errno == EFAULT ? 0 : 216);
    }
    child_status = wait_for_child(pid);
    if (child_status != 0) {
        fprintf(stderr, "postfork_bad_env_child_status=%d\n", child_status);
        return 217;
    }
    printf("postfork_bad_env_efault=1 kind=%s mode=%s\n",
           bad_string ? "string" : "vector",
           use_vfork ? "vfork" : "fork");
    fflush(stdout);
    call_hot_target(5);
    execv(self, parent_argv);
    perror("postfork-bad-env-parent-exec");
    return 218;
}

static int
run_vfork_child_env_capacity_fallback(const char* self, int long_preload)
{
    const char* observer = getenv(LOADER_OBSERVER_ENV);
    const char* child = observer != NULL && observer[0] != '\0' ? observer : self;
    const char* child_mode = child == self ?
        "child-print-ld" :
        (long_preload ? "capacity-long-preload" : "capacity-env-slots");
    char* const child_argv[] = {(char*)child, (char*)child_mode, NULL};
    char* const parent_argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = long_preload ? make_long_preload_child_env() :
                                make_large_child_env();
    pid_t pid = vfork();
    int child_status;

    if (pid < 0) {
        perror("postfork-capacity-vfork");
        return 219;
    }
    if (pid == 0) {
        execve(child, child_argv, envp);
        _exit(220);
    }
    child_status = wait_for_child(pid);
    if (child_status != 0) {
        fprintf(stderr, "postfork_capacity_child_status=%d\n", child_status);
        return 221;
    }
    const size_t bounded_entries = long_preload ? 3 : 514;
    size_t input_entries = 0;
    size_t input_pad_validated_count = 0;
    size_t input_pad_mismatch_count = 0;
    size_t input_preload_entries = 0;
    size_t input_preload_length = 0;
    int input_preload_all_x = 1;
    size_t input_path_entries = 0;
    size_t input_loader_path_entries = 0;

    for (size_t index = 0; index < bounded_entries; index++) {
        const char* entry = envp[index];

        if (entry == NULL) {
            continue;
        }
        input_entries++;
        if (strncmp(entry, "LD_PRELOAD=", strlen("LD_PRELOAD=")) == 0) {
            const char* value = entry + strlen("LD_PRELOAD=");

            input_preload_entries++;
            input_preload_length += strlen(value);
            for (const char* scan = value; *scan != '\0'; scan++) {
                if (*scan != 'x') {
                    input_preload_all_x = 0;
                }
            }
        } else if (strncmp(entry, "PATH=", strlen("PATH=")) == 0) {
            input_path_entries++;
        } else if (strncmp(entry,
                           "LD_LIBRARY_PATH=",
                           strlen("LD_LIBRARY_PATH=")) == 0) {
            input_loader_path_entries++;
        }
    }
    if (!long_preload) {
        char expected[64];

        for (size_t index = 0; index < 512; index++) {
            snprintf(expected,
                     sizeof(expected),
                     "CHILD_PAD_%04zu=x",
                     index);
            if (envp[index + 2] != NULL &&
                strcmp(envp[index + 2], expected) == 0) {
                input_pad_validated_count++;
            } else {
                input_pad_mismatch_count++;
            }
        }
    }
    printf("postfork_capacity_passthrough=1 kind=%s input_entries=%zu "
           "input_pad_validated_count=%zu input_pad_mismatch_count=%zu "
           "input_preload_entries=%zu input_preload_length=%zu "
           "input_preload_all_x=%d input_path_entries=%zu "
           "input_loader_path_entries=%zu input_terminator_null=%d\n",
           long_preload ? "long-preload" : "env-slots",
           input_entries,
           input_pad_validated_count,
           input_pad_mismatch_count,
           input_preload_entries,
           input_preload_length,
           input_preload_all_x,
           input_path_entries,
           input_loader_path_entries,
           envp[bounded_entries] == NULL ? 1 : 0);
    fflush(stdout);
    call_hot_target(5);
    execv(self, parent_argv);
    perror("postfork-capacity-parent-exec");
    return 222;
}

static int
run_vfork_preload_entries_fallback(const char* self)
{
    const char* observer = getenv(LOADER_OBSERVER_ENV);
    const char* child = observer != NULL && observer[0] != '\0' ? observer : self;
    const char* child_mode = child == self ?
        "child-print-ld" : "capacity-preload-entries";
    char* const child_argv[] = {(char*)child, (char*)child_mode, NULL};
    char* const parent_argv[] = {(char*)self, (char*)"child-basic", NULL};
    char** envp = make_preload_entries_without_terminator();
    pid_t pid = vfork();
    int child_status;

    if (pid < 0) {
        perror("postfork-preload-entries-vfork");
        return 223;
    }
    if (pid == 0) {
        execve(child, child_argv, envp);
        _exit(224);
    }
    child_status = wait_for_child(pid);
    if (child_status != 0) {
        fprintf(stderr, "postfork_preload_entries_child_status=%d\n", child_status);
        return 225;
    }
    const size_t bounded_entries = 256;
    size_t input_entries = 0;
    size_t input_preload_entries = 0;
    size_t input_nonempty_preload_entries = 0;
    size_t input_path_entries = 0;
    size_t input_loader_path_entries = 0;

    for (size_t index = 0; index < bounded_entries; index++) {
        const char* entry = envp[index];

        if (entry == NULL) {
            continue;
        }
        input_entries++;
        if (strncmp(entry, "LD_PRELOAD=", strlen("LD_PRELOAD=")) == 0) {
            input_preload_entries++;
            if (entry[strlen("LD_PRELOAD=")] != '\0') {
                input_nonempty_preload_entries++;
            }
        } else if (strncmp(entry, "PATH=", strlen("PATH=")) == 0) {
            input_path_entries++;
        } else if (strncmp(entry,
                           "LD_LIBRARY_PATH=",
                           strlen("LD_LIBRARY_PATH=")) == 0) {
            input_loader_path_entries++;
        }
    }
    printf("postfork_preload_entries_passthrough=1 input_entries=%zu "
           "input_preload_entries=%zu input_nonempty_preload_entries=%zu "
           "input_path_entries=%zu "
           "input_loader_path_entries=%zu input_terminator_null=%d\n",
           input_entries,
           input_preload_entries,
           input_nonempty_preload_entries,
           input_path_entries,
           input_loader_path_entries,
           envp[bounded_entries] == NULL ? 1 : 0);
    fflush(stdout);
    call_hot_target(5);
    execv(self, parent_argv);
    perror("postfork-preload-entries-parent-exec");
    return 226;
}

typedef void (*PeakFiniFn)(void);
typedef int (*PeakCheckpointFn)(const char*, char* const[]);
typedef void (*PeakTestVoidFn)(void);
typedef int (*PeakTestReadyFn)(void);

static void*
fini_thread_main(void* data)
{
    PeakFiniFn fini = data;

    fini();
    return NULL;
}

typedef struct {
    const char* self;
    PeakCheckpointFn checkpoint;
    int result;
} CheckpointThreadArgs;

static void*
checkpoint_thread_main(void* data)
{
    CheckpointThreadArgs* args = data;
    char* const argv[] = {(char*)args->self, (char*)"child-basic", NULL};

    args->result = args->checkpoint(args->self, argv);
    return NULL;
}

typedef struct {
    PeakTestReadyFn function;
    int result;
} TestHookThreadArgs;

static void*
test_hook_thread_main(void* data)
{
    TestHookThreadArgs* args = data;

    args->result = args->function();
    return NULL;
}

static int
wait_for_test_hook(int (*hook)(void), const char* label)
{
    struct timespec deadline;

    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        perror("clock_gettime");
        return -1;
    }
    deadline.tv_sec += 5;

    for (;;) {
        struct timespec now;

        if (hook()) {
            return 0;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            perror("clock_gettime");
            return -1;
        }
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            break;
        }
        sched_yield();
    }
    fprintf(stderr, "timed out waiting for %s\n", label);
    return -1;
}

static int
join_test_thread(pthread_t thread, const char* label)
{
    int rc = pthread_join(thread, NULL);

    if (rc == 0) {
        return 0;
    }
    fprintf(stderr, "pthread_join %s failed: %s\n", label, strerror(rc));
    return -1;
}

static int
run_exec_checkpoint_concurrent_fini_callbacks(const char* self)
{
    pthread_t fini_thread;
    pthread_t checkpoint_thread;
    PeakFiniFn fini = (PeakFiniFn)dlsym(RTLD_DEFAULT, "peak_test_fini");
    PeakCheckpointFn checkpoint =
        (PeakCheckpointFn)dlsym(RTLD_DEFAULT, "peak_checkpoint_for_exec");
    PeakTestVoidFn pause_enable = (PeakTestVoidFn)dlsym(
        RTLD_DEFAULT, "peak_test_checkpoint_reader_pause_enable");
    PeakTestReadyFn reader_is_held = (PeakTestReadyFn)dlsym(
        RTLD_DEFAULT, "peak_test_checkpoint_reader_is_held");
    PeakTestVoidFn reader_release = (PeakTestVoidFn)dlsym(
        RTLD_DEFAULT, "peak_test_checkpoint_reader_release");
    PeakTestReadyFn fini_waiting = (PeakTestReadyFn)dlsym(
        RTLD_DEFAULT, "peak_test_fini_waiting_for_checkpoint_reader");
    CheckpointThreadArgs checkpoint_args = {.self = self, .checkpoint = checkpoint};

    if (fini == NULL || checkpoint == NULL || pause_enable == NULL ||
        reader_is_held == NULL || reader_release == NULL ||
        fini_waiting == NULL) {
        fprintf(stderr, "checkpoint/fini test hooks unavailable\n");
        return 180;
    }

    call_hot_target(5);
    pause_enable();
    int rc = pthread_create(&checkpoint_thread,
                            NULL,
                            checkpoint_thread_main,
                            &checkpoint_args);
    if (rc != 0) {
        fprintf(stderr,
                "pthread_create checkpoint reader failed: %s\n",
                strerror(rc));
        return 181;
    }
    if (wait_for_test_hook(reader_is_held,
                           "checkpoint reader lifetime gate") != 0) {
        reader_release();
        return join_test_thread(checkpoint_thread, "checkpoint reader") == 0 ?
                   182 :
                   185;
    }
    rc = pthread_create(&fini_thread, NULL, fini_thread_main, (void*)fini);
    if (rc != 0) {
        fprintf(stderr, "pthread_create fini failed: %s\n", strerror(rc));
        reader_release();
        return join_test_thread(checkpoint_thread, "checkpoint reader") == 0 ?
                   183 :
                   185;
    }
    if (wait_for_test_hook(fini_waiting,
                           "fini waiting for checkpoint reader") != 0) {
        reader_release();
        int checkpoint_join =
            join_test_thread(checkpoint_thread, "checkpoint reader");
        int fini_join = join_test_thread(fini_thread, "fini");
        return checkpoint_join == 0 && fini_join == 0 ? 184 : 185;
    }
    printf("checkpoint_reader_gate_held=1 fini_waiting=1\n");
    fflush(stdout);
    reader_release();
    int checkpoint_join =
        join_test_thread(checkpoint_thread, "checkpoint reader");
    int fini_join = join_test_thread(fini_thread, "fini");
    if (checkpoint_join != 0 || fini_join != 0) {
        return 185;
    }

    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    execv(self, argv);
    perror("exec-concurrent-fini-callbacks");
    return 186;
}

static int
run_exec_checkpoint_snapshot_lock_contention(const char* self)
{
    PeakTestReadyFn hold = (PeakTestReadyFn)dlsym(
        RTLD_DEFAULT,
        "peak_general_listener_test_checkpoint_snapshot_lock_hold");
    PeakTestReadyFn release = (PeakTestReadyFn)dlsym(
        RTLD_DEFAULT,
        "peak_general_listener_test_checkpoint_snapshot_lock_release");
    PeakCheckpointFn checkpoint =
        (PeakCheckpointFn)dlsym(RTLD_DEFAULT, "peak_checkpoint_for_exec");
    char* const missing_argv[] = {(char*)"missing-exec", NULL};
    char* const child_argv[] = {(char*)self, (char*)"child-basic", NULL};
    int failure = 0;
    int hold_rc;
    int release_rc;

    if (hold == NULL || release == NULL || checkpoint == NULL) {
        fprintf(stderr, "checkpoint snapshot-lock test hooks unavailable\n");
        return 187;
    }

    call_hot_target(5);
    hold_rc = hold();
    if (hold_rc != 0) {
        fprintf(stderr,
                "checkpoint snapshot-lock holder failed: %s\n",
                strerror(hold_rc));
        return 188;
    }

    errno = EALREADY;
    if (checkpoint(self, child_argv) != -1 || errno != EALREADY) {
        fprintf(stderr,
                "checkpoint snapshot contention did not fail open: errno=%d\n",
                errno);
        failure = 189;
        goto release_holder;
    }

    errno = EALREADY;
    if (execv("/definitely/missing/peak-checkpoint-lock", missing_argv) != -1 ||
        errno != ENOENT) {
        fprintf(stderr,
                "missing exec under checkpoint contention failed: errno=%d\n",
                errno);
        failure = 190;
    }

release_holder:
    release_rc = release();
    if (release_rc != 0) {
        fprintf(stderr,
                "checkpoint snapshot-lock release failed: %s\n",
                strerror(release_rc));
        return 191;
    }
    if (failure != 0) {
        return failure;
    }

    errno = ERANGE;
    if (checkpoint(self, child_argv) != 0 || errno != ERANGE) {
        fprintf(stderr,
                "checkpoint after snapshot-lock release failed: errno=%d\n",
                errno);
        return 192;
    }

    hold_rc = hold();
    if (hold_rc != 0) {
        fprintf(stderr,
                "second checkpoint snapshot-lock hold failed: %s\n",
                strerror(hold_rc));
        return 193;
    }
    errno = EALREADY;
    if (checkpoint(self, child_argv) != -1 || errno != EALREADY) {
        fprintf(stderr,
                "second checkpoint snapshot contention did not fail open: errno=%d\n",
                errno);
        failure = 194;
    }
    release_rc = release();
    if (release_rc != 0) {
        fprintf(stderr,
                "second checkpoint snapshot-lock release failed: %s\n",
                strerror(release_rc));
        return 195;
    }
    if (failure != 0) {
        return failure;
    }

    printf("checkpoint_snapshot_lock_contention=1 errno_preserved=1 "
           "missing_exec_enoent=1 cleanup=1 repeatable=1 success=1\n");
    fflush(stdout);
    execv(self, child_argv);
    perror("exec-checkpoint-snapshot-lock-contention");
    return 196;
}

static int
run_exec_checkpoint_statistics_snapshot_contention(const char* self)
{
    PeakTestReadyFn mutation_begin = (PeakTestReadyFn)dlsym(
        RTLD_DEFAULT, "peak_general_listener_test_checkpoint_mutation_begin");
    PeakTestVoidFn mutation_release = (PeakTestVoidFn)dlsym(
        RTLD_DEFAULT, "peak_general_listener_test_checkpoint_mutation_release");
    PeakTestVoidFn pause_enable = (PeakTestVoidFn)dlsym(
        RTLD_DEFAULT, "peak_general_listener_test_checkpoint_busy_pause_enable");
    PeakTestReadyFn busy_is_held = (PeakTestReadyFn)dlsym(
        RTLD_DEFAULT, "peak_general_listener_test_checkpoint_busy_is_held");
    PeakTestVoidFn busy_release = (PeakTestVoidFn)dlsym(
        RTLD_DEFAULT, "peak_general_listener_test_checkpoint_busy_release");
    PeakCheckpointFn checkpoint =
        (PeakCheckpointFn)dlsym(RTLD_DEFAULT, "peak_checkpoint_for_exec");
    CheckpointThreadArgs args = {.self = self, .checkpoint = checkpoint, .result = -1};
    pthread_t checkpoint_thread;

    if (mutation_begin == NULL || mutation_release == NULL || pause_enable == NULL ||
        busy_is_held == NULL || busy_release == NULL || checkpoint == NULL) {
        fprintf(stderr, "checkpoint statistics test hooks unavailable\n");
        return 197;
    }
    call_hot_target(1);
    if (mutation_begin() != 0) {
        fprintf(stderr, "checkpoint statistics mutation setup failed\n");
        return 198;
    }
    pause_enable();
    int rc = pthread_create(&checkpoint_thread, NULL, checkpoint_thread_main, &args);
    if (rc != 0) {
        mutation_release();
        fprintf(stderr, "pthread_create statistics checkpoint failed: %s\n", strerror(rc));
        return 199;
    }
    if (wait_for_test_hook(busy_is_held, "checkpoint statistics contention") != 0) {
        mutation_release();
        busy_release();
        return join_test_thread(checkpoint_thread, "statistics checkpoint") == 0 ?
                   200 : 201;
    }
    mutation_release();
    busy_release();
    if (join_test_thread(checkpoint_thread, "statistics checkpoint") != 0 ||
        args.result != 0) {
        fprintf(stderr, "checkpoint statistics snapshot did not retry successfully\n");
        return 201;
    }
    printf("checkpoint_statistics_contention=1 busy_observed=1 "
           "coherent_tuple=1 success=1\n");
    fflush(stdout);
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    execv(self, argv);
    perror("exec-checkpoint-statistics-snapshot-contention");
    return 202;
}

static int
run_exec_checkpoint_statistics_shadow_invalidation(const char* self)
{
    PeakTestReadyFn mutation_begin = (PeakTestReadyFn)dlsym(
        RTLD_DEFAULT, "peak_general_listener_test_checkpoint_mutation_begin");
    PeakTestVoidFn mutation_release = (PeakTestVoidFn)dlsym(
        RTLD_DEFAULT, "peak_general_listener_test_checkpoint_mutation_release");
    PeakTestReadyFn mutation_contend = (PeakTestReadyFn)dlsym(
        RTLD_DEFAULT, "peak_general_listener_test_checkpoint_mutation_contend");
    PeakTestReadyFn contender_invalidated = (PeakTestReadyFn)dlsym(
        RTLD_DEFAULT,
        "peak_general_listener_test_checkpoint_mutation_contender_invalidated");
    PeakCheckpointFn checkpoint =
        (PeakCheckpointFn)dlsym(RTLD_DEFAULT, "peak_checkpoint_for_exec");
    TestHookThreadArgs contender_args = {.function = mutation_contend, .result = 0};
    pthread_t contender_thread;

    if (mutation_begin == NULL || mutation_release == NULL || mutation_contend == NULL ||
        contender_invalidated == NULL || checkpoint == NULL) {
        fprintf(stderr, "checkpoint shadow invalidation test hooks unavailable\n");
        return 203;
    }
    call_hot_target(1);
    if (mutation_begin() != 0) {
        fprintf(stderr, "checkpoint shadow invalidation mutation setup failed\n");
        return 204;
    }
    int rc = pthread_create(&contender_thread,
                            NULL,
                            test_hook_thread_main,
                            &contender_args);
    if (rc != 0) {
        mutation_release();
        fprintf(stderr, "pthread_create shadow contender failed: %s\n", strerror(rc));
        return 205;
    }
    if (wait_for_test_hook(contender_invalidated, "checkpoint shadow invalidation") != 0) {
        mutation_release();
        return join_test_thread(contender_thread, "shadow contender") == 0 ? 206 : 207;
    }
    if (join_test_thread(contender_thread, "shadow contender") != 0 ||
        contender_args.result != 0) {
        mutation_release();
        fprintf(stderr, "checkpoint shadow contender did not return promptly\n");
        return 208;
    }
    errno = ERANGE;
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};
    if (checkpoint(self, argv) != -1 || errno != ERANGE) {
        mutation_release();
        fprintf(stderr, "checkpoint shadow invalidation did not fail closed errno=%d\n",
                errno);
        return 209;
    }
    mutation_release();
    printf("checkpoint_shadow_invalidation=1 contender_returned=1 "
           "errno_preserved=1 no_artifact=1 success=1\n");
    fflush(stdout);
    execv(self, argv);
    perror("exec-checkpoint-statistics-shadow-invalidation");
    return 210;
}

static int
run_exec_checkpoint_direct_disabled(const char* self)
{
    PeakCheckpointFn checkpoint =
        (PeakCheckpointFn)dlsym(RTLD_DEFAULT, "peak_checkpoint_for_exec");
    char* const argv[] = {(char*)self, (char*)"child-basic", NULL};

    if (checkpoint == NULL) {
        fprintf(stderr, "direct disabled checkpoint hook unavailable\n");
        return 212;
    }
    errno = ERANGE;
    if (checkpoint(self, argv) != -1 || errno != ERANGE) {
        fprintf(stderr, "direct disabled checkpoint did not preserve errno=%d\n", errno);
        return 213;
    }
    printf("checkpoint_direct_disabled=1 errno_preserved=1 success=1\n");
    fflush(stdout);
    execv(self, argv);
    perror("exec-checkpoint-direct-disabled");
    return 214;
}

static int
run_exec_checkpoint_fork_child_fini(const char* self)
{
    pthread_t checkpoint_thread;
    PeakCheckpointFn checkpoint =
        (PeakCheckpointFn)dlsym(RTLD_DEFAULT, "peak_checkpoint_for_exec");
    PeakTestVoidFn pause_enable = (PeakTestVoidFn)dlsym(
        RTLD_DEFAULT, "peak_test_checkpoint_reader_pause_enable");
    PeakTestReadyFn reader_is_held = (PeakTestReadyFn)dlsym(
        RTLD_DEFAULT, "peak_test_checkpoint_reader_is_held");
    PeakTestVoidFn reader_release = (PeakTestVoidFn)dlsym(
        RTLD_DEFAULT, "peak_test_checkpoint_reader_release");
    CheckpointThreadArgs checkpoint_args = {.self = self, .checkpoint = checkpoint};
    pid_t pid;
    int child_status;
    int checkpoint_child_ok = 0;
    int failed_exec_child_ok = 0;
    int normal_return_child_ok = 0;

    if (checkpoint == NULL || pause_enable == NULL ||
        reader_is_held == NULL || reader_release == NULL) {
        fprintf(stderr, "checkpoint/fork test hooks unavailable\n");
        return 195;
    }

    call_hot_target(5);
    pause_enable();
    int create_rc = pthread_create(&checkpoint_thread,
                                   NULL,
                                   checkpoint_thread_main,
                                   &checkpoint_args);
    if (create_rc != 0) {
        fprintf(stderr,
                "pthread_create fork checkpoint reader failed: %s\n",
                strerror(create_rc));
        return 196;
    }
    if (wait_for_test_hook(reader_is_held,
                           "checkpoint reader before fork") != 0) {
        reader_release();
        int join_rc = join_test_thread(checkpoint_thread,
                                       "fork checkpoint reader");
        return join_rc == 0 ? 196 : 198;
    }

    pid = fork();
    if (pid == 0) {
        char* const child_argv[] = {(char*)self, (char*)"child-basic", NULL};

        alarm(10);
        errno = EAGAIN;
        if (checkpoint(self, child_argv) != -1 || errno != EAGAIN) {
            _exit(200);
        }
        exit(0);
    }
    if (pid < 0) {
        int fork_errno = errno;

        reader_release();
        int join_rc = join_test_thread(checkpoint_thread,
                                       "fork checkpoint reader");
        fprintf(stderr,
                "fork checkpoint child failed: %s\n",
                strerror(fork_errno));
        return join_rc == 0 ? 197 : 198;
    }

    child_status = wait_for_child(pid);
    checkpoint_child_ok = child_status == 0;
    if (!checkpoint_child_ok) {
        fprintf(stderr, "checkpoint_fork_child_status=%d\n", child_status);
    }

    pid = fork();
    if (pid == 0) {
        char* const child_argv[] = {(char*)"missing-exec", NULL};

        alarm(10);
        errno = 0;
        if (execv("/definitely/missing/peak-exec-child", child_argv) != -1 ||
            errno != ENOENT) {
            _exit(201);
        }
        exit(0);
    }
    if (pid < 0) {
        perror("fork failed-exec child");
    } else {
        child_status = wait_for_child(pid);
        failed_exec_child_ok = child_status == 0;
        if (!failed_exec_child_ok) {
            fprintf(stderr, "failed_exec_fork_child_status=%d\n", child_status);
        }
    }

    pid = fork();
    if (pid == 0) {
        alarm(10);
        return 0;
    }
    if (pid < 0) {
        perror("fork normal-return child");
    } else {
        child_status = wait_for_child(pid);
        normal_return_child_ok = child_status == 0;
        if (!normal_return_child_ok) {
            fprintf(stderr, "normal_return_fork_child_status=%d\n", child_status);
        }
    }

    reader_release();
    if (join_test_thread(checkpoint_thread, "checkpoint reader") != 0) {
        return 198;
    }
    if (!checkpoint_child_ok || !failed_exec_child_ok ||
        !normal_return_child_ok) {
        return 199;
    }
    printf("fork_child_checkpoint_skipped=1 failed_exec_exit=1 "
           "normal_return_exit=1 parent_pid=%ld\n",
           (long)getpid());
    return 0;
}

int
main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <mode>\n", argv[0]);
        return 2;
    }
    if (argc >= 3 && strcmp(argv[argc - 1], CONTROLLER_QUIESCE_ARG) == 0) {
        quiesce_controller_for_required_checkpoint();
    }

    if (strcmp(argv[1], "child-basic") == 0) {
        return run_child_basic();
    }
    if (strcmp(argv[1], "child-clone-private-success") == 0) {
        puts("clone_private_failed_exec_enoent=1");
        return run_child_basic();
    }
    if (strcmp(argv[1], "child-print-ld") == 0) {
        return run_child_print_ld();
    }
    if (strcmp(argv[1], "child-check-env") == 0 && argc >= 3) {
        return run_child_check_env(argv[2]);
    }
    if (strcmp(argv[1], "child-stderr-sentinel") == 0) {
        return run_child_stderr_sentinel();
    }
    if (strcmp(argv[1], "execv-success") == 0) {
        return run_execv_success(argv[0]);
    }
    if (strcmp(argv[1], "signal-handler-execve") == 0) {
        return run_signal_handler_execve_default_bypass();
    }
    if (strcmp(argv[1], "default-disabled-family-bypass") == 0) {
        return run_default_disabled_family_bypass();
    }
    if (strcmp(argv[1], "execv-zero-call") == 0) {
        return run_execv_zero_call_checkpoint(argv[0]);
    }
    if (strcmp(argv[1], "execv-write-target-success") == 0) {
        return run_execv_write_target_success(argv[0]);
    }
    if (strcmp(argv[1], "execve-custom-env") == 0) {
        return run_execve_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "execve-call-env-optin-default-bypass") == 0) {
        return run_execve_call_env_optin_default_bypass(argv[0]);
    }
    if (strcmp(argv[1], "raw-syscall-execve-custom-env") == 0) {
        return run_raw_syscall_execve_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "raw-syscall-execve-failure") == 0) {
        return run_raw_syscall_execve_failure();
    }
    if (strcmp(argv[1], "raw-syscall-execve-observer-bypass") == 0) {
        return run_raw_syscall_execve_observer_bypass();
    }
    if (strcmp(argv[1], "raw-syscall-nonexec") == 0) {
        return run_raw_syscall_nonexec_passthrough();
    }
    if (strcmp(argv[1], "execve-child-peak-env-only") == 0) {
        return run_execve_child_peak_env_only(argv[0]);
    }
    if (strcmp(argv[1], "execve-null-env") == 0) {
        return run_execve_null_env(argv[0]);
    }
    if (strcmp(argv[1], "execve-loader-path-missing") == 0) {
        return run_execve_loader_path_env(argv[0], "loader-path-missing",
                                          NULL, NULL, 0);
    }
    if (strcmp(argv[1], "execve-loader-path-explicit") == 0) {
        return run_execve_loader_path_env(argv[0], "loader-path-explicit",
                                          env_or_default(
                                              "EXEC_CHAIN_TEST_CHILD_LOADER_PATH",
                                              "/tmp/child-loader"),
                                          NULL, 0);
    }
    if (strcmp(argv[1], "execve-loader-path-empty") == 0) {
        return run_execve_loader_path_env(argv[0], "loader-path-empty",
                                          "", NULL, 0);
    }
    if (strcmp(argv[1], "execve-loader-path-duplicate") == 0) {
        return run_execve_loader_path_env(
            argv[0], "loader-path-duplicate",
            env_or_default("EXEC_CHAIN_TEST_CHILD_LOADER_PATH", "/tmp/child-loader"),
            env_or_default("EXEC_CHAIN_TEST_CHILD_LOADER_PATH_SECOND",
                           "/tmp/child-loader-second"),
            0);
    }
    if (strcmp(argv[1], "execve-loader-path-chain-disabled") == 0) {
        return run_execve_loader_path_env(argv[0], "loader-path-disabled",
                                          NULL, NULL, 1);
    }
    if (strcmp(argv[1], "execve-loader-path-secure") == 0) {
        return run_execve_loader_path_env(argv[0], "loader-path-secure",
                                          NULL, NULL, 0);
    }
    if (strcmp(argv[1], "execve-loader-path-preload-present") == 0) {
        return run_execve_preloaded_env(argv[0]);
    }
    if (strcmp(argv[1], "execve-large-env") == 0) {
        return run_execve_large_env(argv[0]);
    }
    if (strcmp(argv[1], "execve-explicit-peak-env") == 0) {
        return run_execve_explicit_peak_env(argv[0]);
    }
    if (strcmp(argv[1], "execve-child-chain-disabled") == 0) {
        return run_execve_child_peak_control(argv[0], "PEAK_EXEC_CHAIN", "0");
    }
    if (strcmp(argv[1], "execve-child-checkpoint-disabled") == 0) {
        return run_execve_child_peak_control(argv[0],
                                            "PEAK_EXEC_CHECKPOINT",
                                            "0");
    }
    if (strcmp(argv[1], "execve-child-propagate-disabled") == 0) {
        return run_execve_child_peak_control(argv[0],
                                            "PEAK_EXEC_PROPAGATE_PEAK_ENV",
                                            "0");
    }
    if (strcmp(argv[1], "execve-bad-env") == 0) {
        return run_execve_bad_env_failure();
    }
    if (strcmp(argv[1], "execve-bad-argv") == 0) {
        return run_execve_bad_argv_failure();
    }
    if (strcmp(argv[1], "exec-failure") == 0) {
        return run_exec_failure();
    }
    if (strcmp(argv[1], "fexecve-custom-env") == 0) {
        return run_fexecve_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "fexecve-bad-env") == 0) {
        return run_fexecve_bad_env_failure(argv[0]);
    }
    if (strcmp(argv[1], "fexecve-bad-argv") == 0) {
        return run_fexecve_bad_argv_failure(argv[0]);
    }
#if defined(PEAK_TEST_HAVE_EXECVEAT)
    if (strcmp(argv[1], "execveat-custom-env") == 0) {
        return run_execveat_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "raw-syscall-execveat-custom-env") == 0) {
        return run_raw_syscall_execveat_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "execveat-bad-env") == 0) {
        return run_execveat_bad_env_failure();
    }
    if (strcmp(argv[1], "execveat-bad-argv") == 0) {
        return run_execveat_bad_argv_failure();
    }
#endif
    if (strcmp(argv[1], "execl-success") == 0) {
        return run_execl_success(argv[0]);
    }
    if (strcmp(argv[1], "execlp-path-search") == 0) {
        return run_execlp_path_search();
    }
    if (strcmp(argv[1], "execle-custom-env") == 0) {
        return run_execle_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "execvp-path-search") == 0) {
        return run_execvp_path_search();
    }
    if (strcmp(argv[1], "helper-named-exec") == 0) {
        return run_helper_named_exec();
    }
    if (strcmp(argv[1], "execvpe-child-env-path-ignored") == 0) {
        return run_execvpe_child_env_path_ignored();
    }
    if (strcmp(argv[1], "execvpe-child-env-path-used") == 0) {
        return run_execvpe_child_env_path_used();
    }
    if (strcmp(argv[1], "execvpe-child-path-duplicate-preload") == 0) {
        return run_execvpe_child_path_duplicate_preload();
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
    if (strcmp(argv[1], "fork-child-exec-parent-exec") == 0) {
        return run_fork_child_exec_parent_exec(argv[0]);
    }
    if (strcmp(argv[1], "vfork-child-exec-parent-exec") == 0) {
        return run_vfork_child_exec_parent_exec(argv[0]);
    }
    if (strcmp(argv[1], "vfork-child-failed-exec-parent-exec") == 0) {
        return run_vfork_child_failed_exec_parent_exec(argv[0]);
    }
    if (strcmp(argv[1], "clone-private-vm-exec") == 0) {
        return run_libc_clone_exec(argv[0], 0);
    }
    if (strcmp(argv[1], "clone-vm-exec") == 0) {
        return run_libc_clone_exec(argv[0], CLONE_VM);
    }
    if (strcmp(argv[1], "clone-vfork-exec") == 0) {
        return run_libc_clone_exec(argv[0], CLONE_VM | CLONE_VFORK);
    }
#if defined(SYS_clone)
    if (strcmp(argv[1], "raw-clone-libc-exec") == 0) {
        return run_raw_clone_exec(argv[0], 0);
    }
    if (strcmp(argv[1], "raw-clone-raw-exec") == 0) {
        return run_raw_clone_exec(argv[0], 1);
    }
#endif
#if defined(PEAK_TEST_HAVE_SYS_CLONE3) && defined(SYS_clone3)
    if (strcmp(argv[1], "clone3-exec") == 0) {
        return run_clone3_exec(argv[0]);
    }
#endif
    if (strcmp(argv[1], "vfork-execl-parent-exec") == 0) {
        return run_vfork_varargs_exec_parent_exec(argv[0], 0);
    }
    if (strcmp(argv[1], "vfork-execlp-parent-exec") == 0) {
        return run_vfork_varargs_exec_parent_exec(argv[0], 1);
    }
    if (strcmp(argv[1], "vfork-execle-parent-exec") == 0) {
        return run_vfork_varargs_exec_parent_exec(argv[0], 2);
    }
    if (strcmp(argv[1], "fork-custom-env-execve") == 0) {
        return run_postfork_custom_env(argv[0], 0, 0, 0, 0, 0);
    }
    if (strcmp(argv[1], "vfork-custom-env-execve") == 0) {
        return run_postfork_custom_env(argv[0], 1, 0, 0, 0, 0);
    }
    if (strcmp(argv[1], "vfork-custom-env-execle") == 0) {
        return run_postfork_custom_env(argv[0], 1, 1, 0, 0, 0);
    }
    if (strcmp(argv[1], "fork-custom-env-disabled") == 0) {
        return run_postfork_custom_env(argv[0], 0, 0, 1, 0, 0);
    }
    if (strcmp(argv[1], "vfork-custom-env-disabled") == 0) {
        return run_postfork_custom_env(argv[0], 1, 0, 1, 0, 0);
    }
    if (strcmp(argv[1], "fork-raw-syscall-custom-env") == 0) {
        return run_postfork_custom_env(argv[0], 0, 0, 0, 1, 0);
    }
    if (strcmp(argv[1], "fork-raw-syscall-chain-disabled") == 0) {
        return run_postfork_custom_env(argv[0], 0, 0, 1, 1, 0);
    }
    if (strcmp(argv[1], "fork-loader-path") == 0) {
        return run_postfork_custom_env(argv[0], 0, 0, 0, 0, 1);
    }
    if (strcmp(argv[1], "vfork-loader-path") == 0) {
        return run_postfork_custom_env(argv[0], 1, 0, 0, 0, 1);
    }
    if (strcmp(argv[1], "fork-loader-path-secure") == 0) {
        return run_postfork_custom_env(argv[0], 0, 0, 0, 0, 1);
    }
    if (strcmp(argv[1], "vfork-loader-path-secure") == 0) {
        return run_postfork_custom_env(argv[0], 1, 0, 0, 0, 1);
    }
    if (strcmp(argv[1], "vfork-loader-path-preload-present") == 0) {
        return run_vfork_preloaded_env(argv[0]);
    }
    if (strcmp(argv[1], "fork-parent-env-exhaustion") == 0) {
        return run_fork_parent_env_exhaustion(argv[0]);
    }
    if (strcmp(argv[1], "vfork-custom-env-execvpe") == 0) {
        return run_vfork_custom_env_api(argv[0], POSTFORK_EXECVPE);
    }
    if (strcmp(argv[1], "vfork-custom-env-execvp") == 0) {
        return run_vfork_custom_env_api(argv[0], POSTFORK_EXECVP);
    }
    if (strcmp(argv[1], "vfork-execvp-native-fallback") == 0) {
        return run_vfork_custom_env_api(argv[0], POSTFORK_EXECVP);
    }
    if (strcmp(argv[1], "vfork-custom-env-execlp") == 0) {
        return run_vfork_custom_env_api(argv[0], POSTFORK_EXECLP);
    }
    if (strcmp(argv[1], "vfork-custom-env-fexecve") == 0) {
        return run_vfork_custom_env_api(argv[0], POSTFORK_FEXECVE);
    }
#if defined(PEAK_TEST_HAVE_EXECVEAT)
    if (strcmp(argv[1], "vfork-custom-env-execveat") == 0) {
        return run_vfork_custom_env_api(argv[0], POSTFORK_EXECVEAT);
    }
    if (strcmp(argv[1], "vfork-raw-syscall-execveat") == 0) {
        return run_vfork_custom_env_api(argv[0], POSTFORK_RAW_EXECVEAT);
    }
#endif
    if (strcmp(argv[1], "fork-bad-env-vector") == 0) {
        return run_postfork_bad_env(argv[0], 0, 0);
    }
    if (strcmp(argv[1], "vfork-bad-env-vector") == 0) {
        return run_postfork_bad_env(argv[0], 1, 0);
    }
    if (strcmp(argv[1], "fork-bad-env-string") == 0) {
        return run_postfork_bad_env(argv[0], 0, 1);
    }
    if (strcmp(argv[1], "vfork-bad-env-string") == 0) {
        return run_postfork_bad_env(argv[0], 1, 1);
    }
    if (strcmp(argv[1], "vfork-env-slots-fallback") == 0) {
        return run_vfork_child_env_capacity_fallback(argv[0], 0);
    }
    if (strcmp(argv[1], "vfork-long-preload-fallback") == 0) {
        return run_vfork_child_env_capacity_fallback(argv[0], 1);
    }
    if (strcmp(argv[1], "vfork-preload-entries-fallback") == 0) {
        return run_vfork_preload_entries_fallback(argv[0]);
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
    if (strcmp(argv[1], "posix-spawn-custom-env") == 0) {
        return run_posix_spawn_custom_env(argv[0]);
    }
    if (strcmp(argv[1], "posix-spawn-call-env-optin-default-bypass") == 0) {
        return run_posix_spawn_call_env_optin_default_bypass(argv[0]);
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
    if (strcmp(argv[1], "posix-spawn-failure") == 0) {
        return run_posix_spawn_failure(argv[0]);
    }
    if (strcmp(argv[1], "posix-spawnp-path-search") == 0) {
        return run_posix_spawnp_path_search();
    }
    if (strcmp(argv[1], "posix-spawnp-custom-env") == 0) {
        return run_posix_spawnp_custom_env();
    }
    if (strcmp(argv[1], "posix-spawn-resolver-null") == 0) {
        return run_posix_spawn_resolver_null(argv[0], 0);
    }
    if (strcmp(argv[1], "posix-spawnp-resolver-null") == 0) {
        return run_posix_spawn_resolver_null(argv[0], 1);
    }
    if (strcmp(argv[1], "posix-spawnp-child-env-path-ignored") == 0) {
        return run_posix_spawnp_child_env_path_ignored();
    }
    if (strcmp(argv[1], "posix-spawnp-bad-env") == 0) {
        return run_posix_spawnp_bad_env();
    }
    if (strcmp(argv[1], "posix-spawnp-bad-argv") == 0) {
        return run_posix_spawnp_bad_argv();
    }
    if (strcmp(argv[1], "posix-spawn-explicit-peak-env") == 0) {
        return run_posix_spawn_explicit_peak_env(argv[0]);
    }
    if (strcmp(argv[1], "exec-concurrent-fini-callbacks") == 0) {
        return run_exec_checkpoint_concurrent_fini_callbacks(argv[0]);
    }
    if (strcmp(argv[1], "exec-checkpoint-snapshot-lock-contention") == 0) {
        return run_exec_checkpoint_snapshot_lock_contention(argv[0]);
    }
    if (strcmp(argv[1], "exec-checkpoint-statistics-snapshot-contention") == 0) {
        return run_exec_checkpoint_statistics_snapshot_contention(argv[0]);
    }
    if (strcmp(argv[1], "exec-checkpoint-statistics-shadow-invalidation") == 0) {
        return run_exec_checkpoint_statistics_shadow_invalidation(argv[0]);
    }
    if (strcmp(argv[1], "exec-checkpoint-direct-disabled") == 0) {
        return run_exec_checkpoint_direct_disabled(argv[0]);
    }
    if (strcmp(argv[1], "exec-checkpoint-fork-child-fini") == 0) {
        return run_exec_checkpoint_fork_child_fini(argv[0]);
    }
    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
