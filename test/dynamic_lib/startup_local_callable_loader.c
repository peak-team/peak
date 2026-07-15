#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

typedef void (*startup_callable_fn)(void);

static void* ifunc_handle;
static void* notype_handle;
static startup_callable_fn ifunc_target;
static startup_callable_fn notype_target;

static startup_callable_fn
load_local_callable(const char* library, const char* symbol, void** handle_out)
{
    startup_callable_fn target;
    void* address;
    void* handle = dlopen(library, RTLD_LAZY | RTLD_LOCAL);

    if (handle == NULL) {
        abort();
    }
    dlerror();
    address = dlsym(handle, symbol);
    if (dlerror() != NULL || address == NULL ||
        sizeof(address) != sizeof(target)) {
        abort();
    }
    memcpy(&target, &address, sizeof(target));
    *handle_out = handle;
    return target;
}

__attribute__((constructor))
static void
startup_local_callable_loader_init(void)
{
    ifunc_target = load_local_callable("./libIfuncTarget.so",
                                       "peak_ifunc_target",
                                       &ifunc_handle);
    notype_target = load_local_callable("./libNotypeTarget.so",
                                        "peak_notype_target",
                                        &notype_handle);
}

__attribute__((visibility("default")))
void
startup_local_callable_loader_call(void)
{
    if (ifunc_handle == NULL || notype_handle == NULL ||
        ifunc_target == NULL || notype_target == NULL) {
        abort();
    }
    ifunc_target();
    notype_target();
}
