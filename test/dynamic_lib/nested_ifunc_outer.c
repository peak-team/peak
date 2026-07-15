#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

typedef void (*nested_ifunc_target_fn)(void);

__attribute__((constructor))
static void nested_ifunc_outer_init(void)
{
    void* handle = dlopen("./libIfuncTarget.so", RTLD_NOW | RTLD_LOCAL);
    void* address;
    nested_ifunc_target_fn target;

    if (handle == NULL) {
        abort();
    }
    address = dlsym(handle, "peak_ifunc_target");
    if (address == NULL || sizeof(address) != sizeof(target)) {
        abort();
    }
    memcpy(&target, &address, sizeof(target));
    target();
}
