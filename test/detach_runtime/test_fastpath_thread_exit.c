#define _GNU_SOURCE

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
static atomic_int aba_worker_ready;
static atomic_int aba_worker_release;
typedef unsigned long (*CountFunction)(size_t);
typedef uint64_t (*DroppedFunction)(void);

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
typedef int (*RemoveAmbiguousMappingFunction)(pthread_t);

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
    (void)arg;
    (void)peak_fastpath_thread_exit_target(0);
    atomic_store_explicit(&detached_worker_done, 1, memory_order_release);
    return NULL;
}

static void*
one_call_worker(void* arg)
{
    (void)arg;
    (void)peak_fastpath_thread_exit_target(0);
    return NULL;
}

static int
run_detached_quarantine_regression(CountFunction call_count,
                                   DroppedFunction dropped_calls,
                                   DroppedFunction dropped_threads,
                                   RemoveAmbiguousMappingFunction remove_ambiguous)
{
    pthread_t detached;
    pthread_t later;

    atomic_store_explicit(&detached_worker_done, 0, memory_order_relaxed);
    if (pthread_create(&detached, NULL, detached_worker, NULL) != 0 ||
        pthread_detach(detached) != 0) {
        fputs("detached worker setup failed\n", stderr);
        return 1;
    }
    while (!atomic_load_explicit(&detached_worker_done, memory_order_acquire)) {
        sched_yield();
    }
    /* A detached thread has no join handoff. Its completed slot must remain
     * quarantined, so the later user thread is explicitly dropped rather than
     * reusing potentially live TLS-destructor state. */
    if (!thread_is_tracked(detached) || remove_ambiguous == NULL ||
        !remove_ambiguous(detached) || thread_is_tracked(detached)) {
        fputs("untracked create did not clear ambiguous detached identity\n", stderr);
        return 1;
    }
    if (pthread_create(&later, NULL, one_call_worker, NULL) != 0 ||
        pthread_join(later, NULL) != 0 || thread_is_tracked(later) ||
        call_count(0) != 2 || dropped_calls() != 1 || dropped_threads() != 1) {
        fputs("detached thread slot was reused or silently excluded\n", stderr);
        return 1;
    }
    puts("fastpath_thread_exit_ok");
    return 0;
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
    RemoveAmbiguousMappingFunction remove_ambiguous =
        (RemoveAmbiguousMappingFunction)dlsym(
            RTLD_DEFAULT,
            "pthread_listener_test_untracked_create_removes_ambiguous_mapping");
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
    if (getenv("PEAK_TEST_DETACHED_QUARANTINE") != NULL) {
        return run_detached_quarantine_regression(call_count, dropped_calls,
                                                  dropped_threads,
                                                  remove_ambiguous);
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
