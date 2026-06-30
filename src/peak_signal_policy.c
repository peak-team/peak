#define _GNU_SOURCE
#include "peak_signal_policy_internal.h"
#include "utils/utils.h"

#include <aio.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <features.h>
#include <fcntl.h>
#include <limits.h>
#include <mqueue.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

static _Atomic int reserved_signal;
static _Atomic int conflict_count;
static _Atomic int migration_count;
static _Atomic int migration_disabled_depth;
static _Atomic int migration_candidate_signal;
static _Atomic int migration_releasing_signal;
static _Atomic int unexpected_delivery_count;
static _Atomic(uintptr_t) last_conflict_api;
static _Atomic(unsigned long) cookie_base;
static __thread int internal_depth;
static pthread_once_t cookie_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t migration_mutex = PTHREAD_MUTEX_INITIALIZER;

static int (*real_sigaction_fn)(int, const struct sigaction*, struct sigaction*);
static void (*(*real_signal_fn)(int, void (*)(int)))(int);
static int (*real_pthread_sigmask_fn)(int, const sigset_t*, sigset_t*);
static int (*real_sigprocmask_fn)(int, const sigset_t*, sigset_t*);
static int (*real_sigwait_fn)(const sigset_t*, int*);
static int (*real_sigwaitinfo_fn)(const sigset_t*, siginfo_t*);
static int (*real_sigtimedwait_fn)(const sigset_t*, siginfo_t*, const struct timespec*);
static int (*real_signalfd_fn)(int, const sigset_t*, int);
static int (*real_signalfd4_fn)(int, const sigset_t*, int, int);
static int (*real_timer_create_fn)(clockid_t, struct sigevent*, timer_t*);
static int (*real_mq_notify_fn)(mqd_t, const struct sigevent*);
static int (*real_aio_read_fn)(struct aiocb*);
static int (*real_aio_write_fn)(struct aiocb*);
static int (*real_aio_fsync_fn)(int, struct aiocb*);
static int (*real_lio_listio_fn)(int,
                                 struct aiocb* const[],
                                 int,
                                 struct sigevent*);
static int (*real_kill_fn)(pid_t, int);
static int (*real_pthread_kill_fn)(pthread_t, int);
static int (*real_sigqueue_fn)(pid_t, int, const union sigval);
static int (*real_raise_fn)(int);
static int (*real_sigsuspend_fn)(const sigset_t*);
static int (*real_pselect_fn)(int,
                              fd_set*,
                              fd_set*,
                              fd_set*,
                              const struct timespec*,
                              const sigset_t*);
static int (*real_ppoll_fn)(struct pollfd*,
                            nfds_t,
                            const struct timespec*,
                            const sigset_t*);
static long (*real_syscall_fn)(long, ...);
static pthread_once_t real_symbols_once = PTHREAD_ONCE_INIT;

extern int peak_exec_handle_syscall(long number,
                                    long a1,
                                    long a2,
                                    long a3,
                                    long a4,
                                    long a5,
                                    long a6,
                                    long* result_out)
    __attribute__((weak));

static int peak_signal_policy_parse_signal_env(const char* value, int* out);
static int peak_signal_policy_signal_is_available(int signum);
static void peak_signal_policy_init_cookie(void);
static int peak_signal_policy_env_forces_signal(void);
static int peak_signal_policy_install_protective_handler(int signum);
static int peak_signal_policy_commit_reserved_signal(int signum);
static int peak_signal_policy_restore_default_handler(int signum);
static int peak_signal_policy_wait_until_migration_enabled(void);
static int peak_signal_policy_safe_read(void* dst,
                                        const void* src,
                                        size_t size);
static int peak_signal_policy_safe_write(void* dst,
                                         const void* src,
                                         size_t size);
static int peak_signal_policy_range_is_writable(const void* ptr, size_t size);

static void*
peak_signal_policy_resolve(const char* name)
{
    return dlsym(RTLD_NEXT, name);
}

static void*
peak_signal_policy_resolve_timer_create(void)
{
#ifdef __GLIBC__
    /*
     * Older glibc exports timer_create@GLIBC_2.2.5 with the raw kernel
     * timer-id ABI and timer_create@@GLIBC_2.3.3 with the user-facing
     * pointer timer_t ABI.  dlsym(RTLD_NEXT, ...) may pick the compat ABI
     * from an unversioned interposer, which corrupts timer_t for callers that
     * later use the default timer_delete ABI.
     */
    void* symbol = dlvsym(RTLD_NEXT, "timer_create", "GLIBC_2.3.3");
    if (symbol != NULL) {
        return symbol;
    }
#endif
    return peak_signal_policy_resolve("timer_create");
}

static void
peak_signal_policy_resolve_real_symbols(void)
{
    real_sigaction_fn =
        (int (*)(int, const struct sigaction*, struct sigaction*))
            peak_signal_policy_resolve("sigaction");
    real_signal_fn =
        (void (*(*)(int, void (*)(int)))(int))
            peak_signal_policy_resolve("signal");
    real_pthread_sigmask_fn =
        (int (*)(int, const sigset_t*, sigset_t*))
            peak_signal_policy_resolve("pthread_sigmask");
    real_sigprocmask_fn =
        (int (*)(int, const sigset_t*, sigset_t*))
            peak_signal_policy_resolve("sigprocmask");
    real_sigwait_fn =
        (int (*)(const sigset_t*, int*))peak_signal_policy_resolve("sigwait");
    real_sigwaitinfo_fn =
        (int (*)(const sigset_t*, siginfo_t*))
            peak_signal_policy_resolve("sigwaitinfo");
    real_sigtimedwait_fn =
        (int (*)(const sigset_t*, siginfo_t*, const struct timespec*))
            peak_signal_policy_resolve("sigtimedwait");
    real_signalfd_fn =
        (int (*)(int, const sigset_t*, int))
            peak_signal_policy_resolve("signalfd");
    real_signalfd4_fn =
        (int (*)(int, const sigset_t*, int, int))
            peak_signal_policy_resolve("signalfd4");
    real_timer_create_fn =
        (int (*)(clockid_t, struct sigevent*, timer_t*))
            peak_signal_policy_resolve_timer_create();
    real_mq_notify_fn =
        (int (*)(mqd_t, const struct sigevent*))
            peak_signal_policy_resolve("mq_notify");
    real_aio_read_fn =
        (int (*)(struct aiocb*))peak_signal_policy_resolve("aio_read");
    real_aio_write_fn =
        (int (*)(struct aiocb*))peak_signal_policy_resolve("aio_write");
    real_aio_fsync_fn =
        (int (*)(int, struct aiocb*))peak_signal_policy_resolve("aio_fsync");
    real_lio_listio_fn =
        (int (*)(int, struct aiocb* const[], int, struct sigevent*))
            peak_signal_policy_resolve("lio_listio");
    real_kill_fn =
        (int (*)(pid_t, int))peak_signal_policy_resolve("kill");
    real_pthread_kill_fn =
        (int (*)(pthread_t, int))peak_signal_policy_resolve("pthread_kill");
    real_sigqueue_fn =
        (int (*)(pid_t, int, const union sigval))
            peak_signal_policy_resolve("sigqueue");
    real_raise_fn =
        (int (*)(int))peak_signal_policy_resolve("raise");
    real_sigsuspend_fn =
        (int (*)(const sigset_t*))peak_signal_policy_resolve("sigsuspend");
    real_pselect_fn =
        (int (*)(int,
                 fd_set*,
                 fd_set*,
                 fd_set*,
                 const struct timespec*,
                 const sigset_t*))peak_signal_policy_resolve("pselect");
    real_ppoll_fn =
        (int (*)(struct pollfd*,
                 nfds_t,
                 const struct timespec*,
                 const sigset_t*))peak_signal_policy_resolve("ppoll");
    real_syscall_fn =
        (long (*)(long, ...))peak_signal_policy_resolve("syscall");
}

static void
peak_signal_policy_ensure_real_symbols(void)
{
    (void)pthread_once(&real_symbols_once,
                       peak_signal_policy_resolve_real_symbols);
}

static int
peak_signal_policy_enabled_for_process(void)
{
    return peak_process_profile_enabled() && peak_process_requests_work();
}

static int
peak_signal_policy_backend_runtime_supported(void)
{
#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__) || defined(__aarch64__))
    return 1;
#else
    return 0;
#endif
}

static int
peak_signal_policy_safe_read(void* dst, const void* src, size_t size)
{
    if (size == 0) {
        return 1;
    }
    if (dst == NULL || src == NULL) {
        return 0;
    }

#ifdef SYS_process_vm_readv
    peak_signal_policy_ensure_real_symbols();
    if (real_syscall_fn != NULL) {
        struct iovec local_iov = {
            .iov_base = dst,
            .iov_len = size
        };
        struct iovec remote_iov = {
            .iov_base = (void*)src,
            .iov_len = size
        };

        peak_signal_policy_enter_internal();
        long rc = real_syscall_fn(SYS_process_vm_readv,
                                  getpid(),
                                  &local_iov,
                                  1,
                                  &remote_iov,
                                  1,
                                  0);
        peak_signal_policy_leave_internal();
        if (rc == (long)size) {
            return 1;
        }
    }
#endif

    if (real_syscall_fn == NULL || size > (size_t)SSIZE_MAX) {
        return 0;
    }

#if defined(SYS_openat) && defined(SYS_pread64) && defined(SYS_close)
    peak_signal_policy_enter_internal();
    long fd = real_syscall_fn(SYS_openat,
                              AT_FDCWD,
                              "/proc/self/mem",
                              O_RDONLY | O_CLOEXEC,
                              0);
    long rc = -1;
    if (fd >= 0) {
        rc = real_syscall_fn(SYS_pread64,
                             fd,
                             dst,
                             size,
                             (off_t)(uintptr_t)src);
        (void)real_syscall_fn(SYS_close, fd);
    }
    peak_signal_policy_leave_internal();
    return rc == (long)size;
#else
    return 0;
#endif
}

static int
peak_signal_policy_safe_write(void* dst, const void* src, size_t size)
{
    if (size == 0) {
        return 1;
    }
    if (dst == NULL || src == NULL) {
        return 0;
    }

#ifdef SYS_process_vm_writev
    peak_signal_policy_ensure_real_symbols();
    if (real_syscall_fn != NULL) {
        struct iovec local_iov = {
            .iov_base = (void*)src,
            .iov_len = size
        };
        struct iovec remote_iov = {
            .iov_base = dst,
            .iov_len = size
        };

        peak_signal_policy_enter_internal();
        long rc = real_syscall_fn(SYS_process_vm_writev,
                                  getpid(),
                                  &local_iov,
                                  1,
                                  &remote_iov,
                                  1,
                                  0);
        peak_signal_policy_leave_internal();
        if (rc == (long)size) {
            return 1;
        }
    }
#endif

    return 0;
}

static int
peak_signal_policy_range_is_writable(const void* ptr, size_t size)
{
    if (size == 0) {
        return 1;
    }
    if (ptr == NULL) {
        return 0;
    }

    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end = start + size;
    if (end < start) {
        return 0;
    }

    peak_signal_policy_enter_internal();
    FILE* fp = fopen("/proc/self/maps", "r");
    if (fp == NULL) {
        peak_signal_policy_leave_internal();
        return 0;
    }

    char line[512];
    int writable = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned long long map_start = 0;
        unsigned long long map_end = 0;
        char perms[5] = { 0 };

        if (sscanf(line,
                   "%llx-%llx %4s",
                   &map_start,
                   &map_end,
                   perms) != 3) {
            continue;
        }
        if (start >= (uintptr_t)map_start &&
            end <= (uintptr_t)map_end) {
            writable = perms[1] == 'w';
            break;
        }
    }

    fclose(fp);
    peak_signal_policy_leave_internal();
    return writable;
}

static int
peak_signal_policy_should_reserve_early(void)
{
    const char* mode = getenv("PEAK_SAFE_DETACH_MODE");
    const char* backend = getenv("PEAK_DETACH_BACKEND");
    const char* reserve_early = getenv("PEAK_SIGNAL_RESERVE_EARLY");

    if (!peak_signal_policy_backend_runtime_supported()) {
        return 0;
    }
    if (reserve_early != NULL && reserve_early[0] != '\0' &&
        strcasecmp(reserve_early, "auto") != 0) {
        if (strcasecmp(reserve_early, "always") == 0 ||
            strcasecmp(reserve_early, "1") == 0 ||
            strcasecmp(reserve_early, "true") == 0 ||
            strcasecmp(reserve_early, "yes") == 0) {
            return 1;
        }
        if (strcasecmp(reserve_early, "never") == 0 ||
            strcasecmp(reserve_early, "0") == 0 ||
            strcasecmp(reserve_early, "false") == 0 ||
            strcasecmp(reserve_early, "no") == 0) {
            return 0;
        }
        if (strcasecmp(reserve_early, "forced-only") == 0 ||
            strcasecmp(reserve_early, "forced") == 0) {
            return peak_signal_policy_env_forces_signal();
        }
    }
    if (backend != NULL &&
        (strcasecmp(backend, "signal") == 0 ||
         strcasecmp(backend, "signals") == 0 ||
         strcasecmp(backend, "in-process") == 0)) {
        return 1;
    }
    if (backend != NULL &&
        (strcasecmp(backend, "helper") == 0 ||
         strcasecmp(backend, "debugger") == 0 ||
         strcasecmp(backend, "ptrace") == 0)) {
        return 0;
    }
    if (mode != NULL &&
        (strcasecmp(mode, "helper") == 0 ||
         strcasecmp(mode, "debugger") == 0 ||
         strcasecmp(mode, "ptrace") == 0)) {
        return 0;
    }
    if (peak_signal_policy_env_forces_signal()) {
        return 1;
    }
    if (mode == NULL ||
        mode[0] == '\0' ||
        strcasecmp(mode, "strict") == 0 ||
        strcasecmp(mode, "auto") == 0 ||
        strcasecmp(mode, "signal") == 0 ||
        strcasecmp(mode, "signals") == 0 ||
        strcasecmp(mode, "in-process") == 0) {
        return 1;
    }
    return 0;
}

static void
peak_signal_policy_record_conflict(const char* api)
{
    atomic_fetch_add_explicit(&conflict_count, 1, memory_order_relaxed);
    atomic_store_explicit(&last_conflict_api,
                          (uintptr_t)api,
                          memory_order_release);
}

static int
peak_signal_policy_is_internal(void)
{
    return internal_depth > 0;
}

void
peak_signal_policy_enter_internal(void)
{
    internal_depth++;
}

void
peak_signal_policy_leave_internal(void)
{
    if (internal_depth > 0) {
        internal_depth--;
    }
}

static int
peak_signal_policy_env_forces_signal(void)
{
    int forced = 0;
    return peak_signal_policy_parse_signal_env(getenv("PEAK_DETACH_SIGNAL"),
                                               &forced) > 0;
}

static int
peak_signal_policy_signal_is_migration_candidate(int signum,
                                                 int old_signum,
                                                 const sigset_t* excluded_set)
{
    return signum != old_signum &&
           signum != SIGRTMIN &&
           signum != SIGRTMIN + 1 &&
           (excluded_set == NULL || sigismember(excluded_set, signum) != 1) &&
           peak_signal_policy_signal_is_available(signum);
}

static int
peak_signal_policy_find_migration_candidate(int old_signum,
                                            const sigset_t* excluded_set)
{
    for (int candidate = SIGRTMAX; candidate >= SIGRTMIN; candidate--) {
        atomic_store_explicit(&migration_candidate_signal,
                              candidate,
                              memory_order_release);
        if (peak_signal_policy_signal_is_migration_candidate(candidate,
                                                             old_signum,
                                                             excluded_set)) {
            return candidate;
        }
        atomic_store_explicit(&migration_candidate_signal,
                              0,
                              memory_order_release);
    }
    return 0;
}

static int
peak_signal_policy_signal_is_transitioning(int signum)
{
    return signum > 0 &&
           (signum == atomic_load_explicit(&migration_candidate_signal,
                                           memory_order_acquire) ||
            signum == atomic_load_explicit(&migration_releasing_signal,
                                           memory_order_acquire));
}

static int
peak_signal_policy_wait_until_migration_enabled(void)
{
    if (atomic_load_explicit(&migration_disabled_depth,
                             memory_order_acquire) == 0) {
        return 1;
    }

    struct timespec start;
    struct timespec now;
    const long timeout_ns = 100000000LL;
    const struct timespec pause = {
        .tv_sec = 0,
        .tv_nsec = 1000000L
    };

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return 0;
    }

    do {
        nanosleep(&pause, NULL);
        if (atomic_load_explicit(&migration_disabled_depth,
                                 memory_order_acquire) == 0) {
            return 1;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return 0;
        }
        long elapsed_ns =
            (now.tv_sec - start.tv_sec) * 1000000000L +
            (now.tv_nsec - start.tv_nsec);
        if (elapsed_ns >= timeout_ns) {
            break;
        }
    } while (1);

    return atomic_load_explicit(&migration_disabled_depth,
                                memory_order_acquire) == 0;
}

static int
peak_signal_policy_migrate_reserved_signal_locked(int old_signum,
                                                  const sigset_t* excluded_set,
                                                  const char* api)
{
    peak_signal_policy_record_conflict(api);

    if (old_signum <= 0 ||
        peak_signal_policy_env_forces_signal() ||
        !peak_signal_policy_wait_until_migration_enabled()) {
        return 0;
    }

    int current = atomic_load_explicit(&reserved_signal,
                                       memory_order_acquire);
    if (current != old_signum) {
        return 1;
    }
    if (atomic_load_explicit(&migration_disabled_depth,
                             memory_order_acquire) > 0) {
        return 0;
    }

    int replacement =
        peak_signal_policy_find_migration_candidate(old_signum, excluded_set);
    if (replacement <= 0) {
        atomic_store_explicit(&migration_candidate_signal,
                              0,
                              memory_order_release);
        return 0;
    }

    peak_signal_policy_init_cookie();
    if (!peak_signal_policy_install_protective_handler(replacement)) {
        atomic_store_explicit(&migration_candidate_signal,
                              0,
                              memory_order_release);
        return 0;
    }

    atomic_store_explicit(&migration_releasing_signal,
                          old_signum,
                          memory_order_release);
    atomic_store_explicit(&reserved_signal, replacement, memory_order_release);
    if (!peak_signal_policy_restore_default_handler(old_signum)) {
        atomic_store_explicit(&reserved_signal, old_signum, memory_order_release);
        (void)peak_signal_policy_restore_default_handler(replacement);
        atomic_store_explicit(&migration_releasing_signal,
                              0,
                              memory_order_release);
        atomic_store_explicit(&migration_candidate_signal,
                              0,
                              memory_order_release);
        return 0;
    }

    (void)peak_signal_policy_unblock_reserved_for_current_thread();
    atomic_fetch_add_explicit(&migration_count, 1, memory_order_relaxed);
    atomic_store_explicit(&migration_releasing_signal,
                          0,
                          memory_order_release);
    atomic_store_explicit(&migration_candidate_signal,
                          0,
                          memory_order_release);
    return 1;
}

static int
peak_signal_policy_prepare_reserved_set_for_user(const sigset_t* set,
                                                 const char* api)
{
    if (!peak_signal_policy_enabled_for_process() ||
        peak_signal_policy_is_internal()) {
        return 1;
    }
    if (set == NULL) {
        return 1;
    }

    sigset_t set_copy;
    if (!peak_signal_policy_safe_read(&set_copy, set, sizeof(set_copy))) {
        errno = EFAULT;
        return 0;
    }

    if (pthread_mutex_lock(&migration_mutex) != 0) {
        errno = EINVAL;
        return 0;
    }

    int signum = atomic_load_explicit(&reserved_signal,
                                      memory_order_acquire);
    if (signum <= 0 || sigismember(&set_copy, signum) != 1) {
        pthread_mutex_unlock(&migration_mutex);
        return 1;
    }

    if (peak_signal_policy_migrate_reserved_signal_locked(signum,
                                                          &set_copy,
                                                          api)) {
        pthread_mutex_unlock(&migration_mutex);
        return 1;
    }

    pthread_mutex_unlock(&migration_mutex);
    errno = EINVAL;
    return 0;
}

static int
peak_signal_policy_prepare_reserved_signal_for_user(int signum,
                                                    const char* api)
{
    if (!peak_signal_policy_enabled_for_process() ||
        peak_signal_policy_is_internal() || signum <= 0) {
        return 1;
    }

    if (pthread_mutex_lock(&migration_mutex) != 0) {
        errno = EINVAL;
        return 0;
    }

    if (signum != atomic_load_explicit(&reserved_signal,
                                       memory_order_acquire)) {
        pthread_mutex_unlock(&migration_mutex);
        return 1;
    }

    if (peak_signal_policy_migrate_reserved_signal_locked(signum, NULL, api)) {
        pthread_mutex_unlock(&migration_mutex);
        return 1;
    }

    pthread_mutex_unlock(&migration_mutex);
    errno = EINVAL;
    return 0;
}

static int
peak_signal_policy_should_hide_raw_sigaction_query(int signum)
{
    if (!peak_signal_policy_enabled_for_process() ||
        peak_signal_policy_is_internal()) {
        return 0;
    }

    int reserved = atomic_load_explicit(&reserved_signal, memory_order_acquire);
    return signum > 0 &&
           (signum == reserved ||
            peak_signal_policy_signal_is_transitioning(signum));
}

static int
peak_signal_policy_event_signal(const struct sigevent* evp, int* signum_out)
{
    if (signum_out != NULL) {
        *signum_out = 0;
    }
    if (evp == NULL) {
        return 1;
    }

    struct sigevent event;
    if (!peak_signal_policy_safe_read(&event, evp, sizeof(event))) {
        return 0;
    }

    if (event.sigev_notify != SIGEV_SIGNAL &&
#ifdef SIGEV_THREAD_ID
        event.sigev_notify != SIGEV_THREAD_ID &&
#endif
        1) {
        return 1;
    }
    if (signum_out != NULL) {
        *signum_out = event.sigev_signo;
    }
    return 1;
}

static int
peak_signal_policy_prepare_event_for_user_excluding(
    const struct sigevent* evp,
    const sigset_t* excluded_set,
    const char* api)
{
    if (!peak_signal_policy_enabled_for_process()) {
        return 1;
    }

    if (peak_signal_policy_is_internal()) {
        return 1;
    }

    int signum = 0;
    if (!peak_signal_policy_event_signal(evp, &signum)) {
        return 1;
    }
    if (signum <= 0) {
        return 1;
    }

    if (pthread_mutex_lock(&migration_mutex) != 0) {
        errno = EINVAL;
        return 0;
    }

    if (signum != atomic_load_explicit(&reserved_signal,
                                       memory_order_acquire)) {
        pthread_mutex_unlock(&migration_mutex);
        return 1;
    }

    if (peak_signal_policy_migrate_reserved_signal_locked(signum,
                                                          excluded_set,
                                                          api)) {
        pthread_mutex_unlock(&migration_mutex);
        return 1;
    }

    pthread_mutex_unlock(&migration_mutex);
    errno = EINVAL;
    return 0;
}

static int
peak_signal_policy_prepare_event_for_user(const struct sigevent* evp,
                                          const char* api)
{
    return peak_signal_policy_prepare_event_for_user_excluding(evp,
                                                               NULL,
                                                               api);
}

static int
peak_signal_policy_raw_set_contains_signal(const void* set,
                                           size_t set_size,
                                           int signum)
{
    if (set == NULL || signum <= 0) {
        return 0;
    }

    size_t copy_size = set_size;
    if (copy_size > sizeof(sigset_t)) {
        copy_size = sizeof(sigset_t);
    }
    if (copy_size == 0) {
        return 0;
    }

    unsigned char bytes[sizeof(sigset_t)];
    memset(bytes, 0, sizeof(bytes));
    if (!peak_signal_policy_safe_read(bytes, set, copy_size)) {
        return -1;
    }

    if (set_size >= sizeof(sigset_t)) {
        return sigismember((const sigset_t*)bytes, signum) == 1 ? 1 : 0;
    }

    size_t bit = (size_t)(signum - 1);
    size_t byte = bit / 8;
    if (byte >= set_size) {
        return 0;
    }
    return (bytes[byte] & (unsigned char)(1u << (bit % 8))) != 0;
}

static void
peak_signal_policy_build_raw_excluded_set(const void* set,
                                          size_t set_size,
                                          sigset_t* excluded_set)
{
    sigemptyset(excluded_set);
    if (set == NULL) {
        return;
    }

    for (int signum = 1; signum < NSIG; signum++) {
        int contains = peak_signal_policy_raw_set_contains_signal(set,
                                                                  set_size,
                                                                  signum);
        if (contains > 0) {
            sigaddset(excluded_set, signum);
        }
    }
}

static int
peak_signal_policy_prepare_reserved_raw_set_for_user(const void* set,
                                                     size_t set_size,
                                                     const char* api)
{
    if (!peak_signal_policy_enabled_for_process() ||
        peak_signal_policy_is_internal()) {
        return 1;
    }

    if (pthread_mutex_lock(&migration_mutex) != 0) {
        errno = EINVAL;
        return 0;
    }

    int signum = atomic_load_explicit(&reserved_signal,
                                      memory_order_acquire);
    int contains = signum > 0 ?
        peak_signal_policy_raw_set_contains_signal(set, set_size, signum) : 0;
    if (signum <= 0 || contains == 0) {
        pthread_mutex_unlock(&migration_mutex);
        return 1;
    }
    if (contains < 0) {
        pthread_mutex_unlock(&migration_mutex);
        return 1;
    }

    sigset_t excluded_set;
    peak_signal_policy_build_raw_excluded_set(set, set_size, &excluded_set);
    if (peak_signal_policy_migrate_reserved_signal_locked(signum,
                                                          &excluded_set,
                                                          api)) {
        pthread_mutex_unlock(&migration_mutex);
        return 1;
    }

    pthread_mutex_unlock(&migration_mutex);
    errno = EINVAL;
    return 0;
}

static void
peak_signal_policy_add_event_signal_to_set(const struct sigevent* evp,
                                           sigset_t* excluded_set)
{
    int signum = 0;
    if (!peak_signal_policy_event_signal(evp, &signum)) {
        return;
    }
    if (signum > 0) {
        sigaddset(excluded_set, signum);
    }
}

static void
peak_signal_policy_collect_lio_event_signals(struct aiocb* const list[],
                                             int nent,
                                             const struct sigevent* evp,
                                             sigset_t* excluded_set)
{
    sigemptyset(excluded_set);
    peak_signal_policy_add_event_signal_to_set(evp, excluded_set);
    uintptr_t list_addr = (uintptr_t)list;
    if (list_addr == 0 || nent <= 0) {
        return;
    }
    struct aiocb* const* user_list = (struct aiocb* const*)list_addr;

    for (int i = 0; i < nent; i++) {
        struct aiocb* aiocbp = NULL;
        if (!peak_signal_policy_safe_read(&aiocbp,
                                          &user_list[i],
                                          sizeof(aiocbp)) ||
            aiocbp == NULL) {
            continue;
        }
        struct aiocb aiocb_copy;
        if (peak_signal_policy_safe_read(&aiocb_copy,
                                         aiocbp,
                                         sizeof(aiocb_copy))) {
            peak_signal_policy_add_event_signal_to_set(&aiocb_copy.aio_sigevent,
                                                       excluded_set);
        }
    }
}

static int
peak_signal_policy_parse_signal_offset(const char* value,
                                       const char* prefix,
                                       int base,
                                       int* out)
{
    size_t len = strlen(prefix);
    if (strncasecmp(value, prefix, len) != 0) {
        return 0;
    }
    char op = value[len];
    if (op != '+' && op != '-') {
        return 0;
    }
    char* end = NULL;
    long offset = strtol(value + len + 1, &end, 10);
    if (end == value + len + 1 || *end != '\0' || offset < 0) {
        return -1;
    }
    long signum = op == '+' ? (long)base + offset : (long)base - offset;
    if (signum < SIGRTMIN || signum > SIGRTMAX) {
        return -1;
    }
    *out = (int)signum;
    return 1;
}

static int
peak_signal_policy_parse_signal_env(const char* value, int* out)
{
    if (value == NULL || value[0] == '\0' ||
        strcasecmp(value, "auto") == 0) {
        return 0;
    }

    int parsed = 0;
    int rc = peak_signal_policy_parse_signal_offset(value,
                                                    "SIGRTMIN",
                                                    SIGRTMIN,
                                                    &parsed);
    if (rc != 0) {
        if (rc > 0) {
            *out = parsed;
        }
        return rc;
    }
    rc = peak_signal_policy_parse_signal_offset(value,
                                                "SIGRTMAX",
                                                SIGRTMAX,
                                                &parsed);
    if (rc != 0) {
        if (rc > 0) {
            *out = parsed;
        }
        return rc;
    }

    char* end = NULL;
    long signum = strtol(value, &end, 10);
    if (end == value || *end != '\0' ||
        signum < SIGRTMIN || signum > SIGRTMAX) {
        return -1;
    }
    *out = (int)signum;
    return 1;
}

static int
peak_signal_policy_signal_is_available(int signum)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_sigaction_fn == NULL) {
        return 0;
    }

    struct sigaction existing;
    memset(&existing, 0, sizeof(existing));
    peak_signal_policy_enter_internal();
    int rc = real_sigaction_fn(signum, NULL, &existing);
    peak_signal_policy_leave_internal();
    return rc == 0 && existing.sa_handler == SIG_DFL;
}

static void
peak_signal_policy_init_cookie_once(void)
{
    uintptr_t seed = 0;

#ifdef SYS_getrandom
    const long random_bytes = syscall(SYS_getrandom,
                                      &seed,
                                      sizeof(seed),
                                      0x0001u /* GRND_NONBLOCK */);
#else
    const long random_bytes = -1;
#endif
    if (random_bytes != (long)sizeof(seed)) {
        seed = (uintptr_t)&cookie_base ^
               ((uintptr_t)getpid() << 17) ^
               (uintptr_t)time(NULL) ^
               UINT64_C(0x9e3779b97f4a7c15);
    }
    if (seed == 0) {
        seed = UINT64_C(0x6a09e667f3bcc909);
    }
    atomic_store_explicit(&cookie_base,
                          (unsigned long)seed,
                          memory_order_release);
}

static void
peak_signal_policy_init_cookie(void)
{
    (void)pthread_once(&cookie_once, peak_signal_policy_init_cookie_once);
}

int
peak_signal_policy_choose_reserved_signal(void)
{
    int current = atomic_load_explicit(&reserved_signal,
                                       memory_order_acquire);
    if (current > 0) {
        return current;
    }

    int forced = 0;
    int parse_rc =
        peak_signal_policy_parse_signal_env(getenv("PEAK_DETACH_SIGNAL"),
                                            &forced);
    if (parse_rc > 0) {
        if (peak_signal_policy_signal_is_available(forced)) {
            return peak_signal_policy_commit_reserved_signal(forced);
        }
        return 0;
    }
    if (parse_rc < 0) {
        return 0;
    }

    for (int candidate = SIGRTMAX; candidate >= SIGRTMIN; candidate--) {
        if (candidate == SIGRTMIN || candidate == SIGRTMIN + 1) {
            continue;
        }
        if (!peak_signal_policy_signal_is_available(candidate)) {
            continue;
        }
        return peak_signal_policy_commit_reserved_signal(candidate);
    }
    return 0;
}

void
peak_signal_policy_set_reserved_signal(int signum)
{
    if (signum > 0) {
        (void)peak_signal_policy_commit_reserved_signal(signum);
    }
}

void
peak_signal_policy_clear_reserved_signal(void)
{
    int signum = atomic_exchange_explicit(&reserved_signal,
                                          0,
                                          memory_order_acq_rel);
    if (signum <= 0) {
        return;
    }

    (void)peak_signal_policy_restore_default_handler(signum);
}

static int
peak_signal_policy_restore_default_handler(int signum)
{
    if (signum <= 0) {
        return 1;
    }

    peak_signal_policy_ensure_real_symbols();
    if (real_sigaction_fn == NULL) {
        return 0;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_DFL;

    peak_signal_policy_enter_internal();
    int rc = real_sigaction_fn(signum, &action, NULL);
    peak_signal_policy_leave_internal();
    return rc == 0;
}

int
peak_signal_policy_reserved_signal(void)
{
    return atomic_load_explicit(&reserved_signal, memory_order_acquire);
}

int
peak_signal_policy_atomics_lock_free(void)
{
    return atomic_is_lock_free(&reserved_signal) &&
           atomic_is_lock_free(&conflict_count) &&
           atomic_is_lock_free(&migration_count) &&
           atomic_is_lock_free(&migration_disabled_depth) &&
           atomic_is_lock_free(&migration_candidate_signal) &&
           atomic_is_lock_free(&migration_releasing_signal) &&
           atomic_is_lock_free(&unexpected_delivery_count) &&
           atomic_is_lock_free(&last_conflict_api) &&
           atomic_is_lock_free(&cookie_base);
}

static unsigned long
peak_signal_policy_cookie_from_base(unsigned long base, int epoch, pid_t tid)
{
    unsigned long e = (unsigned long)(uint32_t)epoch;
    unsigned long t = (unsigned long)(uint32_t)tid;
    return base ^ (e << 32) ^ (t * 0x45d9f3bu);
}

static unsigned long
peak_signal_policy_cookie_for_preinitialized(int epoch, pid_t tid)
{
    unsigned long base =
        atomic_load_explicit(&cookie_base, memory_order_acquire);
    if (base == 0) {
        return 0;
    }
    return peak_signal_policy_cookie_from_base(base, epoch, tid);
}

unsigned long
peak_signal_policy_cookie_for(int epoch, pid_t tid)
{
    peak_signal_policy_init_cookie();
    return peak_signal_policy_cookie_for_preinitialized(epoch, tid);
}

int
peak_signal_policy_cookie_matches_async(const siginfo_t* info,
                                        int epoch,
                                        pid_t tid)
{
    if (info == NULL || info->si_code != SI_QUEUE ||
        info->si_pid != getpid() || info->si_uid != getuid()) {
        return 0;
    }
    unsigned long cookie =
        peak_signal_policy_cookie_for_preinitialized(epoch, tid);
    if (cookie == 0) {
        return 0;
    }
    return (unsigned long)(uintptr_t)info->si_value.sival_ptr ==
           cookie;
}

void
peak_signal_policy_note_unexpected_delivery(void)
{
    atomic_fetch_add_explicit(&unexpected_delivery_count,
                              1,
                              memory_order_relaxed);
}

static void
peak_signal_policy_protective_handler(int signo,
                                      siginfo_t* info,
                                      void* context)
{
    (void)signo;
    (void)info;
    (void)context;

    peak_signal_policy_note_unexpected_delivery();
}

static int
peak_signal_policy_install_protective_handler(int signum)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_sigaction_fn == NULL) {
        return 0;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, signum);
    action.sa_sigaction = peak_signal_policy_protective_handler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;

    peak_signal_policy_enter_internal();
    int rc = real_sigaction_fn(signum, &action, NULL);
    peak_signal_policy_leave_internal();
    return rc == 0;
}

static int
peak_signal_policy_commit_reserved_signal(int signum)
{
    if (signum <= 0) {
        return 0;
    }

    if (pthread_mutex_lock(&migration_mutex) != 0) {
        return 0;
    }

    int current = atomic_load_explicit(&reserved_signal,
                                       memory_order_acquire);
    if (current > 0) {
        pthread_mutex_unlock(&migration_mutex);
        return current;
    }

    peak_signal_policy_init_cookie();
    atomic_store_explicit(&migration_candidate_signal,
                          signum,
                          memory_order_release);
    if (!peak_signal_policy_signal_is_available(signum)) {
        atomic_store_explicit(&migration_candidate_signal,
                              0,
                              memory_order_release);
        pthread_mutex_unlock(&migration_mutex);
        return 0;
    }
    if (!peak_signal_policy_install_protective_handler(signum)) {
        atomic_store_explicit(&reserved_signal, 0, memory_order_release);
        atomic_store_explicit(&migration_candidate_signal,
                              0,
                              memory_order_release);
        pthread_mutex_unlock(&migration_mutex);
        return 0;
    }
    atomic_store_explicit(&reserved_signal,
                          signum,
                          memory_order_release);
    atomic_store_explicit(&migration_candidate_signal,
                          0,
                          memory_order_release);
    pthread_mutex_unlock(&migration_mutex);
    return signum;
}

int
peak_signal_policy_unexpected_delivery_count(void)
{
    return atomic_load_explicit(&unexpected_delivery_count,
                                memory_order_acquire);
}

int
peak_signal_policy_send_thread_signal(pid_t tid,
                                      int signum,
                                      unsigned long cookie)
{
#ifdef SYS_rt_tgsigqueueinfo
    peak_signal_policy_ensure_real_symbols();
    if (real_syscall_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }

    siginfo_t info;
    memset(&info, 0, sizeof(info));
    info.si_signo = signum;
    info.si_code = SI_QUEUE;
    info.si_pid = getpid();
    info.si_uid = getuid();
    info.si_value.sival_ptr = (void*)(uintptr_t)cookie;

    peak_signal_policy_enter_internal();
    long rc = real_syscall_fn(SYS_rt_tgsigqueueinfo,
                              getpid(),
                              tid,
                              signum,
                              &info);
    peak_signal_policy_leave_internal();
    return (int)rc;
#else
    (void)tid;
    (void)signum;
    (void)cookie;
    errno = ENOSYS;
    return -1;
#endif
}

int
peak_signal_policy_unblock_reserved_for_current_thread(void)
{
    int signum = atomic_load_explicit(&reserved_signal,
                                      memory_order_acquire);
    if (signum <= 0) {
        return 0;
    }
    peak_signal_policy_ensure_real_symbols();
    if (real_pthread_sigmask_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, signum);
    peak_signal_policy_enter_internal();
    int rc = real_pthread_sigmask_fn(SIG_UNBLOCK, &set, NULL);
    peak_signal_policy_leave_internal();
    return rc;
}

int
peak_signal_policy_conflict_count(void)
{
    return atomic_load_explicit(&conflict_count, memory_order_acquire);
}

int
peak_signal_policy_migration_count(void)
{
    return atomic_load_explicit(&migration_count, memory_order_acquire);
}

const char*
peak_signal_policy_last_conflict_api(void)
{
    return (const char*)atomic_load_explicit(&last_conflict_api,
                                             memory_order_acquire);
}

void
peak_signal_policy_push_migration_disabled(void)
{
    atomic_fetch_add_explicit(&migration_disabled_depth,
                              1,
                              memory_order_acq_rel);
}

void
peak_signal_policy_pop_migration_disabled(void)
{
    int current = atomic_load_explicit(&migration_disabled_depth,
                                       memory_order_acquire);
    while (current > 0) {
        if (atomic_compare_exchange_weak_explicit(&migration_disabled_depth,
                                                  &current,
                                                  current - 1,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return;
        }
    }
}

#ifdef PEAK_ENABLE_TEST_HOOKS
int
peak_signal_policy_test_block_reserved_for_current_thread(void)
{
    int signum = atomic_load_explicit(&reserved_signal,
                                      memory_order_acquire);
    if (signum <= 0) {
        errno = EINVAL;
        return -1;
    }
    peak_signal_policy_ensure_real_symbols();
    if (real_pthread_sigmask_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, signum);
    peak_signal_policy_enter_internal();
    int rc = real_pthread_sigmask_fn(SIG_BLOCK, &set, NULL);
    peak_signal_policy_leave_internal();
    return rc;
}

int
peak_signal_policy_test_send_bad_cookie_to_current_thread(void)
{
    int signum = atomic_load_explicit(&reserved_signal,
                                      memory_order_acquire);
    if (signum <= 0) {
        errno = EINVAL;
        return -1;
    }

    union sigval value;
    value.sival_ptr =
        (void*)(uintptr_t)(peak_signal_policy_cookie_for(1, getpid()) ^
                           UINT64_C(0x5a5a5a5a));

    peak_signal_policy_enter_internal();
    int rc = sigqueue(getpid(), signum, value);
    peak_signal_policy_leave_internal();
    return rc;
}
#endif

__attribute__((visibility("default"))) int
sigaction(int signum, const struct sigaction* act, struct sigaction* oldact)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_sigaction_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_sigaction_fn(signum, act, oldact);
    }
    int reserved = atomic_load_explicit(&reserved_signal, memory_order_acquire);
    if (!peak_signal_policy_is_internal() && act == NULL && oldact != NULL &&
        signum > 0 &&
        (signum == reserved ||
         peak_signal_policy_signal_is_transitioning(signum))) {
        if (!peak_signal_policy_range_is_writable(oldact, sizeof(*oldact))) {
            errno = EFAULT;
            return -1;
        }
        memset(oldact, 0, sizeof(*oldact));
        sigemptyset(&oldact->sa_mask);
        oldact->sa_handler = SIG_DFL;
        return 0;
    }
    if (act != NULL &&
        !peak_signal_policy_prepare_reserved_signal_for_user(signum,
                                                             "sigaction")) {
        return -1;
    }
    return real_sigaction_fn(signum, act, oldact);
}

__attribute__((visibility("default"))) void (*signal(int signum, void (*handler)(int)))(int)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_signal_fn == NULL) {
        errno = ENOSYS;
        return SIG_ERR;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_signal_fn(signum, handler);
    }
    if (!peak_signal_policy_prepare_reserved_signal_for_user(signum,
                                                            "signal")) {
        return SIG_ERR;
    }
    return real_signal_fn(signum, handler);
}

__attribute__((visibility("default"))) int
pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_pthread_sigmask_fn == NULL) {
        errno = ENOSYS;
        return errno;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_pthread_sigmask_fn(how, set, oldset);
    }
    if (how == SIG_BLOCK || how == SIG_SETMASK) {
        if (!peak_signal_policy_prepare_reserved_set_for_user(
                set,
                "pthread_sigmask")) {
            return errno != 0 ? errno : EINVAL;
        }
    }
    return real_pthread_sigmask_fn(how, set, oldset);
}

__attribute__((visibility("default"))) int
sigprocmask(int how, const sigset_t* set, sigset_t* oldset)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_sigprocmask_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_sigprocmask_fn(how, set, oldset);
    }
    if (how == SIG_BLOCK || how == SIG_SETMASK) {
        if (!peak_signal_policy_prepare_reserved_set_for_user(set,
                                                              "sigprocmask")) {
            return -1;
        }
    }
    return real_sigprocmask_fn(how, set, oldset);
}

__attribute__((visibility("default"))) int
sigwait(const sigset_t* set, int* sig)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_sigwait_fn == NULL) {
        errno = ENOSYS;
        return errno;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_sigwait_fn(set, sig);
    }
    if (!peak_signal_policy_prepare_reserved_set_for_user(set, "sigwait")) {
        return errno != 0 ? errno : EINVAL;
    }
    return real_sigwait_fn(set, sig);
}

__attribute__((visibility("default"))) int
sigwaitinfo(const sigset_t* set, siginfo_t* info)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_sigwaitinfo_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_sigwaitinfo_fn(set, info);
    }
    if (!peak_signal_policy_prepare_reserved_set_for_user(set,
                                                          "sigwaitinfo")) {
        return -1;
    }
    return real_sigwaitinfo_fn(set, info);
}

__attribute__((visibility("default"))) int
sigtimedwait(const sigset_t* set,
             siginfo_t* info,
             const struct timespec* timeout)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_sigtimedwait_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_sigtimedwait_fn(set, info, timeout);
    }
    if (!peak_signal_policy_prepare_reserved_set_for_user(set,
                                                          "sigtimedwait")) {
        return -1;
    }
    return real_sigtimedwait_fn(set, info, timeout);
}

__attribute__((visibility("default"))) int
signalfd(int fd, const sigset_t* mask, int flags)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_signalfd_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_signalfd_fn(fd, mask, flags);
    }
    if (!peak_signal_policy_prepare_reserved_set_for_user(mask, "signalfd")) {
        return -1;
    }
    return real_signalfd_fn(fd, mask, flags);
}

__attribute__((visibility("default"))) int
signalfd4(int fd, const sigset_t* mask, int sizemask, int flags)
{
    peak_signal_policy_ensure_real_symbols();
    if (!peak_signal_policy_enabled_for_process()) {
        if (real_signalfd4_fn != NULL) {
            return real_signalfd4_fn(fd, mask, sizemask, flags);
        }
#ifdef SYS_signalfd4
        if (real_syscall_fn != NULL) {
            return (int)real_syscall_fn(SYS_signalfd4,
                                        fd,
                                        mask,
                                        sizemask,
                                        flags);
        }
#endif
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_prepare_reserved_set_for_user(mask, "signalfd4")) {
        return -1;
    }
    if (real_signalfd4_fn == NULL) {
#ifdef SYS_signalfd4
        if (real_syscall_fn != NULL) {
            return (int)real_syscall_fn(SYS_signalfd4,
                                        fd,
                                        mask,
                                        sizemask,
                                        flags);
        }
#endif
        errno = ENOSYS;
        return -1;
    }
    return real_signalfd4_fn(fd, mask, sizemask, flags);
}

__attribute__((visibility("default"))) int
timer_create(clockid_t clockid, struct sigevent* evp, timer_t* timerid)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_timer_create_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_timer_create_fn(clockid, evp, timerid);
    }
    if (!peak_signal_policy_prepare_event_for_user(evp, "timer_create")) {
        return -1;
    }
    peak_signal_policy_enter_internal();
    int rc = real_timer_create_fn(clockid, evp, timerid);
    int saved_errno = errno;
    peak_signal_policy_leave_internal();
    errno = saved_errno;
    return rc;
}

__attribute__((visibility("default"))) int
mq_notify(mqd_t mqdes, const struct sigevent* sevp)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_mq_notify_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_mq_notify_fn(mqdes, sevp);
    }
    if (!peak_signal_policy_prepare_event_for_user(sevp, "mq_notify")) {
        return -1;
    }
    peak_signal_policy_enter_internal();
    int rc = real_mq_notify_fn(mqdes, sevp);
    int saved_errno = errno;
    peak_signal_policy_leave_internal();
    errno = saved_errno;
    return rc;
}

__attribute__((visibility("default"))) int
aio_read(struct aiocb* aiocbp)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_aio_read_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_aio_read_fn(aiocbp);
    }
    struct aiocb aiocb_copy;
    if (peak_signal_policy_safe_read(&aiocb_copy,
                                     aiocbp,
                                     sizeof(aiocb_copy)) &&
        !peak_signal_policy_prepare_event_for_user(&aiocb_copy.aio_sigevent,
                                                   "aio_read")) {
        return -1;
    }
    peak_signal_policy_enter_internal();
    int rc = real_aio_read_fn(aiocbp);
    int saved_errno = errno;
    peak_signal_policy_leave_internal();
    errno = saved_errno;
    return rc;
}

__attribute__((visibility("default"))) int
aio_write(struct aiocb* aiocbp)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_aio_write_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_aio_write_fn(aiocbp);
    }
    struct aiocb aiocb_copy;
    if (peak_signal_policy_safe_read(&aiocb_copy,
                                     aiocbp,
                                     sizeof(aiocb_copy)) &&
        !peak_signal_policy_prepare_event_for_user(&aiocb_copy.aio_sigevent,
                                                   "aio_write")) {
        return -1;
    }
    peak_signal_policy_enter_internal();
    int rc = real_aio_write_fn(aiocbp);
    int saved_errno = errno;
    peak_signal_policy_leave_internal();
    errno = saved_errno;
    return rc;
}

__attribute__((visibility("default"))) int
aio_fsync(int op, struct aiocb* aiocbp)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_aio_fsync_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_aio_fsync_fn(op, aiocbp);
    }
    struct aiocb aiocb_copy;
    if (peak_signal_policy_safe_read(&aiocb_copy,
                                     aiocbp,
                                     sizeof(aiocb_copy)) &&
        !peak_signal_policy_prepare_event_for_user(&aiocb_copy.aio_sigevent,
                                                   "aio_fsync")) {
        return -1;
    }
    peak_signal_policy_enter_internal();
    int rc = real_aio_fsync_fn(op, aiocbp);
    int saved_errno = errno;
    peak_signal_policy_leave_internal();
    errno = saved_errno;
    return rc;
}

__attribute__((visibility("default"))) int
lio_listio(int mode,
           struct aiocb* const list[],
           int nent,
           struct sigevent* sevp)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_lio_listio_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_lio_listio_fn(mode, list, nent, sevp);
    }

    sigset_t excluded_set;
    peak_signal_policy_collect_lio_event_signals(list,
                                                 nent,
                                                 sevp,
                                                 &excluded_set);
    if (!peak_signal_policy_prepare_event_for_user_excluding(sevp,
                                                             &excluded_set,
                                                             "lio_listio")) {
        return -1;
    }
    uintptr_t list_addr = (uintptr_t)list;
    struct aiocb* const* user_list = (struct aiocb* const*)list_addr;
    for (int i = 0; list_addr != 0 && i < nent; i++) {
        struct aiocb* aiocbp = NULL;
        if (!peak_signal_policy_safe_read(&aiocbp,
                                          &user_list[i],
                                          sizeof(aiocbp)) ||
            aiocbp == NULL) {
            continue;
        }
        struct aiocb aiocb_copy;
        if (!peak_signal_policy_safe_read(&aiocb_copy,
                                          aiocbp,
                                          sizeof(aiocb_copy))) {
            continue;
        }
        if (!peak_signal_policy_prepare_event_for_user_excluding(
                &aiocb_copy.aio_sigevent,
                &excluded_set,
                "lio_listio")) {
            return -1;
        }
    }
    peak_signal_policy_enter_internal();
    int rc = real_lio_listio_fn(mode, list, nent, sevp);
    int saved_errno = errno;
    peak_signal_policy_leave_internal();
    errno = saved_errno;
    return rc;
}

__attribute__((visibility("default"))) int
kill(pid_t pid, int sig)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_kill_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_kill_fn(pid, sig);
    }
    if (!peak_signal_policy_prepare_reserved_signal_for_user(sig, "kill")) {
        return -1;
    }
    return real_kill_fn(pid, sig);
}

__attribute__((visibility("default"))) int
pthread_kill(pthread_t thread, int sig)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_pthread_kill_fn == NULL) {
        errno = ENOSYS;
        return errno;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_pthread_kill_fn(thread, sig);
    }
    if (!peak_signal_policy_prepare_reserved_signal_for_user(sig,
                                                             "pthread_kill")) {
        return EINVAL;
    }
    return real_pthread_kill_fn(thread, sig);
}

__attribute__((visibility("default"))) int
sigqueue(pid_t pid, int sig, const union sigval value)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_sigqueue_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_sigqueue_fn(pid, sig, value);
    }
    if (!peak_signal_policy_prepare_reserved_signal_for_user(sig, "sigqueue")) {
        return -1;
    }
    return real_sigqueue_fn(pid, sig, value);
}

__attribute__((visibility("default"))) int
raise(int sig)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_raise_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_raise_fn(sig);
    }
    if (!peak_signal_policy_prepare_reserved_signal_for_user(sig, "raise")) {
        return -1;
    }
    return real_raise_fn(sig);
}

__attribute__((visibility("default"))) int
sigsuspend(const sigset_t* mask)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_sigsuspend_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_sigsuspend_fn(mask);
    }
    if (!peak_signal_policy_prepare_reserved_set_for_user(mask, "sigsuspend")) {
        return -1;
    }
    return real_sigsuspend_fn(mask);
}

__attribute__((visibility("default"))) int
pselect(int nfds,
        fd_set* readfds,
        fd_set* writefds,
        fd_set* exceptfds,
        const struct timespec* timeout,
        const sigset_t* sigmask)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_pselect_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_pselect_fn(nfds,
                               readfds,
                               writefds,
                               exceptfds,
                               timeout,
                               sigmask);
    }
    if (!peak_signal_policy_prepare_reserved_set_for_user(sigmask, "pselect")) {
        return -1;
    }
    return real_pselect_fn(nfds, readfds, writefds, exceptfds, timeout, sigmask);
}

__attribute__((visibility("default"))) int
ppoll(struct pollfd* fds,
      nfds_t nfds,
      const struct timespec* timeout,
      const sigset_t* sigmask)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_ppoll_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return real_ppoll_fn(fds, nfds, timeout, sigmask);
    }
    if (!peak_signal_policy_prepare_reserved_set_for_user(sigmask, "ppoll")) {
        return -1;
    }
    return real_ppoll_fn(fds, nfds, timeout, sigmask);
}

typedef struct {
    const void* sigmask;
    size_t sigsetsize;
} PeakSignalPolicyPselect6Sigmask;

typedef struct {
    uintptr_t handler;
    unsigned long flags;
    uintptr_t restorer;
    unsigned long mask;
} PeakSignalPolicyRawSigaction;

static long
peak_signal_policy_forward_syscall(long number,
                                   long a1,
                                   long a2,
                                   long a3,
                                   long a4,
                                   long a5,
                                   long a6)
{
    return real_syscall_fn(number, a1, a2, a3, a4, a5, a6);
}

__attribute__((visibility("default"))) long
peak_signal_policy_syscall(long number,
                           long a1,
                           long a2,
                           long a3,
                           long a4,
                           long a5,
                           long a6) __asm__("syscall");

__attribute__((visibility("default"))) long
peak_signal_policy_syscall(long number,
                           long a1,
                           long a2,
                           long a3,
                           long a4,
                           long a5,
                           long a6)
{
    long exec_result = -1;

    if (peak_signal_policy_enabled_for_process() &&
        peak_exec_handle_syscall != NULL &&
        peak_exec_handle_syscall(number,
                                 a1,
                                 a2,
                                 a3,
                                 a4,
                                 a5,
                                 a6,
                                 &exec_result)) {
        return exec_result;
    }

    peak_signal_policy_ensure_real_symbols();
    if (real_syscall_fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (!peak_signal_policy_enabled_for_process()) {
        return peak_signal_policy_forward_syscall(number,
                                                  a1,
                                                  a2,
                                                  a3,
                                                  a4,
                                                  a5,
                                                  a6);
    }

    switch (number) {
#ifdef SYS_gettid
        case SYS_gettid:
            return real_syscall_fn(number);
#endif
#ifdef SYS_rt_sigaction
        case SYS_rt_sigaction:
            if (a2 != 0 &&
                !peak_signal_policy_prepare_reserved_signal_for_user(
                    (int)a1,
                    "syscall:rt_sigaction")) {
                return -1;
            }
            if (a2 == 0 && a3 != 0 &&
                peak_signal_policy_should_hide_raw_sigaction_query((int)a1)) {
                PeakSignalPolicyRawSigaction action;
                memset(&action, 0, sizeof(action));
                long rc = real_syscall_fn(number, a1, 0, &action, a4);
                if (rc != 0) {
                    return rc;
                }
                memset(&action, 0, sizeof(action));
                action.handler = (uintptr_t)SIG_DFL;
                if (!peak_signal_policy_safe_write((void*)(uintptr_t)a3,
                                                   &action,
                                                   sizeof(action))) {
                    errno = EFAULT;
                    return -1;
                }
                return 0;
            }
            return real_syscall_fn(number, a1, a2, a3, a4);
#endif
#ifdef SYS_rt_sigprocmask
        case SYS_rt_sigprocmask:
            if ((a1 == SIG_BLOCK || a1 == SIG_SETMASK) &&
                !peak_signal_policy_prepare_reserved_raw_set_for_user(
                    (const void*)(uintptr_t)a2,
                    a4 > 0 ? (size_t)a4 : 0,
                    "syscall:rt_sigprocmask")) {
                return -1;
            }
            return real_syscall_fn(number, a1, a2, a3, a4);
#endif
#ifdef SYS_rt_sigsuspend
        case SYS_rt_sigsuspend:
            if (!peak_signal_policy_prepare_reserved_raw_set_for_user(
                    (const void*)(uintptr_t)a1,
                    a2 > 0 ? (size_t)a2 : 0,
                    "syscall:rt_sigsuspend")) {
                return -1;
            }
            return real_syscall_fn(number, a1, a2);
#endif
#ifdef SYS_rt_sigtimedwait
        case SYS_rt_sigtimedwait:
            if (!peak_signal_policy_prepare_reserved_raw_set_for_user(
                    (const void*)(uintptr_t)a1,
                    a4 > 0 ? (size_t)a4 : 0,
                    "syscall:rt_sigtimedwait")) {
                return -1;
            }
            return real_syscall_fn(number, a1, a2, a3, a4);
#endif
#ifdef SYS_signalfd
        case SYS_signalfd:
            if (!peak_signal_policy_prepare_reserved_raw_set_for_user(
                    (const void*)(uintptr_t)a2,
                    a3 > 0 ? (size_t)a3 : 0,
                    "syscall:signalfd")) {
                return -1;
            }
            return real_syscall_fn(number, a1, a2, a3);
#endif
#ifdef SYS_signalfd4
        case SYS_signalfd4:
            if (!peak_signal_policy_prepare_reserved_raw_set_for_user(
                    (const void*)(uintptr_t)a2,
                    a3 > 0 ? (size_t)a3 : 0,
                    "syscall:signalfd4")) {
                return -1;
            }
            return real_syscall_fn(number, a1, a2, a3, a4);
#endif
#ifdef SYS_timer_create
        case SYS_timer_create:
            if (!peak_signal_policy_prepare_event_for_user(
                    (const struct sigevent*)(uintptr_t)a2,
                    "syscall:timer_create")) {
                return -1;
            }
            return real_syscall_fn(number, a1, a2, a3);
#endif
#ifdef SYS_mq_notify
        case SYS_mq_notify:
            if (!peak_signal_policy_prepare_event_for_user(
                    (const struct sigevent*)(uintptr_t)a2,
                    "syscall:mq_notify")) {
                return -1;
            }
            return real_syscall_fn(number, a1, a2);
#endif
#ifdef SYS_kill
        case SYS_kill:
            if (!peak_signal_policy_prepare_reserved_signal_for_user(
                    (int)a2,
                    "syscall:kill")) {
                return -1;
            }
            return real_syscall_fn(number, a1, a2);
#endif
#ifdef SYS_tkill
        case SYS_tkill:
            if (!peak_signal_policy_prepare_reserved_signal_for_user(
                    (int)a2,
                    "syscall:tkill")) {
                return -1;
            }
            return real_syscall_fn(number, a1, a2);
#endif
#ifdef SYS_tgkill
        case SYS_tgkill:
            if ((int)a3 != 0 &&
                !peak_signal_policy_prepare_reserved_signal_for_user(
                    (int)a3,
                    "syscall:tgkill")) {
                return -1;
            }
            return real_syscall_fn(number, a1, a2, a3);
#endif
#ifdef SYS_rt_sigqueueinfo
        case SYS_rt_sigqueueinfo:
            if (!peak_signal_policy_prepare_reserved_signal_for_user(
                    (int)a2,
                    "syscall:rt_sigqueueinfo")) {
                return -1;
            }
            return real_syscall_fn(number, a1, a2, a3);
#endif
#ifdef SYS_rt_tgsigqueueinfo
        case SYS_rt_tgsigqueueinfo:
            if (!peak_signal_policy_prepare_reserved_signal_for_user(
                    (int)a3,
                    "syscall:rt_tgsigqueueinfo")) {
                return -1;
            }
            return real_syscall_fn(number, a1, a2, a3, a4);
#endif
#ifdef SYS_ppoll
        case SYS_ppoll:
            if (!peak_signal_policy_prepare_reserved_raw_set_for_user(
                    (const void*)(uintptr_t)a4,
                    a5 > 0 ? (size_t)a5 : 0,
                    "syscall:ppoll")) {
                return -1;
            }
            return real_syscall_fn(number, a1, a2, a3, a4, a5);
#endif
#ifdef SYS_pselect6
        case SYS_pselect6:
            if (a6 != 0) {
                PeakSignalPolicyPselect6Sigmask sigmask;
                if (peak_signal_policy_safe_read(&sigmask,
                                                 (const void*)(uintptr_t)a6,
                                                 sizeof(sigmask))) {
                    if (!peak_signal_policy_prepare_reserved_raw_set_for_user(
                            sigmask.sigmask,
                            sigmask.sigsetsize,
                            "syscall:pselect6")) {
                        return -1;
                    }
                }
            }
            return peak_signal_policy_forward_syscall(number,
                                                      a1,
                                                      a2,
                                                      a3,
                                                      a4,
                                                      a5,
                                                      a6);
#endif
        default:
            return peak_signal_policy_forward_syscall(number,
                                                      a1,
                                                      a2,
                                                      a3,
                                                      a4,
                                                      a5,
                                                      a6);
    }
}

__attribute__((constructor))
static void
peak_signal_policy_constructor(void)
{
    if (peak_signal_policy_enabled_for_process() &&
        peak_signal_policy_should_reserve_early()) {
        (void)peak_signal_policy_choose_reserved_signal();
        (void)peak_signal_policy_unblock_reserved_for_current_thread();
    }
}
