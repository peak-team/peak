#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    long expected;
    long arrived;
    int open;
} StartGate;

typedef struct {
    StartGate* gate;
    uint64_t calls;
    uint64_t seed;
    int cpu;
    int affinity_error;
} WorkerState;

static atomic_int stop_requested;
static unsigned int work_iterations = 1;

static double
monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static inline uint64_t
hotpath_work(uint64_t value)
{
    for (unsigned int iteration = 0;
         iteration < work_iterations;
         iteration++) {
        value = value * UINT64_C(2862933555777941757) +
                UINT64_C(3037000493);
        __asm__ volatile("" : "+r"(value) :: "memory");
    }
    return value;
}

__attribute__((noinline, used, externally_visible,
               visibility("default")))
uint64_t
peak_callback_hotpath_target(uint64_t value)
{
    return hotpath_work(value);
}

static double
measure_raw_work_ns(unsigned int iterations)
{
    const uint64_t samples = UINT64_C(200000);
    uint64_t value = UINT64_C(0x9e3779b97f4a7c15);
    work_iterations = iterations;
    double start = monotonic_seconds();
    for (uint64_t sample = 0; sample < samples; sample++) {
        value = hotpath_work(value + sample);
    }
    double elapsed = monotonic_seconds() - start;
    __asm__ volatile("" : "+r"(value) :: "memory");
    return elapsed * 1e9 / (double)samples;
}

static double
calibrate_work(long target_ns)
{
    unsigned int iterations = 1;
    double measured_ns = measure_raw_work_ns(iterations);

    while (measured_ns < (double)target_ns && iterations < (1U << 24)) {
        double ratio = (double)target_ns / (measured_ns > 0.0 ? measured_ns : 1.0);
        unsigned int next =
            ratio > 2.0 ? iterations * 2U
                        : iterations + (iterations / 4U) + 1U;
        if (next <= iterations) {
            break;
        }
        iterations = next;
        measured_ns = measure_raw_work_ns(iterations);
    }

    work_iterations = iterations;
    return measured_ns;
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
start_gate_worker_wait(StartGate* gate)
{
    pthread_mutex_lock(&gate->mutex);
    gate->arrived++;
    if (gate->arrived == gate->expected) {
        pthread_cond_broadcast(&gate->cond);
    }
    while (!gate->open) {
        pthread_cond_wait(&gate->cond, &gate->mutex);
    }
    pthread_mutex_unlock(&gate->mutex);
}

static double
start_gate_open(StartGate* gate)
{
    pthread_mutex_lock(&gate->mutex);
    while (gate->arrived != gate->expected) {
        pthread_cond_wait(&gate->cond, &gate->mutex);
    }
    double start = monotonic_seconds();
    gate->open = 1;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->mutex);
    return start;
}

static void*
worker_main(void* arg)
{
    WorkerState* state = arg;
    uint64_t calls = 0;
    uint64_t value = state->seed;

    if (state->cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(state->cpu, &set);
        state->affinity_error =
            pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }
    start_gate_worker_wait(state->gate);
    while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
        for (unsigned int burst = 0; burst < 256; burst++) {
            value = peak_callback_hotpath_target(value + calls);
            calls++;
        }
    }
    __asm__ volatile("" : "+r"(value) :: "memory");
    state->calls = calls;
    return NULL;
}

static int*
parse_cpu_list(long threads)
{
    const char* value = getenv("PEAK_BENCH_CPU_LIST");
    int* cpus = calloc((size_t)threads, sizeof(*cpus));
    const char* cursor = value;

    if (cpus == NULL) {
        return NULL;
    }
    for (long index = 0; index < threads; index++) {
        cpus[index] = -1;
    }
    if (value == NULL || *value == '\0') {
        return cpus;
    }

    for (long index = 0; index < threads && *cursor != '\0'; index++) {
        char* end = NULL;
        errno = 0;
        long cpu = strtol(cursor, &end, 10);
        if (errno != 0 || end == cursor || cpu < 0 || cpu >= CPU_SETSIZE) {
            free(cpus);
            return NULL;
        }
        cpus[index] = (int)cpu;
        cursor = end;
        if (*cursor == ',') {
            cursor++;
        } else if (*cursor != '\0') {
            free(cpus);
            return NULL;
        }
    }
    return cpus;
}

static long
parse_long_arg(int argc, char** argv, const char* name, long fallback)
{
    for (int index = 1; index + 1 < argc; index++) {
        if (strcmp(argv[index], name) == 0) {
            char* end = NULL;
            errno = 0;
            long value = strtol(argv[index + 1], &end, 10);
            if (errno != 0 || end == argv[index + 1] || *end != '\0') {
                return -1;
            }
            return value;
        }
    }
    return fallback;
}

static double
parse_double_arg(int argc, char** argv, const char* name, double fallback)
{
    for (int index = 1; index + 1 < argc; index++) {
        if (strcmp(argv[index], name) == 0) {
            char* end = NULL;
            errno = 0;
            double value = strtod(argv[index + 1], &end);
            if (errno != 0 || end == argv[index + 1] || *end != '\0') {
                return -1.0;
            }
            return value;
        }
    }
    return fallback;
}

int
main(int argc, char** argv)
{
    long threads = parse_long_arg(argc, argv, "--threads", 1);
    long target_ns = parse_long_arg(argc, argv, "--target-ns", 10);
    double seconds = parse_double_arg(argc, argv, "--seconds", 1.0);
    pthread_t* tids;
    WorkerState* states;
    int* cpus;
    StartGate gate;

    if (threads <= 0 || target_ns <= 0 || seconds <= 0.0) {
        fprintf(stderr, "threads, target-ns, and seconds must be positive\n");
        return 2;
    }

    double calibrated_work_ns = calibrate_work(target_ns);
    tids = calloc((size_t)threads, sizeof(*tids));
    states = calloc((size_t)threads, sizeof(*states));
    cpus = parse_cpu_list(threads);
    if (tids == NULL || states == NULL || cpus == NULL ||
        start_gate_init(&gate, threads) != 0) {
        perror("benchmark setup");
        free(tids);
        free(states);
        free(cpus);
        return 2;
    }

    atomic_store_explicit(&stop_requested, 0, memory_order_relaxed);
    for (long index = 0; index < threads; index++) {
        states[index].gate = &gate;
        states[index].seed =
            UINT64_C(0x9e3779b97f4a7c15) ^ (uint64_t)index;
        states[index].cpu = cpus[index];
        if (pthread_create(&tids[index], NULL, worker_main, &states[index]) != 0) {
            fprintf(stderr, "pthread_create failed at worker %ld\n", index);
            atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
            return 2;
        }
    }

    double start = start_gate_open(&gate);
    struct timespec delay = {
        .tv_sec = (time_t)seconds,
        .tv_nsec = (long)((seconds - (double)(time_t)seconds) * 1e9)
    };
    nanosleep(&delay, NULL);
    atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);

    uint64_t calls = 0;
    for (long index = 0; index < threads; index++) {
        pthread_join(tids[index], NULL);
        if (states[index].affinity_error != 0) {
            fprintf(stderr,
                    "pthread_setaffinity_np failed for worker %ld: %s\n",
                    index,
                    strerror(states[index].affinity_error));
            free(cpus);
            free(states);
            free(tids);
            return 2;
        }
        calls += states[index].calls;
    }
    double elapsed = monotonic_seconds() - start;
    double calls_per_sec = (double)calls / elapsed;

    printf("threads=%ld target_ns=%ld calibrated_work_ns=%.3f "
           "work_iterations=%u calls=%lu elapsed=%.9f "
           "calls_per_sec=%.3f ns_per_call=%.3f "
           "thread_ns_per_call=%.3f\n",
           threads,
           target_ns,
           calibrated_work_ns,
           work_iterations,
           (unsigned long)calls,
           elapsed,
           calls_per_sec,
           1e9 / calls_per_sec,
           (double)threads * 1e9 / calls_per_sec);

    pthread_cond_destroy(&gate.cond);
    pthread_mutex_destroy(&gate.mutex);
    free(states);
    free(tids);
    free(cpus);
    return 0;
}
