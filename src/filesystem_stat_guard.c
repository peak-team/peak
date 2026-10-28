#ifdef _FILE_OFFSET_BITS
#undef _FILE_OFFSET_BITS
#endif
#define _GNU_SOURCE
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#include "internal/filesystem_stat_guard.h"
#if defined(PEAK_ATTACH_RUNTIME) && defined(__linux__)
#include "detach_controller.h"
#include "internal/gum_module_mutation.h"
#include "logging.h"
#endif
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
 * Closing entry preserves existing readers; active-reader wait has a 16 ms
 * deadline before deferring without a signal or ptrace stop. A completed drain
 * remains safe after writer scheduling delay. Nested queries on
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
#define DRAIN_BUDGET_NS 16000000L
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
#ifdef PEAK_ATTACH_RUNTIME
    /* This DSO is loaded after libc, so RTLD_NEXT cannot find earlier libc
     * entries. Attached wrappers have private names and cannot resolve here. */
    void *scope = RTLD_DEFAULT;
#else
    void *scope = RTLD_NEXT;
#endif
    next_statfs = dlsym(scope, "statfs");
    next_fstatfs = dlsym(scope, "fstatfs");
    next_statfs64 = dlsym(scope, "statfs64");
    next_fstatfs64 = dlsym(scope, "fstatfs64");
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
        /* Exhaustion during normal nesting borrows the counted outer query.
         * Cancellation unwinds it last; a nonlocal exit leaves its stable
         * admitted record counted, so even a stale head remains fail closed.
         * Fork preserves and retags that record before returning to this call. */
        if (!index && nested)
            return 1;
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
        /* A descheduled writer may observe completed drain after its budget.
         * The closed, same-generation zero-reader gate is already safe. */
        if (!(current & COUNT_MASK))
        {
            errno = saved;
            return 1;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
            (now.tv_sec - start.tv_sec) * 1000000000LL +
                now.tv_nsec - start.tv_nsec >= DRAIN_BUDGET_NS)
        {
            peak_filesystem_stat_guard_resume();
            gate_closed = 0;
            goto deferred;
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
peak_filesystem_stat_guard_controller_after_fork_child(void)
{
    /* The controller's atfork registration can precede lazy query resolution.
     * Its child mutex is reset rather than unlocked: discard the matching
     * inherited bypass scope without resolving symbols inside the callback.
     * Reader generations remain owned by the guard's separate child callback. */
    update_control(CONTROL_DEPTH_MASK | CONTROL_SATURATED, 0);
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
#ifdef PEAK_ATTACH_RUNTIME
#define GUARD_WRAPPER_NAME(name) peak_attach_guard_##name
#define GUARD_WRAPPER_SCOPE static
#else
#define GUARD_WRAPPER_NAME(name) name
#define GUARD_WRAPPER_SCOPE __attribute__((visibility("default")))
#endif
#define WRAP(name, argtype, buftype, raw)                                         \
    GUARD_WRAPPER_SCOPE int GUARD_WRAPPER_NAME(name)(argtype arg, buftype *buf)     \
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

#ifdef PEAK_ATTACH_RUNTIME
static _Atomic(uintptr_t) guarded_entries[4];

int
peak_filesystem_stat_guard_owns_address(const void *address)
{
    for (size_t i = 0; i < G_N_ELEMENTS(guarded_entries); i++)
        if ((uintptr_t) address != 0 && (uintptr_t) address ==
            atomic_load_explicit(&guarded_entries[i], memory_order_acquire))
            return 1;
    return 0;
}

int
peak_filesystem_stat_guard_attach(void)
{
    const char *names[] = {"statfs", "fstatfs", "statfs64", "fstatfs64"};
    gpointer wrappers[] = {peak_attach_guard_statfs, peak_attach_guard_fstatfs,
        peak_attach_guard_statfs64, peak_attach_guard_fstatfs64};
    gpointer targets[4], originals[4] = {NULL};
    size_t owners[4], unique_count = 0, prepared_count = 0;
    gboolean installed[4] = {FALSE}, success = TRUE;
    PeakDetachRequest requests[4] = {0};
    PeakDetachBatchResult results[4] = {0};
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    GumInterceptor *guard_interceptor;
    int saved_errno = errno;

    initialize();
    if (!atomic_load_explicit(&resolved, memory_order_acquire) || !fork_safe) {
        errno = saved_errno;
        return 0;
    }
    targets[0] = (gpointer) next_statfs;
    targets[1] = (gpointer) next_fstatfs;
    targets[2] = (gpointer) next_statfs64;
    targets[3] = (gpointer) next_fstatfs64;
    guard_interceptor = gum_interceptor_obtain();
    /* 64-bit libc commonly exports each native/64 pair at the same entry.
     * Replace each address once; the ABI equality above covers those aliases. */
    for (size_t i = 0; i < G_N_ELEMENTS(targets); i++) {
        if (targets[i] == NULL) {
            /* Native APIs are required; optional explicit-64 aliases can be
             * absent on non-glibc systems. Never replace a null address. */
            if (i < 2)
                success = FALSE;
            owners[i] = SIZE_MAX;
            continue;
        }
        size_t owner;
        for (owner = 0; owner < unique_count; owner++)
            if (requests[owner].function_address == targets[i])
                break;
        owners[i] = owner;
        if (owner != unique_count)
            continue;
        requests[unique_count] = (PeakDetachRequest){
            .hook_id = SIZE_MAX - 48 - unique_count,
            .symbol_name = names[i],
            .function_address = targets[i],
            .interceptor = guard_interceptor,
            .operation = PEAK_DETACH_OPERATION_REPLACE,
            .replacement_function = wrappers[i],
            .original_function = &originals[unique_count],
        };
        unique_count++;
    }
    if (!success || unique_count == 0) {
        g_object_unref(guard_interceptor);
        errno = saved_errno;
        return 0;
    }
    peak_gum_module_mutation_begin();
    gboolean prepared = peak_detach_controller_prepare_hook_mutation_batch(
        requests, unique_count, results, &prepared_count, &status);
    if (!prepared || prepared_count != unique_count) {
        if (prepared &&
            !peak_detach_controller_finish_hook_mutation_batch(&status))
            peak_detach_controller_abort_after_failed_finish(
                "filesystem-stat attach refusal", status);
        peak_gum_module_mutation_end();
        peak_log_warn("[peak] filesystem-stat guards could not obtain a safe attach window (%s)\n",
                      peak_detach_controller_status_string(status));
        g_object_unref(guard_interceptor);
        errno = saved_errno;
        return 0;
    }
    gum_interceptor_begin_transaction(guard_interceptor);
    for (size_t i = 0; i < unique_count; i++) {
        /* Standard replacements retain Gum's recursion protection during
         * late TLS first touch and preserve the libc call through a trampoline. */
        installed[i] = gum_interceptor_replace(guard_interceptor,
            requests[i].function_address, requests[i].replacement_function,
            &originals[i], NULL) == GUM_REPLACE_OK;
        success = success && installed[i];
    }
    if (!success) {
        for (size_t i = 0; i < unique_count; i++)
            if (installed[i])
                gum_interceptor_revert(guard_interceptor,
                                       requests[i].function_address);
    } else {
        next_statfs = (int (*)(const char *, struct statfs *)) originals[owners[0]];
        next_fstatfs = (int (*)(int, struct statfs *)) originals[owners[1]];
        if (owners[2] != SIZE_MAX)
            next_statfs64 = (int (*)(const char *, struct statfs64 *)) originals[owners[2]];
        if (owners[3] != SIZE_MAX)
            next_fstatfs64 = (int (*)(int, struct statfs64 *)) originals[owners[3]];
    }
    gum_interceptor_end_transaction(guard_interceptor);
    if (!peak_detach_controller_finish_hook_mutation_batch(&status))
        peak_detach_controller_abort_after_failed_finish(
            "filesystem-stat attach finish", status);
    peak_gum_module_mutation_end();
    if (!success)
        peak_log_warn("[peak] filesystem-stat replacement failed; attachment was rolled back\n");
    else
        for (size_t i = 0; i < G_N_ELEMENTS(targets); i++)
            atomic_store_explicit(&guarded_entries[i], (uintptr_t) targets[i],
                                  memory_order_release);
    /* Gum owns the installed replacement contexts; the library and guard
     * state remain resident until process exit. Never revert these guards
     * before the last PEAK physical stop window has completed. */
    g_object_unref(guard_interceptor);
    errno = saved_errno;
    return success;
}
#endif
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
peak_filesystem_stat_guard_controller_after_fork_child(void)
{
}
void
peak_filesystem_stat_guard_after_fork_child(void)
{
}
#endif

#if defined(PEAK_ATTACH_RUNTIME) && !defined(__linux__)
int peak_filesystem_stat_guard_attach(void)
{
    return 1;
}
int peak_filesystem_stat_guard_owns_address(const void *address)
{
    (void) address;
    return 0;
}
#endif
