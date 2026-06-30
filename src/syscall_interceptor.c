#include "syscall_interceptor.h"
#include "general_listener.h"
#include "peak_logging.h"
#include "syscall_interceptor_policy.h"

#include <stdint.h>

#undef g_printerr
#define g_printerr(...) peak_log_warn(__VA_ARGS__)

static GumInterceptor* syscall_interceptor;

static gpointer hook_address;

static int (*original_close)(int);

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

static int
peak_syscall_interceptor_has_inline_close_syscall(gpointer address)
{
    const uint8_t* code = (const uint8_t*)address;

    if (code == NULL) {
        return 0;
    }

    /*
     * Some libc builds export close as the same tiny nocancel wrapper used by
     * stdio internals, or put that private entrypoint inside the first bytes of
     * the public close implementation:
     *
     *   mov $SYS_close,%eax; syscall
     *
     * Frontera's glibc starts __close_nocancel nine bytes after close.
     * Replacing close can therefore overwrite bytes that fclose() reaches via
     * the private label. Skip the stderr-protection wrapper instead of risking
     * a process-wide close crash.
     */
    return peak_syscall_interceptor_has_inline_close_syscall_bytes(
        code, PEAK_CLOSE_PATCH_HAZARD_SCAN_BYTES);
}


int syscall_interceptor_attach()
{
    GumReplaceReturn replace_check = -1;
    syscall_interceptor = gum_interceptor_obtain();

    gum_interceptor_begin_transaction(syscall_interceptor);
    hook_address = peak_general_listener_find_function("close");
    if (hook_address) {
        if (peak_syscall_interceptor_has_inline_close_syscall(hook_address)) {
            peak_log_info("[peak] skipping close support wrapper: resolved close is an inline syscall stub\n");
            hook_address = NULL;
            replace_check = 0;
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
