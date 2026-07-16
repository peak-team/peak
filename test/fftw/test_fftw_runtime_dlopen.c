#define _GNU_SOURCE
#define PEAK_ENABLE_TEST_HOOKS 1
#include "dlopen_interceptor.h"

#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void* (*fftw_malloc_fn)(size_t size);
typedef void (*fftw_free_fn)(void* pointer);
typedef void (*plain_target_fn)(void);
typedef void (*set_manual_drain_fn)(gboolean enabled);
typedef void (*plain_hook_fn)(void);
typedef unsigned long long (*sync_scan_count_fn)(void);
typedef void (*get_diagnostics_fn)(
    PeakDlopenDynamicAttachDiagnostics* diagnostics);

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
    plain_hook_fn force_sync_timeout;
    sync_scan_count_fn sync_scan_count;
    get_diagnostics_fn get_diagnostics;
    PeakDlopenDynamicAttachDiagnostics before = { 0 };
    PeakDlopenDynamicAttachDiagnostics after = { 0 };
    const char* mode = argc == 3 ? argv[2] : "default";
    unsigned long long scans_before;
    unsigned long long scans_after_load;

    if ((argc != 2 && argc != 3) ||
        (strcmp(mode, "default") != 0 && strcmp(mode, "mixed") != 0 &&
         strcmp(mode, "retry") != 0 && strcmp(mode, "fast") != 0)) {
        fprintf(stderr,
                "usage: %s /path/to/provider [default|mixed|retry|fast]\n",
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
                  "dlopen_interceptor_test_force_sync_prepare_timeout_once",
                  &force_sync_timeout,
                  sizeof(force_sync_timeout));
    load_function(RTLD_DEFAULT,
                  "dlopen_interceptor_test_sync_scan_count",
                  &sync_scan_count,
                  sizeof(sync_scan_count));
    load_function(RTLD_DEFAULT,
                  "dlopen_interceptor_get_dynamic_attach_diagnostics",
                  &get_diagnostics,
                  sizeof(get_diagnostics));

    /*
     * Keep the controller fallback from hiding a broken synchronous attach.
     * The first FFTW calls below must be visible before any queued request is
     * drained.
     */
    set_manual_drain(1);
    get_diagnostics(&before);
    scans_before = sync_scan_count();
    if (strcmp(mode, "retry") == 0) {
        force_sync_timeout();
    }

    void* handle = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return EXIT_FAILURE;
    }
    scans_after_load = sync_scan_count();

    load_function(handle, "fftw_malloc", &fftw_malloc, sizeof(fftw_malloc));
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

    if (strcmp(mode, "mixed") == 0) {
        explicit_drain();
        non_fftw_target();
    } else if (strcmp(mode, "retry") == 0) {
        explicit_drain();
        non_fftw_target();
        if (call_fftw_pair(fftw_malloc, fftw_free) != 0) {
            return EXIT_FAILURE;
        }
    } else if (strcmp(mode, "fast") == 0) {
        void* second_handle = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
        if (second_handle == NULL) {
            fprintf(stderr, "second dlopen failed: %s\n", dlerror());
            return EXIT_FAILURE;
        }
        if (sync_scan_count() != scans_after_load) {
            fputs("resolved FFTW targets were rescanned\n", stderr);
            return EXIT_FAILURE;
        }
        if (dlclose(second_handle) != 0) {
            fprintf(stderr, "second dlclose failed: %s\n", dlerror());
            return EXIT_FAILURE;
        }
    }

    get_diagnostics(&after);
    if (scans_after_load != scans_before + 1) {
        fprintf(stderr,
                "expected one synchronous scan, before=%llu after=%llu\n",
                scans_before,
                scans_after_load);
        return EXIT_FAILURE;
    }
    if ((strcmp(mode, "default") == 0 || strcmp(mode, "fast") == 0) &&
        after.enqueued != before.enqueued) {
        fputs("FFTW-only synchronous success queued duplicate work\n", stderr);
        return EXIT_FAILURE;
    }
    if (strcmp(mode, "mixed") == 0 &&
        (after.enqueued != before.enqueued + 1 ||
         after.drained != before.drained + 1)) {
        fputs("mixed target did not use exactly one asynchronous request\n",
              stderr);
        return EXIT_FAILURE;
    }
    if (strcmp(mode, "retry") == 0 &&
        (after.enqueued != before.enqueued ||
         after.requeued != before.requeued + 1 ||
         after.drained != before.drained + 1)) {
        fputs("synchronous timeout did not requeue exactly once\n", stderr);
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

    printf("fftw_runtime_dlopen_ok mode=%s sync_scans=%llu enqueued_delta=%llu requeued_delta=%llu drained_delta=%llu\n",
           mode,
           scans_after_load - scans_before,
           after.enqueued - before.enqueued,
           after.requeued - before.requeued,
           after.drained - before.drained);
    return EXIT_SUCCESS;
}
