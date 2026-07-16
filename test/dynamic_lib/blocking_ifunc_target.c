#include <sched.h>
#include <stdatomic.h>

typedef void (*peak_blocking_ifunc_fn)(void);

static _Atomic int resolution_allowed;
static volatile unsigned long implementation_sink;

__attribute__((noinline))
static void
peak_blocking_ifunc_implementation(void)
{
    for (unsigned long value = 1; value <= 16; value++) {
        implementation_sink = (implementation_sink << 1) ^ value;
    }
}

static peak_blocking_ifunc_fn
peak_blocking_ifunc_resolver(void)
{
    while (!atomic_load_explicit(&resolution_allowed,
                                 memory_order_acquire)) {
        sched_yield();
    }
    return peak_blocking_ifunc_implementation;
}

__attribute__((visibility("default"),
               ifunc("peak_blocking_ifunc_resolver")))
void peak_blocking_ifunc(void);

__attribute__((visibility("default")))
void
peak_allow_blocking_ifunc_resolution(void)
{
    atomic_store_explicit(&resolution_allowed, 1, memory_order_release);
}
