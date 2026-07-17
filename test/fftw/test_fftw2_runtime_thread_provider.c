#ifndef PEAK_FFTW2_THREAD_SYMBOL
#error "PEAK_FFTW2_THREAD_SYMBOL must name the exported FFTW2 thread function"
#endif

static volatile unsigned long provider_call_count;

__attribute__((visibility("default"), noinline))
void PEAK_FFTW2_THREAD_SYMBOL(void)
{
    provider_call_count++;
}
