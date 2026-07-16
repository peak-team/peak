#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*peak_void_fn)(void);

static peak_void_fn
load_function(void* handle, const char* name)
{
    void* address;
    peak_void_fn function = NULL;

    dlerror();
    address = dlsym(handle, name);
    if (address == NULL || dlerror() != NULL ||
        sizeof(function) != sizeof(address)) {
        fprintf(stderr, "failed to resolve %s\n", name);
        exit(EXIT_FAILURE);
    }
    memcpy(&function, &address, sizeof(function));
    return function;
}

int
main(void)
{
    void* handle = dlopen("./libBlockingIfuncTarget.so",
                          RTLD_NOW | RTLD_LOCAL);
    peak_void_fn allow_resolution;
    peak_void_fn target;

    if (handle == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return EXIT_FAILURE;
    }
    allow_resolution = load_function(
        handle,
        "peak_allow_blocking_ifunc_resolution");
    allow_resolution();
    target = load_function(handle, "peak_blocking_ifunc");
    target();
    fputs("blocking_ifunc_after_dlopen_ok\n", stderr);
    return EXIT_SUCCESS;
}
