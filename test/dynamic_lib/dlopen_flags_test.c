#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>

static int expect_null(const char *label, void *handle) {
    if (handle == NULL) {
        return 0;
    }

    fprintf(stderr, "%s unexpectedly succeeded\n", label);
    return 1;
}

static int expect_handle(const char *label, void *handle) {
    if (handle != NULL) {
        return 0;
    }

    const char *err = dlerror();
    fprintf(stderr, "%s failed: %s\n", label, err ? err : "unknown dlopen error");
    return 1;
}

int main(void) {
#ifndef RTLD_NOLOAD
    fprintf(stderr, "RTLD_NOLOAD is unavailable on this platform\n");
    return 77;
#else
    int failures = 0;

    dlerror();
    failures += expect_null("RTLD_NOLOAD before load",
                            dlopen("./libB.so", RTLD_NOLOAD | RTLD_LAZY));

    void *lazy_handle = dlopen("./libB.so", RTLD_LAZY);
    failures += expect_handle("RTLD_LAZY load", lazy_handle);

    void *noload_handle = dlopen("./libB.so", RTLD_NOLOAD | RTLD_NOW);
    failures += expect_handle("RTLD_NOLOAD after load", noload_handle);

    void *now_handle = dlopen("./libB.so", RTLD_NOW);
    failures += expect_handle("RTLD_NOW load", now_handle);

#ifdef RTLD_DEEPBIND
    void *deepbind_handle = dlopen("./libB.so", RTLD_NOW | RTLD_DEEPBIND);
    failures += expect_handle("RTLD_DEEPBIND load", deepbind_handle);
#endif

    return failures == 0 ? 0 : 1;
#endif
}
