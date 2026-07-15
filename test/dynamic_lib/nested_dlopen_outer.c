#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>

typedef void (*nested_target_fn)(void);

__attribute__((constructor))
static void nested_dlopen_outer_init(void)
{
    void* handle = dlopen("./libB.so", RTLD_NOW | RTLD_LOCAL);
    nested_target_fn target;

    if (handle == NULL) {
        abort();
    }
    target = (nested_target_fn)dlsym(handle, "b_dynamic");
    if (target == NULL) {
        abort();
    }
    target();
}
