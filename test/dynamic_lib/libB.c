#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>

static void whereis(const char *name, void *p) {
    Dl_info info;
    if (dladdr(p, &info) && info.dli_fname) {
        // printf("[where] %-12s @ %-18p from %s\n", name, p, info.dli_fname);
    } else {
        // printf("[where] %-12s @ %-18p (dladdr: unknown)\n", name, p);
    }
}

__attribute__((visibility("default"), noinline))
void b_dynamic(void) {
    whereis("b_dynamic", (void*) &b_dynamic);
    // puts("libB: b_dynamic() says hello");
}