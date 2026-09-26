#ifdef _FILE_OFFSET_BITS
#undef _FILE_OFFSET_BITS
#endif
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include "internal/filesystem_stat_guard.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#if defined(__linux__)
#include <dlfcn.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>
/* A successful reader CAS increments a count; a successful writer CAS changes
 * zero to STOP_BIT. These operations cannot both succeed for an overlapping
 * real syscall. Writers defer immediately rather than waiting on application
 * locks. Gate reopening precedes backend release acknowledgements so a nested
 * user signal handler cannot deadlock while calling a filesystem-stat wrapper.
 *
 * Wrappers preserve libc return/errno and do not retry EINTR or block user
 * signals. Cancellation cleanup releases admitted readers. siglongjmp cannot
 * unwind that cleanup: its leaked reader conservatively defers later stops.
 * Direct/raw syscalls and hidden libc calls bypass these four public wrappers.
 * Existing aborted stops with unarrived queued private signals are a separate
 * release-bookkeeping limitation; this gate protects normal completed stops.
 */
#define STOP_BIT (UINT32_C(1) << 31)
static _Atomic uint32_t regions;
static _Atomic unsigned fork_generation;
static _Atomic unsigned long deferred_stops;
static _Thread_local unsigned controller_depth;
static _Thread_local int resolving;
static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static int fork_safe;
static int (*next_statfs)(const char *, struct statfs *);
static int (*next_fstatfs)(int, struct statfs *);
static int (*next_statfs64)(const char *, struct statfs64 *);
static int (*next_fstatfs64)(int, struct statfs64 *);
static void
resolve_functions(void)
{
    resolving = 1;
    next_statfs = dlsym(RTLD_NEXT, "statfs");
    next_fstatfs = dlsym(RTLD_NEXT, "fstatfs");
    next_statfs64 = dlsym(RTLD_NEXT, "statfs64");
    next_fstatfs64 = dlsym(RTLD_NEXT, "fstatfs64");
    /* Register before any reader admission, independently of controller init. */
    fork_safe =
        pthread_atfork(NULL, NULL, peak_filesystem_stat_guard_after_fork_child) == 0;
    resolving = 0;
}
static void
initialize(void)
{
    int saved = errno;
    if (!resolving)
        (void)pthread_once(&resolve_once, resolve_functions);
    errno = saved;
}
static int
enter_region(unsigned *generation)
{
    if (controller_depth)
        return 0;
    for (;;)
    {
        unsigned before = atomic_load_explicit(&fork_generation, memory_order_seq_cst);
        uint32_t old = atomic_load_explicit(&regions, memory_order_seq_cst);
        if ((old & STOP_BIT) || old == STOP_BIT - 1)
        {
            (void)sched_yield();
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(
                &regions, &old, old + 1, memory_order_seq_cst, memory_order_seq_cst))
        {
            if (before == atomic_load_explicit(&fork_generation, memory_order_seq_cst))
            {
                *generation = before;
                return 1;
            }
            /* Fork from a nested user handler may reset admission between
             * generation sampling and CAS. Withdraw a surviving admission,
             * never subtract from the reset zero state, and re-enter. */
            uint32_t current = atomic_load_explicit(&regions, memory_order_seq_cst);
            while (current && !(current & STOP_BIT) &&
                   !atomic_compare_exchange_weak_explicit(
                       &regions, &current, current - 1, memory_order_seq_cst,
                       memory_order_seq_cst))
            {
            }
        }
    }
}
static void
leave_region(int admitted)
{
    if (admitted)
    {
        uint32_t current = atomic_load_explicit(&regions, memory_order_seq_cst);
        while (current && !(current & STOP_BIT) &&
               !atomic_compare_exchange_weak_explicit(&regions, &current, current - 1,
                                                      memory_order_seq_cst,
                                                      memory_order_seq_cst))
        {
        }
    }
}
int
peak_filesystem_stat_guard_try_stop(void)
{
    int saved = errno;
    initialize();
    uint32_t expected = 0;
    int ok = fork_safe && atomic_compare_exchange_strong_explicit(
                              &regions, &expected, STOP_BIT, memory_order_seq_cst,
                              memory_order_seq_cst);
    if (!ok)
        atomic_fetch_add_explicit(&deferred_stops, 1, memory_order_relaxed);
    errno = saved;
    return ok;
}
__attribute__((visibility("default"))) unsigned long
peak_filesystem_stat_guard_deferred_stop_count(void)
{
    return atomic_load_explicit(&deferred_stops, memory_order_relaxed);
}
void
peak_filesystem_stat_guard_resume(void)
{
    atomic_fetch_and_explicit(&regions, ~STOP_BIT, memory_order_seq_cst);
}
void
peak_filesystem_stat_guard_controller_enter(void)
{
    ++controller_depth;
}
void
peak_filesystem_stat_guard_controller_leave(void)
{
    if (controller_depth)
        --controller_depth;
}
void
peak_filesystem_stat_guard_after_fork_child(void)
{
    atomic_store_explicit(&regions, 0, memory_order_seq_cst);
    atomic_fetch_add_explicit(&fork_generation, 1, memory_order_seq_cst);
    controller_depth = 0;
}
/* Bootstrap recursion uses the kernel ABI only where native and64 layouts match.
 * Normal published calls always use libc, preserving its ABI and signal semantics. */
#if defined(__x86_64__) || defined(__aarch64__)
_Static_assert(sizeof(struct statfs) == sizeof(struct statfs64),
               "statfs bootstrap ABI");
#define RAW_STATFS(path, buf) ((int)syscall(SYS_statfs, path, buf))
#define RAW_FSTATFS(fd, buf) ((int)syscall(SYS_fstatfs, fd, buf))
#else
#define RAW_STATFS(path, buf) (errno = ENOSYS, -1)
#define RAW_FSTATFS(fd, buf) (errno = ENOSYS, -1)
#endif
typedef struct
{
    unsigned generation;
    int admitted;
} RegionToken;
static void
cleanup_region(void *data)
{
    RegionToken *token = data;
    if (token->generation ==
        atomic_load_explicit(&fork_generation, memory_order_seq_cst))
        leave_region(token->admitted);
    token->admitted = 0;
}
#define WRAP(name, argtype, buftype, raw)                                              \
    __attribute__((visibility("default"))) int name(argtype arg, buftype *buf)         \
    {                                                                                  \
        int incoming = errno;                                                          \
        if (resolving)                                                                 \
            return raw(arg, buf);                                                      \
        int old_cancel, end_cancel;                                                    \
        (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel);             \
        initialize();                                                                  \
        RegionToken token = {0};                                                       \
        token.admitted = enter_region(&token.generation);                              \
        int rc, outgoing;                                                              \
        pthread_cleanup_push(cleanup_region, &token);                                  \
        (void)pthread_setcancelstate(old_cancel, NULL);                                \
        errno = incoming;                                                              \
        rc = next_##name ? next_##name(arg, buf) : raw(arg, buf);                      \
        outgoing = errno;                                                              \
        (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &end_cancel);             \
        cleanup_region(&token);                                                        \
        pthread_cleanup_pop(0);                                                        \
        (void)pthread_setcancelstate(end_cancel, NULL);                                \
        errno = outgoing;                                                              \
        return rc;                                                                     \
    }
WRAP(statfs, const char *, struct statfs, RAW_STATFS)
WRAP(fstatfs, int, struct statfs, RAW_FSTATFS)
WRAP(statfs64, const char *, struct statfs64, RAW_STATFS)
WRAP(fstatfs64, int, struct statfs64, RAW_FSTATFS)
#else
unsigned long
peak_filesystem_stat_guard_deferred_stop_count(void)
{
    return 0;
}
int
peak_filesystem_stat_guard_try_stop(void)
{
    return 1;
}
void
peak_filesystem_stat_guard_resume(void)
{
}
void
peak_filesystem_stat_guard_controller_enter(void)
{
}
void
peak_filesystem_stat_guard_controller_leave(void)
{
}
void
peak_filesystem_stat_guard_after_fork_child(void)
{
}
#endif
