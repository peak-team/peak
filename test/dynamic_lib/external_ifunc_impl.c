static volatile unsigned long peak_external_ifunc_sink;

__attribute__((noinline, visibility("default")))
void
peak_external_ifunc_impl(void)
{
    for (unsigned long i = 1; i <= 16; i++) {
        peak_external_ifunc_sink =
            (peak_external_ifunc_sink << 1) ^ i;
    }
}
