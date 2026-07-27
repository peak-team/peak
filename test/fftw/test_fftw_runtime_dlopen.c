#define _GNU_SOURCE
#define PEAK_ENABLE_TEST_HOOKS 1
#include "dlopen_interceptor.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void* (*fftw_malloc_fn)(size_t size);
typedef void (*fftw_free_fn)(void* pointer);
typedef void (*plain_target_fn)(void);
typedef void (*set_manual_drain_fn)(gboolean enabled);
typedef void (*plain_hook_fn)(void);
typedef void (*get_diagnostics_fn)(
    PeakDlopenDynamicAttachDiagnostics* diagnostics);

typedef struct {
    const char* provider;
    unsigned int iterations;
    _Atomic int* failures;
} DlopenStressArgs;

static void*
run_dlopen_stress(void* data)
{
    DlopenStressArgs* args = data;

    for (unsigned int i = 0; i < args->iterations; i++) {
        void* handle = dlopen(args->provider, RTLD_LAZY | RTLD_LOCAL);
        if (handle == NULL || dlclose(handle) != 0) {
            atomic_fetch_add_explicit(args->failures, 1, memory_order_relaxed);
            break;
        }
    }
    return NULL;
}

static void
load_function(void* handle,
              const char* name,
              void* function_pointer,
              size_t function_pointer_size)
{
    dlerror();
    void* address = dlsym(handle, name);
    const char* error = dlerror();

    if (error != NULL || address == NULL ||
        function_pointer_size != sizeof(address)) {
        fprintf(stderr,
                "failed to resolve %s: %s\n",
                name,
                error != NULL ? error : "invalid function address");
        exit(EXIT_FAILURE);
    }
    memcpy(function_pointer, &address, sizeof(address));
}

static int
call_fftw_pair(fftw_malloc_fn fftw_malloc, fftw_free_fn fftw_free)
{
    void* allocation = fftw_malloc(64);

    if (allocation == NULL) {
        fputs("fftw_malloc failed\n", stderr);
        return 1;
    }
    fftw_free(allocation);
    return 0;
}

int
main(int argc, char** argv)
{
    fftw_malloc_fn fftw_malloc;
    fftw_free_fn fftw_free;
    plain_target_fn non_fftw_target = NULL;
    set_manual_drain_fn set_manual_drain;
    plain_hook_fn explicit_drain;
    get_diagnostics_fn get_diagnostics;
    PeakDlopenDynamicAttachDiagnostics before = { 0 };
    PeakDlopenDynamicAttachDiagnostics after = { 0 };
    const char* mode = argc >= 3 ? argv[2] : "default";
    int mode_has_argument;

    mode_has_argument = strcmp(mode, "probe") == 0 ||
                        strcmp(mode, "single") == 0 ||
                        strcmp(mode, "extension") == 0;
    if ((argc != 2 && argc != 3 && argc != 4) ||
        (strcmp(mode, "default") != 0 && strcmp(mode, "mixed") != 0 &&
         strcmp(mode, "retry") != 0 && strcmp(mode, "fast") != 0 &&
         strcmp(mode, "retry-fast") != 0 &&
         strcmp(mode, "close") != 0 && strcmp(mode, "stress") != 0 &&
         strcmp(mode, "tail") != 0 &&
         strcmp(mode, "probe") != 0 && strcmp(mode, "single") != 0 &&
         strcmp(mode, "extension") != 0) ||
        mode_has_argument != (argc == 4)) {
        fprintf(stderr,
                "usage: %s /path/to/provider [default|mixed|retry|retry-fast|fast|close|stress|tail|probe /path/to/unrelated|single symbol|extension /path/to/extension]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    load_function(RTLD_DEFAULT,
                  "dlopen_interceptor_test_set_manual_drain",
                  &set_manual_drain,
                  sizeof(set_manual_drain));
    load_function(RTLD_DEFAULT,
                  "dlopen_interceptor_test_drain_dynamic_attach_queue",
                  &explicit_drain,
                  sizeof(explicit_drain));
    load_function(RTLD_DEFAULT,
                  "dlopen_interceptor_get_dynamic_attach_diagnostics",
                  &get_diagnostics,
                  sizeof(get_diagnostics));

    /* Make deferred controller attachment deterministic before target calls. */
    set_manual_drain(1);
    get_diagnostics(&before);

    if (strcmp(mode, "close") == 0) {
        void* close_handle = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
        if (close_handle == NULL || dlclose(close_handle) != 0) {
            fputs("immediate-close setup failed\n", stderr);
            return EXIT_FAILURE;
        }
        explicit_drain();
        get_diagnostics(&after);
        if (after.enqueued != before.enqueued + 1 ||
            after.drained != before.drained + 1 ||
            after.dropped_noload != before.dropped_noload + 1 ||
            after.queue_length != 0) {
            fputs("immediate dlclose did not fail safe at RTLD_NOLOAD\n",
                  stderr);
            return EXIT_FAILURE;
        }
        set_manual_drain(0);
        puts("fftw_runtime_dlopen_ok mode=close deferred_noload_drop=1");
        return EXIT_SUCCESS;
    }

    if (strcmp(mode, "stress") == 0) {
        enum { THREAD_COUNT = 32, ITERATIONS = 200 };
        pthread_t threads[THREAD_COUNT];
        gboolean created[THREAD_COUNT] = { FALSE };
        _Atomic int stress_failures = 0;
        DlopenStressArgs stress_args = {
            .provider = argv[1],
            .iterations = ITERATIONS,
            .failures = &stress_failures,
        };

        set_manual_drain(0);
        for (unsigned int i = 0; i < THREAD_COUNT; i++) {
            if (pthread_create(&threads[i],
                               NULL,
                               run_dlopen_stress,
                               &stress_args) != 0) {
                atomic_fetch_add_explicit(&stress_failures,
                                          1,
                                          memory_order_relaxed);
            } else {
                created[i] = TRUE;
            }
        }
        for (unsigned int i = 0; i < THREAD_COUNT; i++) {
            if (created[i]) {
                pthread_join(threads[i], NULL);
            }
        }
        set_manual_drain(1);
        for (unsigned int i = 0; i < 4096; i++) {
            explicit_drain();
            get_diagnostics(&after);
            if (after.queue_length == 0) {
                break;
            }
            sched_yield();
        }
        if (atomic_load_explicit(&stress_failures,
                                 memory_order_relaxed) != 0 ||
            after.queue_length != 0 ||
            after.enqueued == before.enqueued ||
            after.dropped_full != before.dropped_full) {
            fprintf(stderr,
                    "concurrent deferred dlopen stress failed: worker_failures=%d queue=%zu enqueued=%llu drained=%llu dropped_full=%llu dropped_noload=%llu\n",
                    atomic_load_explicit(&stress_failures,
                                         memory_order_relaxed),
                    after.queue_length,
                    after.enqueued - before.enqueued,
                    after.drained - before.drained,
                    after.dropped_full - before.dropped_full,
                    after.dropped_noload - before.dropped_noload);
            return EXIT_FAILURE;
        }
        set_manual_drain(0);
        printf("fftw_runtime_dlopen_ok mode=stress threads=%u iterations=%u enqueued=%llu drained=%llu\n",
               THREAD_COUNT,
               ITERATIONS,
               after.enqueued - before.enqueued,
               after.drained - before.drained);
        return EXIT_SUCCESS;
    }

    if (strcmp(mode, "probe") == 0) {
        void* unrelated_handle = dlopen(argv[3], RTLD_LAZY | RTLD_LOCAL);
        if (unrelated_handle == NULL) {
            fprintf(stderr, "unrelated dlopen failed: %s\n", dlerror());
            return EXIT_FAILURE;
        }
        explicit_drain();
        if (dlclose(unrelated_handle) != 0) {
            fprintf(stderr, "unrelated dlclose failed: %s\n", dlerror());
            return EXIT_FAILURE;
        }
    }

    void* handle = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return EXIT_FAILURE;
    }
    explicit_drain();

    if (strcmp(mode, "single") == 0) {
        plain_target_fn single_target;
        load_function(handle, argv[3], &single_target, sizeof(single_target));
        single_target();
    } else {
        load_function(handle,
                      "fftw_malloc",
                      &fftw_malloc,
                      sizeof(fftw_malloc));
        load_function(handle, "fftw_free", &fftw_free, sizeof(fftw_free));
        if (strcmp(mode, "mixed") == 0 || strcmp(mode, "retry") == 0) {
            load_function(handle,
                          "peak_runtime_non_fftw_target",
                          &non_fftw_target,
                          sizeof(non_fftw_target));
            non_fftw_target();
        }

        if (call_fftw_pair(fftw_malloc, fftw_free) != 0) {
            return EXIT_FAILURE;
        }
        if (strcmp(mode, "tail") == 0) {
            plain_target_fn direct_implementation_calls;
            load_function(handle,
                          "peak_tail_jump_call_implementations_directly",
                          &direct_implementation_calls,
                          sizeof(direct_implementation_calls));
            direct_implementation_calls();
        }
    }

    if (strcmp(mode, "mixed") == 0) {
        PeakDlopenDynamicAttachDiagnostics post_drain_before;
        PeakDlopenDynamicAttachDiagnostics post_drain_after;
        void* second_handle;

        explicit_drain();
        non_fftw_target();
        get_diagnostics(&post_drain_before);
        second_handle = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
        if (second_handle == NULL) {
            fprintf(stderr, "post-drain dlopen failed: %s\n", dlerror());
            return EXIT_FAILURE;
        }
        get_diagnostics(&post_drain_after);
        if (post_drain_after.enqueued != post_drain_before.enqueued) {
            fputs("resolved mixed targets kept the dlopen callback armed\n",
                  stderr);
            return EXIT_FAILURE;
        }
        if (dlclose(second_handle) != 0) {
            fprintf(stderr, "post-drain dlclose failed: %s\n", dlerror());
            return EXIT_FAILURE;
        }
    } else if (strcmp(mode, "retry") == 0) {
        explicit_drain();
        non_fftw_target();
        if (call_fftw_pair(fftw_malloc, fftw_free) != 0) {
            return EXIT_FAILURE;
        }
        void* second_handle = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
        if (second_handle == NULL) {
            fprintf(stderr, "post-retry dlopen failed: %s\n", dlerror());
            return EXIT_FAILURE;
        }
        if (dlclose(second_handle) != 0) {
            fprintf(stderr, "post-retry dlclose failed: %s\n", dlerror());
            return EXIT_FAILURE;
        }
    } else if (strcmp(mode, "retry-fast") == 0) {
        void* second_handle = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
        if (second_handle == NULL) {
            fprintf(stderr, "retry dlopen failed: %s\n", dlerror());
            return EXIT_FAILURE;
        }
        if (dlclose(second_handle) != 0) {
            fprintf(stderr, "retry dlclose failed: %s\n", dlerror());
            return EXIT_FAILURE;
        }
        explicit_drain();
        if (call_fftw_pair(fftw_malloc, fftw_free) != 0) {
            return EXIT_FAILURE;
        }
    } else if (strcmp(mode, "fast") == 0) {
        void* second_handle = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
        if (second_handle == NULL) {
            fprintf(stderr, "second dlopen failed: %s\n", dlerror());
            return EXIT_FAILURE;
        }
        if (dlclose(second_handle) != 0) {
            fprintf(stderr, "second dlclose failed: %s\n", dlerror());
            return EXIT_FAILURE;
        }
    } else if (strcmp(mode, "extension") == 0) {
        plain_target_fn extension_target;
        void* extension_handle = dlopen(argv[3], RTLD_LAZY | RTLD_LOCAL);
        if (extension_handle == NULL) {
            fprintf(stderr, "extension dlopen failed: %s\n", dlerror());
            return EXIT_FAILURE;
        }
        explicit_drain();
        load_function(extension_handle,
                      "fftw_mpi_init",
                      &extension_target,
                      sizeof(extension_target));
        extension_target();
        if (dlclose(extension_handle) != 0) {
            fprintf(stderr, "extension dlclose failed: %s\n", dlerror());
            return EXIT_FAILURE;
        }
    }

    get_diagnostics(&after);
    unsigned long long expected_request_delta =
        (strcmp(mode, "probe") == 0 ||
         strcmp(mode, "extension") == 0) ? 2 : 1;
    if (after.enqueued != before.enqueued + expected_request_delta ||
        after.drained != before.drained + expected_request_delta ||
        after.requeued != before.requeued) {
        fputs("deferred loader work did not drain exactly once per provider\n",
              stderr);
        return EXIT_FAILURE;
    }
    if (after.queue_length != 0) {
        fprintf(stderr, "dynamic attach queue not empty: %zu\n",
                after.queue_length);
        return EXIT_FAILURE;
    }

    set_manual_drain(0);

    if (dlclose(handle) != 0) {
        fprintf(stderr, "dlclose failed: %s\n", dlerror());
        return EXIT_FAILURE;
    }

    printf("fftw_runtime_dlopen_ok mode=%s deferred_requests=%llu enqueued_delta=%llu requeued_delta=%llu drained_delta=%llu\n",
           mode,
           expected_request_delta,
           after.enqueued - before.enqueued,
           after.requeued - before.requeued,
           after.drained - before.drained);
    return EXIT_SUCCESS;
}
