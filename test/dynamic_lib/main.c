#define _GNU_SOURCE
#include <math.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

void normal_function(void) {
    puts("main: normal_function called");
}

int main(void) {
    // FIXME: currently, PEAK cannot intercept the dynamic loading of libB.so inside libA.so

    puts("main: start");

    dlerror(); // clear
    void *hA = dlopen("./libA.so", RTLD_NOW /*| RTLD_LOCAL*/);
    if (!hA) { fprintf(stderr, "main: dlopen libA failed: %s\n", dlerror()); exit(1); }

    // typedefs for libA’s exported functions
    typedef void (*fn_void)(void);

    fn_void a_dynamic_calls_b_dynamic =
        (fn_void)dlsym(hA, "a_dynamic_calls_b_dynamic");
    const char *e1 = dlerror();
    if (e1) { fprintf(stderr, "main: dlsym a_dynamic_calls_b_dynamic: %s\n", e1); exit(1); }

    fn_void a_dynamic_calls_a_static =
        (fn_void)dlsym(hA, "a_dynamic_calls_a_static");
    const char *e2 = dlerror();
    if (e2) { fprintf(stderr, "main: dlsym a_dynamic_calls_a_static: %s\n", e2); exit(1); }

    fn_void a_test_try_resolve_b_static =
        (fn_void)dlsym(hA, "a_test_try_resolve_b_static");
    const char *e3 = dlerror();
    if (e3) { fprintf(stderr, "main: dlsym a_test_try_resolve_b_static: %s\n", e3); exit(1); }

    // Call libA APIs (which will internally dlopen/dlsym libB and call b_dynamic)
    a_dynamic_calls_b_dynamic();
    a_dynamic_calls_a_static();
    a_test_try_resolve_b_static();

    normal_function();

    dlclose(hA);
    puts("main: end");
    return 0;
}
