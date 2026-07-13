#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

enum PeakPreinitCase {
    PEAK_PREINIT_EXECVE,
    PEAK_PREINIT_EXECV,
    PEAK_PREINIT_EXECL,
    PEAK_PREINIT_EXECLE,
    PEAK_PREINIT_EXECVP,
    PEAK_PREINIT_EXECVP_EACCES_ELOOP,
    PEAK_PREINIT_EXECLP,
    PEAK_PREINIT_EXECVPE,
    PEAK_PREINIT_FEXECVE,
    PEAK_PREINIT_EXECVEAT,
    PEAK_PREINIT_RAW_EXECVE,
    PEAK_PREINIT_RAW_EXECVEAT,
    PEAK_PREINIT_POSIX_SPAWN,
    PEAK_PREINIT_POSIX_SPAWNP,
    PEAK_PREINIT_CASE_COUNT,
};

typedef struct {
    const char* name;
    int result;
    int error_number;
} PeakPreinitResult;

static PeakPreinitResult peak_preinit_results[PEAK_PREINIT_CASE_COUNT] = {
    [PEAK_PREINIT_EXECVE] = {"execve", 0, 0},
    [PEAK_PREINIT_EXECV] = {"execv", 0, 0},
    [PEAK_PREINIT_EXECL] = {"execl", 0, 0},
    [PEAK_PREINIT_EXECLE] = {"execle", 0, 0},
    [PEAK_PREINIT_EXECVP] = {"execvp", 0, 0},
    [PEAK_PREINIT_EXECVP_EACCES_ELOOP] = {
        "execvp_eacces_eloop", 0, 0
    },
    [PEAK_PREINIT_EXECLP] = {"execlp", 0, 0},
#if defined(PEAK_TEST_HAVE_EXECVPE)
    [PEAK_PREINIT_EXECVPE] = {"execvpe", 0, 0},
#endif
#if defined(PEAK_TEST_HAVE_FEXECVE)
    [PEAK_PREINIT_FEXECVE] = {"fexecve", 0, 0},
#endif
#if defined(PEAK_TEST_HAVE_EXECVEAT)
    [PEAK_PREINIT_EXECVEAT] = {"execveat", 0, 0},
#endif
#if defined(PEAK_TEST_EXEC_RAW_SYSCALL_SUPPORTED)
    [PEAK_PREINIT_RAW_EXECVE] = {"raw_execve", 0, 0},
#if defined(PEAK_TEST_HAVE_EXECVEAT)
    [PEAK_PREINIT_RAW_EXECVEAT] = {"raw_execveat", 0, 0},
#endif
#endif
    [PEAK_PREINIT_POSIX_SPAWN] = {"posix_spawn", 0, 0},
    [PEAK_PREINIT_POSIX_SPAWNP] = {"posix_spawnp", 0, 0},
};

static char* const peak_preinit_argv[] = {
    (char*)"peak-preinit-command-does-not-exist",
    NULL,
};
static char* const peak_preinit_envp[] = {NULL};
static char* const peak_preinit_eacces_eloop_argv[] = {
    (char*)"peak-preinit-eacces-eloop",
    NULL,
};
static PeakPreinitResult peak_resolver_reentry = {
    "execve", 0, 0
};
static PeakPreinitResult peak_spawn_resolver_reentry[] = {
    {"posix_spawn", 0, 0},
    {"posix_spawnp", 0, 0},
};
static PeakPreinitResult peak_spawn_publication_reentry[] = {
    {"posix_spawn", 0, 0},
    {"posix_spawnp", 0, 0},
};
static pthread_mutex_t peak_spawn_publication_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t peak_spawn_publication_cond = PTHREAD_COND_INITIALIZER;
static pthread_t peak_spawn_publication_workers[2];
static int peak_spawn_publication_worker_created[2];
static int peak_spawn_publication_waiter_entered[2];
static int peak_spawn_publication_worker_done[2];
static int peak_spawn_publication_waited[2];
static int peak_spawn_raw_fork_child_ok[] = {0, 0};
static int peak_resolver_raw_fork_child_ok;

static pid_t
peak_test_raw_fork(void)
{
#if defined(SYS_fork) && !defined(PEAK_TEST_FORCE_CLONE_RAW_FORK)
    return (pid_t)syscall(SYS_fork);
#elif defined(SYS_clone)
    /* SIGCHLD without CLONE_VM gives fork-like private address-space semantics. */
    return (pid_t)syscall(SYS_clone,
                          (unsigned long)SIGCHLD,
                          NULL,
                          NULL,
                          NULL,
                          0UL);
#else
    errno = ENOSYS;
    return -1;
#endif
}

static void
peak_preinit_record(enum PeakPreinitCase preinit_case,
                    int result,
                    int error_number)
{
    peak_preinit_results[preinit_case].result = result;
    peak_preinit_results[preinit_case].error_number = error_number;
}

static void
peak_exec_from_preinit(int argc, char** argv, char** envp)
{
    static const char missing_path[] =
        "/definitely/missing/peak-preinit-exec";
    static const char missing_command[] =
        "peak-preinit-command-does-not-exist";
    pid_t pid = -1;
    int result;
    int saved_errno;

    (void)argc;
    (void)argv;
    environ = envp;

    errno = EALREADY;
    result = execve(missing_path, peak_preinit_argv, peak_preinit_envp);
    saved_errno = errno;
    peak_preinit_record(PEAK_PREINIT_EXECVE, result, saved_errno);

    errno = EALREADY;
    result = execv(missing_path, peak_preinit_argv);
    saved_errno = errno;
    peak_preinit_record(PEAK_PREINIT_EXECV, result, saved_errno);

    errno = EALREADY;
    result = execl(missing_path, missing_command, (char*)NULL);
    saved_errno = errno;
    peak_preinit_record(PEAK_PREINIT_EXECL, result, saved_errno);

    errno = EALREADY;
    result = execle(missing_path,
                    missing_command,
                    (char*)NULL,
                    peak_preinit_envp);
    saved_errno = errno;
    peak_preinit_record(PEAK_PREINIT_EXECLE, result, saved_errno);

    errno = EALREADY;
    result = execvp(missing_command, peak_preinit_argv);
    saved_errno = errno;
    peak_preinit_record(PEAK_PREINIT_EXECVP, result, saved_errno);

    errno = EALREADY;
    result = execvp("peak-preinit-eacces-eloop",
                    peak_preinit_eacces_eloop_argv);
    saved_errno = errno;
    peak_preinit_record(
        PEAK_PREINIT_EXECVP_EACCES_ELOOP, result, saved_errno);

    errno = EALREADY;
    result = execlp(missing_command, missing_command, (char*)NULL);
    saved_errno = errno;
    peak_preinit_record(PEAK_PREINIT_EXECLP, result, saved_errno);

#if defined(PEAK_TEST_HAVE_EXECVPE)
    errno = EALREADY;
    result = execvpe(missing_command, peak_preinit_argv, peak_preinit_envp);
    saved_errno = errno;
    peak_preinit_record(PEAK_PREINIT_EXECVPE, result, saved_errno);
#endif

#if defined(PEAK_TEST_HAVE_FEXECVE)
    errno = EALREADY;
    result = fexecve(INT_MAX, peak_preinit_argv, peak_preinit_envp);
    saved_errno = errno;
    peak_preinit_record(PEAK_PREINIT_FEXECVE, result, saved_errno);
#endif

#if defined(PEAK_TEST_HAVE_EXECVEAT)
    errno = EALREADY;
    result = execveat(AT_FDCWD,
                      missing_path,
                      peak_preinit_argv,
                      peak_preinit_envp,
                      0);
    saved_errno = errno;
    peak_preinit_record(PEAK_PREINIT_EXECVEAT, result, saved_errno);
#endif

#if defined(PEAK_TEST_EXEC_RAW_SYSCALL_SUPPORTED)
    errno = EALREADY;
    result = (int)syscall(SYS_execve,
                          missing_path,
                          peak_preinit_argv,
                          peak_preinit_envp);
    saved_errno = errno;
    peak_preinit_record(PEAK_PREINIT_RAW_EXECVE, result, saved_errno);
#if defined(PEAK_TEST_HAVE_EXECVEAT)
    errno = EALREADY;
    result = (int)syscall(SYS_execveat,
                          AT_FDCWD,
                          missing_path,
                          peak_preinit_argv,
                          peak_preinit_envp,
                          0);
    saved_errno = errno;
    peak_preinit_record(PEAK_PREINIT_RAW_EXECVEAT, result, saved_errno);
#endif
#endif

    errno = EALREADY;
    result = posix_spawn(&pid,
                         missing_path,
                         NULL,
                         NULL,
                         peak_preinit_argv,
                         peak_preinit_envp);
    saved_errno = errno;
    peak_preinit_record(PEAK_PREINIT_POSIX_SPAWN, result, saved_errno);

    errno = EALREADY;
    result = posix_spawnp(&pid,
                          missing_command,
                          NULL,
                          NULL,
                          peak_preinit_argv,
                          peak_preinit_envp);
    saved_errno = errno;
    peak_preinit_record(PEAK_PREINIT_POSIX_SPAWNP, result, saved_errno);
}

__attribute__((visibility("default"), used))
void
peak_test_exec_resolver_reentry_hook(void)
{
    static const char missing_path[] =
        "/definitely/missing/peak-resolver-reentry-exec";
    pid_t pid;
    pid_t waited;
    int status;

    errno = EALREADY;
    peak_resolver_reentry.result =
        execve(missing_path, peak_preinit_argv, peak_preinit_envp);
    peak_resolver_reentry.error_number = errno;

    pid = peak_test_raw_fork();
    if (pid == 0) {
        alarm(2);
        errno = EALREADY;
        if (execve(missing_path,
                   peak_preinit_argv,
                   peak_preinit_envp) == -1 && errno == ENOENT) {
            _exit(0);
        }
        _exit(1);
    }
    if (pid < 0) {
        return;
    }
    status = 0;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    peak_resolver_raw_fork_child_ok =
        waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

__attribute__((visibility("default"), used))
void
peak_test_exec_spawn_resolver_reentry_hook(int path_search)
{
    static const char missing_path[] =
        "/definitely/missing/peak-spawn-resolver-reentry";
    PeakPreinitResult* result = &peak_spawn_resolver_reentry[path_search != 0];
    pid_t pid = -1;

    errno = EALREADY;
    if (path_search) {
        result->result = posix_spawnp(&pid,
                                      "peak-spawn-resolver-reentry",
                                      NULL,
                                      NULL,
                                      peak_preinit_argv,
                                      peak_preinit_envp);
    } else {
        result->result = posix_spawn(&pid,
                                     missing_path,
                                     NULL,
                                     NULL,
                                     peak_preinit_argv,
                                     peak_preinit_envp);
    }
    result->error_number = errno;
}

typedef struct {
    int path_search;
    PeakPreinitResult result;
} PeakSpawnPublicationThread;

static void*
peak_spawn_publication_thread(void* opaque)
{
    PeakSpawnPublicationThread* thread = opaque;
    pid_t pid = -1;

    errno = EALREADY;
    if (thread->path_search) {
        thread->result.result = posix_spawnp(&pid,
                                             "peak-publication-reentry",
                                             NULL,
                                             NULL,
                                             peak_preinit_argv,
                                             peak_preinit_envp);
    } else {
        thread->result.result = posix_spawn(&pid,
                                            "/definitely/missing/peak-publication-reentry",
                                            NULL,
                                            NULL,
                                            peak_preinit_argv,
                                            peak_preinit_envp);
    }
    thread->result.error_number = errno;
    (void)pthread_mutex_lock(&peak_spawn_publication_lock);
    peak_spawn_publication_reentry[thread->path_search] = thread->result;
    peak_spawn_publication_worker_done[thread->path_search] = 1;
    (void)pthread_cond_broadcast(&peak_spawn_publication_cond);
    (void)pthread_mutex_unlock(&peak_spawn_publication_lock);
    return NULL;
}

__attribute__((visibility("default"), used))
void
peak_test_exec_spawn_waiter_hook(int path_search)
{
    int index = path_search != 0;

    (void)pthread_mutex_lock(&peak_spawn_publication_lock);
    peak_spawn_publication_waiter_entered[index] = 1;
    (void)pthread_cond_broadcast(&peak_spawn_publication_cond);
    (void)pthread_mutex_unlock(&peak_spawn_publication_lock);
}

__attribute__((visibility("default"), used))
void
peak_test_exec_spawn_publication_hook(int path_search)
{
    int index = path_search != 0;
    int pipefd[2];
    pid_t pid;
    pid_t waited;
    int status = 0;
    int child_result[2] = {0, 0};

    peak_spawn_publication_reentry[index] = (PeakPreinitResult){
        path_search ? "posix_spawnp" : "posix_spawn", 0, 0};
    peak_spawn_publication_waiter_entered[index] = 0;
    peak_spawn_publication_worker_done[index] = 0;
    peak_spawn_publication_waited[index] = 0;
    {
        static PeakSpawnPublicationThread threads[2];

        threads[index] = (PeakSpawnPublicationThread){
            .path_search = index,
            .result = {path_search ? "posix_spawnp" : "posix_spawn", 0, 0},
        };
        if (pthread_create(&peak_spawn_publication_workers[index],
                           NULL,
                           peak_spawn_publication_thread,
                           &threads[index]) == 0) {
            peak_spawn_publication_worker_created[index] = 1;
            (void)pthread_mutex_lock(&peak_spawn_publication_lock);
            while (!peak_spawn_publication_waiter_entered[index] &&
                   !peak_spawn_publication_worker_done[index]) {
                (void)pthread_cond_wait(&peak_spawn_publication_cond,
                                        &peak_spawn_publication_lock);
            }
            peak_spawn_publication_waited[index] =
                peak_spawn_publication_waiter_entered[index] &&
                !peak_spawn_publication_worker_done[index];
            (void)pthread_mutex_unlock(&peak_spawn_publication_lock);
        }
    }

    if (pipe(pipefd) != 0) {
        return;
    }
    pid = peak_test_raw_fork();
    if (pid == 0) {
        pid_t child_pid = -1;

        (void)close(pipefd[0]);
        errno = EALREADY;
        child_result[0] = path_search ?
            posix_spawnp(&child_pid,
                         "peak-raw-fork-resolver",
                         NULL,
                         NULL,
                         peak_preinit_argv,
                         peak_preinit_envp) :
            posix_spawn(&child_pid,
                        "/definitely/missing/peak-raw-fork-resolver",
                        NULL,
                        NULL,
                        peak_preinit_argv,
                        peak_preinit_envp);
        child_result[1] = errno;
        (void)write(pipefd[1], child_result, sizeof(child_result));
        _exit(0);
    }
    (void)close(pipefd[1]);
    if (pid > 0 && read(pipefd[0], child_result, sizeof(child_result)) ==
                       (ssize_t)sizeof(child_result)) {
        do {
            waited = waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
        peak_spawn_raw_fork_child_ok[path_search != 0] =
            waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
            child_result[0] == ENOSYS && child_result[1] == EALREADY;
    }
    (void)close(pipefd[0]);
}

__attribute__((section(".preinit_array"), used))
static void (*const peak_exec_preinit_entry)(int, char**, char**) =
    peak_exec_from_preinit;

int
main(void)
{
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    printf("resolver_reentry_case=%s result=%d errno=%d\n",
           peak_resolver_reentry.name,
           peak_resolver_reentry.result,
           peak_resolver_reentry.error_number);
    for (size_t index = 0;
         index < sizeof(peak_spawn_resolver_reentry) /
                     sizeof(peak_spawn_resolver_reentry[0]);
         index++) {
        const PeakPreinitResult* result = &peak_spawn_resolver_reentry[index];

        printf("spawn_resolver_reentry_case=%s result=%d errno=%d\n",
               result->name, result->result, result->error_number);
    }
    printf("resolver_raw_fork_child_ok=%d\n",
           peak_resolver_raw_fork_child_ok);
    for (size_t index = 0;
         index < sizeof(peak_spawn_publication_workers) /
                     sizeof(peak_spawn_publication_workers[0]);
         index++) {
        if (peak_spawn_publication_worker_created[index]) {
            (void)pthread_join(peak_spawn_publication_workers[index], NULL);
        }
    }
    for (size_t index = 0; index < PEAK_PREINIT_CASE_COUNT; index++) {
        const PeakPreinitResult* result = &peak_preinit_results[index];

        if (result->name != NULL) {
            printf("preinit_case=%s result=%d errno=%d\n",
                   result->name,
                   result->result,
                   result->error_number);
        }
    }
    for (size_t index = 0;
         index < sizeof(peak_spawn_publication_reentry) /
                     sizeof(peak_spawn_publication_reentry[0]);
         index++) {
        const PeakPreinitResult* result = &peak_spawn_publication_reentry[index];

        printf("spawn_publication_waited_case=%s ok=%d\n",
               result->name, peak_spawn_publication_waited[index]);

        printf("spawn_publication_reentry_case=%s result=%d errno=%d\n",
               result->name, result->result, result->error_number);
        printf("spawn_raw_fork_child_case=%s ok=%d\n",
               result->name, peak_spawn_raw_fork_child_ok[index]);
    }
    return 0;
}
