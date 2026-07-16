#include <stddef.h>

void* fftw_malloc(size_t size);
void fftw_free(void* pointer);

__attribute__((visibility("default"), noinline))
void test_fftw_consumer_run(void)
{
    void* allocation = fftw_malloc(64);
    fftw_free(allocation);
}
