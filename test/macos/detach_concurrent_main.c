#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#define PEAK_MACOS_WORKER_COUNT 4u
#define PEAK_MACOS_TEST_DURATION_US 4000000u
#define PEAK_MACOS_LOOP_PAUSE_US 250u

unsigned long peak_macos_smoke_target(unsigned long value);

typedef struct {
    unsigned long seed;
    unsigned long calls;
    unsigned long result;
    unsigned long expected;
} PeakMacosWorker;

typedef struct {
    unsigned long calls;
    unsigned long result;
    unsigned long expected;
} PeakMacosCreator;

typedef struct {
    unsigned long value;
    unsigned long result;
} PeakMacosShortThread;

static _Atomic int peak_macos_stop_requested = 0;
static _Atomic int peak_macos_creator_failed = 0;

static unsigned long
peak_macos_expected(unsigned long value)
{
    return value * 3 + 1;
}

static void*
peak_macos_worker_main(void* opaque)
{
    PeakMacosWorker* worker = opaque;
    unsigned long sequence = 0;

    while (atomic_load_explicit(&peak_macos_stop_requested,
                                memory_order_acquire) == 0) {
        unsigned long value = worker->seed + (sequence & 1023u);
        unsigned long expected = peak_macos_expected(value);

        worker->result += peak_macos_smoke_target(value);
        worker->expected += expected;
        worker->calls++;
        sequence++;
        usleep(PEAK_MACOS_LOOP_PAUSE_US);
    }

    return NULL;
}

static void*
peak_macos_short_thread_main(void* opaque)
{
    PeakMacosShortThread* child = opaque;

    child->result = peak_macos_smoke_target(child->value);
    return NULL;
}

static void*
peak_macos_creator_main(void* opaque)
{
    PeakMacosCreator* creator = opaque;
    unsigned long sequence = 0;

    while (atomic_load_explicit(&peak_macos_stop_requested,
                                memory_order_acquire) == 0) {
        PeakMacosShortThread child = {
            .value = 4096u + (sequence & 1023u),
            .result = 0
        };
        pthread_t thread;

        if (pthread_create(&thread,
                           NULL,
                           peak_macos_short_thread_main,
                           &child) != 0 ||
            pthread_join(thread, NULL) != 0) {
            atomic_store_explicit(&peak_macos_creator_failed,
                                  1,
                                  memory_order_release);
            break;
        }

        creator->calls++;
        creator->result += child.result;
        creator->expected += peak_macos_expected(child.value);
        sequence++;
        usleep(PEAK_MACOS_LOOP_PAUSE_US);
    }

    return NULL;
}

int
main(void)
{
    PeakMacosWorker workers[PEAK_MACOS_WORKER_COUNT] = { 0 };
    PeakMacosCreator creator = { 0 };
    pthread_t worker_threads[PEAK_MACOS_WORKER_COUNT];
    pthread_t creator_thread;
    size_t workers_started = 0;
    unsigned long worker_calls = 0;
    unsigned long result = 0;
    unsigned long expected = 0;

    for (size_t i = 0; i < PEAK_MACOS_WORKER_COUNT; i++) {
        workers[i].seed = (unsigned long)i * 2048u;
        if (pthread_create(&worker_threads[i],
                           NULL,
                           peak_macos_worker_main,
                           &workers[i]) != 0) {
            atomic_store_explicit(&peak_macos_stop_requested,
                                  1,
                                  memory_order_release);
            for (size_t j = 0; j < workers_started; j++) {
                pthread_join(worker_threads[j], NULL);
            }
            return 2;
        }
        workers_started++;
    }

    if (pthread_create(&creator_thread,
                       NULL,
                       peak_macos_creator_main,
                       &creator) != 0) {
        atomic_store_explicit(&peak_macos_stop_requested,
                              1,
                              memory_order_release);
        for (size_t i = 0; i < workers_started; i++) {
            pthread_join(worker_threads[i], NULL);
        }
        return 3;
    }

    usleep(PEAK_MACOS_TEST_DURATION_US);
    atomic_store_explicit(&peak_macos_stop_requested,
                          1,
                          memory_order_release);

    if (pthread_join(creator_thread, NULL) != 0) {
        return 4;
    }
    for (size_t i = 0; i < workers_started; i++) {
        if (pthread_join(worker_threads[i], NULL) != 0) {
            return 5;
        }
        worker_calls += workers[i].calls;
        result += workers[i].result;
        expected += workers[i].expected;
    }
    result += creator.result;
    expected += creator.expected;

    printf("macos_concurrent_workers=%lu creator_threads=%lu result=%lu\n",
           worker_calls,
           creator.calls,
           result);

    if (atomic_load_explicit(&peak_macos_creator_failed,
                             memory_order_acquire) != 0 ||
        worker_calls < 1000u || creator.calls < 100u || result != expected) {
        return 1;
    }
    return 0;
}
