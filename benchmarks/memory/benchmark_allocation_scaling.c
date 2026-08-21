#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum {
    MODE_PAIR = 0,
    MODE_REALLOC,
    MODE_MIXED,
    MODE_ALIGNED,
    MODE_POSIX,
} BenchmarkMode;

typedef struct {
    pthread_barrier_t* ready;
    pthread_barrier_t* done;
    size_t iterations;
    BenchmarkMode mode;
    int cpu;
    uintptr_t checksum;
} Worker;

static uint64_t
nanoseconds(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) abort();
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}

static void
pin_worker(int cpu)
{
    cpu_set_t affinity;

    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    if (pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity) != 0) {
        abort();
    }
}

static void*
worker_main(void* opaque)
{
    Worker* worker = opaque;
    void* pointer = NULL;

    pin_worker(worker->cpu);
    if (worker->mode == MODE_REALLOC) {
        pointer = malloc(64);
        if (pointer == NULL) abort();
    }
    pthread_barrier_wait(worker->ready);
    for (size_t i = 0; i < worker->iterations; ++i) {
        if (worker->mode == MODE_REALLOC) {
            size_t size = (i & 1u) == 0 ? 128u : 64u;
            void* resized = realloc(pointer, size);
            if (resized == NULL) abort();
            pointer = resized;
            ((volatile unsigned char*)pointer)[0] = (unsigned char)i;
        } else {
            size_t first_size = 64u + (i & 63u);
            if (worker->mode == MODE_ALIGNED) {
                pointer = aligned_alloc(64u, 128u);
            } else if (worker->mode == MODE_POSIX) {
                if (posix_memalign(&pointer, 64u, 128u) != 0) abort();
            } else {
                pointer = malloc(first_size);
            }
            if (pointer == NULL) abort();
            if (worker->mode == MODE_MIXED) {
                void* resized = realloc(pointer, first_size + 64u);
                if (resized == NULL) abort();
                pointer = resized;
            }
            ((volatile unsigned char*)pointer)[0] = (unsigned char)i;
            worker->checksum ^= (uintptr_t)pointer;
            free(pointer);
            pointer = NULL;
        }
    }
    pthread_barrier_wait(worker->done);
    free(pointer);
    return NULL;
}

static size_t
collect_allowed_cpus(int* cpus, size_t capacity)
{
    cpu_set_t affinity;
    struct {
        int package;
        int core;
    } selected[CPU_SETSIZE];
    size_t count = 0;

    if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0) return 0;
    for (int cpu = 0; cpu < CPU_SETSIZE && count < capacity; ++cpu) {
        char path[128];
        FILE* file;
        int package;
        int core;
        int duplicate = 0;

        if (!CPU_ISSET(cpu, &affinity)) continue;
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
                 cpu);
        file = fopen(path, "r");
        if (file == NULL || fscanf(file, "%d", &package) != 1) {
            if (file != NULL) fclose(file);
            return 0;
        }
        fclose(file);
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
        file = fopen(path, "r");
        if (file == NULL || fscanf(file, "%d", &core) != 1) {
            if (file != NULL) fclose(file);
            return 0;
        }
        fclose(file);
        for (size_t i = 0; i < count; ++i) {
            if (selected[i].package == package && selected[i].core == core) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) continue;
        selected[count].package = package;
        selected[count].core = core;
        cpus[count++] = cpu;
    }
    return count;
}

__attribute__((noinline, noclone, used, externally_visible,
               visibility("default")))
uint64_t
run_allocation_benchmark(size_t thread_count,
                         size_t iterations,
                         BenchmarkMode mode)
{
    pthread_t* threads = calloc(thread_count, sizeof(*threads));
    Worker* workers = calloc(thread_count, sizeof(*workers));
    int* cpus = calloc(thread_count, sizeof(*cpus));
    pthread_barrier_t ready;
    pthread_barrier_t done;
    uint64_t start;
    uint64_t finish;

    if (threads == NULL || workers == NULL || cpus == NULL ||
        collect_allowed_cpus(cpus, thread_count) != thread_count) {
        abort();
    }
    if (pthread_barrier_init(&ready, NULL, (unsigned)thread_count + 1u) != 0 ||
        pthread_barrier_init(&done, NULL, (unsigned)thread_count + 1u) != 0) {
        abort();
    }
    for (size_t i = 0; i < thread_count; ++i) {
        workers[i].ready = &ready;
        workers[i].done = &done;
        workers[i].iterations = iterations;
        workers[i].mode = mode;
        workers[i].cpu = cpus[i];
        if (pthread_create(&threads[i], NULL, worker_main, &workers[i]) != 0) {
            abort();
        }
    }
    pthread_barrier_wait(&ready);
    start = nanoseconds();
    pthread_barrier_wait(&done);
    finish = nanoseconds();
    for (size_t i = 0; i < thread_count; ++i) {
        if (pthread_join(threads[i], NULL) != 0) abort();
    }
    pthread_barrier_destroy(&done);
    pthread_barrier_destroy(&ready);
    free(cpus);
    free(workers);
    free(threads);
    return finish - start;
}

static int
parse_positive_size(const char* text, size_t* out)
{
    char* end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
        parsed > SIZE_MAX) {
        return 0;
    }
    *out = (size_t)parsed;
    return 1;
}

int
main(int argc, char** argv)
{
    size_t threads;
    size_t iterations;
    BenchmarkMode mode;
    uint64_t elapsed;
    uint64_t allocator_calls;

    if (argc != 4 || !parse_positive_size(argv[1], &threads) ||
        !parse_positive_size(argv[2], &iterations)) {
        return 2;
    }
    if (strcmp(argv[3], "pair") == 0) {
        mode = MODE_PAIR;
        allocator_calls = 2;
    } else if (strcmp(argv[3], "realloc") == 0) {
        mode = MODE_REALLOC;
        allocator_calls = 1;
    } else if (strcmp(argv[3], "mixed") == 0) {
        mode = MODE_MIXED;
        allocator_calls = 3;
    } else if (strcmp(argv[3], "aligned") == 0) {
        mode = MODE_ALIGNED;
        allocator_calls = 2;
    } else if (strcmp(argv[3], "posix") == 0) {
        mode = MODE_POSIX;
        allocator_calls = 2;
    } else {
        return 2;
    }

    elapsed = run_allocation_benchmark(threads, iterations, mode);
    allocator_calls *= (uint64_t)threads * (uint64_t)iterations;
    printf("mode=%s threads=%zu physical_cores=%zu iterations=%zu elapsed_ns=%llu "
           "allocator_calls=%llu ns_per_call=%.3f\n",
           argv[3], threads, threads, iterations,
           (unsigned long long)elapsed,
           (unsigned long long)allocator_calls,
           (double)elapsed / (double)allocator_calls);
    return 0;
}
