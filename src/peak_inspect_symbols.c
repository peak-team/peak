#include "internal/target_resolver.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <string.h>

/* cxx_utils also serves report formatting; inspection never truncates names. */
bool peak_truncate_function_name = false;

static gboolean
peak_inspect_load_module(const char* module, GPtrArray* handles)
{
    void* handle = dlopen(module, RTLD_NOW | RTLD_LOCAL);

    if (handle == NULL) {
        fprintf(stderr, "unable to load module: %s\n", module);
        return FALSE;
    }
    g_ptr_array_add(handles, handle);
    return TRUE;
}

static void
peak_inspect_close_module(gpointer data)
{
    if (data != NULL) {
        dlclose(data);
    }
}

int
main(int argc, char** argv)
{
    PeakTargetResolution resolution = {0};
    PeakTargetResolveResult result;
    const char* module;
    char* module_copy = NULL;
    GPtrArray* handles;
    int argument = 2;
    int module_arguments_end;

    if (argc < 3 || strcmp(argv[1], "inspect-symbols") != 0) {
        fprintf(stderr,
                "usage: peak inspect-symbols [--module PATH]... <selector>\n");
        return 2;
    }
    while (argument < argc - 1 && strcmp(argv[argument], "--module") == 0) {
        argument += 2;
    }
    if (argument != argc - 1 || strcmp(argv[argument], "--module") == 0 ||
        !peak_target_resolver_validate_selector(argv[argument])) {
        fprintf(stderr,
                "usage: peak inspect-symbols [--module PATH]... <selector>\n");
        return 2;
    }
    module_arguments_end = argument;
    gum_init_embedded();
    handles = g_ptr_array_new_with_free_func(peak_inspect_close_module);
    for (argument = 2; argument < module_arguments_end; argument += 2) {
        if (!peak_inspect_load_module(argv[argument + 1], handles)) {
            g_ptr_array_unref(handles);
            gum_deinit_embedded();
            return 2;
        }
    }
    argument = module_arguments_end;
    module = argv[argument];
    if (!peak_target_resolver_dup_selector_module(module, &module_copy)) {
        g_ptr_array_unref(handles);
        gum_deinit_embedded();
        return 2;
    }
    if (module_copy != NULL) {
        if (!peak_inspect_load_module(module_copy, handles)) {
            g_free(module_copy);
            g_ptr_array_unref(handles);
            gum_deinit_embedded();
            return 2;
        }
    }
    result = peak_target_resolver_resolve(argv[argument], NULL, TRUE,
                                          &resolution);
    peak_target_resolver_print(stdout, argv[argument], &resolution);
    peak_target_resolution_clear(&resolution);
    g_ptr_array_unref(handles);
    g_free(module_copy);
    gum_deinit_embedded();
    return result == PEAK_TARGET_RESOLVE_UNIQUE ? 0 :
           result == PEAK_TARGET_RESOLVE_INVALID ? 2 : 1;
}
