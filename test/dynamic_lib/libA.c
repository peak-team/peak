#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

/* We will resolve this at runtime from libB */
typedef void (*b_dynamic_fn_t)(void);

/* Cache handle and symbol pointer so we dlopen/dlsym once */
static void *libb_handle = NULL;
static b_dynamic_fn_t b_dynamic_ptr = NULL;

/* Helper: load libB and resolve b_dynamic */
static void ensure_b_dynamic_loaded(void) {
    if (b_dynamic_ptr != NULL) return;

    dlerror(); /* clear */
    /* Load from current directory; adjust path as needed */
    libb_handle = dlopen("./libB.so", RTLD_NOW /*| RTLD_LOCAL*/);
    if (!libb_handle) {
        fprintf(stderr, "libA: dlopen libB failed: %s\n", dlerror());
        exit(1);
    }

    dlerror(); /* clear */
    b_dynamic_ptr = (b_dynamic_fn_t) dlsym(libb_handle, "b_dynamic");
    const char *err = dlerror();
    if (err || b_dynamic_ptr == NULL) {
        fprintf(stderr, "libA: dlsym b_dynamic failed: %s\n", err ? err : "NULL");
        exit(1);
    }
}

/* Try to prove static symbol in B is not resolvable */
static void try_resolve_b_static_once(void) {
    dlerror(); /* clear */
    void (*b_static_ptr)(void) = (void (*)(void)) dlsym(libb_handle ? libb_handle : RTLD_DEFAULT, "b_static");
    const char *err = dlerror();
    if (b_static_ptr != NULL && err == NULL) {
        /* This should not happen unless B was built/exported differently */
        fprintf(stderr, "WARNING: unexpectedly resolved b_static=%p\n", (void*)b_static_ptr);
    } else {
        fprintf(stderr, "libA: as expected, dlsym(\"b_static\") failed: %s\n", err ? err : "not found");
    }
}

/* static function in libA that calls libB's dynamic function via function pointer */
static void a_static_calls_b_dynamic(void) {
    puts("libA: a_static_calls_b_dynamic() -> calling b_dynamic() via dlsym");
    ensure_b_dynamic_loaded();
    b_dynamic_ptr();
}

/* exported function in libA that calls b_dynamic (resolved via dlopen/dlsym) */
void a_dynamic_calls_b_dynamic(void) {
    puts("libA: a_dynamic_calls_b_dynamic() -> calling b_dynamic() via dlsym");
    ensure_b_dynamic_loaded();
    b_dynamic_ptr();
}

/* exported function in libA that calls the static function above */
void a_dynamic_calls_a_static(void) {
    puts("libA: a_dynamic_calls_a_static() -> calling a_static_calls_b_dynamic()");
    a_static_calls_b_dynamic();
}

/* optional initializer to demonstrate that b_static is not resolvable */
__attribute__((constructor))
static void liba_ctor(void) {
    /* Only for demo; safe to ignore failures */
    /* Delay dlopen until first use; but we can still show that b_static can’t be found
       even after libB is loaded later. */
    fprintf(stderr, "libA: ctor; will show that b_static is not resolvable later.\n");
}

/* optional destructor to close libB (not strictly required for short-lived test) */
__attribute__((destructor))
static void liba_dtor(void) {
    if (libb_handle) {
        dlclose(libb_handle);
        libb_handle = NULL;
    }
    b_dynamic_ptr = NULL;
}

/* An exported function to explicitly test resolution of b_static */
void a_test_try_resolve_b_static(void) {
    ensure_b_dynamic_loaded(); /* make sure B is loaded */
    try_resolve_b_static_once();
}
