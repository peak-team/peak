#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef int (*lazy_profile_target_fn)(int);

int
main(void)
{
    dlerror();
    void* handle = dlopen("./liblazy_undefined.so", RTLD_LAZY | RTLD_LOCAL);
    if (handle == NULL) {
        const char* error = dlerror();
        fprintf(stderr,
                "RTLD_LAZY dlopen failed: %s\n",
                error != NULL ? error : "unknown error");
        return 1;
    }

    dlerror();
    lazy_profile_target_fn target =
        (lazy_profile_target_fn)dlsym(handle, "lazy_profile_target");
    const char* error = dlerror();
    if (error != NULL || target == NULL) {
        fprintf(stderr,
                "dlsym lazy_profile_target failed: %s\n",
                error != NULL ? error : "unknown error");
        return 1;
    }

    int result = 0;
    for (int i = 0; i < 1000; i++) {
        result += target(i);
        usleep(1000);
    }

    printf("lazy_dlopen_ok result=%d\n", result);
    return 0;
}
