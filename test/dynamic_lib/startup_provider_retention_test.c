#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef size_t (*retained_slots_fn)(void);

void startup_loader_call(void);
int startup_loader_drop_application_handle(void);
int startup_loader_provider_is_loaded(void);

int
main(void)
{
    void* address;
    retained_slots_fn retained_slots;

    dlerror();
    address = dlsym(RTLD_DEFAULT,
                    "dlopen_interceptor_test_retained_handle_slots");
    if (dlerror() != NULL || address == NULL ||
        sizeof(address) != sizeof(retained_slots)) {
        return 4;
    }
    memcpy(&retained_slots, &address, sizeof(retained_slots));
    if (retained_slots() != 1) {
        fputs("startup_provider_pin_was_not_deduplicated\n", stderr);
        return 5;
    }

    startup_loader_call();
    if (startup_loader_drop_application_handle() != 0) {
        return 2;
    }
    if (!startup_loader_provider_is_loaded()) {
        fputs("startup_provider_was_unloaded\n", stderr);
        return 3;
    }
    startup_loader_call();
    fputs("startup_provider_retained_ok\n", stderr);
    return 0;
}
