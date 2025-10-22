#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>

void b_dynamic(void);

static void whereis(const char *name, void *p) {
    Dl_info info;
    if (dladdr(p, &info) && info.dli_fname) {
        // printf("[where] %-12s @ %-18p from %s\n", name, p, info.dli_fname);
    } else {
        // printf("[where] %-12s @ %-18p (dladdr: unknown)\n", name, p);
    }
}

__attribute__((visibility("default"), noinline))
void a_call(void) {
    whereis("a_call",   (void*) &a_call);
    whereis("b_dynamic",(void*) &b_dynamic);
    // puts("libA: calling b_dynamic() for 5 times");
    for (int i = 0; i < 5; i++) b_dynamic();
}