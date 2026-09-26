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
#include <time.h>
#if defined(__linux__)
#include <dlfcn.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>
/* One atomic word contains fork generation, reader count and closed admission.
 * Closing entry preserves existing readers; the writer drains for at most 1 ms
 * before deferring without sending a signal or ptrace stop. Nested queries on
 * an admitted thread retain that thread's outer reader, preventing self-wait.
 * Gate reopening still precedes backend release acknowledgements.
 *
 * Wrappers preserve return/errno, never retry EINTR and do not mask user signals.
 * Cancellation unwinds admission; siglongjmp may leak a reader, causing bounded
 * safe deferral. Raw/hidden libc calls and preexisting late aborted private
 * signal delivery remain outside this normal-stop protection.
 */
#define STOP_BIT (UINT64_C(1) << 31)
#define COUNT_MASK (STOP_BIT - 1)
#define GENERATION_SHIFT 32
#define DRAIN_BUDGET_NS 1000000L
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0),
               "filesystem-stat admission requires lock-free 64-bit atomics");
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2,
               "filesystem-stat TLS publication requires lock-free pointers");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "filesystem-stat TLS reservations require lock-free atomics");
static _Atomic uint64_t regions;
typedef struct RegionToken
{
    _Atomic unsigned generation;
    _Atomic int admitted;
    _Atomic int used;
    _Atomic(struct RegionToken *) previous;
} RegionToken;
/* TLS storage remains valid after siglongjmp abandons a wrapper stack frame.
 * Exhaustion establishes a reusable permanent reader: later stops safely defer
 * without allocating memory or changing application query return semantics. */
/* Existing initial-exec TLS also constrains dlopen of the whole DSO. Keep the
 * inline nesting quota small; deeper nesting uses the safe poison fallback. */
#define TOKEN_CAPACITY 8
static _Thread_local RegionToken tokens[TOKEN_CAPACITY];
static _Thread_local _Atomic(RegionToken *) active_tokens;
static _Thread_local _Atomic uint64_t overflow_poison;
static RegionToken *
reserve_token(void)
{
    for (unsigned i = 0; i < TOKEN_CAPACITY; ++i)
        if (!atomic_exchange_explicit(&tokens[i].used, 1, memory_order_seq_cst))
            return &tokens[i];
    return NULL;
}
static _Thread_local _Atomic unsigned writer_generation;
static _Thread_local _Atomic int writer_owned;
static _Atomic unsigned long deferred_stops;
static _Thread_local _Atomic unsigned controller_depth;
static _Thread_local _Atomic int resolving;
static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static _Atomic int resolved;
static int fork_safe;
static int (*next_statfs)(const char *, struct statfs *);
static int (*next_fstatfs)(int, struct statfs *);
static int (*next_statfs64)(const char *, struct statfs64 *);
static int (*next_fstatfs64)(int, struct statfs64 *);
#ifdef PEAK_FILESYSTEM_STAT_GUARD_TESTING
static void (*child_reset_hook)(void);
void peak_filesystem_stat_guard_test_child_hook(void (*hook)(void))
{
    child_reset_hook = hook;
}
uint64_t peak_filesystem_stat_guard_test_state(void)
{
    return atomic_load_explicit(&regions, memory_order_seq_cst);
}
#endif
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
    atomic_store_explicit(&resolved, 1, memory_order_release);
    resolving = 0;
}
static void
initialize(void)
{
    int saved = errno;
    if (!atomic_load_explicit(&resolved, memory_order_acquire) && !resolving)
        (void)pthread_once(&resolve_once, resolve_functions);
    errno = saved;
}
static unsigned
word_generation(uint64_t word)
{
    return (unsigned)(word >> GENERATION_SHIFT);
}
static int
enter_region(RegionToken *token)
{
    if (controller_depth)
        return 0;
    for (;;)
    {
        uint64_t old = atomic_load_explicit(&regions, memory_order_seq_cst);
        unsigned current_generation = word_generation(old);
        if (!token && overflow_poison == ((uint64_t)current_generation << GENERATION_SHIFT | 1))
            return 1;
        RegionToken *head = atomic_load_explicit(&active_tokens, memory_order_seq_cst);
        int nested = head && head->admitted &&
                     head->generation == current_generation;
        if (((old & STOP_BIT) && !nested) || (old & COUNT_MASK) == COUNT_MASK)
        {
            (void)sched_yield();
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(
                &regions, &old, old + 1, memory_order_seq_cst, memory_order_seq_cst))
        {
            if (!token)
            {
                /* Nonlocal exits can exhaust stable TLS slots. Preserve query
                 * semantics with a permanently held reader, not an unguarded
                 * syscall or an allocation inside a user's signal handler. */
                overflow_poison = ((uint64_t)current_generation << GENERATION_SHIFT) | 1;
                if (current_generation != word_generation(
                        atomic_load_explicit(&regions, memory_order_seq_cst)))
                    continue;
                return 1;
            }
            token->generation = current_generation;
            token->admitted = 1;
            token->previous = active_tokens;
            active_tokens = token;
            /* A child fork preserves published current-thread tokens. If fork
             * occurred before publication, its reset invalidates this CAS. */
            if (token->generation != word_generation(
                    atomic_load_explicit(&regions, memory_order_seq_cst)))
            {
                active_tokens = token->previous;
                token->admitted = 0;
                continue;
            }
            return 1;
        }
    }
}
static void
leave_region(RegionToken *token)
{
    if (!token->admitted)
        return;
    /* Revoke nested eligibility BEFORE dropping the last held reader. A user
     * handler in this gap can wait only until the bounded writer reopens entry. */
    active_tokens = token->previous;
    token->admitted = 0;
    unsigned generation = token->generation;
    uint64_t current = atomic_load_explicit(&regions, memory_order_seq_cst);
    while (word_generation(current) == generation && (current & COUNT_MASK))
    {
        if (atomic_compare_exchange_weak_explicit(
                &regions, &current, current - 1, memory_order_seq_cst,
                memory_order_seq_cst))
            break;
    }
}
int
peak_filesystem_stat_guard_try_stop(void)
{
    int saved = errno;
    int ok = 0;
    initialize();
    uint64_t current = atomic_load_explicit(&regions, memory_order_seq_cst);
    unsigned generation = word_generation(current);
    struct timespec start, now;
    RegionToken *head = atomic_load_explicit(&active_tokens, memory_order_seq_cst);
    if (!atomic_load_explicit(&resolved, memory_order_acquire) || !fork_safe || writer_owned ||
        (head && head->admitted && head->generation == generation) ||
        clock_gettime(CLOCK_MONOTONIC, &start) != 0)
        goto deferred;
    for (;;)
    {
        if ((current & STOP_BIT) || word_generation(current) != generation)
            goto deferred;
        if (atomic_compare_exchange_weak_explicit(
                &regions, &current, current | STOP_BIT, memory_order_seq_cst,
                memory_order_seq_cst))
            break;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
            (now.tv_sec - start.tv_sec) * 1000000000LL +
                now.tv_nsec - start.tv_nsec >= DRAIN_BUDGET_NS)
            goto deferred;
    }
    writer_generation = generation;
    writer_owned = 1;
    for (;;)
    {
        current = atomic_load_explicit(&regions, memory_order_seq_cst);
        if (word_generation(current) != generation)
            goto deferred;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
            (now.tv_sec - start.tv_sec) * 1000000000LL +
                now.tv_nsec - start.tv_nsec >= DRAIN_BUDGET_NS)
        {
            peak_filesystem_stat_guard_resume();
            goto deferred;
        }
        if (!(current & COUNT_MASK))
        {
            ok = 1;
            break;
        }
        (void)sched_yield();
    }
    errno = saved;
    return ok;
deferred:
    if (writer_owned && writer_generation == generation &&
        word_generation(atomic_load_explicit(&regions, memory_order_seq_cst)) != generation)
        writer_owned = 0;
    atomic_fetch_add_explicit(&deferred_stops, 1, memory_order_relaxed);
    errno = saved;
    return 0;
}
__attribute__((visibility("default"))) unsigned long
peak_filesystem_stat_guard_deferred_stop_count(void)
{
    return atomic_load_explicit(&deferred_stops, memory_order_relaxed);
}
void
peak_filesystem_stat_guard_resume(void)
{
    if (!writer_owned)
        return;
    unsigned generation = writer_generation;
    writer_owned = 0;
    uint64_t current = atomic_load_explicit(&regions, memory_order_seq_cst);
    while (word_generation(current) == generation && (current & STOP_BIT))
    {
        if (atomic_compare_exchange_weak_explicit(
                &regions, &current, current & ~STOP_BIT, memory_order_seq_cst,
                memory_order_seq_cst))
            break;
    }
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
    writer_owned = 0;
    controller_depth = 0;
    /* Open inherited entry before retagging TLS tokens. A nested application
     * handler must not wait behind a writer thread that disappeared at fork. */
    atomic_fetch_and_explicit(&regions, ~STOP_BIT, memory_order_seq_cst);
    for (;;)
    {
        uint64_t previous = atomic_load_explicit(&regions, memory_order_seq_cst);
        unsigned generation = word_generation(previous) + 1U;
        uint64_t live_readers = overflow_poison ? 1 : 0;
        if (overflow_poison)
            overflow_poison = ((uint64_t)generation << GENERATION_SHIFT) | 1;
        for (RegionToken *token = active_tokens; token; token = token->previous)
        {
            if (token->admitted)
            {
                token->generation = generation;
                ++live_readers;
            }
        }
#ifdef PEAK_FILESYSTEM_STAT_GUARD_TESTING
        if (child_reset_hook)
            child_reset_hook();
#endif
        /* CAS also avoids rewinding an epoch if a nested handler forks again. */
        if (atomic_compare_exchange_weak_explicit(
                &regions, &previous,
                ((uint64_t)generation << GENERATION_SHIFT) | live_readers,
                memory_order_seq_cst, memory_order_seq_cst))
            break;
    }
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
static void
cleanup_region(void *data)
{
    RegionToken *token = data;
    if (!token)
        return;
    leave_region(token);
    token->admitted = 0;
    atomic_store_explicit(&token->used, 0, memory_order_seq_cst);
}
#define WRAP(name, argtype, buftype, raw)                                         \
    __attribute__((visibility("default"))) int name(argtype arg, buftype *buf)    \
    {                                                                             \
        int incoming = errno;                                                     \
        if (resolving && !atomic_load_explicit(&resolved, memory_order_acquire))  \
            return raw(arg, buf);                                                 \
        int old_cancel, end_cancel;                                               \
        (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel);        \
        initialize();                                                             \
        RegionToken *token = reserve_token();                                     \
        (void)enter_region(token);                                                \
        int rc, outgoing;                                                         \
        pthread_cleanup_push(cleanup_region, token);                              \
        (void)pthread_setcancelstate(old_cancel, NULL);                           \
        errno = incoming;                                                         \
        rc = next_##name ? next_##name(arg, buf) : raw(arg, buf);                 \
        outgoing = errno;                                                         \
        (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &end_cancel);        \
        cleanup_region(token);                                                    \
        pthread_cleanup_pop(0);                                                   \
        (void)pthread_setcancelstate(end_cancel, NULL);                           \
        errno = outgoing;                                                         \
        return rc;                                                                \
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
