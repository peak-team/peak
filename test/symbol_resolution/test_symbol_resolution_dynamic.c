#define _GNU_SOURCE

#include "dlopen_interceptor.h"
#include "general_listener.h"
#include "internal/general_listener/selector_test_hooks.h"
#include "internal/target_resolver.h"

#include <dlfcn.h>
#if defined(__linux__)
#include <link.h>
#endif
#include <stdio.h>
#include <stdlib.h>
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
    int ambiguous_sequential;
    int exact_instance;
    int mixed;
    int selector_env_snapshot;
    int local;
    int plain_c;
    int legacy;
    int legacy_disabled;
    PeakDlopenSelectorDiagnostics selector_diagnostics;

    if (argc != 2 ||
        (strcmp(argv[1], "unique") != 0 && strcmp(argv[1], "ambiguous") != 0 &&
         strcmp(argv[1], "mixed") != 0 && strcmp(argv[1], "local") != 0 &&
         strcmp(argv[1], "plain-c") != 0 &&
         strcmp(argv[1], "legacy") != 0 &&
         strcmp(argv[1], "legacy-disabled") != 0 &&
         strcmp(argv[1], "ambiguous-sequential") != 0 &&
         strcmp(argv[1], "exact-instance") != 0 &&
         strcmp(argv[1], "env-snapshot") != 0 &&
         strcmp(argv[1], "startup-unique") != 0 &&
         strcmp(argv[1], "startup-ambiguous") != 0 &&
         strcmp(argv[1], "startup-plain-c") != 0)) {
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
    if (strcmp(argv[1], "startup-plain-c") == 0) {
        PeakTargetResolverDiagnostics startup_diagnostics;

        invoke = (fixture_invoke_fn)dlsym(RTLD_DEFAULT,
                                          "peak_symbol_fixture_invoke");
        peak_target_resolver_get_diagnostics(&startup_diagnostics);
        if (invoke == NULL || invoke() == 0 ||
            peak_general_listener_test_call_count(0) == 0 ||
            startup_diagnostics.module_passes != 0 ||
            peak_general_listener_test_startup_selector_batches() != 0) {
            fprintf(stderr, "not ok - startup C target entered resolver path\n");
            return 1;
        }
        puts("startup_plain_c_fast_path_ok");
        return 0;
    }
    ambiguous = strcmp(argv[1], "ambiguous") == 0;
    ambiguous_sequential = strcmp(argv[1], "ambiguous-sequential") == 0;
    ambiguous |= ambiguous_sequential;
    exact_instance = strcmp(argv[1], "exact-instance") == 0;
    mixed = strcmp(argv[1], "mixed") == 0;
    local = strcmp(argv[1], "local") == 0;
    plain_c = strcmp(argv[1], "plain-c") == 0;
    legacy = strcmp(argv[1], "legacy") == 0;
    legacy_disabled = strcmp(argv[1], "legacy-disabled") == 0;
    selector_env_snapshot = strcmp(argv[1], "env-snapshot") == 0;
    dlopen_interceptor_test_set_manual_drain(TRUE);
    dlopen_interceptor_test_reset_selector_diagnostics();
    if ((mixed ? !load_fixture(PEAK_TEST_SYMBOL_MODULE_MISSING, &module_a)
               : !load_fixture(PEAK_TEST_SYMBOL_MODULE_A, &module_a)) ||
        ((ambiguous || exact_instance) && !ambiguous_sequential &&
         !(exact_instance
               ? (module_b = dlopen(PEAK_TEST_SYMBOL_MODULE_B,
                                    RTLD_NOW | RTLD_NOLOAD)) != NULL
               : load_fixture(PEAK_TEST_SYMBOL_MODULE_B, &module_b)))) {
        return 1;
    }
    if (!dlopen_interceptor_test_enqueue_loaded_dynamic_attach(
            mixed ? PEAK_TEST_SYMBOL_MODULE_MISSING : PEAK_TEST_SYMBOL_MODULE_A,
            module_a) ||
        (ambiguous && !ambiguous_sequential &&
         !dlopen_interceptor_test_enqueue_loaded_dynamic_attach(
                          PEAK_TEST_SYMBOL_MODULE_B, module_b))) {
        fprintf(stderr, "not ok - dynamic attach queue rejected loaded module\n");
        return 1;
    }
    if (selector_env_snapshot &&
        unsetenv("PEAK_ENABLE_CXX_SYMBOL_SCAN") != 0) {
        fprintf(stderr, "not ok - unable to clear selector environment\n");
        return 1;
    }
    if (ambiguous_sequential) {
        dlopen_interceptor_test_drain_dynamic_attach_queue();
        if (!load_fixture(PEAK_TEST_SYMBOL_MODULE_B, &module_b) ||
            !dlopen_interceptor_test_enqueue_loaded_dynamic_attach(
                PEAK_TEST_SYMBOL_MODULE_B, module_b)) {
            return 1;
        }
    }
    usleep(10000);
    dlopen_interceptor_test_drain_dynamic_attach_queue();
    dlopen_interceptor_test_drain_dynamic_attach_queue();
    dlopen_interceptor_get_dynamic_attach_diagnostics(&diagnostics);
    dlopen_interceptor_test_get_selector_diagnostics(&selector_diagnostics);
    if (plain_c && (selector_diagnostics.deferred_module_sync_drains != 0 ||
                    selector_diagnostics.selector_resolver_batches != 0)) {
        fprintf(stderr, "not ok - ordinary C target entered selector path\n");
        return 1;
    }
    if (legacy && (selector_diagnostics.deferred_module_sync_drains == 0 ||
                   selector_diagnostics.selector_resolver_batches != 1)) {
        fprintf(stderr, "not ok - legacy selector skipped synced resolver batch\n");
        return 1;
    }
    if (ambiguous) {
        invoke = (fixture_invoke_fn)dlsym(module_a,
                                          "peak_symbol_fixture_invoke");
        if (diagnostics.retained_handles != 0 || invoke == NULL ||
            invoke() == 0 || peak_general_listener_test_call_count(0) != 0) {
            fprintf(stderr, "not ok - terminal selector retained a hooked module\n");
            return 1;
        }
    } else if (legacy_disabled) {
        invoke = (fixture_invoke_fn)dlsym(module_a, "peak_symbol_fixture_invoke");
        if (diagnostics.retained_handles != 0 || invoke == NULL ||
            invoke() == 0 || peak_general_listener_test_call_count(0) != 0) {
            fprintf(stderr, "not ok - legacy selector attached without opt-in\n");
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
                         : local ? "peak_symbol_fixture_invoke_local"
                                 : "peak_symbol_fixture_invoke");
            if (local &&
                dlsym(module_a, "_ZN9peak_test20local_dynamic_targetEi") != NULL) {
                fprintf(stderr, "not ok - hidden local target unexpectedly exported\n");
                return 1;
            }
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
         selector_env_snapshot ? "dynamic_selector_env_snapshot_ok" :
         legacy_disabled ? "dynamic_legacy_disabled_ok" :
         local ? "dynamic_local_selector_ok" :
                 "dynamic_unique_selector_ok");
    return 0;
}
