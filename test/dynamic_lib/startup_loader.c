#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

typedef void (*startup_target_fn)(void);

static void* application_handle;
static startup_target_fn startup_target;
static startup_target_fn startup_target_second;

__attribute__((constructor))
static void
startup_loader_init(void)
{
    void* address;

    application_handle = dlopen("./libStartupUnloadableTarget.so",
                                RTLD_NOW | RTLD_GLOBAL);
    if (application_handle == NULL) {
        abort();
    }
    dlerror();
    address = dlsym(application_handle, "peak_startup_unloadable_target");
    if (dlerror() != NULL || address == NULL ||
        sizeof(address) != sizeof(startup_target)) {
        abort();
    }
    memcpy(&startup_target, &address, sizeof(startup_target));

    dlerror();
    address = dlsym(application_handle,
                    "peak_startup_unloadable_target_second");
    if (dlerror() != NULL || address == NULL ||
        sizeof(address) != sizeof(startup_target_second)) {
        abort();
    }
    memcpy(&startup_target_second, &address, sizeof(startup_target_second));
}

__attribute__((visibility("default")))
void
startup_loader_call(void)
{
    if (startup_target == NULL || startup_target_second == NULL) {
        abort();
    }
    startup_target();
    startup_target_second();
}

__attribute__((visibility("default")))
int
startup_loader_drop_application_handle(void)
{
    void* handle = application_handle;

    application_handle = NULL;
    return handle != NULL ? dlclose(handle) : -1;
}

__attribute__((visibility("default")))
int
startup_loader_provider_is_loaded(void)
{
#ifdef RTLD_NOLOAD
    void* handle = dlopen("./libStartupUnloadableTarget.so",
                          RTLD_LAZY | RTLD_NOLOAD);
    if (handle == NULL) {
        return 0;
    }
    dlclose(handle);
    return 1;
#else
    return 1;
#endif
}
