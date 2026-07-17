#include <stddef.h>

void* fftw_malloc(size_t size);

/* Keep a real DT_NEEDED edge to the fake FFTW core provider. */
void* (*peak_fftw_runtime_extension_core_anchor)(size_t) = fftw_malloc;

static volatile unsigned long extension_call_count;

__attribute__((visibility("default"), noinline))
void fftw_mpi_init(void)
{
    extension_call_count++;
}
