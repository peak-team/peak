#include "syscall_interceptor.h"
#include "general_listener.h"

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


int syscall_interceptor_attach()
{
    GumReplaceReturn replace_check = -1;
    syscall_interceptor = gum_interceptor_obtain();

    gum_interceptor_begin_transaction(syscall_interceptor);
    hook_address = gum_find_function("close");
    if (hook_address) {
        if (peak_general_listener_attach_target_is_supported("close",
                                                             hook_address)) {
            replace_check = gum_interceptor_replace_fast(syscall_interceptor,
                                         hook_address, (gpointer)&peak_close,
                                         (gpointer*)(&original_close));
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
