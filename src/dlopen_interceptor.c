#include "general_listener.h"
#include "dlopen_interceptor.h"

static GumInterceptor* dlopen_interceptor;
extern GumInterceptor* interceptor;
extern GumInvocationListener** array_listener;
extern gpointer* hook_address;
static gpointer* dlopen_hook_address = NULL;
extern size_t peak_hook_address_count;
extern char** peak_hook_strings;
extern char** peak_demangled_strings;

static void* (*original_dlopen)(const char *filename, int flags);

static void*
peak_dlopen(const char *filename, int flags) {
    // Clear any existing error
    dlerror();

    void *handle = original_dlopen(filename, flags);
    const char *err = dlerror();
    // If dlopen failed or no filename, don’t do rescan
    if (err != NULL || handle == NULL || filename == NULL) {
        if (err != NULL) {
            g_printerr("dlopen error: %s\n", err);
        }
        return handle;
    }

    gum_interceptor_ignore_current_thread(interceptor);
    gum_interceptor_begin_transaction(interceptor);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i]) {
            // If already hooked, skip rescan of the target function
            continue;
        }

        // TODO: Consider using dlsym to find the function in the newly loaded library
        // TODO: Consider refactor code with cuda_interceptor.c
        hook_address[i] = gum_find_function(peak_hook_strings[i]);
        if (!hook_address[i]) {
            // If not found, try to find by demangling all symbols and comparing
            gchar* truncate_hook = g_strdup_printf("*%s*", peak_hook_strings[i]);
            GArray* addresses = gum_find_functions_matching(truncate_hook);
            for (gsize j = 0; j < addresses->len; j++) {
                gpointer addr = g_array_index(addresses, gpointer, j);
                gchar* mangled = gum_symbol_name_from_address(addr);
                gboolean function_match = FALSE;
                if (!mangled) continue;
            
                char* demangled = cxa_demangle(mangled);
                g_free(mangled);
                if (!demangled) continue;

                gchar* function_name = extract_function_name(demangled);
                if (strcmp(peak_hook_strings[i], function_name) == 0) {
                    hook_address[i] = addr;
                    peak_demangled_strings[i] = g_strdup(demangled);
                    function_match = TRUE;
                }
                free(demangled);
                free(function_name);

                if (function_match) {
                    break;
                }
            }
            g_array_free(addresses, TRUE);
            g_free(truncate_hook);
        } else {
            char* demangled = cxa_demangle(peak_hook_strings[i]);
            peak_demangled_strings[i] = g_strdup(demangled);
            free(demangled);
        }
        
        if (hook_address[i]) {
            // g_printerr ("%s address = %p\n", peak_hook_strings[i], hook_address[i]);
            array_listener[i] = g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);
            PEAKGENERAL_LISTENER(array_listener[i])->hook_id = i;
            gum_interceptor_attach(interceptor,
                                   hook_address[i],
                                   array_listener[i],
                                   NULL);
        }
    }
    gum_interceptor_end_transaction(interceptor);
    gum_interceptor_unignore_current_thread(interceptor);

    // FIXME: didn't add peak_general_overhead_bootstrapping
    
    return handle;
}

int dlopen_interceptor_attach()
{
    GumReplaceReturn replace_check = -1;
    dlopen_interceptor = gum_interceptor_obtain();

    gum_interceptor_begin_transaction(dlopen_interceptor);
    dlopen_hook_address = gum_find_function("dlopen");
    if (dlopen_hook_address) {
        replace_check = gum_interceptor_replace_fast(dlopen_interceptor, 
                        dlopen_hook_address, (gpointer)&peak_dlopen,
                                     (gpointer*)(&original_dlopen));
    }
    gum_interceptor_end_transaction(dlopen_interceptor);
    return replace_check;
}

void dlopen_interceptor_dettach()
{
    gum_interceptor_revert(dlopen_interceptor, dlopen_hook_address);
    g_object_unref(dlopen_interceptor);
}