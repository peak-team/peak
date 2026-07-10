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

#define PEAK_MILC_PHASE_COUNT 13U
#define PEAK_MILC_PHASE_TARGETS 64U
#define PEAK_MILC_TARGET_COUNT \
    (PEAK_MILC_PHASE_COUNT * PEAK_MILC_PHASE_TARGETS)
#define PEAK_MILC_MAX_THREADS 256U

static atomic_uint_fast64_t side_effect;
static atomic_uint_fast64_t global_true_calls;
static atomic_uint completed_worker_count;
static atomic_uint_fast64_t target_call_counts[PEAK_MILC_TARGET_COUNT];
static atomic_uint_fast64_t
    phase_target_call_counts[PEAK_MILC_PHASE_COUNT][PEAK_MILC_TARGET_COUNT];
static atomic_uint_fast64_t
    target_thread_call_counts[PEAK_MILC_TARGET_COUNT][PEAK_MILC_MAX_THREADS];
static unsigned int target_inner_work;

static uint64_t
milc_like_inner_work(uint64_t value, unsigned int iterations);

#define PEAK_MILC_PHASES(X) \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12)

#define PEAK_MILC_SLOTS(X, phase) \
    X(phase, s00, 0) X(phase, s01, 1) X(phase, s02, 2) X(phase, s03, 3) \
    X(phase, s04, 4) X(phase, s05, 5) X(phase, s06, 6) X(phase, s07, 7) \
    X(phase, s08, 8) X(phase, s09, 9) X(phase, s10, 10) X(phase, s11, 11) \
    X(phase, s12, 12) X(phase, s13, 13) X(phase, s14, 14) X(phase, s15, 15) \
    X(phase, s16, 16) X(phase, s17, 17) X(phase, s18, 18) X(phase, s19, 19) \
    X(phase, s20, 20) X(phase, s21, 21) X(phase, s22, 22) X(phase, s23, 23) \
    X(phase, s24, 24) X(phase, s25, 25) X(phase, s26, 26) X(phase, s27, 27) \
    X(phase, s28, 28) X(phase, s29, 29) X(phase, s30, 30) X(phase, s31, 31) \
    X(phase, s32, 32) X(phase, s33, 33) X(phase, s34, 34) X(phase, s35, 35) \
    X(phase, s36, 36) X(phase, s37, 37) X(phase, s38, 38) X(phase, s39, 39) \
    X(phase, s40, 40) X(phase, s41, 41) X(phase, s42, 42) X(phase, s43, 43) \
    X(phase, s44, 44) X(phase, s45, 45) X(phase, s46, 46) X(phase, s47, 47) \
    X(phase, s48, 48) X(phase, s49, 49) X(phase, s50, 50) X(phase, s51, 51) \
    X(phase, s52, 52) X(phase, s53, 53) X(phase, s54, 54) X(phase, s55, 55) \
    X(phase, s56, 56) X(phase, s57, 57) X(phase, s58, 58) X(phase, s59, 59) \
    X(phase, s60, 60) X(phase, s61, 61) X(phase, s62, 62) X(phase, s63, 63)

#define PEAK_MILC_HOT_FN(phase, slot, slot_value) \
    __attribute__((noinline, noclone, used, externally_visible, visibility("default"))) \
    void peak_milc_hot_##phase##_##slot(uint64_t value) \
    { \
        const unsigned int target_index = \
            (unsigned int)(phase) * PEAK_MILC_PHASE_TARGETS + (slot_value); \
        unsigned int logical_phase = (unsigned int)((value >> 48) & 0xffffU); \
        unsigned int logical_thread = (unsigned int)((value >> 40) & 0xffU); \
        logical_phase %= PEAK_MILC_PHASE_COUNT; \
        logical_thread %= PEAK_MILC_MAX_THREADS; \
        atomic_fetch_add_explicit(&target_call_counts[target_index], \
                                  UINT64_C(1), \
                                  memory_order_relaxed); \
        atomic_fetch_add_explicit(&global_true_calls, \
                                  UINT64_C(1), \
                                  memory_order_relaxed); \
        atomic_fetch_add_explicit( \
            &phase_target_call_counts[logical_phase][target_index], \
            UINT64_C(1), \
            memory_order_relaxed); \
        atomic_fetch_add_explicit( \
            &target_thread_call_counts[target_index][logical_thread], \
            UINT64_C(1), \
            memory_order_relaxed); \
        asm volatile( \
            "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" \
            "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t" \
            : : "r"(value) : "memory"); \
        if (target_inner_work > 0) { \
            atomic_fetch_add_explicit(&side_effect, \
                                      milc_like_inner_work(value, \
                                                           target_inner_work), \
                                      memory_order_relaxed); \
        } \
    }

#define PEAK_MILC_DEFINE_PHASE(phase) \
    PEAK_MILC_SLOTS(PEAK_MILC_HOT_FN, phase)

PEAK_MILC_PHASES(PEAK_MILC_DEFINE_PHASE)

__attribute__((noinline, noclone, used, externally_visible, visibility("default")))
void peak_milc_long_compute(uint64_t value)
{
    uint64_t acc = value;

    for (unsigned int i = 0; i < 4096; i++) {
        acc = acc * UINT64_C(6364136223846793005) +
              UINT64_C(1442695040888963407);
    }

    atomic_fetch_add_explicit(&side_effect, acc, memory_order_relaxed);
}

typedef void (*PeakMilcTargetFn)(uint64_t);

#define PEAK_MILC_HOT_PTR(phase, slot, slot_value) \
    peak_milc_hot_##phase##_##slot,
#define PEAK_MILC_PTR_PHASE(phase) \
    PEAK_MILC_SLOTS(PEAK_MILC_HOT_PTR, phase)

static PeakMilcTargetFn hot_targets[PEAK_MILC_TARGET_COUNT] = {
    PEAK_MILC_PHASES(PEAK_MILC_PTR_PHASE)
};

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int expected;
    int arrived;
    int open;
    double start;
    double deadline;
} StartGate;

typedef struct {
    StartGate* gate;
    uint64_t iterations;
    uint64_t calls;
    uint64_t sweeps;
    unsigned int seed;
    unsigned int thread_index;
    unsigned int phase_repeats;
    int cpu_id;
    int affinity_error;
} WorkerState;

typedef struct {
    double elapsed;
    uint64_t true_calls;
} CallCheckpoint;

static double
monotonic_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int
start_gate_init(StartGate* gate, int expected)
{
    memset(gate, 0, sizeof(*gate));
    gate->expected = expected;
    return pthread_mutex_init(&gate->mutex, NULL) == 0 &&
           pthread_cond_init(&gate->cond, NULL) == 0;
}

static void
start_gate_wait(StartGate* gate)
{
    pthread_mutex_lock(&gate->mutex);
    gate->arrived++;
    pthread_cond_broadcast(&gate->cond);
    while (!gate->open) {
        pthread_cond_wait(&gate->cond, &gate->mutex);
    }
    pthread_mutex_unlock(&gate->mutex);
}

static double
start_gate_open(StartGate* gate, double seconds)
{
    pthread_mutex_lock(&gate->mutex);
    while (gate->arrived < gate->expected) {
        pthread_cond_wait(&gate->cond, &gate->mutex);
    }
    gate->start = monotonic_seconds();
    gate->deadline = gate->start + seconds;
    gate->open = 1;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->mutex);
    return gate->start;
}

static void
start_gate_destroy(StartGate* gate)
{
    pthread_cond_destroy(&gate->cond);
    pthread_mutex_destroy(&gate->mutex);
}

static uint64_t
milc_like_inner_work(uint64_t value, unsigned int iterations)
{
    uint64_t acc = value;

    for (unsigned int i = 0; i < iterations; i++) {
        acc ^= acc >> 33;
        acc *= UINT64_C(0xff51afd7ed558ccd);
        acc ^= acc >> 29;
    }

    return acc;
}

static void
call_target(WorkerState* state,
            unsigned int logical_phase,
            unsigned int target_index)
{
    uint64_t value =
        ((uint64_t)logical_phase << 48) |
        ((uint64_t)state->thread_index << 40) |
        ((((uint64_t)state->seed << 16) ^
          state->calls ^
          (uint64_t)target_index) &
         UINT64_C(0x000000ffffffffff));

    hot_targets[target_index](value);
    state->calls++;
}

static void
call_phase_targets(WorkerState* state,
                   unsigned int logical_phase,
                   unsigned int physical_phase,
                   unsigned int stride,
                   unsigned int salt)
{
    unsigned int base = physical_phase * PEAK_MILC_PHASE_TARGETS;

    if (stride == 0) {
        stride = 1;
    }
    for (unsigned int slot = salt % stride;
         slot < PEAK_MILC_PHASE_TARGETS;
         slot += stride) {
        call_target(state, logical_phase, base + slot);
    }
}

static void*
worker_main(void* arg)
{
    WorkerState* state = (WorkerState*)arg;

    if (state->cpu_id >= 0) {
        cpu_set_t cpu_set;

        CPU_ZERO(&cpu_set);
        CPU_SET(state->cpu_id, &cpu_set);
        state->affinity_error =
            pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);
    }
    start_gate_wait(state->gate);
    while (state->iterations > 0 ? state->sweeps < state->iterations
                                 : monotonic_seconds() < state->gate->deadline) {
        unsigned int primary =
            (unsigned int)(state->sweeps % PEAK_MILC_PHASE_COUNT);

        for (unsigned int repeat = 0; repeat < state->phase_repeats; repeat++) {
            call_phase_targets(state, primary, primary, 1, 0);
        }

        state->sweeps++;
        if ((state->calls & UINT64_C(0xffff)) == 0) {
            peak_milc_long_compute(((uint64_t)state->seed << 32) ^
                                   state->calls);
        }
    }

    atomic_fetch_add_explicit(&side_effect,
                              ((uint64_t)state->seed << 32) ^ state->calls,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&completed_worker_count, 1U, memory_order_release);
    return NULL;
}

static int
has_flag(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static long
parse_long_arg(int argc, char** argv, const char* name, long default_value)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            char* end = NULL;
            long value = strtol(argv[i + 1], &end, 10);
            if (end != argv[i + 1] && *end == '\0' && value > 0) {
                return value;
            }
        }
    }
    return default_value;
}

static uint64_t
load_target_count(unsigned int target_index)
{
    return atomic_load_explicit(&target_call_counts[target_index],
                                memory_order_relaxed);
}

static uint64_t
load_total_target_count(void)
{
    return atomic_load_explicit(&global_true_calls, memory_order_relaxed);
}

static void
record_checkpoint(CallCheckpoint* checkpoints,
                  unsigned int* checkpoint_count,
                  unsigned int checkpoint_limit,
                  double elapsed,
                  uint64_t true_calls)
{
    const double min_elapsed_delta = 0.000001;

    if (*checkpoint_count >= checkpoint_limit) {
        return;
    }
    if (*checkpoint_count > 0) {
        const CallCheckpoint* previous = &checkpoints[*checkpoint_count - 1];

        if (true_calls <= previous->true_calls) {
            return;
        }
        if (elapsed <= previous->elapsed) {
            elapsed = previous->elapsed + min_elapsed_delta;
        }
    }

    checkpoints[*checkpoint_count].elapsed = elapsed;
    checkpoints[*checkpoint_count].true_calls = true_calls;
    (*checkpoint_count)++;
}

static void
record_timed_checkpoint_sample(CallCheckpoint* checkpoints,
                               unsigned int* checkpoint_count,
                               unsigned int checkpoint_limit,
                               long checkpoint_calls,
                               uint64_t* next_checkpoint_calls,
                               double elapsed,
                               uint64_t true_calls)
{
    uint64_t interval;

    if (checkpoint_calls <= 0 ||
        *checkpoint_count >= checkpoint_limit ||
        true_calls < *next_checkpoint_calls) {
        return;
    }

    record_checkpoint(checkpoints,
                      checkpoint_count,
                      checkpoint_limit,
                      elapsed,
                      true_calls);

    interval = (uint64_t)checkpoint_calls;
    *next_checkpoint_calls =
        UINT64_MAX - true_calls < interval ? UINT64_MAX : true_calls + interval;
}

static void
record_fixed_checkpoint_thresholds(CallCheckpoint* checkpoints,
                                   unsigned int* checkpoint_count,
                                   unsigned int checkpoint_limit,
                                   long checkpoint_calls,
                                   uint64_t* next_checkpoint_calls,
                                   double elapsed,
                                   uint64_t true_calls)
{
    uint64_t interval;

    if (checkpoint_calls <= 0) {
        return;
    }
    interval = (uint64_t)checkpoint_calls;
    while (*checkpoint_count < checkpoint_limit &&
           true_calls >= *next_checkpoint_calls) {
        double checkpoint_elapsed = elapsed;

        if (*checkpoint_count > 0) {
            const CallCheckpoint* previous =
                &checkpoints[*checkpoint_count - 1];
            uint64_t observed_calls = true_calls - previous->true_calls;

            if (observed_calls > 0 && elapsed > previous->elapsed) {
                double threshold_calls =
                    (double)(*next_checkpoint_calls - previous->true_calls);
                checkpoint_elapsed =
                    previous->elapsed +
                    (elapsed - previous->elapsed) *
                        (threshold_calls / (double)observed_calls);
            }
        }

        record_checkpoint(checkpoints,
                          checkpoint_count,
                          checkpoint_limit,
                          checkpoint_elapsed,
                          *next_checkpoint_calls);
        if (UINT64_MAX - *next_checkpoint_calls < interval) {
            *next_checkpoint_calls = UINT64_MAX;
            break;
        }
        *next_checkpoint_calls += interval;
    }
}

static void
record_final_checkpoint(CallCheckpoint* checkpoints,
                        unsigned int* checkpoint_count,
                        unsigned int checkpoint_limit,
                        long checkpoint_calls,
                        double elapsed,
                        uint64_t true_calls)
{
    if (checkpoint_calls <= 0) {
        return;
    }
    record_checkpoint(checkpoints,
                      checkpoint_count,
                      checkpoint_limit,
                      elapsed,
                      true_calls);
}

static void
sleep_until(double deadline)
{
    struct timespec ts;

    ts.tv_sec = (time_t)deadline;
    ts.tv_nsec = (long)((deadline - (double)ts.tv_sec) * 1e9);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

static void
print_window(unsigned int index, double elapsed, uint64_t true_calls)
{
    printf("milc_like_window index=%u elapsed=%.9f true_calls=%llu\n",
           index,
           elapsed,
           (unsigned long long)true_calls);
    fflush(stdout);
}

static void
print_checkpoint(unsigned int index, double elapsed, uint64_t true_calls)
{
    printf("milc_like_checkpoint index=%u elapsed=%.9f true_calls=%llu\n",
           index,
           elapsed,
           (unsigned long long)true_calls);
}

static void
print_target_counts(void)
{
    printf("milc_like_target_calls count=%u values=",
           (unsigned int)PEAK_MILC_TARGET_COUNT);
    for (unsigned int i = 0; i < PEAK_MILC_TARGET_COUNT; i++) {
        printf("%s%llu",
               i == 0 ? "" : ",",
               (unsigned long long)load_target_count(i));
    }
    printf("\n");
}

static void
print_phase_breadth(void)
{
    printf("milc_like_phase_breadth phases=%u values=",
           (unsigned int)PEAK_MILC_PHASE_COUNT);
    for (unsigned int phase = 0; phase < PEAK_MILC_PHASE_COUNT; phase++) {
        unsigned int breadth = 0;

        for (unsigned int target = 0; target < PEAK_MILC_TARGET_COUNT;
             target++) {
            if (atomic_load_explicit(&phase_target_call_counts[phase][target],
                                     memory_order_relaxed) != 0) {
                breadth++;
            }
        }
        printf("%s%u:%u", phase == 0 ? "" : ",", phase, breadth);
    }
    printf("\n");
}

static void
print_target_phase_calls(void)
{
    printf("milc_like_target_phase_calls phases=%u targets=%u values=",
           (unsigned int)PEAK_MILC_PHASE_COUNT,
           (unsigned int)PEAK_MILC_TARGET_COUNT);
    for (unsigned int phase = 0; phase < PEAK_MILC_PHASE_COUNT; phase++) {
        for (unsigned int target = 0; target < PEAK_MILC_TARGET_COUNT;
             target++) {
            uint64_t count = atomic_load_explicit(
                &phase_target_call_counts[phase][target],
                memory_order_relaxed);
            printf("%s%llu",
                   phase == 0 && target == 0 ? "" : ",",
                   (unsigned long long)count);
        }
    }
    printf("\n");
}

static void
print_target_thread_info(unsigned int thread_count)
{
    printf("milc_like_target_thread_info threads=%u targets=%u breadth=",
           thread_count,
           (unsigned int)PEAK_MILC_TARGET_COUNT);
    for (unsigned int target = 0; target < PEAK_MILC_TARGET_COUNT; target++) {
        unsigned int breadth = 0;

        for (unsigned int thread = 0; thread < thread_count; thread++) {
            if (atomic_load_explicit(&target_thread_call_counts[target][thread],
                                     memory_order_relaxed) != 0) {
                breadth++;
            }
        }
        printf("%s%u", target == 0 ? "" : ",", breadth);
    }

    printf(" max=");
    for (unsigned int target = 0; target < PEAK_MILC_TARGET_COUNT; target++) {
        uint64_t max_calls = 0;

        for (unsigned int thread = 0; thread < thread_count; thread++) {
            uint64_t calls = atomic_load_explicit(
                &target_thread_call_counts[target][thread],
                memory_order_relaxed);
            if (calls > max_calls) {
                max_calls = calls;
            }
        }
        printf("%s%llu",
               target == 0 ? "" : ",",
               (unsigned long long)max_calls);
    }
    printf("\n");
}

int
main(int argc, char** argv)
{
    long thread_count = parse_long_arg(argc, argv, "--threads", 4);
    long seconds = parse_long_arg(argc, argv, "--seconds", 3);
    long iterations = parse_long_arg(argc, argv, "--iterations", 0);
    long inner_work = parse_long_arg(argc, argv, "--inner-work", 0);
    long phase_repeats = parse_long_arg(argc, argv, "--phase-repeats", 1);
    long window_ms = parse_long_arg(argc, argv, "--window-ms", 1000);
    long checkpoint_calls =
        parse_long_arg(argc, argv, "--checkpoint-calls", 0);
    long checkpoint_limit =
        parse_long_arg(argc, argv, "--checkpoint-limit", 4096);
    int pin_workers = has_flag(argc, argv, "--pin-workers");
    StartGate gate;
    pthread_t* threads = NULL;
    WorkerState* states = NULL;
    CallCheckpoint* checkpoints = NULL;
    unsigned int checkpoint_count = 0;
    uint64_t next_checkpoint_calls = 0;
    double start;
    double elapsed;
    double deadline;
    uint64_t total_calls = 0;
    uint64_t total_sweeps = 0;
    uint64_t initial_calls;
    int allowed_cpus[CPU_SETSIZE];
    unsigned int allowed_cpu_count = 0;
    unsigned int pinned_worker_count = 0;

    target_inner_work = (unsigned int)inner_work;
    if (thread_count > 256) {
        thread_count = 256;
    }
    if (phase_repeats > 1048576) {
        phase_repeats = 1048576;
    }
    if (checkpoint_limit > 65536) {
        checkpoint_limit = 65536;
    }
    if (checkpoint_calls > 0) {
        checkpoints = calloc((size_t)checkpoint_limit, sizeof(*checkpoints));
        if (checkpoints == NULL) {
            fprintf(stderr, "checkpoint allocation failed\n");
            return 2;
        }
    }
    if (pin_workers) {
        cpu_set_t allowed_set;

        CPU_ZERO(&allowed_set);
        if (sched_getaffinity(0, sizeof(allowed_set), &allowed_set) != 0) {
            perror("sched_getaffinity");
            return 2;
        }
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
            if (CPU_ISSET(cpu, &allowed_set)) {
                allowed_cpus[allowed_cpu_count++] = cpu;
            }
        }
        if (allowed_cpu_count == 0) {
            fprintf(stderr, "allowed CPU set is empty\n");
            return 2;
        }
    }
    if (!start_gate_init(&gate, (int)thread_count)) {
        fprintf(stderr, "failed to initialize start gate\n");
        return 2;
    }

    threads = calloc((size_t)thread_count, sizeof(*threads));
    states = calloc((size_t)thread_count, sizeof(*states));
    if (threads == NULL || states == NULL) {
        fprintf(stderr, "allocation failed\n");
        free(checkpoints);
        free(threads);
        free(states);
        start_gate_destroy(&gate);
        return 2;
    }

    atomic_store_explicit(&completed_worker_count, 0U, memory_order_relaxed);
    for (long i = 0; i < thread_count; i++) {
        states[i].gate = &gate;
        states[i].iterations = (uint64_t)iterations;
        states[i].seed = (unsigned int)(0x9e3779b9U + (unsigned int)i);
        states[i].thread_index = (unsigned int)i;
        states[i].phase_repeats = (unsigned int)phase_repeats;
        states[i].cpu_id = pin_workers
                               ? allowed_cpus[((unsigned int)i *
                                               allowed_cpu_count) /
                                              (unsigned int)thread_count]
                               : -1;
        if (pthread_create(&threads[i], NULL, worker_main, &states[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            free(checkpoints);
            free(threads);
            free(states);
            start_gate_destroy(&gate);
            return 2;
        }
    }

    initial_calls = load_total_target_count();
    start = start_gate_open(&gate, (double)seconds);
    deadline = gate.deadline;
    if (checkpoint_calls > 0) {
        checkpoints[checkpoint_count].elapsed = 0.0;
        checkpoints[checkpoint_count].true_calls = initial_calls;
        checkpoint_count++;
        next_checkpoint_calls = initial_calls + (uint64_t)checkpoint_calls;
    }
    if (iterations == 0) {
        double window_seconds = (double)window_ms / 1000.0;
        double next_window = start + window_seconds;
        unsigned int window_index = 0;

        print_window(window_index, 0.0, initial_calls);
        while (monotonic_seconds() < deadline) {
            double now = monotonic_seconds();
            uint64_t true_calls = load_total_target_count();

            if (now >= next_window) {
                window_index++;
                print_window(window_index, now - start, true_calls);
                next_window += window_seconds;
            }
            record_timed_checkpoint_sample(checkpoints,
                                           &checkpoint_count,
                                           (unsigned int)checkpoint_limit,
                                           checkpoint_calls,
                                           &next_checkpoint_calls,
                                           now - start,
                                           true_calls);
            sleep_until(now + 0.001);
        }
        window_index++;
        print_window(window_index,
                     monotonic_seconds() - start,
                     load_total_target_count());
    } else {
        while (atomic_load_explicit(&completed_worker_count,
                                    memory_order_acquire) <
               (unsigned int)thread_count) {
            double now = monotonic_seconds();
            uint64_t true_calls = load_total_target_count();

            record_fixed_checkpoint_thresholds(checkpoints,
                                               &checkpoint_count,
                                               (unsigned int)checkpoint_limit,
                                               checkpoint_calls,
                                               &next_checkpoint_calls,
                                               now - start,
                                               true_calls);
            sleep_until(now + 0.001);
        }
    }

    for (long i = 0; i < thread_count; i++) {
        pthread_join(threads[i], NULL);
        total_calls += states[i].calls;
        total_sweeps += states[i].sweeps;
        if (pin_workers && states[i].affinity_error == 0) {
            pinned_worker_count++;
        }
    }
    elapsed = monotonic_seconds() - start;
    if (iterations > 0) {
        record_fixed_checkpoint_thresholds(checkpoints,
                                           &checkpoint_count,
                                           (unsigned int)checkpoint_limit,
                                           checkpoint_calls,
                                           &next_checkpoint_calls,
                                           elapsed,
                                           load_total_target_count());
        record_final_checkpoint(checkpoints,
                                &checkpoint_count,
                                (unsigned int)checkpoint_limit,
                                checkpoint_calls,
                                elapsed,
                                load_total_target_count());
    } else {
        record_timed_checkpoint_sample(checkpoints,
                                       &checkpoint_count,
                                       (unsigned int)checkpoint_limit,
                                       checkpoint_calls,
                                       &next_checkpoint_calls,
                                       elapsed,
                                       load_total_target_count());
    }

    printf("milc_like threads=%ld seconds=%ld elapsed=%.9f calls=%llu "
           "calls_per_sec=%.3f targets=%u phases=%u sweeps=%llu "
           "side_effect=%llu\n",
           thread_count,
           seconds,
           elapsed,
           (unsigned long long)total_calls,
           elapsed > 0.0 ? (double)total_calls / elapsed : 0.0,
           (unsigned int)PEAK_MILC_TARGET_COUNT,
           (unsigned int)PEAK_MILC_PHASE_COUNT,
           (unsigned long long)total_sweeps,
           (unsigned long long)atomic_load_explicit(&side_effect,
                                                    memory_order_relaxed));
    printf("milc_like_affinity enabled=%d allowed_cpus=%u pinned_workers=%u "
           "first_cpu=%d\n",
           pin_workers,
           allowed_cpu_count,
           pinned_worker_count,
           pin_workers ? allowed_cpus[0] : -1);
    print_target_counts();
    print_phase_breadth();
    print_target_phase_calls();
    print_target_thread_info((unsigned int)thread_count);
    printf("milc_like_checkpoints count=%u checkpoint_calls=%llu\n",
           checkpoint_count,
           (unsigned long long)(checkpoint_calls > 0 ? checkpoint_calls : 0));
    for (unsigned int i = 0; i < checkpoint_count; i++) {
        print_checkpoint(i, checkpoints[i].elapsed, checkpoints[i].true_calls);
    }

    free(checkpoints);
    free(threads);
    free(states);
    start_gate_destroy(&gate);
    if (pin_workers && pinned_worker_count != (unsigned int)thread_count) {
        fprintf(stderr,
                "failed to pin all workers: pinned=%u expected=%ld\n",
                pinned_worker_count,
                thread_count);
        return 2;
    }
    return 0;
}
