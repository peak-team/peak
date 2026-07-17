static volatile int unrelated_call_count;

__attribute__((visibility("default"), noinline))
void peak_fftw_runtime_unrelated_target(void)
{
    unrelated_call_count++;
}
