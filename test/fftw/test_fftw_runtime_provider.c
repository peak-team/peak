#include <stddef.h>
#include <stdlib.h>

static volatile size_t provider_call_count;

__attribute__((visibility("default"), noinline))
void* fftw_malloc(size_t size)
{
    void* pointer = malloc(size);
    provider_call_count++;
    return pointer;
}

__attribute__((visibility("default"), noinline))
void fftw_free(void* pointer)
{
    free(pointer);
    provider_call_count++;
}

__attribute__((visibility("default"), noinline))
void peak_runtime_non_fftw_target(void)
{
    provider_call_count++;
}
