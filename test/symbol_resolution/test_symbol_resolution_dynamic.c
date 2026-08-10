#define _GNU_SOURCE

#include "dlopen_interceptor.h"
#include "general_listener.h"

#include <dlfcn.h>
#if defined(__linux__)
#include <link.h>
#endif
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef int (*fixture_invoke_fn)(void);
typedef int (*widget_func_fn)(int, double);

static int
load_fixture(const char* path, void** handle_out)
{
    *handle_out = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (*handle_out == NULL) {
        fprintf(stderr, "not ok - unable to load fixture %s\n", path);
        return 0;
    }
    return 1;
}

int
main(int argc, char** argv)
{
    PeakDlopenDynamicAttachDiagnostics diagnostics;
    void* module_a = NULL;
    void* module_b = NULL;
    fixture_invoke_fn invoke;
    int ambiguous;
    int exact_instance;
    int mixed;

    if (argc != 2 ||
        (strcmp(argv[1], "unique") != 0 && strcmp(argv[1], "ambiguous") != 0 &&
         strcmp(argv[1], "mixed") != 0 &&
         strcmp(argv[1], "exact-instance") != 0 &&
         strcmp(argv[1], "startup-unique") != 0 &&
         strcmp(argv[1], "startup-ambiguous") != 0)) {
        return 2;
    }
    if (strcmp(argv[1], "startup-unique") == 0) {
        invoke = (fixture_invoke_fn)dlsym(RTLD_DEFAULT,
                                          "peak_symbol_fixture_invoke");
        if (invoke == NULL || invoke() == 0 ||
            peak_general_listener_test_call_count(0) == 0) {
            fprintf(stderr, "not ok - startup selector did not hook the requested overload\n");
            return 1;
        }
        puts("startup_unique_selector_ok");
        return 0;
    }
    if (strcmp(argv[1], "startup-ambiguous") == 0) {
        invoke = (fixture_invoke_fn)dlsym(RTLD_DEFAULT,
                                          "peak_symbol_fixture_invoke");
        if (invoke == NULL || invoke() == 0 ||
            peak_general_listener_test_call_count(0) != 0) {
            fprintf(stderr, "not ok - ambiguous startup selector attached a target\n");
            return 1;
        }
        puts("startup_ambiguous_terminal_ok");
        return 0;
    }
    ambiguous = strcmp(argv[1], "ambiguous") == 0;
    exact_instance = strcmp(argv[1], "exact-instance") == 0;
    mixed = strcmp(argv[1], "mixed") == 0;
    dlopen_interceptor_test_set_manual_drain(TRUE);
    if ((mixed ? !load_fixture(PEAK_TEST_SYMBOL_MODULE_MISSING, &module_a)
               : !load_fixture(PEAK_TEST_SYMBOL_MODULE_A, &module_a)) ||
        ((ambiguous || exact_instance) &&
         !(exact_instance
               ? (module_b = dlopen(PEAK_TEST_SYMBOL_MODULE_B,
                                    RTLD_NOW | RTLD_NOLOAD)) != NULL
               : load_fixture(PEAK_TEST_SYMBOL_MODULE_B, &module_b)))) {
        return 1;
    }
    if (!dlopen_interceptor_test_enqueue_loaded_dynamic_attach(
            mixed ? PEAK_TEST_SYMBOL_MODULE_MISSING : PEAK_TEST_SYMBOL_MODULE_A,
            module_a) ||
        (ambiguous && !dlopen_interceptor_test_enqueue_loaded_dynamic_attach(
                          PEAK_TEST_SYMBOL_MODULE_B, module_b))) {
        fprintf(stderr, "not ok - dynamic attach queue rejected loaded module\n");
        return 1;
    }
    usleep(10000);
    dlopen_interceptor_test_drain_dynamic_attach_queue();
    dlopen_interceptor_test_drain_dynamic_attach_queue();
    dlopen_interceptor_get_dynamic_attach_diagnostics(&diagnostics);
    if (ambiguous) {
        invoke = (fixture_invoke_fn)dlsym(module_a,
                                          "peak_symbol_fixture_invoke");
        if (diagnostics.retained_handles != 0 || invoke == NULL ||
            invoke() == 0 || peak_general_listener_test_call_count(0) != 0) {
            fprintf(stderr, "not ok - terminal selector retained a hooked module\n");
            return 1;
        }
    } else {
        if (diagnostics.retained_handles == 0) {
            fprintf(stderr,
                    "not ok - module-qualified selector did not attach (drained=%llu)\n",
                    diagnostics.drained);
            return 1;
        }
        if (exact_instance) {
            widget_func_fn function_a = (widget_func_fn)dlsym(
                module_a, "_ZN9peak_test6Widget4funcEid");
            widget_func_fn function_b = (widget_func_fn)dlsym(
                module_b, "_ZN9peak_test6Widget4funcEid");
#if defined(__linux__)
            struct link_map* map_a = NULL;
            struct link_map* map_b = NULL;
#endif

            if (function_a == NULL || function_b == NULL ||
                function_a == function_b) {
                fprintf(stderr, "not ok - distinct fixture functions unavailable\n");
                return 1;
            }
#if defined(__linux__)
            if (dlinfo(module_a, RTLD_DI_LINKMAP, &map_a) != 0 ||
                dlinfo(module_b, RTLD_DI_LINKMAP, &map_b) != 0 ||
                map_a == map_b) {
                fprintf(stderr, "not ok - fixture modules lack distinct identities\n");
                return 1;
            }
#endif
            (void)function_b(1, 2.0);
            if (peak_general_listener_test_call_count(0) != 0) {
                fprintf(stderr, "not ok - interposed module received requested hook\n");
                return 1;
            }
            (void)function_a(1, 2.0);
            if (peak_general_listener_test_call_count(0) == 0) {
                fprintf(stderr, "not ok - requested module did not receive hook\n");
                return 1;
            }
        } else {
            invoke = (fixture_invoke_fn)dlsym(
                module_a, mixed ? "peak_symbol_missing_dynamic_target"
                                : "peak_symbol_fixture_invoke");
            if (invoke == NULL || invoke() == 0) {
                fprintf(stderr, "not ok - fixture invocation failed\n");
                return 1;
            }
            if (peak_general_listener_test_call_count(0) == 0) {
                fprintf(stderr, "not ok - dynamic selector retained a module without a callback\n");
                return 1;
            }
        }
    }
    dlopen_interceptor_test_set_manual_drain(FALSE);
    puts(ambiguous ? "dynamic_ambiguous_terminal_ok" :
         mixed ? "dynamic_mixed_targets_ok" :
         exact_instance ? "dynamic_exact_instance_ok" :
                 "dynamic_unique_selector_ok");
    return 0;
}
