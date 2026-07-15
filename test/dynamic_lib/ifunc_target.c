typedef void (*peak_ifunc_target_fn)(void);

static volatile unsigned long peak_ifunc_sink;

__attribute__((noinline))
static void peak_ifunc_impl(void)
{
    for (unsigned long i = 1; i <= 16; i++) {
        peak_ifunc_sink = (peak_ifunc_sink << 1) ^ i;
    }
}

static peak_ifunc_target_fn peak_ifunc_resolve(void)
{
    return peak_ifunc_impl;
}

__attribute__((visibility("default"), ifunc("peak_ifunc_resolve")))
void peak_ifunc_target(void);
