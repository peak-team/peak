#define _GNU_SOURCE
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_int stop_requested;
static atomic_uint_fast64_t side_effect;
static atomic_uint_fast64_t unrelated_effect;
static atomic_long stale_threads_blocked;
static int active_start_open;

typedef enum {
    STALE_MODE_PARKED = 0,
    STALE_MODE_UNRELATED_SPIN,
    STALE_MODE_UNRELATED_SLEEP
} StaleMode;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    long expected;
    long arrived;
    int open;
} StartGate;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} Blocker;

typedef struct {
    uint64_t calls;
    unsigned int seed;
    int stale;
    int active_after_stale;
    StaleMode stale_mode;
    long stale_calls;
    StartGate* start_gate;
    Blocker* blocker;
} WorkerState;

typedef int (*PeakRequestDetachFn)(size_t hook_id);

__attribute__((noinline))
void peak_detach_stale_target(uint64_t value)
{
    atomic_fetch_add_explicit(&side_effect, value + 1, memory_order_relaxed);
    asm volatile("" ::: "memory");
}

__attribute__((noinline))
void peak_detach_stale_unrelated(uint64_t value)
{
    atomic_fetch_add_explicit(&unrelated_effect,
                              value + 1u,
                              memory_order_relaxed);
    asm volatile("" ::: "memory");
}

static double
monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static long
parse_long_arg(int argc, char** argv, const char* name, long fallback)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            char* end = NULL;
            errno = 0;
            long value = strtol(argv[i + 1], &end, 10);
            if (errno == 0 && end != argv[i + 1] && *end == '\0' && value >= 0) {
                return value;
            }
        }
    }
    return fallback;
}

static int
has_flag_arg(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void*
optional_symbol(const char* name)
{
    dlerror();
    void* symbol = dlsym(RTLD_DEFAULT, name);
    const char* error = dlerror();
    if (error != NULL) {
        return NULL;
    }
    return symbol;
}

static int
request_detach_after_workers_start(void)
{
    PeakRequestDetachFn request_detach =
        (PeakRequestDetachFn)optional_symbol(
            "peak_general_listener_request_detach");
    double deadline = monotonic_seconds() + 5.0;

    if (request_detach == NULL) {
        fprintf(stderr, "peak_general_listener_request_detach is unavailable\n");
        return 0;
    }

    while (monotonic_seconds() < deadline) {
        if (request_detach(0)) {
            return 1;
        }

        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 20 * 1000 * 1000;
        nanosleep(&ts, NULL);
    }

    fprintf(stderr, "failed to request stale-thread detach after workers started\n");
    return 0;
}

static const char*
parse_string_arg(int argc, char** argv, const char* name, const char* fallback)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return fallback;
}

static int
parse_stale_mode(const char* value, StaleMode* mode_out)
{
    if (strcmp(value, "parked") == 0) {
        *mode_out = STALE_MODE_PARKED;
        return 0;
    }
    if (strcmp(value, "unrelated-spin") == 0) {
        *mode_out = STALE_MODE_UNRELATED_SPIN;
        return 0;
    }
    if (strcmp(value, "unrelated-sleep") == 0) {
        *mode_out = STALE_MODE_UNRELATED_SLEEP;
        return 0;
    }
    return -1;
}

static int
start_gate_init(StartGate* gate, long expected)
{
    memset(gate, 0, sizeof(*gate));
    gate->expected = expected;
    if (pthread_mutex_init(&gate->mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&gate->cond, NULL) != 0) {
        pthread_mutex_destroy(&gate->mutex);
        return -1;
    }
    return 0;
}

static void
start_gate_wait(StartGate* gate)
{
    pthread_mutex_lock(&gate->mutex);
    gate->arrived++;
    if (gate->arrived == gate->expected) {
        gate->open = 1;
        pthread_cond_broadcast(&gate->cond);
    } else {
        while (!gate->open) {
            pthread_cond_wait(&gate->cond, &gate->mutex);
        }
    }
    pthread_mutex_unlock(&gate->mutex);
}

static void
start_gate_abort(StartGate* gate)
{
    pthread_mutex_lock(&gate->mutex);
    gate->open = 1;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->mutex);
}

static void
start_gate_destroy(StartGate* gate)
{
    pthread_cond_destroy(&gate->cond);
    pthread_mutex_destroy(&gate->mutex);
}

static int
blocker_init(Blocker* blocker)
{
    if (pthread_mutex_init(&blocker->mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&blocker->cond, NULL) != 0) {
        pthread_mutex_destroy(&blocker->mutex);
        return -1;
    }
    return 0;
}

static void
blocker_release(Blocker* blocker)
{
    pthread_mutex_lock(&blocker->mutex);
    pthread_cond_broadcast(&blocker->cond);
    pthread_mutex_unlock(&blocker->mutex);
}

static void
blocker_destroy(Blocker* blocker)
{
    pthread_cond_destroy(&blocker->cond);
    pthread_mutex_destroy(&blocker->mutex);
}

static void*
worker_main(void* arg)
{
    WorkerState* state = (WorkerState*)arg;
    uint64_t local_calls = 0;
    unsigned int seed = state->seed;

    start_gate_wait(state->start_gate);

    if (state->stale) {
        for (long i = 0; i < state->stale_calls; i++) {
            peak_detach_stale_target((uint64_t)(seed + local_calls));
            local_calls++;
        }

        atomic_fetch_add_explicit(&stale_threads_blocked, 1, memory_order_relaxed);
        if (state->stale_mode == STALE_MODE_PARKED) {
            pthread_mutex_lock(&state->blocker->mutex);
            while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
                pthread_cond_wait(&state->blocker->cond, &state->blocker->mutex);
            }
            pthread_mutex_unlock(&state->blocker->mutex);
        } else if (state->stale_mode == STALE_MODE_UNRELATED_SPIN) {
            while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
                peak_detach_stale_unrelated((uint64_t)(seed + local_calls));
            }
        } else {
            struct timespec nap = { 0, 1000000L };
            while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
                peak_detach_stale_unrelated((uint64_t)(seed + local_calls));
                nanosleep(&nap, NULL);
            }
        }
    } else {
        if (state->active_after_stale) {
            pthread_mutex_lock(&state->blocker->mutex);
            while (!active_start_open &&
                   !atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
                pthread_cond_wait(&state->blocker->cond, &state->blocker->mutex);
            }
            pthread_mutex_unlock(&state->blocker->mutex);
        }
        while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
            peak_detach_stale_target((uint64_t)(seed + local_calls));
            local_calls++;
        }
    }

    state->calls = local_calls;
    return NULL;
}

int
main(int argc, char** argv)
{
    long active_threads = parse_long_arg(argc, argv, "--active-threads", 8);
    long stale_threads = parse_long_arg(argc, argv, "--stale-threads", 24);
    long stale_calls = parse_long_arg(argc, argv, "--stale-calls", 2048);
    long seconds = parse_long_arg(argc, argv, "--seconds", 2);
    long stale_ready_timeout =
        parse_long_arg(argc, argv, "--stale-ready-timeout", 5);
    int active_after_stale = has_flag_arg(argc, argv, "--active-after-stale");
    int request_detach_after_start =
        has_flag_arg(argc, argv, "--request-detach-after-start");
    const char* stale_mode_name =
        parse_string_arg(argc, argv, "--stale-mode", "parked");
    StaleMode stale_mode;
    long total_threads = active_threads + stale_threads;
    pthread_t* tids = NULL;
    WorkerState* states = NULL;
    StartGate gate;
    Blocker blocker;
    long created_threads = 0;

    if (active_threads <= 0 ||
        stale_threads <= 0 ||
        seconds <= 0 ||
        stale_ready_timeout <= 0) {
        fprintf(stderr,
                "active-threads, stale-threads, seconds, and "
                "stale-ready-timeout must be positive\n");
        return 2;
    }
    if (parse_stale_mode(stale_mode_name, &stale_mode) != 0) {
        fprintf(stderr, "stale-mode must be parked, unrelated-spin, or unrelated-sleep\n");
        return 2;
    }

    tids = calloc((size_t)total_threads, sizeof(*tids));
    states = calloc((size_t)total_threads, sizeof(*states));
    if (tids == NULL || states == NULL) {
        perror("calloc");
        free(tids);
        free(states);
        return 2;
    }
    if (start_gate_init(&gate, total_threads + 1) != 0 ||
        blocker_init(&blocker) != 0) {
        perror("init");
        free(tids);
        free(states);
        return 2;
    }

    atomic_store_explicit(&stop_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&side_effect, 0, memory_order_relaxed);
    atomic_store_explicit(&unrelated_effect, 0, memory_order_relaxed);
    atomic_store_explicit(&stale_threads_blocked, 0, memory_order_relaxed);
    active_start_open = active_after_stale ? 0 : 1;

    for (long i = 0; i < total_threads; i++) {
        states[i].seed = (unsigned int)(0x85ebca6bu + (unsigned int)i);
        states[i].stale = i < stale_threads;
        states[i].active_after_stale = active_after_stale;
        states[i].stale_mode = stale_mode;
        states[i].stale_calls = stale_calls;
        states[i].start_gate = &gate;
        states[i].blocker = &blocker;
        if (pthread_create(&tids[i], NULL, worker_main, &states[i]) != 0) {
            perror("pthread_create");
            atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
            start_gate_abort(&gate);
            blocker_release(&blocker);
            for (long j = 0; j < created_threads; j++) {
                pthread_join(tids[j], NULL);
            }
            start_gate_destroy(&gate);
            blocker_destroy(&blocker);
            free(tids);
            free(states);
            return 2;
        }
        created_threads++;
    }

    start_gate_wait(&gate);
    double wait_start = monotonic_seconds();
    while (atomic_load_explicit(&stale_threads_blocked, memory_order_relaxed) <
           stale_threads) {
        if (monotonic_seconds() - wait_start > (double)stale_ready_timeout) {
            fprintf(stderr, "timed out waiting for stale threads to block\n");
            atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
            pthread_mutex_lock(&blocker.mutex);
            active_start_open = 1;
            pthread_cond_broadcast(&blocker.cond);
            pthread_mutex_unlock(&blocker.mutex);
            blocker_release(&blocker);
            for (long i = 0; i < created_threads; i++) {
                pthread_join(tids[i], NULL);
            }
            start_gate_destroy(&gate);
            blocker_destroy(&blocker);
            free(tids);
            free(states);
            return 2;
        }
        usleep(1000);
    }

    pthread_mutex_lock(&blocker.mutex);
    active_start_open = 1;
    pthread_cond_broadcast(&blocker.cond);
    pthread_mutex_unlock(&blocker.mutex);

    if (request_detach_after_start &&
        !request_detach_after_workers_start()) {
        atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
        blocker_release(&blocker);
        for (long i = 0; i < created_threads; i++) {
            pthread_join(tids[i], NULL);
        }
        start_gate_destroy(&gate);
        blocker_destroy(&blocker);
        free(tids);
        free(states);
        return 2;
    }

    double start = monotonic_seconds();
    usleep((useconds_t)seconds * 1000000u);
    atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
    blocker_release(&blocker);

    uint64_t active_calls = 0;
    uint64_t stale_total_calls = 0;
    for (long i = 0; i < total_threads; i++) {
        pthread_join(tids[i], NULL);
        if (states[i].stale) {
            stale_total_calls += states[i].calls;
        } else {
            active_calls += states[i].calls;
        }
    }
    double elapsed = monotonic_seconds() - start;

    printf("active_threads=%ld stale_threads=%ld stale_mode=%s stale_blocked=%ld "
           "active_calls=%lu stale_calls=%lu elapsed=%.6f "
           "calls_per_sec=%.3f side_effect=%lu unrelated_effect=%lu\n",
           active_threads,
           stale_threads,
           stale_mode_name,
           (long)atomic_load_explicit(&stale_threads_blocked, memory_order_relaxed),
           (unsigned long)active_calls,
           (unsigned long)stale_total_calls,
           elapsed,
           (double)active_calls / elapsed,
           (unsigned long)atomic_load_explicit(&side_effect, memory_order_relaxed),
           (unsigned long)atomic_load_explicit(&unrelated_effect, memory_order_relaxed));

    start_gate_destroy(&gate);
    blocker_destroy(&blocker);
    free(tids);
    free(states);
    return 0;
}
