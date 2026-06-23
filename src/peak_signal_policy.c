#define _GNU_SOURCE
#include "peak_signal_policy_internal.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
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
#include <sys/random.h>
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

static int peak_signal_policy_parse_signal_env(const char* value, int* out);
static int peak_signal_policy_signal_is_available(int signum);
static void peak_signal_policy_init_cookie(void);
static int peak_signal_policy_install_protective_handler(int signum);
static int peak_signal_policy_commit_reserved_signal(int signum);
static int peak_signal_policy_restore_default_handler(int signum);
static int peak_signal_policy_wait_until_migration_enabled(void);

static void*
peak_signal_policy_resolve(const char* name)
{
    return dlsym(RTLD_NEXT, name);
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
            peak_signal_policy_resolve("timer_create");
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
peak_signal_policy_backend_runtime_supported(void)
{
#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__) || defined(__aarch64__))
    return 1;
#else
    return 0;
#endif
}

static int
peak_signal_policy_should_reserve_early(void)
{
    const char* mode = getenv("PEAK_SAFE_DETACH_MODE");
    const char* backend = getenv("PEAK_DETACH_BACKEND");

    if (!peak_signal_policy_backend_runtime_supported()) {
        return 0;
    }
    if (getenv("PEAK_DETACH_SIGNAL") != NULL) {
        return 1;
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
peak_signal_policy_tid_blocks_signal(pid_t tid, int signum)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%ld/status", (long)tid);

    FILE* fp = fopen(path, "r");
    if (fp == NULL) {
        return errno == ENOENT || errno == ESRCH ? 0 : -1;
    }

    char line[256];
    int blocked = -1;
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strncmp(line, "SigBlk:", 7) != 0) {
            continue;
        }

        char* value = line + 7;
        while (*value == ' ' || *value == '\t') {
            value++;
        }

        errno = 0;
        unsigned long long mask = strtoull(value, NULL, 16);
        if (errno == 0 && signum > 0 && signum <= 64) {
            unsigned long long bit = 1ull << (unsigned int)(signum - 1);
            blocked = (mask & bit) != 0 ? 1 : 0;
        }
        break;
    }
    fclose(fp);
    return blocked;
}

static int
peak_signal_policy_signal_unblocked_in_all_threads(int signum)
{
    DIR* dir = opendir("/proc/self/task");
    if (dir == NULL) {
        return 0;
    }

    int ok = 1;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        char* end = NULL;
        errno = 0;
        long tid = strtol(entry->d_name, &end, 10);
        if (errno != 0 || end == entry->d_name || *end != '\0' || tid <= 0) {
            continue;
        }

        int blocked = peak_signal_policy_tid_blocks_signal((pid_t)tid, signum);
        if (blocked != 0) {
            ok = 0;
            break;
        }
    }

    closedir(dir);
    return ok;
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
           peak_signal_policy_signal_is_available(signum) &&
           peak_signal_policy_signal_unblocked_in_all_threads(signum);
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
    if (peak_signal_policy_is_internal()) {
        return 1;
    }

    if (pthread_mutex_lock(&migration_mutex) != 0) {
        errno = EINVAL;
        return 0;
    }

    int signum = atomic_load_explicit(&reserved_signal,
                                      memory_order_acquire);
    if (signum <= 0 || set == NULL || sigismember(set, signum) != 1) {
        pthread_mutex_unlock(&migration_mutex);
        return 1;
    }

    if (peak_signal_policy_migrate_reserved_signal_locked(signum, set, api)) {
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
    if (peak_signal_policy_is_internal() || signum <= 0) {
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
peak_signal_policy_event_signal(const struct sigevent* evp)
{
    if (evp == NULL ||
        (evp->sigev_notify != SIGEV_SIGNAL &&
#ifdef SIGEV_THREAD_ID
         evp->sigev_notify != SIGEV_THREAD_ID &&
#endif
         1)) {
        return 0;
    }
    return evp->sigev_signo;
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

    if (getrandom(&seed, sizeof(seed), GRND_NONBLOCK) != (ssize_t)sizeof(seed)) {
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

unsigned long
peak_signal_policy_cookie_for(int epoch, pid_t tid)
{
    peak_signal_policy_init_cookie();
    unsigned long base =
        atomic_load_explicit(&cookie_base, memory_order_acquire);
    unsigned long e = (unsigned long)(uint32_t)epoch;
    unsigned long t = (unsigned long)(uint32_t)tid;
    return base ^ (e << 32) ^ (t * 0x45d9f3bu);
}

int
peak_signal_policy_cookie_matches(const siginfo_t* info, int epoch, pid_t tid)
{
    if (info == NULL || info->si_code != SI_QUEUE ||
        info->si_pid != getpid() || info->si_uid != getuid()) {
        return 0;
    }
    return (unsigned long)(uintptr_t)info->si_value.sival_ptr ==
           peak_signal_policy_cookie_for(epoch, tid);
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
    int reserved = atomic_load_explicit(&reserved_signal, memory_order_acquire);
    if (!peak_signal_policy_is_internal() && act == NULL && oldact != NULL &&
        signum > 0 &&
        (signum == reserved ||
         peak_signal_policy_signal_is_transitioning(signum))) {
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
    if (how == SIG_BLOCK || how == SIG_SETMASK) {
        if (!peak_signal_policy_prepare_reserved_set_for_user(
                set,
                "pthread_sigmask")) {
            return EINVAL;
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
    if (!peak_signal_policy_prepare_reserved_set_for_user(set, "sigwait")) {
        return EINVAL;
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
    if (!peak_signal_policy_prepare_reserved_set_for_user(mask, "signalfd")) {
        return -1;
    }
    return real_signalfd_fn(fd, mask, flags);
}

__attribute__((visibility("default"))) int
signalfd4(int fd, const sigset_t* mask, int sizemask, int flags)
{
    peak_signal_policy_ensure_real_symbols();
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
    int signum = peak_signal_policy_event_signal(evp);
    if (signum > 0 &&
        !peak_signal_policy_prepare_reserved_signal_for_user(signum,
                                                             "timer_create")) {
        return -1;
    }
    return real_timer_create_fn(clockid, evp, timerid);
}

__attribute__((visibility("default"))) int
kill(pid_t pid, int sig)
{
    peak_signal_policy_ensure_real_symbols();
    if (real_kill_fn == NULL) {
        errno = ENOSYS;
        return -1;
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
    if (!peak_signal_policy_prepare_reserved_set_for_user(sigmask, "ppoll")) {
        return -1;
    }
    return real_ppoll_fn(fds, nfds, timeout, sigmask);
}

__attribute__((constructor))
static void
peak_signal_policy_constructor(void)
{
    if (peak_signal_policy_should_reserve_early()) {
        (void)peak_signal_policy_choose_reserved_signal();
        (void)peak_signal_policy_unblock_reserved_for_current_thread();
    }
}
