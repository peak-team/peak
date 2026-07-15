#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef void (*peak_external_ifunc_fn)(void);

__attribute__((visibility("default")))
void* peak_external_ifunc_selected;

static peak_external_ifunc_fn
find_target(void* handle)
{
    peak_external_ifunc_fn function = NULL;
    void* address;

    dlerror();
    address = dlsym(handle, "peak_external_ifunc_target");
    if (address == NULL || dlerror() != NULL ||
        sizeof(function) != sizeof(address)) {
        return NULL;
    }
    memcpy(&function, &address, sizeof(function));
    return function;
}

int
main(void)
{
    void* implementation_handle;
    void* target_handle;
    void* loaded_probe;
    peak_external_ifunc_fn function;

    implementation_handle =
        dlopen("./libExternalIfuncImpl.so", RTLD_NOW | RTLD_GLOBAL);
    if (implementation_handle == NULL) {
        fputs(dlerror(), stderr);
        return 1;
    }
    dlerror();
    peak_external_ifunc_selected =
        dlsym(implementation_handle, "peak_external_ifunc_impl");
    if (peak_external_ifunc_selected == NULL || dlerror() != NULL) {
        return 2;
    }
    target_handle =
        dlopen("./libExternalIfuncTarget.so", RTLD_NOW | RTLD_LOCAL);
    if (target_handle == NULL) {
        fputs(dlerror(), stderr);
        return 3;
    }

    function = find_target(target_handle);
    if (function == NULL) {
        return 4;
    }
    function();

    if (dlclose(target_handle) != 0 ||
        dlclose(implementation_handle) != 0) {
        return 5;
    }

#ifdef RTLD_NOLOAD
    loaded_probe = dlopen("./libExternalIfuncImpl.so",
                          RTLD_NOW | RTLD_NOLOAD);
    if (loaded_probe == NULL) {
        fputs("external_ifunc_provider_was_unloaded\n", stderr);
        return 6;
    }
    dlclose(loaded_probe);
#else
    loaded_probe = NULL;
    (void)loaded_probe;
#endif

    function();
    fputs("external_ifunc_provider_retained_ok\n", stderr);
    return 0;
}
