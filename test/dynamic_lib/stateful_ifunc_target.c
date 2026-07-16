#include <stdatomic.h>

typedef void (*peak_stateful_ifunc_fn)(void);

static _Atomic int resolver_calls;
static _Atomic int implementation_a_calls;
static _Atomic int implementation_b_calls;

static void
peak_stateful_ifunc_implementation_a(void)
{
    atomic_fetch_add_explicit(&implementation_a_calls,
                              1,
                              memory_order_relaxed);
}

static void
peak_stateful_ifunc_implementation_b(void)
{
    atomic_fetch_add_explicit(&implementation_b_calls,
                              1,
                              memory_order_relaxed);
}

static peak_stateful_ifunc_fn
peak_stateful_ifunc_resolver(void)
{
    int call = atomic_fetch_add_explicit(&resolver_calls,
                                         1,
                                         memory_order_relaxed);

    return call == 0
        ? peak_stateful_ifunc_implementation_a
        : peak_stateful_ifunc_implementation_b;
}

__attribute__((visibility("default"),
               ifunc("peak_stateful_ifunc_resolver")))
void peak_stateful_ifunc(void);

__attribute__((visibility("default")))
int
peak_stateful_ifunc_resolver_calls(void)
{
    return atomic_load_explicit(&resolver_calls, memory_order_relaxed);
}

__attribute__((visibility("default")))
int
peak_stateful_ifunc_a_calls(void)
{
    return atomic_load_explicit(&implementation_a_calls,
                                memory_order_relaxed);
}

__attribute__((visibility("default")))
int
peak_stateful_ifunc_b_calls(void)
{
    return atomic_load_explicit(&implementation_b_calls,
                                memory_order_relaxed);
}
