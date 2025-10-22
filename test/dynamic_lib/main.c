#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>

void a_call(void);

static void whereis(const char *name, void *p) {
    Dl_info info;
    if (dladdr(p, &info) && info.dli_fname) {
        // printf("[where] %-12s @ %-18p from %s\n", name, p, info.dli_fname);
    } else {
        // printf("[where] %-12s @ %-18p (dladdr: unknown)\n", name, p);
    }
}

int main(void) {
    whereis("main",   (void*) &main);
    whereis("a_call", (void*) &a_call);

    // puts("main: invoking a_call() for multiple times");
    a_call();
    // puts("main: done");
    return 0;
}