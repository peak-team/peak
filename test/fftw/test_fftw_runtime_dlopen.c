#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef double fftw_complex[2];
typedef struct fftw_plan_s* fftw_plan;

typedef void* (*fftw_malloc_fn)(size_t size);
typedef void (*fftw_free_fn)(void* pointer);
typedef fftw_plan (*fftw_plan_dft_1d_fn)(int n,
                                         fftw_complex* input,
                                         fftw_complex* output,
                                         int sign,
                                         unsigned flags);
typedef void (*fftw_execute_fn)(const fftw_plan plan);
typedef void (*fftw_destroy_plan_fn)(fftw_plan plan);

static void*
required_symbol(void* handle, const char* name)
{
    void* address;
    const char* error;

    dlerror();
    address = dlsym(handle, name);
    error = dlerror();
    if (error != NULL || address == NULL) {
        fprintf(stderr,
                "required FFTW symbol %s is unavailable: %s\n",
                name,
                error != NULL ? error : "null address");
        exit(EXIT_FAILURE);
    }

    return address;
}

static void
load_required_function(void* handle,
                       const char* name,
                       void* function_pointer,
                       size_t function_pointer_size)
{
    void* address = required_symbol(handle, name);

    if (function_pointer_size != sizeof(address)) {
        fprintf(stderr,
                "cannot represent FFTW symbol %s as a function pointer\n",
                name);
        exit(EXIT_FAILURE);
    }
    memcpy(function_pointer, &address, sizeof(address));
}

int
main(int argc, char** argv)
{
    enum {
        transform_size = 16,
        fftw_forward = -1,
        fftw_estimate = 1U << 6
    };
    const char* library_path;
    void* handle;
    fftw_malloc_fn fftw_malloc;
    fftw_free_fn fftw_free;
    fftw_plan_dft_1d_fn fftw_plan_dft_1d;
    fftw_execute_fn fftw_execute;
    fftw_destroy_plan_fn fftw_destroy_plan;
    fftw_complex* input;
    fftw_complex* output;
    fftw_plan plan;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/libfftw3.so\n", argv[0]);
        return EXIT_FAILURE;
    }

    library_path = argv[1];
    handle = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr,
                "failed to load FFTW provider %s: %s\n",
                library_path,
                dlerror());
        return EXIT_FAILURE;
    }

    load_required_function(handle,
                           "fftw_malloc",
                           &fftw_malloc,
                           sizeof(fftw_malloc));
    load_required_function(handle,
                           "fftw_free",
                           &fftw_free,
                           sizeof(fftw_free));
    load_required_function(handle,
                           "fftw_plan_dft_1d",
                           &fftw_plan_dft_1d,
                           sizeof(fftw_plan_dft_1d));
    load_required_function(handle,
                           "fftw_execute",
                           &fftw_execute,
                           sizeof(fftw_execute));
    load_required_function(handle,
                           "fftw_destroy_plan",
                           &fftw_destroy_plan,
                           sizeof(fftw_destroy_plan));

    input = fftw_malloc(sizeof(*input) * transform_size);
    output = fftw_malloc(sizeof(*output) * transform_size);
    if (input == NULL || output == NULL) {
        fprintf(stderr, "fftw_malloc failed\n");
        return EXIT_FAILURE;
    }

    for (int i = 0; i < transform_size; i++) {
        input[i][0] = (double)i;
        input[i][1] = 0.0;
        output[i][0] = 0.0;
        output[i][1] = 0.0;
    }

    plan = fftw_plan_dft_1d(transform_size,
                            input,
                            output,
                            fftw_forward,
                            fftw_estimate);
    if (plan == NULL) {
        fprintf(stderr, "fftw_plan_dft_1d failed\n");
        return EXIT_FAILURE;
    }

    fftw_execute(plan);
    fftw_destroy_plan(plan);
    fftw_free(output);
    fftw_free(input);

    if (dlclose(handle) != 0) {
        fprintf(stderr, "failed to close FFTW provider: %s\n", dlerror());
        return EXIT_FAILURE;
    }

    puts("fftw_runtime_dlopen_ok");
    return EXIT_SUCCESS;
}
