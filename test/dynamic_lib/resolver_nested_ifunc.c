#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

typedef void (*nested_target_fn)(void);

static void* nested_handle;

static void
resolver_nested_impl(void)
{
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
#endif
}

static void (*
resolver_nested_resolver(void)
)(void)
{
    void* address;
    nested_target_fn target;

    nested_handle = dlopen("./libB.so", RTLD_LAZY | RTLD_LOCAL);
    if (nested_handle == NULL) {
        abort();
    }

    dlerror();
    address = dlsym(nested_handle, "b_dynamic");
    if (dlerror() != NULL || address == NULL || sizeof(address) != sizeof(target)) {
        abort();
    }
    memcpy(&target, &address, sizeof(target));

    /* The nested target's very first call occurs inside this IFUNC resolver. */
    target();
    return &resolver_nested_impl;
}

__attribute__((visibility("default"), ifunc("resolver_nested_resolver")))
void peak_resolver_nested_ifunc(void);
