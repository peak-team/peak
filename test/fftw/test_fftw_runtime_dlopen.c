#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void* (*fftw_malloc_fn)(size_t size);
typedef void (*fftw_free_fn)(void* pointer);

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

int
main(int argc, char** argv)
{
    fftw_malloc_fn fftw_malloc;
    fftw_free_fn fftw_free;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/provider\n", argv[0]);
        return EXIT_FAILURE;
    }

    void* handle = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
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
        fprintf(stderr, "dlclose failed: %s\n", dlerror());
        return EXIT_FAILURE;
    }

    puts("fftw_runtime_dlopen_ok");
    return EXIT_SUCCESS;
}
