#define _GNU_SOURCE
#include <errno.h>
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

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    long expected;
    long arrived;
    int open;
} StartGate;

__attribute__((noinline))
void peak_detach_hot_target(uint64_t value)
{
    atomic_fetch_add_explicit(&side_effect, value + 1, memory_order_relaxed);
    asm volatile("" ::: "memory");
}

typedef struct {
    uint64_t calls;
    unsigned int seed;
    StartGate* start_gate;
} WorkerState;

static double
monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void*
worker_main(void* arg)
{
    WorkerState* state = (WorkerState*)arg;
    uint64_t local_calls = 0;
    unsigned int seed = state->seed;

    pthread_mutex_lock(&state->start_gate->mutex);
    state->start_gate->arrived++;
    if (state->start_gate->arrived == state->start_gate->expected) {
        state->start_gate->open = 1;
        pthread_cond_broadcast(&state->start_gate->cond);
    } else {
        while (!state->start_gate->open) {
            pthread_cond_wait(&state->start_gate->cond,
                              &state->start_gate->mutex);
        }
    }
    pthread_mutex_unlock(&state->start_gate->mutex);

    while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
        peak_detach_hot_target((uint64_t)(seed + local_calls));
        local_calls++;
    }

    state->calls = local_calls;
    return NULL;
}

static long
parse_long_arg(int argc, char** argv, const char* name, long fallback)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            char* end = NULL;
            errno = 0;
            long value = strtol(argv[i + 1], &end, 10);
            if (errno == 0 && end != argv[i + 1] && *end == '\0' && value > 0) {
                return value;
            }
        }
    }
    return fallback;
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

int
main(int argc, char** argv)
{
    long threads = parse_long_arg(argc, argv, "--threads", 4);
    long seconds = parse_long_arg(argc, argv, "--seconds", 3);
    pthread_t* tids = calloc((size_t)threads, sizeof(*tids));
    WorkerState* states = calloc((size_t)threads, sizeof(*states));
    StartGate gate;
    long created_threads = 0;
    if (tids == NULL || states == NULL) {
        perror("calloc");
        return 2;
    }
    if (start_gate_init(&gate, threads + 1) != 0) {
        perror("start_gate_init");
        free(tids);
        free(states);
        return 2;
    }

    atomic_store_explicit(&stop_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&side_effect, 0, memory_order_relaxed);
    for (long i = 0; i < threads; i++) {
        states[i].seed = (unsigned int)(0x9e3779b9u + (unsigned int)i);
        states[i].start_gate = &gate;
        if (pthread_create(&tids[i], NULL, worker_main, &states[i]) != 0) {
            perror("pthread_create");
            atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
            start_gate_abort(&gate);
            for (long j = 0; j < created_threads; j++) {
                pthread_join(tids[j], NULL);
            }
            start_gate_destroy(&gate);
            free(tids);
            free(states);
            return 2;
        }
        created_threads++;
    }

    start_gate_wait(&gate);
    double start = monotonic_seconds();
    usleep((useconds_t)seconds * 1000000u);
    atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);

    uint64_t calls = 0;
    for (long i = 0; i < threads; i++) {
        pthread_join(tids[i], NULL);
        calls += states[i].calls;
    }
    double elapsed = monotonic_seconds() - start;

    printf("threads=%ld seconds=%ld calls=%lu elapsed=%.6f calls_per_sec=%.3f side_effect=%lu\n",
           threads,
           seconds,
           (unsigned long)calls,
           elapsed,
           (double)calls / elapsed,
           (unsigned long)atomic_load_explicit(&side_effect, memory_order_relaxed));

    start_gate_destroy(&gate);
    free(tids);
    free(states);
    return 0;
}
