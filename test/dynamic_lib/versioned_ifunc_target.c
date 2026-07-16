#include <stdatomic.h>

typedef void (*peak_versioned_ifunc_fn)(void);

static _Atomic int implementation_calls;

static void
peak_versioned_ifunc_implementation(void)
{
    atomic_fetch_add_explicit(&implementation_calls,
                              1,
                              memory_order_relaxed);
}

static peak_versioned_ifunc_fn
peak_versioned_ifunc_resolver(void)
{
    return peak_versioned_ifunc_implementation;
}

__attribute__((visibility("default"),
               ifunc("peak_versioned_ifunc_resolver")))
void peak_versioned_ifunc(void);

__attribute__((visibility("default")))
int
peak_versioned_ifunc_calls(void)
{
    return atomic_load_explicit(&implementation_calls,
                                memory_order_relaxed);
}
