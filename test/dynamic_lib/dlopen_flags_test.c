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

    /*
     * This DSO has an unresolved function relocation that is never called.
     * RTLD_LAZY must defer it, while RTLD_NOW must reject the load.  This
     * makes the test sensitive to a wrapper changing the caller's binding
     * mode during the real load.
     */
    void *lazy_unresolved_handle =
        dlopen("./libLazyUnresolved.so", RTLD_LAZY);
    failures += expect_handle("RTLD_LAZY unresolved load",
                              lazy_unresolved_handle);
    if (lazy_unresolved_handle != NULL) {
        dlclose(lazy_unresolved_handle);
    }

    dlerror();
    failures += expect_null("RTLD_NOW unresolved load",
                            dlopen("./libLazyUnresolved.so", RTLD_NOW));

#ifdef RTLD_DEEPBIND
    void *deepbind_handle = dlopen("./libB.so", RTLD_NOW | RTLD_DEEPBIND);
    failures += expect_handle("RTLD_DEEPBIND load", deepbind_handle);
#endif

    return failures == 0 ? 0 : 1;
#endif
}
