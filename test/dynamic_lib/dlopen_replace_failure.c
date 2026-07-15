#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*PeakBoolFunction)(void);
typedef void (*TargetFunction)(void);

static volatile unsigned int target_sink;

__attribute__((noinline, visibility("default"))) void
peak_dlopen_replace_failure_target(void)
{
    unsigned int value = target_sink + 1;

    for (unsigned int i = 0; i < 32; i++) {
        value = value * 33U + i;
        target_sink = value;
    }
}

static void*
required_symbol(const char* name)
{
    void* symbol;
    const char* error;

    dlerror();
    symbol = dlsym(RTLD_DEFAULT, name);
    error = dlerror();
    if (symbol == NULL || error != NULL) {
        fprintf(stderr,
                "missing required symbol %s: %s\n",
                name,
                error != NULL ? error : "not found");
        exit(EXIT_FAILURE);
    }
    return symbol;
}

static void
copy_function_pointer(void* destination, size_t destination_size, void* symbol)
{
    if (destination_size != sizeof(symbol)) {
        fprintf(stderr, "function pointer size mismatch\n");
        exit(EXIT_FAILURE);
    }
    memcpy(destination, &symbol, destination_size);
}

int
main(void)
{
    PeakBoolFunction replacement_owned = NULL;
    PeakBoolFunction uninstalled_state_clean = NULL;
    PeakBoolFunction dettach = NULL;
    TargetFunction dynamic_target = NULL;
    void* library;
    void* symbol;

    symbol = required_symbol(
        "dlopen_interceptor_test_replacement_installed_by_peak");
    copy_function_pointer(&replacement_owned,
                          sizeof(replacement_owned),
                          symbol);
    symbol = required_symbol(
        "dlopen_interceptor_test_uninstalled_replacement_state_is_clean");
    copy_function_pointer(&uninstalled_state_clean,
                          sizeof(uninstalled_state_clean),
                          symbol);
    symbol = required_symbol("dlopen_interceptor_test_dettach");
    copy_function_pointer(&dettach, sizeof(dettach), symbol);

    if (replacement_owned()) {
        fprintf(stderr, "PEAK claimed ownership after replace-fast failure\n");
        return EXIT_FAILURE;
    }
    if (!uninstalled_state_clean()) {
        fprintf(stderr, "failed dlopen replacement retained install state\n");
        return EXIT_FAILURE;
    }

    peak_dlopen_replace_failure_target();
    peak_dlopen_replace_failure_target();

    library = dlopen("./libB.so", RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        fprintf(stderr, "real dlopen failed: %s\n", dlerror());
        return EXIT_FAILURE;
    }
    dlerror();
    symbol = dlsym(library, "b_dynamic");
    if (symbol == NULL || dlerror() != NULL) {
        fprintf(stderr, "missing b_dynamic from libB.so\n");
        return EXIT_FAILURE;
    }
    copy_function_pointer(&dynamic_target, sizeof(dynamic_target), symbol);
    dynamic_target();
    if (dlclose(library) != 0) {
        fprintf(stderr, "dlclose failed: %s\n", dlerror());
        return EXIT_FAILURE;
    }

    if (!dettach()) {
        fprintf(stderr, "teardown failed after replace-fast failure\n");
        return EXIT_FAILURE;
    }
    if (replacement_owned() || !uninstalled_state_clean()) {
        fprintf(stderr, "teardown recreated ownership after failed install\n");
        return EXIT_FAILURE;
    }

    puts("dlopen_replace_failure_cleanup_ok");
    return EXIT_SUCCESS;
}
