#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*gate_epoch_fn)(void);
typedef void* (*fftw_malloc_fn)(size_t size);
typedef void (*fftw_free_fn)(void* pointer);

static const char* provider_path;

static void
load_function(void* handle,
              const char* name,
              void* function_pointer,
              size_t function_pointer_size)
{
    void* address;
    const char* error;

    dlerror();
    address = dlsym(handle, name);
    error = dlerror();
    if (address == NULL || error != NULL ||
        function_pointer_size != sizeof(address)) {
        fprintf(stderr,
                "failed to resolve %s: %s\n",
                name,
                error != NULL ? error : "invalid function address");
        exit(EXIT_FAILURE);
    }
    memcpy(function_pointer, &address, sizeof(address));
}

static void*
load_provider(void* unused)
{
    (void)unused;
    void* handle = dlopen(provider_path, RTLD_LAZY | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "loader dlopen failed: %s\n", dlerror());
        return (void*)1;
    }
    /* Deferred cancellation must become observable only after on-leave cleanup. */
    pthread_testcancel();
    return handle;
}

int
main(int argc, char** argv)
{
    gate_epoch_fn gate_epoch;
    pthread_t loader;
    void* loader_result = NULL;
    fftw_malloc_fn fftw_malloc;
    fftw_free_fn fftw_free;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/provider\n", argv[0]);
        return EXIT_FAILURE;
    }
    provider_path = argv[1];
    alarm(15);

    load_function(RTLD_DEFAULT,
                  "peak_detach_controller_test_thread_creation_gate_epoch",
                  &gate_epoch,
                  sizeof(gate_epoch));
    if (pthread_create(&loader, NULL, load_provider, NULL) != 0) {
        perror("pthread_create");
        return EXIT_FAILURE;
    }

    for (unsigned int attempt = 0; gate_epoch() == 0; attempt++) {
        if (attempt >= 5000) {
            fputs("strict mutation gate did not become active\n", stderr);
            return EXIT_FAILURE;
        }
        usleep(1000);
    }
    if (pthread_cancel(loader) != 0) {
        fputs("pthread_cancel failed\n", stderr);
        return EXIT_FAILURE;
    }
    if (pthread_join(loader, &loader_result) != 0) {
        fputs("pthread_join failed\n", stderr);
        return EXIT_FAILURE;
    }
    if (loader_result != PTHREAD_CANCELED) {
        fprintf(stderr,
                "loader cancellation was not delivered after callback cleanup: %p\n",
                loader_result);
        return EXIT_FAILURE;
    }
    if (gate_epoch() != 0) {
        fputs("strict mutation gate remained active after cancellation\n", stderr);
        return EXIT_FAILURE;
    }

    void* handle = dlopen(provider_path, RTLD_LAZY | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "follow-up dlopen failed: %s\n", dlerror());
        return EXIT_FAILURE;
    }
    load_function(handle, "fftw_malloc", &fftw_malloc, sizeof(fftw_malloc));
    load_function(handle, "fftw_free", &fftw_free, sizeof(fftw_free));
    void* allocation = fftw_malloc(64);
    if (allocation == NULL) {
        fputs("fftw_malloc failed\n", stderr);
        return EXIT_FAILURE;
    }
    fftw_free(allocation);
    if (dlclose(handle) != 0) {
        fprintf(stderr, "follow-up dlclose failed: %s\n", dlerror());
        return EXIT_FAILURE;
    }

    alarm(0);
    puts("fftw_dlopen_cancellation_cleanup_ok");
    return EXIT_SUCCESS;
}
