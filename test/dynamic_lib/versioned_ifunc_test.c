#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*peak_target_fn)(void);
typedef int (*peak_counter_fn)(void);

int
main(void)
{
    void* handle = dlopen("./libVersionedIfuncTarget.so",
                          RTLD_NOW | RTLD_LOCAL);
    void* address;
    peak_target_fn target = NULL;
    peak_counter_fn count = NULL;

    if (handle == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return EXIT_FAILURE;
    }
    dlerror();
    address = dlvsym(handle, "peak_versioned_ifunc", "PEAK_IFUNC_1.0");
    if (address == NULL || dlerror() != NULL ||
        sizeof(address) != sizeof(target)) {
        fputs("versioned IFUNC lookup failed\n", stderr);
        return EXIT_FAILURE;
    }
    memcpy(&target, &address, sizeof(target));

    dlerror();
    address = dlsym(handle, "peak_versioned_ifunc_calls");
    if (address == NULL || dlerror() != NULL ||
        sizeof(address) != sizeof(count)) {
        fputs("versioned IFUNC counter lookup failed\n", stderr);
        return EXIT_FAILURE;
    }
    memcpy(&count, &address, sizeof(count));

    target();
    if (count() != 1) {
        fputs("versioned IFUNC implementation was not called once\n", stderr);
        return EXIT_FAILURE;
    }
    fputs("versioned_ifunc_dlvsym_ok\n", stderr);
    return EXIT_SUCCESS;
}
