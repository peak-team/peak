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
/* Four stable 8-byte records and one 8-byte control word keep this module's
 * TLS footprint at 40 bytes, including application-handler-safe bookkeeping.
 * Record: generation[0:31], previous index[32:34], used[35], admitted[36].
 * Control: head index[0:2], resolving[3], writer[4], poison[5], saturated[6],
 * controller depth[7:31], shared writer/poison generation[32:63]. Index 0 is
 * the empty-list/overflow sentinel; valid record indices are 1 through 4.
 * Permanent poison excludes writer admission, so their epochs can share bits.
 */
#define TOKEN_CAPACITY 4
#define INDEX_MASK UINT64_C(7)
#define TOKEN_PREVIOUS_SHIFT 32
#define TOKEN_USED (UINT64_C(1) << 35)
#define TOKEN_ADMITTED (UINT64_C(1) << 36)
#define TOKEN_GENERATION_MASK UINT64_C(0xffffffff)
#define CONTROL_RESOLVING (UINT64_C(1) << 3)
#define CONTROL_WRITER (UINT64_C(1) << 4)
#define CONTROL_POISON (UINT64_C(1) << 5)
#define CONTROL_SATURATED (UINT64_C(1) << 6)
#define CONTROL_DEPTH_SHIFT 7
#define CONTROL_DEPTH_MASK UINT64_C(0xffffff80)
#define CONTROL_GENERATION_MASK UINT64_C(0xffffffff00000000)
_Static_assert(TOKEN_CAPACITY <= INDEX_MASK, "filesystem-stat token index range");
static _Thread_local _Atomic uint64_t tokens[TOKEN_CAPACITY];
static _Thread_local _Atomic uint64_t control;
static _Atomic unsigned long deferred_stops;
static uint64_t
update_control(uint64_t mask, uint64_t value)
{
    uint64_t old = atomic_load_explicit(&control, memory_order_seq_cst);
    while (!atomic_compare_exchange_weak_explicit(
        &control, &old, (old & ~mask) | (value & mask), memory_order_seq_cst,
        memory_order_seq_cst))
    {
    }
    return old;
}
static unsigned
reserve_token(void)
{
    for (unsigned i = 0; i < TOKEN_CAPACITY; ++i)
    {
        uint64_t old = atomic_load_explicit(&tokens[i], memory_order_seq_cst);
        while (!(old & TOKEN_USED))
            if (atomic_compare_exchange_weak_explicit(
                    &tokens[i], &old, TOKEN_USED, memory_order_seq_cst,
                    memory_order_seq_cst))
                return i + 1;
    }
    return 0;
}
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
    update_control(CONTROL_RESOLVING, CONTROL_RESOLVING);
    next_statfs = dlsym(RTLD_NEXT, "statfs");
    next_fstatfs = dlsym(RTLD_NEXT, "fstatfs");
    next_statfs64 = dlsym(RTLD_NEXT, "statfs64");
    next_fstatfs64 = dlsym(RTLD_NEXT, "fstatfs64");
    /* Register before any reader admission, independently of controller init. */
    fork_safe =
        pthread_atfork(NULL, NULL, peak_filesystem_stat_guard_after_fork_child) == 0;
    atomic_store_explicit(&resolved, 1, memory_order_release);
    update_control(CONTROL_RESOLVING, 0);
}
static void
initialize(void)
{
    int saved = errno;
    if (!atomic_load_explicit(&resolved, memory_order_acquire) && !(atomic_load_explicit(&control, memory_order_seq_cst) & CONTROL_RESOLVING))
        (void)pthread_once(&resolve_once, resolve_functions);
    errno = saved;
}
static unsigned
word_generation(uint64_t word)
{
    return (unsigned)(word >> GENERATION_SHIFT);
}
static int
enter_region(unsigned index)
{
    uint64_t state = atomic_load_explicit(&control, memory_order_seq_cst);
    if ((state & CONTROL_DEPTH_MASK) &&
        (!(state & CONTROL_SATURATED) || (state & CONTROL_WRITER)))
        return 0;
    for (;;)
    {
        uint64_t old = atomic_load_explicit(&regions, memory_order_seq_cst);
        unsigned generation = word_generation(old);
        state = atomic_load_explicit(&control, memory_order_seq_cst);
        if (!index && (state & CONTROL_POISON) && word_generation(state) == generation)
            return 1;
        unsigned head = (unsigned)(state & INDEX_MASK);
        uint64_t head_token = head ? atomic_load_explicit(
            &tokens[head - 1], memory_order_seq_cst) : 0;
        int nested = (head_token & TOKEN_ADMITTED) &&
                     (unsigned)(head_token & TOKEN_GENERATION_MASK) == generation;
        if (((old & STOP_BIT) && !nested) || (old & COUNT_MASK) == COUNT_MASK)
        {
            (void)sched_yield();
            continue;
        }
        if (!atomic_compare_exchange_weak_explicit(
                &regions, &old, old + 1, memory_order_seq_cst, memory_order_seq_cst))
            continue;
        if (!index)
        {
            /* A permanent reader preserves query semantics after nonlocal exits
             * exhaust records. Later calls reuse it instead of growing count. */
            update_control(CONTROL_GENERATION_MASK | CONTROL_POISON,
                           ((uint64_t)generation << GENERATION_SHIFT) | CONTROL_POISON);
            state = atomic_load_explicit(&control, memory_order_seq_cst);
            if ((state & CONTROL_POISON) && word_generation(state) == word_generation(
                    atomic_load_explicit(&regions, memory_order_seq_cst)))
                return 1;
            continue;
        }
        state = atomic_load_explicit(&control, memory_order_seq_cst);
        uint64_t metadata = generation | TOKEN_USED | TOKEN_ADMITTED |
                            ((state & INDEX_MASK) << TOKEN_PREVIOUS_SHIFT);
        atomic_store_explicit(&tokens[index - 1], metadata, memory_order_seq_cst);
        update_control(INDEX_MASK, index);
        /* Child reset preserves published live records; a fork before head
         * publication invalidates this admission and requires readmission. */
        metadata = atomic_load_explicit(&tokens[index - 1], memory_order_seq_cst);
        if ((unsigned)(metadata & TOKEN_GENERATION_MASK) != word_generation(
                atomic_load_explicit(&regions, memory_order_seq_cst)))
        {
            update_control(INDEX_MASK, (metadata >> TOKEN_PREVIOUS_SHIFT) & INDEX_MASK);
            atomic_fetch_and_explicit(&tokens[index - 1], ~TOKEN_ADMITTED, memory_order_seq_cst);
            continue;
        }
        return 1;
    }
}
static void
leave_region(unsigned index)
{
    uint64_t metadata = atomic_load_explicit(&tokens[index - 1], memory_order_seq_cst);
    if (!(metadata & TOKEN_ADMITTED))
        return;
    /* Revoke nested eligibility before dropping the last reader. */
    update_control(INDEX_MASK, (metadata >> TOKEN_PREVIOUS_SHIFT) & INDEX_MASK);
    atomic_fetch_and_explicit(&tokens[index - 1], ~TOKEN_ADMITTED, memory_order_seq_cst);
    unsigned generation = (unsigned)(atomic_load_explicit(
        &tokens[index - 1], memory_order_seq_cst) & TOKEN_GENERATION_MASK);
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
    initialize();
    uint64_t current = atomic_load_explicit(&regions, memory_order_seq_cst);
    unsigned generation = word_generation(current);
    int gate_closed = 0;
    struct timespec start, now;
    uint64_t state = atomic_load_explicit(&control, memory_order_seq_cst);
    unsigned head = (unsigned)(state & INDEX_MASK);
    uint64_t head_token = head ? atomic_load_explicit(
        &tokens[head - 1], memory_order_seq_cst) : 0;
    if (!atomic_load_explicit(&resolved, memory_order_acquire) || !fork_safe ||
        (state & (CONTROL_WRITER | CONTROL_POISON | CONTROL_SATURATED)) ||
        ((head_token & TOKEN_ADMITTED) &&
         (unsigned)(head_token & TOKEN_GENERATION_MASK) == generation) ||
        clock_gettime(CLOCK_MONOTONIC, &start) != 0)
        goto deferred;
    for (;;)
    {
        if ((current & STOP_BIT) || word_generation(current) != generation)
            goto deferred;
        if (atomic_compare_exchange_weak_explicit(
                &regions, &current, current | STOP_BIT, memory_order_seq_cst,
                memory_order_seq_cst))
        {
            gate_closed = 1;
            break;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
            (now.tv_sec - start.tv_sec) * 1000000000LL +
                now.tv_nsec - start.tv_nsec >= DRAIN_BUDGET_NS)
            goto deferred;
    }
    /* Fork can occur between global gate closure and ownership publication.
     * Do not overwrite a child's poison epoch with stale writer ownership. */
    for (;;)
    {
        state = atomic_load_explicit(&control, memory_order_seq_cst);
        if ((state & CONTROL_POISON) || word_generation(
                atomic_load_explicit(&regions, memory_order_seq_cst)) != generation)
            goto deferred;
        uint64_t next = (state & ~CONTROL_GENERATION_MASK) |
                        ((uint64_t)generation << GENERATION_SHIFT) | CONTROL_WRITER;
        if (atomic_compare_exchange_weak_explicit(
                &control, &state, next, memory_order_seq_cst, memory_order_seq_cst))
            break;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
            (now.tv_sec - start.tv_sec) * 1000000000LL +
                now.tv_nsec - start.tv_nsec >= DRAIN_BUDGET_NS)
            goto deferred;
    }
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
            gate_closed = 0;
            goto deferred;
        }
        if (!(current & COUNT_MASK))
        {
            errno = saved;
            return 1;
        }
        (void)sched_yield();
    }
deferred:
    if (gate_closed)
    {
        current = atomic_load_explicit(&regions, memory_order_seq_cst);
        while (word_generation(current) == generation && (current & STOP_BIT))
            if (atomic_compare_exchange_weak_explicit(
                    &regions, &current, current & ~STOP_BIT, memory_order_seq_cst,
                    memory_order_seq_cst))
                break;
    }
    state = atomic_load_explicit(&control, memory_order_seq_cst);
    while ((state & CONTROL_WRITER) && word_generation(state) == generation &&
           word_generation(atomic_load_explicit(&regions, memory_order_seq_cst)) != generation)
        if (atomic_compare_exchange_weak_explicit(
                &control, &state, state & ~CONTROL_WRITER, memory_order_seq_cst,
                memory_order_seq_cst))
            break;
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
    uint64_t state = atomic_fetch_and_explicit(&control, ~CONTROL_WRITER, memory_order_seq_cst);
    if (!(state & CONTROL_WRITER))
        return;
    unsigned generation = word_generation(state);
    uint64_t current = atomic_load_explicit(&regions, memory_order_seq_cst);
    while (word_generation(current) == generation && (current & STOP_BIT))
        if (atomic_compare_exchange_weak_explicit(
                &regions, &current, current & ~STOP_BIT, memory_order_seq_cst,
                memory_order_seq_cst))
            break;
}
void
peak_filesystem_stat_guard_controller_enter(void)
{
    uint64_t state = atomic_load_explicit(&control, memory_order_seq_cst);
    for (;;)
    {
        uint64_t next = state;
        if ((state & CONTROL_DEPTH_MASK) == CONTROL_DEPTH_MASK)
            next |= CONTROL_SATURATED;
        else if (!(state & CONTROL_SATURATED))
            next += UINT64_C(1) << CONTROL_DEPTH_SHIFT;
        if (atomic_compare_exchange_weak_explicit(
                &control, &state, next, memory_order_seq_cst, memory_order_seq_cst))
            break;
    }
}
void
peak_filesystem_stat_guard_controller_leave(void)
{
    uint64_t state = atomic_load_explicit(&control, memory_order_seq_cst);
    while ((state & CONTROL_DEPTH_MASK) && !(state & CONTROL_SATURATED))
        if (atomic_compare_exchange_weak_explicit(
                &control, &state, state - (UINT64_C(1) << CONTROL_DEPTH_SHIFT),
                memory_order_seq_cst, memory_order_seq_cst))
            break;
    /* Saturation never carries into flags/epoch. Future stops fail closed and
     * ordinary queries cannot bypass admission after current ownership ends. */
}
void
peak_filesystem_stat_guard_after_fork_child(void)
{
    update_control(CONTROL_WRITER | CONTROL_DEPTH_MASK | CONTROL_SATURATED, 0);
    atomic_fetch_and_explicit(&regions, ~STOP_BIT, memory_order_seq_cst);
    for (;;)
    {
        uint64_t previous = atomic_load_explicit(&regions, memory_order_seq_cst);
        unsigned generation = word_generation(previous) + 1U;
        uint64_t state = update_control(CONTROL_GENERATION_MASK,
                                       (uint64_t)generation << GENERATION_SHIFT);
        uint64_t live_readers = (state & CONTROL_POISON) ? 1 : 0;
        unsigned index = (unsigned)(state & INDEX_MASK);
        while (index)
        {
            uint64_t metadata = atomic_load_explicit(&tokens[index - 1], memory_order_seq_cst);
            if (metadata & TOKEN_ADMITTED)
            {
                while (!atomic_compare_exchange_weak_explicit(
                    &tokens[index - 1], &metadata,
                    (metadata & ~TOKEN_GENERATION_MASK) | generation,
                    memory_order_seq_cst, memory_order_seq_cst))
                {
                }
                ++live_readers;
            }
            index = (unsigned)((metadata >> TOKEN_PREVIOUS_SHIFT) & INDEX_MASK);
        }
#ifdef PEAK_FILESYSTEM_STAT_GUARD_TESTING
        if (child_reset_hook)
            child_reset_hook();
#endif
        if (atomic_compare_exchange_weak_explicit(
                &regions, &previous,
                ((uint64_t)generation << GENERATION_SHIFT) | live_readers,
                memory_order_seq_cst, memory_order_seq_cst))
            break;
    }
}
/* Bootstrap recursion uses the kernel ABI only where native and 64-bit layouts match.
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
    unsigned index = *(unsigned *)data;
    if (!index)
        return;
    leave_region(index);
    atomic_store_explicit(&tokens[index - 1], 0, memory_order_seq_cst);
}
#define WRAP(name, argtype, buftype, raw)                                         \
    __attribute__((visibility("default"))) int name(argtype arg, buftype *buf)    \
    {                                                                             \
        int incoming = errno;                                                     \
        if ((atomic_load_explicit(&control, memory_order_seq_cst) &                \
             CONTROL_RESOLVING) &&                                                \
            !atomic_load_explicit(&resolved, memory_order_acquire))               \
            return raw(arg, buf);                                                 \
        int old_cancel, end_cancel;                                               \
        (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel);        \
        initialize();                                                             \
        unsigned token = reserve_token();                                        \
        (void)enter_region(token);                                                \
        int rc, outgoing;                                                         \
        pthread_cleanup_push(cleanup_region, &token);                              \
        (void)pthread_setcancelstate(old_cancel, NULL);                           \
        errno = incoming;                                                         \
        rc = next_##name ? next_##name(arg, buf) : raw(arg, buf);                 \
        outgoing = errno;                                                         \
        (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &end_cancel);        \
        cleanup_region(&token);                                                    \
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
