#define _GNU_SOURCE

#include "pthread_listener.h"

#include <pthread.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* TLS-destructor call sites use a constant mode.  Keep GCC from redirecting
 * them to an unexported IPA clone, which PEAK correctly does not target. */
#if defined(__GNUC__) && !defined(__clang__)
#define PEAK_TEST_TARGET_NOIPA __attribute__((noclone, noipa))
#else
#define PEAK_TEST_TARGET_NOIPA
#endif

static atomic_int cancel_target_entered;
static atomic_int user_destructor_calls;
static pthread_key_t user_destructor_key;
static void (*clear_current_slot)(void);
static int (*thread_is_tracked)(pthread_t);
static int (*current_thread_has_slot)(void);
static void (*release_start_publication)(void);
static atomic_int start_timeout_child_untracked;
static atomic_int destructor_tracking_failed;
static atomic_int threshold_holder_ready;
static atomic_int threshold_holder_release;
static atomic_int retired_active_ready;
static atomic_int retired_active_release;
static atomic_int detached_worker_done;
static atomic_int detached_worker_release;
static atomic_int detached_pending_ready;
static int (*mark_current_thread_final_destructor)(void);
static void (*join_detach_race_enable)(void);
static unsigned int (*join_detach_race_paused)(void);
static void (*join_detach_race_release)(void);
static atomic_int aba_worker_ready;
static atomic_int aba_worker_release;
typedef unsigned long (*CountFunction)(size_t);
typedef uint64_t (*DroppedFunction)(void);

typedef void (*ReclamationDiagnosticsFunction)(
    PeakPthreadReclamationDiagnostics*);

typedef struct {
    pthread_t target;
    atomic_int target_ready;
    atomic_int target_release;
    int join_status;
    int detach_status;
} JoinDetachRace;

static int
checkpoint_name_matches(const char* name, long pid)
{
    char prefix[64];
    const char* session;

    if (snprintf(prefix, sizeof(prefix), "fastpath-checkpoint-%ld-", pid) >=
            (int)sizeof(prefix) ||
        strncmp(name, prefix, strlen(prefix)) != 0) {
        return 0;
    }
    session = name + strlen(prefix);
    for (size_t index = 0; index < 16; index++) {
        if (!((session[index] >= '0' && session[index] <= '9') ||
              (session[index] >= 'a' && session[index] <= 'f'))) {
            return 0;
        }
    }
    return strcmp(session + 16, "-exec1.csv") == 0;
}
typedef void (*MarkNextHelperFunction)(void);
typedef int (*StaleGenerationRemoveFunction)(pthread_t);

__attribute__((noinline, used, externally_visible, visibility("default")))
PEAK_TEST_TARGET_NOIPA
uintptr_t peak_fastpath_thread_exit_target(uintptr_t mode);

static void
user_destructor_target(void* value)
{
    int pass = atomic_fetch_add_explicit(&user_destructor_calls,
                                         1,
                                         memory_order_relaxed);
    (void)peak_fastpath_thread_exit_target(0);
    const char* max_threads = getenv("PEAK_MAX_NUM_THREADS");
    if (pass % 4 == 3 &&
        max_threads != NULL && strcmp(max_threads, "0") != 0 &&
        !thread_is_tracked(pthread_self())) {
        atomic_store_explicit(&destructor_tracking_failed, 1,
                              memory_order_release);
    }
    /* Re-arm through every POSIX destructor iteration.  This is adversarial:
     * PEAK's key may run before this key in the final iteration. */
    if (pass % 4 != 3) {
        (void)pthread_setspecific(user_destructor_key, value);
    }
}

__attribute__((noinline, used, externally_visible, visibility("default")))
PEAK_TEST_TARGET_NOIPA
uintptr_t
peak_fastpath_thread_exit_target(uintptr_t mode)
{
    __asm__ volatile("" : "+r"(mode) :: "memory");
    if (mode == 1) {
        pthread_exit(NULL);
    }
    if (mode == 2) {
        atomic_store_explicit(&cancel_target_entered, 1,
                              memory_order_release);
        for (;;) {
            pthread_testcancel();
            sched_yield();
        }
    }
    return mode + 1;
}

static void*
exit_worker(void* arg)
{
    (void)arg;
    (void)pthread_setspecific(user_destructor_key, (void*)1);
    (void)peak_fastpath_thread_exit_target(0);
    (void)peak_fastpath_thread_exit_target(1);
    return (void*)1;
}

static void*
start_timeout_worker(void* arg)
{
    (void)arg;
    if (current_thread_has_slot != NULL && !current_thread_has_slot()) {
        atomic_store_explicit(&start_timeout_child_untracked, 1,
                              memory_order_release);
    }
    (void)peak_fastpath_thread_exit_target(0);
    if (release_start_publication != NULL) {
        release_start_publication();
    }
    return NULL;
}

static int
run_start_publication_timeout(void (*pause_enable)(void))
{
    pthread_t thread;

    if (pause_enable == NULL || release_start_publication == NULL ||
        current_thread_has_slot == NULL) {
        fputs("missing pthread start-timeout hooks\n", stderr);
        return 1;
    }
    atomic_store_explicit(&start_timeout_child_untracked, 0,
                          memory_order_release);
    pause_enable();
    if (pthread_create(&thread, NULL, start_timeout_worker, NULL) != 0 ||
        pthread_join(thread, NULL) != 0) {
        fputs("pthread start-timeout worker failed\n", stderr);
        return 1;
    }
    if (!atomic_load_explicit(&start_timeout_child_untracked,
                              memory_order_acquire) ||
        thread_is_tracked(thread)) {
        fputs("pthread start-timeout child was published late\n", stderr);
        return 1;
    }
    puts("pthread_start_publication_timeout_ok");
    return 0;
}

static void*
cancel_worker(void* arg)
{
    (void)arg;
    (void)peak_fastpath_thread_exit_target(0);
    (void)peak_fastpath_thread_exit_target(2);
    return (void*)1;
}

static void*
nonwrapper_worker(void* arg)
{
    (void)arg;
    clear_current_slot();
    (void)peak_fastpath_thread_exit_target(0);
    return NULL;
}

/* Keep one slot active while another retires.  The aggregate reaches ten
 * calls (main=1, holder=7, retiring=2), but no individual slot does. */
static void*
threshold_holder_worker(void* arg)
{
    (void)arg;
    for (int index = 0; index < 7; index++) {
        (void)peak_fastpath_thread_exit_target(0);
    }
    atomic_store_explicit(&threshold_holder_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&threshold_holder_release,
                                 memory_order_acquire)) {
        sched_yield();
    }
    for (int index = 0; index < 3; index++) {
        (void)peak_fastpath_thread_exit_target(0);
    }
    return NULL;
}

static void*
threshold_retiring_worker(void* arg)
{
    (void)arg;
    (void)peak_fastpath_thread_exit_target(0);
    (void)peak_fastpath_thread_exit_target(0);
    return NULL;
}

static void*
retired_five_worker(void* arg)
{
    (void)arg;
    for (int index = 0; index < 5; index++) {
        (void)peak_fastpath_thread_exit_target(0);
    }
    return NULL;
}

static void*
reused_active_five_worker(void* arg)
{
    (void)arg;
    for (int index = 0; index < 5; index++) {
        (void)peak_fastpath_thread_exit_target(0);
    }
    atomic_store_explicit(&retired_active_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&retired_active_release,
                                 memory_order_acquire)) {
        sched_yield();
    }
    for (int index = 0; index < 5; index++) {
        (void)peak_fastpath_thread_exit_target(0);
    }
    return NULL;
}

static void*
detached_worker(void* arg)
{
    intptr_t mode = (intptr_t)arg;

    (void)pthread_setspecific(user_destructor_key, (void*)1);
    (void)peak_fastpath_thread_exit_target(0);
    atomic_store_explicit(&detached_worker_done, 1, memory_order_release);
    if (mode == 2) {
        if (mark_current_thread_final_destructor == NULL ||
            !mark_current_thread_final_destructor()) {
            atomic_store_explicit(&destructor_tracking_failed, 1,
                                  memory_order_release);
        }
        atomic_store_explicit(&detached_pending_ready, 1,
                              memory_order_release);
    }
    while (mode != 0 &&
           !atomic_load_explicit(&detached_worker_release,
                                 memory_order_acquire)) {
        sched_yield();
    }
    return NULL;
}

static void*
one_call_worker(void* arg)
{
    (void)arg;
    (void)peak_fastpath_thread_exit_target(0);
    return NULL;
}

static void*
join_detach_race_target(void* data)
{
    JoinDetachRace* race = data;

    (void)pthread_setspecific(user_destructor_key, (void*)1);
    (void)peak_fastpath_thread_exit_target(0);
    atomic_store_explicit(&race->target_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&race->target_release,
                                 memory_order_acquire)) {
        sched_yield();
    }
    return NULL;
}

static void*
join_detach_race_joiner(void* data)
{
    JoinDetachRace* race = data;

    race->join_status = pthread_join(race->target, NULL);
    return NULL;
}

static void*
join_detach_race_detacher(void* data)
{
    JoinDetachRace* race = data;

    race->detach_status = pthread_detach(race->target);
    return NULL;
}

static int
wait_for_detached_reclamation(ReclamationDiagnosticsFunction get_diagnostics,
                              uint64_t expected_reclaimed)
{
    for (unsigned int attempt = 0; attempt < 500; attempt++) {
        PeakPthreadReclamationDiagnostics diagnostics;

        get_diagnostics(&diagnostics);
        if (diagnostics.reclaimed >= expected_reclaimed) {
            return 0;
        }
        usleep(10000);
    }
    return 1;
}

static int
wait_for_alive_detached_deferral(
    ReclamationDiagnosticsFunction get_diagnostics,
    uint64_t initial_deferred_alive,
    uint64_t expected_reclaimed)
{
    for (unsigned int attempt = 0; attempt < 500; attempt++) {
        PeakPthreadReclamationDiagnostics diagnostics;

        get_diagnostics(&diagnostics);
        if (diagnostics.deferred_alive > initial_deferred_alive) {
            if (diagnostics.reclaimed == expected_reclaimed &&
                diagnostics.pending == 1) {
                return 0;
            }
            fprintf(stderr,
                    "live deferral mismatch: deferred=%llu reclaimed=%llu "
                    "pending=%lu\n",
                    (unsigned long long)diagnostics.deferred_alive,
                    (unsigned long long)diagnostics.reclaimed,
                    (unsigned long)diagnostics.pending);
            return 1;
        }
        usleep(10000);
    }
    PeakPthreadReclamationDiagnostics diagnostics;
    get_diagnostics(&diagnostics);
    fprintf(stderr,
            "live deferral timeout: scans=%llu candidates=%llu deferred=%llu "
            "reclaimed=%llu pending=%lu\n",
            (unsigned long long)diagnostics.scans,
            (unsigned long long)diagnostics.candidates_checked,
            (unsigned long long)diagnostics.deferred_alive,
            (unsigned long long)diagnostics.reclaimed,
            (unsigned long)diagnostics.pending);
    return 1;
}

static int
run_join_detach_race_regression(
    CountFunction call_count,
    DroppedFunction dropped_calls,
    DroppedFunction dropped_threads,
    ReclamationDiagnosticsFunction get_diagnostics)
{
#if defined(__linux__)
    enum { RACE_ROUNDS = 16 };
    PeakPthreadReclamationDiagnostics diagnostics;

    if (join_detach_race_enable == NULL || join_detach_race_paused == NULL ||
        join_detach_race_release == NULL || get_diagnostics == NULL) {
        fputs("missing join/detach race test hooks\n", stderr);
        return 1;
    }
    get_diagnostics(&diagnostics);
    uint64_t expected_reclaimed = diagnostics.reclaimed;

    for (int round = 0; round < RACE_ROUNDS; round++) {
        JoinDetachRace race = {
            .join_status = -1,
            .detach_status = -1,
        };
        pthread_t joiner;
        pthread_t detacher;

        atomic_init(&race.target_ready, 0);
        atomic_init(&race.target_release, 0);
        if (pthread_create(&race.target, NULL, join_detach_race_target,
                           &race) != 0) {
            fputs("join/detach race target create failed\n", stderr);
            return 1;
        }
        while (!atomic_load_explicit(&race.target_ready,
                                     memory_order_acquire)) {
            sched_yield();
        }

        join_detach_race_enable();
        if (pthread_create(&joiner, NULL, join_detach_race_joiner, &race) !=
                0 ||
            pthread_create(&detacher, NULL, join_detach_race_detacher, &race) !=
                0) {
            atomic_store_explicit(&race.target_release, 1,
                                  memory_order_release);
            join_detach_race_release();
            fputs("join/detach race caller create failed\n", stderr);
            return 1;
        }
        for (unsigned int attempt = 0;
             attempt < 500 && join_detach_race_paused() != 2; attempt++) {
            usleep(10000);
        }
        if (join_detach_race_paused() != 2) {
            atomic_store_explicit(&race.target_release, 1,
                                  memory_order_release);
            join_detach_race_release();
            (void)pthread_join(joiner, NULL);
            (void)pthread_join(detacher, NULL);
            fputs("join/detach callers did not capture one generation\n",
                  stderr);
            return 1;
        }

        atomic_store_explicit(&race.target_release, 1, memory_order_release);
        join_detach_race_release();
        if (pthread_join(joiner, NULL) != 0 ||
            pthread_join(detacher, NULL) != 0) {
            fputs("join/detach race caller cleanup failed\n", stderr);
            return 1;
        }
        if (race.join_status != 0 && race.detach_status != 0) {
            fprintf(stderr,
                    "join/detach race had no native winner: join=%d detach=%d\n",
                    race.join_status, race.detach_status);
            return 1;
        }
        if (race.join_status != 0) {
            expected_reclaimed++;
            if (wait_for_detached_reclamation(get_diagnostics,
                                              expected_reclaimed) != 0) {
                fputs("join/detach race detached winner was not reclaimed\n",
                      stderr);
                return 1;
            }
        }

        unsigned long expected_calls = 1UL + (unsigned long)(round + 1) * 5UL;
        int expected_destructors = (round + 1) * 4;
        if (call_count(0) != expected_calls || dropped_calls() != 0 ||
            dropped_threads() != 0 ||
            atomic_load_explicit(&user_destructor_calls,
                                 memory_order_relaxed) !=
                expected_destructors) {
            fprintf(stderr,
                    "join/detach race accounting mismatch at round %d: "
                    "calls=%lu expected=%lu dropped=%llu/%llu destructors=%d\n",
                    round, call_count(0), expected_calls,
                    (unsigned long long)dropped_calls(),
                    (unsigned long long)dropped_threads(),
                    atomic_load_explicit(&user_destructor_calls,
                                         memory_order_relaxed));
            return 1;
        }
    }

    get_diagnostics(&diagnostics);
    if (diagnostics.pending != 0 || diagnostics.retire_failures != 0) {
        fputs("join/detach race left retirement state pending\n", stderr);
        return 1;
    }
    puts("fastpath_thread_exit_ok");
    return 0;
#else
    (void)call_count;
    (void)dropped_calls;
    (void)dropped_threads;
    (void)get_diagnostics;
    puts("fastpath_thread_exit_ok");
    return 0;
#endif
}

static int
run_detached_reclamation_regression(
    CountFunction call_count,
    DroppedFunction dropped_calls,
    DroppedFunction dropped_threads,
    ReclamationDiagnosticsFunction get_diagnostics)
{
#if defined(__linux__)
    enum { DETACHED_ITERATIONS = 12 };
    uint64_t expected_reclaimed = 0;
    PeakPthreadReclamationDiagnostics initial_diagnostics;

    if (get_diagnostics == NULL ||
        mark_current_thread_final_destructor == NULL) {
        fputs("missing detached reclamation test hooks\n", stderr);
        return 1;
    }
    get_diagnostics(&initial_diagnostics);

    for (int iteration = 0; iteration < DETACHED_ITERATIONS; iteration++) {
        pthread_t detached;
        pthread_attr_t attr;
        pthread_attr_t* attr_pointer = NULL;
        int destructor_count_before = atomic_load_explicit(
            &user_destructor_calls, memory_order_relaxed);

        atomic_store_explicit(&detached_worker_done, 0, memory_order_relaxed);
        atomic_store_explicit(&detached_worker_release, 0,
                              memory_order_relaxed);
        atomic_store_explicit(&detached_pending_ready, 0,
                              memory_order_relaxed);
        if (iteration % 3 == 0) {
            if (pthread_attr_init(&attr) != 0 ||
                pthread_attr_setdetachstate(&attr,
                                            PTHREAD_CREATE_DETACHED) != 0) {
                fputs("detached worker attribute setup failed\n", stderr);
                return 1;
            }
            attr_pointer = &attr;
        }
        void* worker_argument = iteration == 0 ? (void*)2 :
                                iteration == 1 ? (void*)1 : NULL;
        if (pthread_create(&detached, attr_pointer, detached_worker,
                           worker_argument) != 0) {
            if (attr_pointer != NULL) {
                (void)pthread_attr_destroy(&attr);
            }
            fputs("detached worker create failed\n", stderr);
            return 1;
        }
        if (attr_pointer != NULL) {
            (void)pthread_attr_destroy(&attr);
        } else if (iteration % 3 == 1) {
            if (iteration == 1) {
                while (!atomic_load_explicit(&detached_worker_done,
                                             memory_order_acquire)) {
                    sched_yield();
                }
            }
            if (pthread_detach(detached) != 0) {
                fputs("running worker detach failed\n", stderr);
                return 1;
            }
            if (iteration == 1) {
                void* unexpected_result = NULL;
                if (pthread_join(detached, &unexpected_result) == 0) {
                    fputs("detached worker unexpectedly joined\n", stderr);
                    return 1;
                }
                if (pthread_detach(detached) == 0) {
                    fputs("failed detach changed detached state\n", stderr);
                    return 1;
                }
            }
            atomic_store_explicit(&detached_worker_release, 1,
                                  memory_order_release);
        }
        while (!atomic_load_explicit(&detached_worker_done,
                                     memory_order_acquire)) {
            sched_yield();
        }
        if (iteration == 0) {
            while (!atomic_load_explicit(&detached_pending_ready,
                                         memory_order_acquire)) {
                sched_yield();
            }
            if (wait_for_alive_detached_deferral(
                    get_diagnostics, initial_diagnostics.deferred_alive,
                    expected_reclaimed) != 0) {
                fputs("live detached destructor slot was reclaimed\n", stderr);
                return 1;
            }
            atomic_store_explicit(&detached_worker_release, 1,
                                  memory_order_release);
        }
        if (iteration % 3 == 2) {
            /* Exercise a successful detach after all four adversarial
             * application TLS destructor passes have completed. */
            while (atomic_load_explicit(&user_destructor_calls,
                                        memory_order_acquire) <
                   destructor_count_before + 4) {
                sched_yield();
            }
            if (pthread_detach(detached) != 0) {
                fputs("late detached worker detach failed\n", stderr);
                return 1;
            }
        }
        expected_reclaimed++;
        if (wait_for_detached_reclamation(get_diagnostics,
                                          expected_reclaimed) != 0) {
            fputs("detached worker slot was not reclaimed\n", stderr);
            return 1;
        }
    }
    unsigned long expected_detached_calls =
        1UL + DETACHED_ITERATIONS * 5UL;
    if (call_count(0) != expected_detached_calls ||
        atomic_load_explicit(&user_destructor_calls,
                             memory_order_relaxed) !=
            DETACHED_ITERATIONS * 4) {
        fprintf(stderr,
                "detached exact-once mismatch: calls=%lu expected=%lu "
                "destructors=%d expected_destructors=%d\n",
                call_count(0), expected_detached_calls,
                atomic_load_explicit(&user_destructor_calls,
                                     memory_order_relaxed),
                DETACHED_ITERATIONS * 4);
        return 1;
    }

    /* Cancellation runs all PEAK cleanup/destructor paths but has no join
     * handoff once detach succeeds. It must reach the same conservative path. */
    atomic_store_explicit(&cancel_target_entered, 0, memory_order_relaxed);
    pthread_t canceled;
    if (pthread_create(&canceled, NULL, cancel_worker, NULL) != 0) {
        fputs("detached cancel worker create failed\n", stderr);
        return 1;
    }
    while (!atomic_load_explicit(&cancel_target_entered,
                                 memory_order_acquire)) {
        sched_yield();
    }
    if (pthread_detach(canceled) != 0 || pthread_cancel(canceled) != 0) {
        fputs("detached cancel worker setup failed\n", stderr);
        return 1;
    }
    expected_reclaimed++;
    if (wait_for_detached_reclamation(get_diagnostics,
                                      expected_reclaimed) != 0) {
        fputs("canceled detached worker slot was not reclaimed\n", stderr);
        return 1;
    }
    unsigned long calls_after_cancellation = call_count(0);

    /* The successful-join fast path remains available after repeated reuse of
     * the single non-main profiling slot. */
    pthread_t joined;
    if (pthread_create(&joined, NULL, one_call_worker, NULL) != 0 ||
        pthread_join(joined, NULL) != 0 || thread_is_tracked(joined)) {
        fputs("joined worker fast path regressed after detached reuse\n",
              stderr);
        return 1;
    }

    PeakPthreadReclamationDiagnostics diagnostics;
    get_diagnostics(&diagnostics);
    if (diagnostics.reclaimed != expected_reclaimed ||
        diagnostics.pending != 0 || diagnostics.max_pending > 1 ||
        diagnostics.scan_budget != 64 ||
        diagnostics.scan_interval_ms != 100 ||
        diagnostics.entries_examined >
            diagnostics.scans * diagnostics.scan_budget ||
        diagnostics.retire_failures != 0 || dropped_calls() != 0 ||
        dropped_threads() != 0 ||
        atomic_load_explicit(&destructor_tracking_failed,
                             memory_order_acquire) != 0 ||
        call_count(0) != calls_after_cancellation + 1) {
        fprintf(stderr,
                "detached reclamation mismatch: calls=%lu dropped=%llu/%llu "
                "scans=%llu examined=%llu reclaimed=%llu pending=%lu "
                "max_pending=%lu failures=%llu\n",
                call_count(0), (unsigned long long)dropped_calls(),
                (unsigned long long)dropped_threads(),
                (unsigned long long)diagnostics.scans,
                (unsigned long long)diagnostics.entries_examined,
                (unsigned long long)diagnostics.reclaimed,
                (unsigned long)diagnostics.pending,
                (unsigned long)diagnostics.max_pending,
                (unsigned long long)diagnostics.retire_failures);
        return 1;
    }
    puts("fastpath_thread_exit_ok");
    return 0;
#else
    (void)call_count;
    (void)dropped_calls;
    (void)dropped_threads;
    (void)get_diagnostics;
    puts("fastpath_thread_exit_ok");
    return 0;
#endif
}

static int
run_helper_silent_regression(CountFunction call_count,
                             DroppedFunction dropped_calls,
                             DroppedFunction dropped_threads,
                             MarkNextHelperFunction mark_next_helper)
{
    pthread_t helper;
    pthread_t user;

    if (mark_next_helper == NULL) {
        fputs("missing helper marker\n", stderr);
        return 1;
    }
    mark_next_helper();
    if (pthread_create(&helper, NULL, one_call_worker, NULL) != 0 ||
        pthread_join(helper, NULL) != 0 ||
        pthread_create(&user, NULL, one_call_worker, NULL) != 0 ||
        pthread_join(user, NULL) != 0 || call_count(0) != 2 ||
        dropped_calls() != 0 || dropped_threads() != 0) {
        fputs("PEAK helper contributed to user accounting diagnostics\n", stderr);
        return 1;
    }
    puts("fastpath_thread_exit_ok");
    return 0;
}

static void*
aba_worker(void* arg)
{
    (void)arg;
    atomic_store_explicit(&aba_worker_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&aba_worker_release, memory_order_acquire)) {
        sched_yield();
    }
    (void)peak_fastpath_thread_exit_target(0);
    return NULL;
}

static int
run_generation_aba_regression(StaleGenerationRemoveFunction stale_remove)
{
    pthread_t worker;

    if (stale_remove == NULL) {
        fputs("missing ABA generation test hook\n", stderr);
        return 1;
    }
    atomic_store_explicit(&aba_worker_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&aba_worker_release, 0, memory_order_relaxed);
    if (pthread_create(&worker, NULL, aba_worker, NULL) != 0) {
        fputs("ABA worker create failed\n", stderr);
        return 1;
    }
    while (!atomic_load_explicit(&aba_worker_ready, memory_order_acquire)) {
        sched_yield();
    }
    if (!stale_remove(worker) || !thread_is_tracked(worker)) {
        atomic_store_explicit(&aba_worker_release, 1, memory_order_release);
        (void)pthread_join(worker, NULL);
        fputs("stale generation removed the replacement mapping\n", stderr);
        return 1;
    }
    atomic_store_explicit(&aba_worker_release, 1, memory_order_release);
    if (pthread_join(worker, NULL) != 0 || thread_is_tracked(worker)) {
        fputs("current generation did not complete normal join cleanup\n", stderr);
        return 1;
    }
    puts("fastpath_thread_exit_ok");
    return 0;
}

static int
run_retired_detach_threshold_regression(void)
{
    const char* trace_path = getenv("PEAK_DETACH_TRACE_PATH");
    if (trace_path == NULL || trace_path[0] == '\0') {
        fputs("missing detach trace path\n", stderr);
        return 1;
    }
    (void)unlink(trace_path);

    pthread_t holder;
    pthread_t retiring;
    atomic_store_explicit(&threshold_holder_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&threshold_holder_release, 0, memory_order_relaxed);
    if (pthread_create(&holder, NULL, threshold_holder_worker, NULL) != 0) {
        fputs("threshold holder create failed\n", stderr);
        return 1;
    }
    while (!atomic_load_explicit(&threshold_holder_ready,
                                 memory_order_acquire)) {
        sched_yield();
    }
    if (pthread_create(&retiring, NULL, threshold_retiring_worker, NULL) != 0 ||
        pthread_join(retiring, NULL) != 0) {
        atomic_store_explicit(&threshold_holder_release, 1, memory_order_release);
        (void)pthread_join(holder, NULL);
        fputs("threshold retiring worker failed\n", stderr);
        return 1;
    }

    /* Retired accounting is report-only: the aggregate total must not fire a
     * detach-count request.  Releasing the holder then advances that same
     * slot from seven to ten and must fire exactly at its local threshold. */
    for (int attempt = 0; attempt < 20; attempt++) {
        FILE* trace = fopen(trace_path, "r");

        if (trace != NULL) {
            char line[2048];

            while (fgets(line, sizeof(line), trace) != NULL) {
                if (strstr(line, ",detach-count,") != NULL) {
                    fclose(trace);
                    atomic_store_explicit(&threshold_holder_release,
                                          1, memory_order_release);
                    (void)pthread_join(holder, NULL);
                    fputs("aggregate retired calls fired detach-count request\n",
                          stderr);
                    return 1;
                }
            }
            fclose(trace);
        }
        usleep(10000);
    }
    atomic_store_explicit(&threshold_holder_release, 1, memory_order_release);
    for (int attempt = 0; attempt < 200; attempt++) {
        FILE* trace = fopen(trace_path, "r");
        if (trace != NULL) {
            char line[2048];
            while (fgets(line, sizeof(line), trace) != NULL) {
                if (strstr(line, ",detach-count,") != NULL) {
                    fclose(trace);
                    (void)pthread_join(holder, NULL);
                    puts("fastpath_thread_exit_ok");
                    return 0;
                }
            }
            fclose(trace);
        }
        usleep(10000);
    }
    (void)pthread_join(holder, NULL);
    fputs("per-slot detach-count request did not report local total 10\n",
          stderr);
    return 1;
}

static int
run_retired_active_detach_threshold_regression(void)
{
    const char* trace_path = getenv("PEAK_DETACH_TRACE_PATH");
    pthread_t retired;
    pthread_t active;

    if (trace_path == NULL || trace_path[0] == '\0') {
        fputs("missing detach trace path\n", stderr);
        return 1;
    }
    (void)unlink(trace_path);
    atomic_store_explicit(&retired_active_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&retired_active_release, 0, memory_order_relaxed);

    if (pthread_create(&retired, NULL, retired_five_worker, NULL) != 0 ||
        pthread_join(retired, NULL) != 0) {
        fputs("retired five-call worker failed\n", stderr);
        return 1;
    }
    if (pthread_create(&active, NULL, reused_active_five_worker, NULL) != 0) {
        fputs("reused active five-call worker failed\n", stderr);
        return 1;
    }
    while (!atomic_load_explicit(&retired_active_ready, memory_order_acquire)) {
        sched_yield();
    }

    /* The second thread reuses the retired slot, so its first five calls must
     * remain below the local threshold despite five retired calls elsewhere. */
    for (int attempt = 0; attempt < 20; attempt++) {
        FILE* trace = fopen(trace_path, "r");

        if (trace != NULL) {
            char line[2048];

            while (fgets(line, sizeof(line), trace) != NULL) {
                if (strstr(line, ",detach-count,") != NULL) {
                    fclose(trace);
                    atomic_store_explicit(&retired_active_release, 1,
                                          memory_order_release);
                    (void)pthread_join(active, NULL);
                    fputs("retired calls were replayed into a reused slot\n",
                          stderr);
                    return 1;
                }
            }
            fclose(trace);
        }
        usleep(10000);
    }
    atomic_store_explicit(&retired_active_release, 1, memory_order_release);
    for (int attempt = 0; attempt < 200; attempt++) {
        FILE* trace = fopen(trace_path, "r");

        if (trace != NULL) {
            char line[2048];

            while (fgets(line, sizeof(line), trace) != NULL) {
                if (strstr(line, ",detach-count,") != NULL) {
                    fclose(trace);
                    (void)pthread_join(active, NULL);
                    puts("fastpath_thread_exit_ok");
                    return 0;
                }
            }
            fclose(trace);
        }
        usleep(10000);
    }
    (void)pthread_join(active, NULL);
    fputs("reused-slot detach-count request did not fire at local 10\n",
          stderr);
    return 1;
}

int
main(void)
{
    CountFunction call_count = (CountFunction)dlsym(
        RTLD_DEFAULT, "peak_general_listener_test_call_count");
    CountFunction thread_count = (CountFunction)dlsym(
        RTLD_DEFAULT, "peak_general_listener_test_thread_count");
    DroppedFunction dropped_calls = (DroppedFunction)dlsym(
        RTLD_DEFAULT, "peak_general_listener_test_dropped_calls");
    DroppedFunction dropped_threads = (DroppedFunction)dlsym(
        RTLD_DEFAULT, "peak_general_listener_test_dropped_threads");
    MarkNextHelperFunction mark_next_helper = (MarkNextHelperFunction)dlsym(
        RTLD_DEFAULT, "pthread_listener_mark_next_created_thread_helper");
    StaleGenerationRemoveFunction stale_generation_remove =
        (StaleGenerationRemoveFunction)dlsym(
            RTLD_DEFAULT,
            "pthread_listener_test_stale_generation_remove_preserves_mapping");
    ReclamationDiagnosticsFunction get_reclamation_diagnostics =
        (ReclamationDiagnosticsFunction)dlsym(
            RTLD_DEFAULT,
            "pthread_listener_get_reclamation_diagnostics");
    typedef void (*FailPublishFunction)(unsigned int);
    FailPublishFunction fail_slot_publish = (FailPublishFunction)dlsym(
        RTLD_DEFAULT, "pthread_listener_test_fail_slot_publish");
    clear_current_slot = (void (*)(void))dlsym(
        RTLD_DEFAULT, "pthread_listener_test_clear_current_thread_slot");
    thread_is_tracked = (int (*)(pthread_t))dlsym(
        RTLD_DEFAULT, "pthread_listener_test_thread_is_tracked");
    current_thread_has_slot = (int (*)(void))dlsym(
        RTLD_DEFAULT, "pthread_listener_test_current_thread_has_slot");
    void (*pause_start_publication)(void) = (void (*)(void))dlsym(
        RTLD_DEFAULT,
        "pthread_listener_test_pause_start_publication_enable");
    release_start_publication = (void (*)(void))dlsym(
        RTLD_DEFAULT,
        "pthread_listener_test_release_start_publication");
    mark_current_thread_final_destructor = (int (*)(void))dlsym(
        RTLD_DEFAULT,
        "pthread_listener_test_mark_current_thread_final_destructor");
    join_detach_race_enable = (void (*)(void))dlsym(
        RTLD_DEFAULT, "pthread_listener_test_join_detach_race_enable");
    join_detach_race_paused = (unsigned int (*)(void))dlsym(
        RTLD_DEFAULT, "pthread_listener_test_join_detach_race_paused");
    join_detach_race_release = (void (*)(void))dlsym(
        RTLD_DEFAULT, "pthread_listener_test_join_detach_race_release");
    if (call_count == NULL || thread_count == NULL || dropped_calls == NULL ||
        dropped_threads == NULL || thread_is_tracked == NULL) {
        fputs("missing accounting test hooks\n", stderr);
        return 1;
    }
    if (pthread_key_create(&user_destructor_key, user_destructor_target) != 0) {
        fputs("user TLS key create failed\n", stderr);
        return 1;
    }
    if (getenv("PEAK_TEST_SLOT_PUBLISH_FAILURE") != NULL) {
        if (fail_slot_publish == NULL) {
            fputs("missing slot publish failure hook\n", stderr);
            return 1;
        }
        fail_slot_publish(1);
    }
    if (getenv("PEAK_TEST_RETIRED_ACTIVE_THRESHOLD") != NULL) {
        return run_retired_active_detach_threshold_regression();
    }
    if (getenv("PEAK_TEST_PTHREAD_START_TIMEOUT") != NULL) {
        return run_start_publication_timeout(pause_start_publication);
    }
    (void)peak_fastpath_thread_exit_target(0);
    if (getenv("PEAK_TEST_RETIRED_DETACH_THRESHOLD") != NULL) {
        return run_retired_detach_threshold_regression();
    }
    if (getenv("PEAK_TEST_DETACHED_RECLAMATION") != NULL) {
        return run_detached_reclamation_regression(
            call_count, dropped_calls, dropped_threads,
            get_reclamation_diagnostics);
    }
    if (getenv("PEAK_TEST_PTHREAD_JOIN_DETACH_RACE") != NULL) {
        return run_join_detach_race_regression(
            call_count, dropped_calls, dropped_threads,
            get_reclamation_diagnostics);
    }
    if (getenv("PEAK_TEST_HELPER_SILENT") != NULL) {
        return run_helper_silent_regression(call_count, dropped_calls,
                                            dropped_threads, mark_next_helper);
    }
    if (getenv("PEAK_TEST_GENERATION_ABA") != NULL) {
        return run_generation_aba_regression(stale_generation_remove);
    }
    for (int iteration = 0; iteration < 128; iteration++) {
        pthread_t thread;
        void* result = (void*)1;

        if (pthread_create(&thread, NULL, exit_worker, NULL) != 0 ||
            pthread_join(thread, &result) != 0 || result != NULL) {
            fputs("pthread_exit worker failed\n", stderr);
            return 1;
        }
        if (thread_is_tracked(thread)) {
            fputs("joined thread remained in listener map\n", stderr);
            return 1;
        }
    }

    atomic_store_explicit(&cancel_target_entered, 0,
                          memory_order_relaxed);
    pthread_t canceled;
    if (pthread_create(&canceled, NULL, cancel_worker, NULL) != 0) {
        fputs("cancel worker create failed\n", stderr);
        return 1;
    }
    while (!atomic_load_explicit(&cancel_target_entered,
                                 memory_order_acquire)) {
        sched_yield();
    }
    if (pthread_cancel(canceled) != 0) {
        fputs("pthread_cancel failed\n", stderr);
        return 1;
    }
    void* canceled_result = NULL;
    if (pthread_join(canceled, &canceled_result) != 0 ||
        canceled_result != PTHREAD_CANCELED) {
        fputs("canceled worker join failed\n", stderr);
        return 1;
    }
    if (thread_is_tracked(canceled)) {
        fputs("joined canceled thread remained in listener map\n", stderr);
        return 1;
    }

    pthread_t final_worker;
    void* final_result = (void*)1;
    if (pthread_create(&final_worker, NULL, exit_worker, NULL) != 0 ||
        pthread_join(final_worker, &final_result) != 0 ||
        final_result != NULL) {
        fputs("reused-slot worker failed\n", stderr);
        return 1;
    }
    if (thread_is_tracked(final_worker)) {
        fputs("joined final thread remained in listener map\n", stderr);
        return 1;
    }

    if (atomic_load_explicit(&user_destructor_calls, memory_order_relaxed) <
        4 * 129) {
        fputs("user TLS destructor target did not run through all passes\n",
              stderr);
        return 1;
    }
    if (atomic_load_explicit(&destructor_tracking_failed,
                             memory_order_acquire) != 0) {
        fputs("fourth-pass user TLS destructor lost listener map visibility\n",
              stderr);
        return 1;
    }

    const char* configured_capacity = getenv("PEAK_MAX_NUM_THREADS");
    if (configured_capacity != NULL && strcmp(configured_capacity, "2") == 0 &&
        getenv("PEAK_TEST_SLOT_PUBLISH_FAILURE") == NULL) {
        if (call_count(0) != 777 || thread_count(0) != 131 ||
            dropped_calls() != 0 || dropped_threads() != 0) {
            fprintf(stderr,
                    "joined-slot accounting mismatch: calls=%lu threads=%lu "
                    "dropped_calls=%llu dropped_threads=%llu destructors=%d\n",
                    call_count(0), thread_count(0),
                    (unsigned long long)dropped_calls(),
                    (unsigned long long)dropped_threads(),
                    atomic_load_explicit(&user_destructor_calls,
                                         memory_order_relaxed));
            return 1;
        }
    }
    if (getenv("PEAK_TEST_SLOT_PUBLISH_FAILURE") != NULL &&
        (dropped_calls() < 6 || dropped_threads() == 0)) {
        fputs("slot publish failure was silently excluded\n", stderr);
        return 1;
    }
    if (getenv("PEAK_TEST_NONWRAPPER") != NULL) {
        pthread_t nonwrapper;
        if (clear_current_slot == NULL ||
            pthread_create(&nonwrapper, NULL, nonwrapper_worker, NULL) != 0 ||
            pthread_join(nonwrapper, NULL) != 0 || call_count(0) != 777 ||
            dropped_calls() != 1 || dropped_threads() != 1) {
            fputs("non-wrapper drop accounting mismatch\n", stderr);
            return 1;
        }
    }
    if (configured_capacity != NULL && strcmp(configured_capacity, "0") == 0) {
        if (call_count(0) != 1 || thread_count(0) != 1 ||
            dropped_calls() != 776 || dropped_threads() != 130) {
            fputs("zero-capacity clamp/drop accounting mismatch\n", stderr);
            return 1;
        }
    }
    if (getenv("PEAK_TEST_CHECKPOINT_EXACT") != NULL) {
        typedef int (*CheckpointFunction)(const char*, char* const[]);
        CheckpointFunction checkpoint = (CheckpointFunction)dlsym(
            RTLD_DEFAULT, "peak_checkpoint_for_exec");
        const char* base = getenv("PEAK_STATSLOG_PATH");
        char path[1024];
        char directory[1024];
        char* slash;
        DIR* stream;
        struct dirent* entry;
        int matches = 0;
        char* argv[] = { (char*)"checkpoint-test", NULL };
        if (checkpoint == NULL || base == NULL ||
            checkpoint("checkpoint-test", argv) != 0 ||
            snprintf(directory, sizeof(directory), "%s", base) >=
                (int)sizeof(directory) ||
            (slash = strrchr(directory, '/')) == NULL) {
            fputs("checkpoint capture failed\n", stderr);
            return 1;
        }
        *slash = '\0';
        stream = opendir(directory);
        if (stream == NULL) {
            fputs("checkpoint directory unavailable\n", stderr);
            return 1;
        }
        if (!checkpoint_name_matches(
                "fastpath-checkpoint-1-0123456789abcdef-exec1.csv", 1) ||
            checkpoint_name_matches("fastpath-checkpoint-p1-exec1.csv", 1)) {
            fputs("checkpoint name contract mismatch\n", stderr);
            return 1;
        }
        while ((entry = readdir(stream)) != NULL) {
            if (checkpoint_name_matches(entry->d_name, (long)getpid())) {
                if (snprintf(path, sizeof(path), "%s/%s", directory,
                             entry->d_name) >= (int)sizeof(path)) {
                    (void)closedir(stream);
                    return 1;
                }
                matches++;
            }
        }
        (void)closedir(stream);
        if (matches != 1) {
            fputs("checkpoint output identity mismatch\n", stderr);
            return 1;
        }
        FILE* checkpoint_csv = fopen(path, "r");
        char line[2048];
        int exact_once = 0;
        if (checkpoint_csv != NULL) {
            while (fgets(line, sizeof(line), checkpoint_csv) != NULL) {
                if (strstr(line,
                           "\"peak_fastpath_thread_exit_target\",777,6,777,") != NULL) {
                    exact_once++;
                }
            }
            fclose(checkpoint_csv);
            (void)unlink(path);
        }
        if (exact_once != 1) {
            fputs("checkpoint retired-plus-active accounting mismatch\n", stderr);
            return 1;
        }
    }

    puts("fastpath_thread_exit_ok");
    return 0;
}
