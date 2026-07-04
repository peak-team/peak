#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "test_detach_many_targets_stress_generated.h"

#define PEAK_STRESS_DEFAULT_HOT_TARGETS 256
#define PEAK_STRESS_DEFAULT_THREADS 64
#define PEAK_STRESS_DEFAULT_SECONDS 20
#define PEAK_STRESS_DEFAULT_BURST 128
#define PEAK_STRESS_MAX_CHURN_BATCH 64
#define PEAK_STRESS_DEFAULT_CHURN_BATCH 64
#define PEAK_STRESS_DEFAULT_CHURN_WORKERS 1
#define PEAK_STRESS_TARGET_WORDS ((PEAK_STRESS_TARGET_COUNT + 63) / 64)

typedef int (*PeakRequestReattachFn)(size_t hook_id);
typedef size_t (*PeakRequestDetachBatchFn)(const size_t* hook_ids,
                                           size_t hook_count);

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    long expected;
    long arrived;
    int open;
} StartGate;

typedef struct {
    long id;
    int target_count;
    int hot_targets;
    int all_target_period;
    int burst;
    uint64_t work_quota;
    int phase_cold_target0_period;
    double start_time;
    long hot_rotation_period_us;
    int active_hot_targets;
    int cold_sweep_period;
    int hot_only_workload;
    StartGate* start_gate;
    uint64_t calls;
    uint64_t same_hot_calls;
    uint64_t mixed_hot_calls;
    uint64_t all_target_calls;
    uint64_t phase_hot_target0_calls;
    uint64_t phase_cold_target0_calls;
    uint64_t rotating_hot_calls;
    uint64_t cold_sweep_calls;
} WorkerState;

typedef struct {
    int worker_index;
    int worker_count;
    int target_count;
    int hot_targets;
    int churn_target_count;
    int batch_size;
    int random_percent;
    int hot_bias_percent;
    long interval_us;
    long jitter_us;
    atomic_int ready;
    atomic_int failed;
    uint64_t detach_requests;
    uint64_t reattach_requests;
} ChurnState;

static atomic_int stop_requested;
static atomic_int cold_phase_active;
static atomic_uint_fast64_t side_effect;
static atomic_uint_fast64_t progress_calls;
static atomic_uint_fast64_t touched_targets[PEAK_STRESS_TARGET_WORDS];
static atomic_uint_fast64_t target_call_counts[PEAK_STRESS_TARGET_COUNT];

static double
monotonic_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void
sleep_us(long usec)
{
    struct timespec ts;

    if (usec <= 0) {
        return;
    }

    ts.tv_sec = usec / 1000000;
    ts.tv_nsec = (usec % 1000000) * 1000;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static long
parse_long_arg(int argc, char** argv, const char* name, long fallback)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            char* end = NULL;
            errno = 0;
            long value = strtol(argv[i + 1], &end, 10);

            if (errno == 0 && end != argv[i + 1] && *end == '\0') {
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

static const char*
parse_string_arg(int argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

static void*
optional_symbol(const char* name)
{
    void* symbol;

    dlerror();
    symbol = dlsym(RTLD_DEFAULT, name);
    if (dlerror() != NULL) {
        return NULL;
    }
    return symbol;
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

static uint32_t
next_random(uint32_t* state)
{
    uint32_t x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void
call_target(int index, uint64_t value)
{
    size_t word = (size_t)index / 64u;
    uint_fast64_t mask = UINT64_C(1) << ((unsigned)index % 64u);

    atomic_fetch_or_explicit(&touched_targets[word], mask, memory_order_relaxed);
    atomic_fetch_add_explicit(&target_call_counts[index],
                              1,
                              memory_order_relaxed);
    stress_targets[index](value);
}

static void
clear_target_coverage(void)
{
    for (size_t i = 0; i < PEAK_STRESS_TARGET_WORDS; i++) {
        atomic_store_explicit(&touched_targets[i], 0, memory_order_relaxed);
    }
    for (int i = 0; i < PEAK_STRESS_TARGET_COUNT; i++) {
        atomic_store_explicit(&target_call_counts[i], 0, memory_order_relaxed);
    }
}

static int
write_target_counts(const char* path, int target_count)
{
    FILE* fp;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }

    fp = fopen(path, "w");
    if (fp == NULL) {
        perror("fopen counts path");
        return -1;
    }

    fprintf(fp, "index,function,actual_calls\n");
    for (int i = 0; i < target_count; i++) {
        fprintf(fp,
                "%d,peak_detach_stress_target_%d,%" PRIuFAST64 "\n",
                i,
                i,
                atomic_load_explicit(&target_call_counts[i],
                                     memory_order_relaxed));
    }

    if (fclose(fp) != 0) {
        perror("fclose counts path");
        return -1;
    }
    return 0;
}

static int
write_target_count_sample(FILE* fp,
                          double elapsed_s,
                          double monotonic_s,
                          int target_count)
{
    if (fp == NULL) {
        return 0;
    }

    for (int i = 0; i < target_count; i++) {
        if (fprintf(fp,
                    "%.6f,%.9f,%d,peak_detach_stress_target_%d,%" PRIuFAST64 "\n",
                    elapsed_s,
                    monotonic_s,
                    i,
                    i,
                    atomic_load_explicit(&target_call_counts[i],
                                         memory_order_relaxed)) < 0) {
            return -1;
        }
    }
    return fflush(fp) == 0 ? 0 : -1;
}

static uint_fast64_t
target_word_mask_for_count(int target_count, size_t word)
{
    int remaining = target_count - (int)(word * 64u);

    if (remaining >= 64) {
        return UINT64_MAX;
    }
    if (remaining <= 0) {
        return 0;
    }
    return (UINT64_C(1) << (unsigned)remaining) - UINT64_C(1);
}

static int
count_touched_range(int start, int end)
{
    int count = 0;

    for (int i = start; i < end; i++) {
        size_t word = (size_t)i / 64u;
        uint_fast64_t mask = UINT64_C(1) << ((unsigned)i % 64u);
        uint_fast64_t value =
            atomic_load_explicit(&touched_targets[word], memory_order_relaxed);

        if ((value & mask) != 0) {
            count++;
        }
    }
    return count;
}

static int
count_touched_targets(int target_count)
{
    int count = 0;

    for (size_t i = 0; i < PEAK_STRESS_TARGET_WORDS; i++) {
        uint_fast64_t mask = target_word_mask_for_count(target_count, i);
        uint_fast64_t value =
            atomic_load_explicit(&touched_targets[i], memory_order_relaxed) & mask;

        count += __builtin_popcountll((unsigned long long)value);
    }
    return count;
}

static void*
worker_main(void* arg)
{
    WorkerState* state = (WorkerState*)arg;
    uint64_t local_calls = 0;
    uint64_t same_hot_calls = 0;
    uint64_t mixed_hot_calls = 0;
    uint64_t all_target_calls = 0;
    uint32_t random_state = 0x9e3779b9u ^ (uint32_t)(state->id * 2654435761u);
    int hot_targets = state->hot_targets;
    int target_count = state->target_count;
    int all_target_period = state->all_target_period;
    int pattern = (int)(state->id % 5);
    int phase_cold_target0_period = state->phase_cold_target0_period;
    int rotating_mode =
        state->hot_rotation_period_us > 0 && state->active_hot_targets > 0;
    uint64_t phase_index = 0;
    int hot_window_start = 0;
    uint64_t phase_hot_target0_calls = 0;
    uint64_t phase_cold_target0_calls = 0;
    uint64_t rotating_hot_calls = 0;
    uint64_t cold_sweep_calls = 0;

    start_gate_wait(state->start_gate);

    while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
        if (rotating_mode) {
            double elapsed_s = monotonic_seconds() - state->start_time;
            uint64_t elapsed_us =
                elapsed_s > 0.0 ? (uint64_t)(elapsed_s * 1000000.0) : 0;

            phase_index = elapsed_us / (uint64_t)state->hot_rotation_period_us;
            hot_window_start =
                (int)((phase_index * (uint64_t)state->active_hot_targets) %
                      (uint64_t)hot_targets);
        }
        for (int burst = 0; burst < state->burst; burst++) {
            int target_index;
            int cold_phase =
                atomic_load_explicit(&cold_phase_active, memory_order_relaxed);

            if (rotating_mode &&
                state->cold_sweep_period > 0 &&
                local_calls % (uint64_t)state->cold_sweep_period == 0) {
                uint64_t sweep_index =
                    local_calls / (uint64_t)state->cold_sweep_period;

                target_index =
                    (int)((sweep_index + (uint64_t)state->id * 131u) %
                          (uint64_t)target_count);
                cold_sweep_calls++;
            } else if (rotating_mode) {
                int offset;

                switch (pattern) {
                    case 0:
                    case 1:
                        offset = (int)((local_calls + (uint64_t)state->id) %
                                       (uint64_t)state->active_hot_targets);
                        break;
                    case 2:
                    case 3:
                        offset = (int)(next_random(&random_state) %
                                       (uint32_t)state->active_hot_targets);
                        break;
                    default:
                        offset = (int)((local_calls * 17u +
                                        (uint64_t)state->id * 31u) %
                                       (uint64_t)state->active_hot_targets);
                        break;
                }
                target_index = (hot_window_start + offset) % hot_targets;
                rotating_hot_calls++;
            } else if (cold_phase && phase_cold_target0_period > 0 &&
                local_calls % (uint64_t)phase_cold_target0_period == 0) {
                target_index = 0;
                phase_cold_target0_calls++;
            } else if (all_target_period > 0 &&
                local_calls % (uint64_t)all_target_period == 0) {
                uint64_t all_call_index =
                    local_calls / (uint64_t)all_target_period;

                target_index =
                    (int)((all_call_index + (uint64_t)state->id * 131u) %
                          (uint64_t)target_count);
                if (cold_phase && target_index == 0 && target_count > 1) {
                    target_index = 1;
                }
                if (target_index < hot_targets) {
                    mixed_hot_calls++;
                } else {
                    all_target_calls++;
                }
            } else {
                switch (pattern) {
                    case 0:
                        if (cold_phase && hot_targets > 1) {
                            target_index = 1 +
                                           (int)(local_calls %
                                                 (uint64_t)(hot_targets - 1));
                            mixed_hot_calls++;
                        } else {
                            target_index = 0;
                            same_hot_calls++;
                        }
                        break;
                    case 1:
                        if (cold_phase && hot_targets > 1) {
                            target_index =
                                1 + (int)((local_calls + (uint64_t)state->id) %
                                          (uint64_t)(hot_targets - 1));
                        } else {
                            target_index =
                                (int)((local_calls + (uint64_t)state->id) %
                                      (uint64_t)hot_targets);
                        }
                        mixed_hot_calls++;
                        break;
                    case 2:
                        if (cold_phase && hot_targets > 1) {
                            target_index =
                                1 + (int)(next_random(&random_state) %
                                          (uint32_t)(hot_targets - 1));
                        } else {
                            target_index = (int)(next_random(&random_state) %
                                                 (uint32_t)hot_targets);
                        }
                        mixed_hot_calls++;
                        break;
                    case 3:
                        if (cold_phase && hot_targets > 1) {
                            target_index =
                                1 + (int)(next_random(&random_state) %
                                          (uint32_t)(hot_targets - 1));
                        } else {
                            target_index = (int)(next_random(&random_state) %
                                                 (uint32_t)hot_targets);
                        }
                        mixed_hot_calls++;
                        break;
                    default:
                        target_index = (int)(next_random(&random_state) %
                                             (uint32_t)(state->hot_only_workload
                                                           ? hot_targets
                                                           : target_count));
                        if (cold_phase && target_index == 0 && target_count > 1) {
                            target_index = 1;
                        }
                        if (target_index < hot_targets) {
                            mixed_hot_calls++;
                        } else {
                            all_target_calls++;
                        }
                        break;
                }
            }
            if (target_index == 0 && !cold_phase) {
                phase_hot_target0_calls++;
            }
            call_target(target_index, local_calls ^ (uint64_t)state->id);
            local_calls++;
        }
        if (state->work_quota > 0) {
            uint64_t progress =
                atomic_fetch_add_explicit(&progress_calls,
                                          (uint64_t)state->burst,
                                          memory_order_relaxed) +
                (uint64_t)state->burst;

            if (progress >= state->work_quota) {
                atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
                break;
            }
        }
    }

    state->calls = local_calls;
    state->same_hot_calls = same_hot_calls;
    state->mixed_hot_calls = mixed_hot_calls;
    state->all_target_calls = all_target_calls;
    state->phase_hot_target0_calls = phase_hot_target0_calls;
    state->phase_cold_target0_calls = phase_cold_target0_calls;
    state->rotating_hot_calls = rotating_hot_calls;
    state->cold_sweep_calls = cold_sweep_calls;
    atomic_fetch_add_explicit(&side_effect,
                              local_calls ^ ((uint64_t)state->id << 32),
                              memory_order_relaxed);
    return NULL;
}

static int
choose_random_churn_target(ChurnState* state, uint32_t* random_state)
{
    uint32_t bias = (uint32_t)state->hot_bias_percent;

    if (state->hot_targets > 0 &&
        bias > 0 &&
        (next_random(random_state) % 100u) < bias) {
        return (int)(next_random(random_state) % (uint32_t)state->hot_targets);
    }
    return (int)(next_random(random_state) %
                 (uint32_t)state->churn_target_count);
}

static void
churn_sleep(ChurnState* state, uint32_t* random_state)
{
    long usec = state->interval_us;

    if (state->jitter_us > 0) {
        usec += (long)(next_random(random_state) %
                       (uint32_t)(state->jitter_us + 1));
    }
    sleep_us(usec);
}

static int
wait_for_peak_hooks(size_t expected, double timeout_s)
{
    size_t* hook_count = (size_t*)optional_symbol("peak_hook_address_count");
    double deadline = monotonic_seconds() + timeout_s;

    if (hook_count == NULL) {
        return -1;
    }

    while (monotonic_seconds() < deadline) {
        if (*hook_count >= expected) {
            return 0;
        }
        sleep_us(20000);
    }
    return -1;
}

static void*
churn_main(void* arg)
{
    ChurnState* state = (ChurnState*)arg;
    PeakRequestDetachBatchFn request_detach_batch =
        (PeakRequestDetachBatchFn)optional_symbol(
            "peak_general_listener_request_detach_batch");
    PeakRequestReattachFn request_reattach =
        (PeakRequestReattachFn)optional_symbol(
            "peak_general_listener_request_reattach");
    size_t* hook_ids = NULL;
    int cursor = state->worker_index * state->batch_size;
    uint32_t random_state =
        0xa511e9b3u ^ (uint32_t)(state->worker_index * 1103515245u);

    if (request_detach_batch == NULL || request_reattach == NULL) {
        fprintf(stderr,
                "controller churn requested but PEAK request symbols are unavailable\n");
        atomic_store_explicit(&state->failed, 1, memory_order_release);
        return NULL;
    }
    if (wait_for_peak_hooks((size_t)state->target_count, 30.0) != 0) {
        fprintf(stderr,
                "controller churn could not observe %d registered hooks\n",
                state->target_count);
        atomic_store_explicit(&state->failed, 1, memory_order_release);
        return NULL;
    }

    hook_ids = calloc((size_t)state->batch_size, sizeof(*hook_ids));
    if (hook_ids == NULL) {
        perror("calloc hook_ids");
        atomic_store_explicit(&state->failed, 1, memory_order_release);
        return NULL;
    }
    atomic_store_explicit(&state->ready, 1, memory_order_release);

    while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
        for (int i = 0; i < state->batch_size; i++) {
            int hook_id = (cursor + i) % state->churn_target_count;

            if (state->random_percent > 0 &&
                (next_random(&random_state) % 100u) <
                    (uint32_t)state->random_percent) {
                hook_id = choose_random_churn_target(state, &random_state);
            }
            hook_ids[i] = (size_t)hook_id;
        }

        size_t accepted = request_detach_batch(hook_ids, (size_t)state->batch_size);

        state->detach_requests += accepted;
        churn_sleep(state, &random_state);

        for (int i = 0; i < state->batch_size; i++) {
            if (request_reattach(hook_ids[i])) {
                state->reattach_requests++;
            }
        }
        cursor += state->batch_size * state->worker_count;
        cursor %= state->churn_target_count;
        churn_sleep(state, &random_state);
    }

    free(hook_ids);
    return NULL;
}

static void
call_cold_targets_once(int hot_targets, int target_count)
{
    for (int i = hot_targets; i < target_count; i++) {
        call_target(i, (uint64_t)i);
    }
}

static int
validate_args(long threads,
              long seconds,
              long target_count,
              long hot_targets,
              long churn_target_count,
              long burst,
              int controller_churn,
              long churn_batch_size,
              long churn_workers,
              long churn_random_percent,
              long churn_hot_bias_percent,
              long churn_jitter_us,
              long work_quota,
              long hot_phase_seconds,
              long phase_cold_target0_period,
              long hot_rotation_period_us,
              long active_hot_targets,
              long cold_sweep_period)
{
    if (threads <= 0 || threads > 4096) {
        fprintf(stderr, "invalid --threads %ld\n", threads);
        return -1;
    }
    if (seconds <= 0 || seconds > 3600) {
        fprintf(stderr, "invalid --seconds %ld\n", seconds);
        return -1;
    }
    if (target_count <= 0 || target_count > PEAK_STRESS_TARGET_COUNT) {
        fprintf(stderr,
                "invalid --targets %ld, max is %d\n",
                target_count,
                PEAK_STRESS_TARGET_COUNT);
        return -1;
    }
    if (hot_targets <= 0 || hot_targets > target_count) {
        fprintf(stderr, "invalid --hot-targets %ld\n", hot_targets);
        return -1;
    }
    if (controller_churn &&
        (churn_target_count <= 0 || churn_target_count > target_count)) {
        fprintf(stderr, "invalid --churn-targets %ld\n", churn_target_count);
        return -1;
    }
    if (controller_churn &&
        (churn_batch_size <= 0 ||
         churn_batch_size > PEAK_STRESS_MAX_CHURN_BATCH ||
         churn_batch_size > churn_target_count)) {
        fprintf(stderr,
                "invalid --churn-batch-size %ld, max is %d and it cannot exceed churn targets\n",
                churn_batch_size,
                PEAK_STRESS_MAX_CHURN_BATCH);
        return -1;
    }
    if (controller_churn && (churn_workers <= 0 || churn_workers > 64)) {
        fprintf(stderr, "invalid --churn-workers %ld\n", churn_workers);
        return -1;
    }
    if (controller_churn &&
        (churn_random_percent < 0 || churn_random_percent > 100)) {
        fprintf(stderr, "invalid --churn-random-percent %ld\n",
                churn_random_percent);
        return -1;
    }
    if (controller_churn &&
        (churn_hot_bias_percent < 0 || churn_hot_bias_percent > 100)) {
        fprintf(stderr, "invalid --churn-hot-bias-percent %ld\n",
                churn_hot_bias_percent);
        return -1;
    }
    if (controller_churn && (churn_jitter_us < 0 || churn_jitter_us > 1000000)) {
        fprintf(stderr, "invalid --churn-jitter-us %ld\n", churn_jitter_us);
        return -1;
    }
    if (work_quota < 0) {
        fprintf(stderr, "invalid --work-quota %ld\n", work_quota);
        return -1;
    }
    if (hot_phase_seconds < 0 || hot_phase_seconds > seconds) {
        fprintf(stderr,
                "invalid --hot-phase-seconds %ld, must be in [0, seconds]\n",
                hot_phase_seconds);
        return -1;
    }
    if (phase_cold_target0_period < 0) {
        fprintf(stderr,
                "invalid --phase-cold-target0-period %ld\n",
                phase_cold_target0_period);
        return -1;
    }
    if (hot_rotation_period_us < 0) {
        fprintf(stderr,
                "invalid --hot-rotation-period-us %ld\n",
                hot_rotation_period_us);
        return -1;
    }
    if (hot_rotation_period_us > 0 &&
        (active_hot_targets <= 0 || active_hot_targets > hot_targets)) {
        fprintf(stderr,
                "invalid --active-hot-targets %ld, must be in [1, hot-targets]\n",
                active_hot_targets);
        return -1;
    }
    if (cold_sweep_period < 0) {
        fprintf(stderr,
                "invalid --cold-sweep-period %ld\n",
                cold_sweep_period);
        return -1;
    }
    if (burst <= 0 || burst > 1000000) {
        fprintf(stderr, "invalid --burst %ld\n", burst);
        return -1;
    }
    return 0;
}

int
main(int argc, char** argv)
{
    long threads = parse_long_arg(argc,
                                  argv,
                                  "--threads",
                                  PEAK_STRESS_DEFAULT_THREADS);
    long seconds = parse_long_arg(argc,
                                  argv,
                                  "--seconds",
                                  PEAK_STRESS_DEFAULT_SECONDS);
    long target_count = parse_long_arg(argc,
                                       argv,
                                       "--targets",
                                       PEAK_STRESS_TARGET_COUNT);
    long hot_targets = parse_long_arg(argc,
                                      argv,
                                      "--hot-targets",
                                      PEAK_STRESS_DEFAULT_HOT_TARGETS);
    long burst = parse_long_arg(argc,
                                argv,
                                "--burst",
                                PEAK_STRESS_DEFAULT_BURST);
    long churn_interval_us = parse_long_arg(argc,
                                            argv,
                                            "--churn-interval-us",
                                            10000);
    long churn_target_count = parse_long_arg(argc,
                                             argv,
                                             "--churn-targets",
                                             hot_targets);
    long churn_batch_size = parse_long_arg(argc,
                                           argv,
                                           "--churn-batch-size",
                                           PEAK_STRESS_DEFAULT_CHURN_BATCH);
    long churn_workers = parse_long_arg(argc,
                                        argv,
                                        "--churn-workers",
                                        PEAK_STRESS_DEFAULT_CHURN_WORKERS);
    long churn_random_percent = parse_long_arg(argc,
                                               argv,
                                               "--churn-random-percent",
                                               0);
    long churn_hot_bias_percent = parse_long_arg(argc,
                                                 argv,
                                                 "--churn-hot-bias-percent",
                                                 0);
    long churn_jitter_us = parse_long_arg(argc,
                                          argv,
                                          "--churn-jitter-us",
                                          0);
    long all_target_period = parse_long_arg(argc,
                                            argv,
                                            "--all-target-period",
                                            16);
    long work_quota = parse_long_arg(argc, argv, "--work-quota", 0);
    long hot_phase_seconds = parse_long_arg(argc,
                                            argv,
                                            "--hot-phase-seconds",
                                            0);
    long phase_cold_target0_period =
        parse_long_arg(argc, argv, "--phase-cold-target0-period", 0);
    long hot_rotation_period_us =
        parse_long_arg(argc, argv, "--hot-rotation-period-us", 0);
    long active_hot_targets =
        parse_long_arg(argc, argv, "--active-hot-targets", hot_targets);
    long cold_sweep_period =
        parse_long_arg(argc, argv, "--cold-sweep-period", 0);
    const char* counts_path = parse_string_arg(argc, argv, "--counts-path");
    const char* sample_counts_path =
        parse_string_arg(argc, argv, "--sample-counts-path");
    long sample_counts_interval_ms =
        parse_long_arg(argc, argv, "--sample-counts-interval-ms", 1000);
    int call_cold_once = has_flag_arg(argc, argv, "--call-cold-targets-once");
    int hot_only_workload = has_flag_arg(argc, argv, "--hot-only-workload");
    int controller_churn = has_flag_arg(argc, argv, "--controller-churn");
    pthread_t* tids = NULL;
    WorkerState* states = NULL;
    pthread_t* churn_tids = NULL;
    ChurnState* churn_states = NULL;
    long churn_started = 0;
    int all_churn_ready = 1;
    int any_churn_failed = 0;
    uint64_t detach_requests = 0;
    uint64_t reattach_requests = 0;
    StartGate gate;
    double start_time;
    double elapsed;
    uint64_t total_calls = 0;
    uint64_t same_hot_calls = 0;
    uint64_t mixed_hot_calls = 0;
    uint64_t all_target_calls = 0;
    uint64_t phase_hot_target0_calls = 0;
    uint64_t phase_cold_target0_calls = 0;
    uint64_t rotating_hot_calls = 0;
    uint64_t cold_sweep_calls = 0;
    long created_threads = 0;
    int distinct_called_targets;
    int distinct_called_hot_targets;
    int distinct_called_cold_targets;
    FILE* sample_counts_fp = NULL;
    double next_sample_time_s = 0.0;

    if (validate_args(threads,
                      seconds,
                      target_count,
                      hot_targets,
                      churn_target_count,
                      burst,
                      controller_churn,
                      churn_batch_size,
                      churn_workers,
                      churn_random_percent,
                      churn_hot_bias_percent,
                      churn_jitter_us,
                      work_quota,
                      hot_phase_seconds,
                      phase_cold_target0_period,
                      hot_rotation_period_us,
                      active_hot_targets,
                      cold_sweep_period) != 0) {
        return 2;
    }

    tids = calloc((size_t)threads, sizeof(*tids));
    states = calloc((size_t)threads, sizeof(*states));
    if (controller_churn) {
        churn_tids = calloc((size_t)churn_workers, sizeof(*churn_tids));
        churn_states = calloc((size_t)churn_workers, sizeof(*churn_states));
    }
    if (tids == NULL || states == NULL ||
        (controller_churn && (churn_tids == NULL || churn_states == NULL))) {
        perror("calloc");
        free(tids);
        free(states);
        free(churn_tids);
        free(churn_states);
        return 2;
    }
    if (start_gate_init(&gate, threads + 1) != 0) {
        perror("start_gate_init");
        free(tids);
        free(states);
        free(churn_tids);
        free(churn_states);
        return 2;
    }

    atomic_store_explicit(&stop_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&cold_phase_active, 0, memory_order_relaxed);
    atomic_store_explicit(&side_effect, 0, memory_order_relaxed);
    atomic_store_explicit(&progress_calls, 0, memory_order_relaxed);
    clear_target_coverage();

    if (call_cold_once) {
        call_cold_targets_once((int)hot_targets, (int)target_count);
    }

    for (long i = 0; i < threads; i++) {
        states[i].id = i;
        states[i].target_count = (int)target_count;
        states[i].hot_targets = (int)hot_targets;
        states[i].all_target_period = (int)all_target_period;
        states[i].burst = (int)burst;
        states[i].work_quota = (uint64_t)work_quota;
        states[i].phase_cold_target0_period =
            (int)phase_cold_target0_period;
        states[i].hot_rotation_period_us = hot_rotation_period_us;
        states[i].active_hot_targets = (int)active_hot_targets;
        states[i].cold_sweep_period = (int)cold_sweep_period;
        states[i].hot_only_workload = hot_only_workload;
        states[i].start_gate = &gate;
        if (pthread_create(&tids[i], NULL, worker_main, &states[i]) != 0) {
            perror("pthread_create worker");
            atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
            start_gate_abort(&gate);
            for (long j = 0; j < created_threads; j++) {
                pthread_join(tids[j], NULL);
            }
            start_gate_destroy(&gate);
            free(tids);
            free(states);
            free(churn_tids);
            free(churn_states);
            return 2;
        }
        created_threads++;
    }

    start_time = monotonic_seconds();
    for (long i = 0; i < created_threads; i++) {
        states[i].start_time = start_time;
    }
    start_gate_wait(&gate);

    if (sample_counts_path != NULL &&
        sample_counts_path[0] != '\0' &&
        sample_counts_interval_ms > 0) {
        sample_counts_fp = fopen(sample_counts_path, "w");
        if (sample_counts_fp == NULL) {
            perror("fopen sample counts path");
            atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
        } else {
            fprintf(sample_counts_fp,
                    "elapsed_s,monotonic_s,index,function,actual_calls\n");
            if (write_target_count_sample(sample_counts_fp,
                                          0.0,
                                          monotonic_seconds(),
                                          (int)hot_targets) != 0) {
                perror("write sample counts");
                atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
            }
            next_sample_time_s = (double)sample_counts_interval_ms / 1000.0;
        }
    }

    if (controller_churn) {
        for (long i = 0; i < churn_workers; i++) {
            ChurnState* churn_state = &churn_states[i];

            memset(churn_state, 0, sizeof(*churn_state));
            churn_state->worker_index = (int)i;
            churn_state->worker_count = (int)churn_workers;
            churn_state->target_count = (int)target_count;
            churn_state->hot_targets = (int)hot_targets;
            churn_state->churn_target_count = (int)churn_target_count;
            churn_state->batch_size = (int)churn_batch_size;
            churn_state->random_percent = (int)churn_random_percent;
            churn_state->hot_bias_percent = (int)churn_hot_bias_percent;
            churn_state->interval_us = churn_interval_us;
            churn_state->jitter_us = churn_jitter_us;
            atomic_init(&churn_state->ready, 0);
            atomic_init(&churn_state->failed, 0);
            if (pthread_create(&churn_tids[i], NULL, churn_main, churn_state) != 0) {
                perror("pthread_create churn");
                atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
                for (long j = 0; j < churn_started; j++) {
                    pthread_join(churn_tids[j], NULL);
                }
                for (long j = 0; j < created_threads; j++) {
                    pthread_join(tids[j], NULL);
                }
                start_gate_destroy(&gate);
                free(tids);
                free(states);
                free(churn_tids);
                free(churn_states);
                return 2;
            }
            churn_started++;
        }
    }

    while (monotonic_seconds() - start_time < (double)seconds &&
           !atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
        double loop_now = monotonic_seconds();
        double loop_elapsed = loop_now - start_time;

        if (hot_phase_seconds > 0 &&
            loop_elapsed >= (double)hot_phase_seconds) {
            atomic_store_explicit(&cold_phase_active, 1, memory_order_relaxed);
        }
        if (sample_counts_fp != NULL &&
            sample_counts_interval_ms > 0 &&
            loop_elapsed >= next_sample_time_s) {
            if (write_target_count_sample(sample_counts_fp,
                                          loop_elapsed,
                                          loop_now,
                                          (int)hot_targets) != 0) {
                perror("write sample counts");
                atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
                break;
            }
            do {
                next_sample_time_s +=
                    (double)sample_counts_interval_ms / 1000.0;
            } while (loop_elapsed >= next_sample_time_s);
        }
        long sleep_interval_us = 100000;
        if (sample_counts_fp != NULL && sample_counts_interval_ms > 0) {
            double until_next_sample_s = next_sample_time_s - loop_elapsed;
            long sample_sleep_us =
                (long)(until_next_sample_s * 1000000.0 + 0.5);

            if (sample_sleep_us < 1000) {
                sample_sleep_us = 1000;
            }
            if (sample_sleep_us < sleep_interval_us) {
                sleep_interval_us = sample_sleep_us;
            }
        }
        sleep_us(sleep_interval_us);
    }

    atomic_store_explicit(&stop_requested, 1, memory_order_relaxed);
    for (long i = 0; i < created_threads; i++) {
        pthread_join(tids[i], NULL);
        total_calls += states[i].calls;
        same_hot_calls += states[i].same_hot_calls;
        mixed_hot_calls += states[i].mixed_hot_calls;
        all_target_calls += states[i].all_target_calls;
        phase_hot_target0_calls += states[i].phase_hot_target0_calls;
        phase_cold_target0_calls += states[i].phase_cold_target0_calls;
        rotating_hot_calls += states[i].rotating_hot_calls;
        cold_sweep_calls += states[i].cold_sweep_calls;
    }
    for (long i = 0; i < churn_started; i++) {
        ChurnState* churn_state = &churn_states[i];

        pthread_join(churn_tids[i], NULL);
        if (!atomic_load_explicit(&churn_state->ready, memory_order_acquire)) {
            all_churn_ready = 0;
        }
        if (atomic_load_explicit(&churn_state->failed, memory_order_acquire)) {
            any_churn_failed = 1;
        }
        detach_requests += churn_state->detach_requests;
        reattach_requests += churn_state->reattach_requests;
    }
    elapsed = monotonic_seconds() - start_time;
    if (sample_counts_fp != NULL) {
        double final_now = monotonic_seconds();
        if (write_target_count_sample(sample_counts_fp,
                                      elapsed,
                                      final_now,
                                      (int)hot_targets) != 0) {
            perror("write final sample counts");
            fclose(sample_counts_fp);
            return 2;
        }
        if (fclose(sample_counts_fp) != 0) {
            perror("fclose sample counts path");
            return 2;
        }
        sample_counts_fp = NULL;
    }

    start_gate_destroy(&gate);
    free(tids);
    free(states);
    free(churn_tids);
    free(churn_states);

    if (total_calls == 0) {
        fprintf(stderr, "many-target stress completed with zero calls\n");
        return 2;
    }
    if (controller_churn &&
        (!all_churn_ready || any_churn_failed ||
         detach_requests == 0 ||
         reattach_requests == 0)) {
        fprintf(stderr,
                "controller churn did not exercise detach/reattach requests "
                "(ready=%d failed=%d detach=%" PRIu64 " reattach=%" PRIu64 ")\n",
                all_churn_ready,
                any_churn_failed,
                detach_requests,
                reattach_requests);
        return 2;
    }
    distinct_called_targets = count_touched_targets((int)target_count);
    distinct_called_hot_targets = count_touched_range(0, (int)hot_targets);
    distinct_called_cold_targets =
        count_touched_range((int)hot_targets, (int)target_count);

    if (write_target_counts(counts_path, (int)target_count) != 0) {
        return 2;
    }

    printf("many_targets_stress_ok threads=%ld targets=%ld hot_targets=%ld "
           "seconds=%ld elapsed=%.6f total_calls=%" PRIu64
           " calls_per_sec=%.3f same_hot_calls=%" PRIu64
           " mixed_hot_calls=%" PRIu64 " all_target_calls=%" PRIu64
           " phase_hot_target0_calls=%" PRIu64
           " phase_cold_target0_calls=%" PRIu64
           " rotating_hot_calls=%" PRIu64
           " cold_sweep_calls=%" PRIu64
           " distinct_called_targets=%d"
           " distinct_called_hot_targets=%d"
           " distinct_called_cold_targets=%d"
           " side_effect=%" PRIuFAST64
           " controller_churn=%d churn_workers=%ld churn_batch_size=%ld"
           " churn_targets=%ld all_target_period=%ld"
           " churn_random_percent=%ld churn_hot_bias_percent=%ld"
           " churn_jitter_us=%ld"
           " hot_rotation_period_us=%ld active_hot_targets=%ld"
           " cold_sweep_period=%ld"
           " churn_ready=%d churn_failed=%d"
           " detach_requests=%" PRIu64
           " reattach_requests=%" PRIu64
           " work_quota=%ld quota_completed=%d\n",
           threads,
           target_count,
           hot_targets,
           seconds,
           elapsed,
           total_calls,
           (double)total_calls / elapsed,
           same_hot_calls,
           mixed_hot_calls,
           all_target_calls,
           phase_hot_target0_calls,
           phase_cold_target0_calls,
           rotating_hot_calls,
           cold_sweep_calls,
           distinct_called_targets,
           distinct_called_hot_targets,
           distinct_called_cold_targets,
           atomic_load_explicit(&side_effect, memory_order_relaxed),
           controller_churn,
           churn_workers,
           churn_batch_size,
           churn_target_count,
           all_target_period,
           churn_random_percent,
           churn_hot_bias_percent,
           churn_jitter_us,
           hot_rotation_period_us,
           active_hot_targets,
           cold_sweep_period,
           all_churn_ready,
           any_churn_failed,
           detach_requests,
           reattach_requests,
           work_quota,
           work_quota <= 0 || total_calls >= (uint64_t)work_quota);
    return 0;
}
