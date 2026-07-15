#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*nested_target_fn)(void);

static void* nested_handle;

static void
startup_resolver_impl(void)
{
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
#endif
}

static void (*
startup_resolver(void)
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
    if (dlerror() != NULL || address == NULL ||
        sizeof(address) != sizeof(target)) {
        abort();
    }
    memcpy(&target, &address, sizeof(target));

    /* This must be attached before startup symbol resolution can return. */
    target();
    return &startup_resolver_impl;
}

__attribute__((visibility("default"), ifunc("startup_resolver")))
void peak_startup_resolver_ifunc(void);

int
main(void)
{
    void* address;
    nested_target_fn target;

    /* Keep the IFUNC lazy until PEAK performs its startup symbol lookup. */
    dlerror();
    address = dlsym(RTLD_DEFAULT, "peak_startup_resolver_ifunc");
    if (dlerror() != NULL || address == NULL ||
        sizeof(address) != sizeof(target)) {
        return 2;
    }
    memcpy(&target, &address, sizeof(target));
    target();
    fputs("startup_resolver_nested_ifunc_ok\n", stderr);
    return 0;
}
