#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*peak_target_fn)(void);
typedef int (*peak_counter_fn)(void);

static int
load_symbol(void* handle,
            const char* name,
            void* destination,
            size_t destination_size)
{
    void* address;

    dlerror();
    address = dlsym(handle, name);
    if (address == NULL || dlerror() != NULL ||
        destination_size != sizeof(address)) {
        fprintf(stderr, "failed to resolve %s\n", name);
        return -1;
    }
    memcpy(destination, &address, sizeof(address));
    return 0;
}

int
main(void)
{
    void* handle = dlopen("./libStatefulIfuncTarget.so",
                          RTLD_NOW | RTLD_LOCAL);
    peak_target_fn target;
    peak_counter_fn resolver_count;
    peak_counter_fn a_count;
    peak_counter_fn b_count;

    if (handle == NULL ||
        load_symbol(handle,
                    "peak_stateful_ifunc",
                    &target,
                    sizeof(target)) != 0 ||
        load_symbol(handle,
                    "peak_stateful_ifunc_resolver_calls",
                    &resolver_count,
                    sizeof(resolver_count)) != 0 ||
        load_symbol(handle,
                    "peak_stateful_ifunc_a_calls",
                    &a_count,
                    sizeof(a_count)) != 0 ||
        load_symbol(handle,
                    "peak_stateful_ifunc_b_calls",
                    &b_count,
                    sizeof(b_count)) != 0) {
        return EXIT_FAILURE;
    }

    target();
    if (resolver_count() != 1 || a_count() != 1 || b_count() != 0) {
        fprintf(stderr,
                "stateful IFUNC mismatch: resolver=%d a=%d b=%d\n",
                resolver_count(),
                a_count(),
                b_count());
        return EXIT_FAILURE;
    }

    fputs("stateful_ifunc_exact_lookup_ok\n", stderr);
    return EXIT_SUCCESS;
}
