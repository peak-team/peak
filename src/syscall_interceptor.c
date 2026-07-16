#include "syscall_interceptor.h"
#include "general_listener.h"
#include "logging.h"

#undef g_printerr
#define g_printerr(...) peak_log_warn(__VA_ARGS__)

static GumInterceptor* syscall_interceptor;

static gpointer hook_address;

static int (*original_close)(int);

#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
#define PEAK_GUM_X86_MAX_REDIRECT_SIZE 16U

static gboolean
peak_address_is_in_function_range(GumAddress address,
                                  const GumMemoryRange* range)
{
    return range != NULL && address >= range->base_address &&
           address - range->base_address < range->size;
}
#endif

static gboolean
peak_close_overlaps_nocancel_entry(gpointer close_address)
{
#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    gpointer nocancel_address =
        peak_general_listener_find_function("__close_nocancel");
    GumMemoryRange close_range;
    GumMemoryRange nocancel_range;
    GumAddress close_entry;
    GumAddress alternate;

    if (close_address == NULL || nocancel_address == NULL) {
        return FALSE;
    }

    close_entry = GUM_ADDRESS(close_address);
    alternate = GUM_ADDRESS(nocancel_address);
    if (gum_process_find_function_range(close_address, &close_range) &&
        peak_address_is_in_function_range(alternate, &close_range)) {
        return TRUE;
    }
    if (gum_process_find_function_range(nocancel_address, &nocancel_range) &&
        peak_address_is_in_function_range(close_entry, &nocancel_range)) {
        return TRUE;
    }

    /* Gum 17.15.3's x86 backend may need a 16-byte entry redirect. */
    return alternate >= close_entry &&
           alternate - close_entry < PEAK_GUM_X86_MAX_REDIRECT_SIZE;
#else
    (void)close_address;
    return FALSE;
#endif
}

/**
 * @brief Custom implementation of the `close` function.
 *
 * This function overrides the default `close` behavior to perform specific actions 
 * when the file descriptor being closed matches certain conditions. In particular, 
 * it ensures that closing `STDERR_FILENO` does not proceed further by returning 0.
 * For all other file descriptors, it delegates the operation to the original 
 * `close` function, `original_close`.
 *
 * @param fd The file descriptor to be closed.
 * 
 * @return 0 if the file descriptor is `STDERR_FILENO`, otherwise the return value of the `original_close` function.
 */
static int
peak_close(int fd) {
    //g_printerr ("close called on %d\n", fd);
    if (fd == STDERR_FILENO) {
        return 0;
    }
    return original_close(fd);
    //return 0;
}


int syscall_interceptor_attach()
{
    GumReplaceReturn replace_check = -1;
    syscall_interceptor = gum_interceptor_obtain();

    gum_interceptor_begin_transaction(syscall_interceptor);
    hook_address = peak_general_listener_find_function("close");
    if (hook_address) {
        if (peak_close_overlaps_nocancel_entry(hook_address)) {
            g_printerr("[peak] skipping close support hook: close and __close_nocancel have overlapping or nearby entries that a wide Gum redirect could overwrite\n");
            hook_address = NULL;
        } else if (peak_general_listener_support_attach_target_is_supported(
                "close", hook_address)) {
            replace_check = gum_interceptor_replace_fast(syscall_interceptor,
                                         hook_address, (gpointer)&peak_close,
                                         (gpointer*)(&original_close),
                                         NULL);
        } else {
            hook_address = NULL;
        }
    }
    gum_interceptor_end_transaction(syscall_interceptor);
    return replace_check;
}

void syscall_interceptor_dettach()
{
    if (syscall_interceptor == NULL || hook_address == NULL) {
        return;
    }

    gum_interceptor_begin_transaction(syscall_interceptor);
    gum_interceptor_revert(syscall_interceptor, hook_address);
    gum_interceptor_end_transaction(syscall_interceptor);
    if (!gum_interceptor_flush(syscall_interceptor)) {
        g_printerr("[peak] syscall interceptor teardown did not flush; leaving syscall interceptor state alive\n");
        return;
    }
    hook_address = NULL;
}
