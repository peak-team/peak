#define _GNU_SOURCE
#include "syscall_interceptor.h"
#include "logging.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#undef g_printerr
#define g_printerr(...) peak_log_warn(__VA_ARGS__)

typedef int (*PeakCloseFunction)(int fd);

static _Atomic(PeakCloseFunction) peak_original_close = NULL;
static atomic_bool peak_close_interceptor_active = ATOMIC_VAR_INIT(false);

void
syscall_interceptor_initialize(void)
{
    PeakCloseFunction resolved_close = NULL;
    PeakCloseFunction expected = NULL;
    void* address;

    if (atomic_load_explicit(&peak_original_close, memory_order_acquire) != NULL) {
        return;
    }

    dlerror();
    address = dlsym(RTLD_NEXT, "close");
    if (address == NULL || dlerror() != NULL ||
        sizeof(resolved_close) != sizeof(address)) {
        return;
    }
    memcpy(&resolved_close, &address, sizeof(resolved_close));
    if (resolved_close == NULL || resolved_close == close) {
        return;
    }

    (void)atomic_compare_exchange_strong_explicit(
        &peak_original_close,
        &expected,
        resolved_close,
        memory_order_release,
        memory_order_relaxed);
}

/*
 * Interpose through the ELF loader instead of rewriting libc's close entry.
 * Older glibc releases expose __close_nocancel from the middle of close's
 * first instruction stream, so a Gum entry patch can corrupt that alternate
 * entry even when the public close symbol itself appears large enough.
 */
__attribute__((visibility("default")))
int
close(int fd)
{
    PeakCloseFunction original_close;

    if (atomic_load_explicit(&peak_close_interceptor_active,
                             memory_order_acquire) &&
        fd == STDERR_FILENO) {
        return 0;
    }

    original_close = atomic_load_explicit(&peak_original_close,
                                           memory_order_acquire);
    if (original_close != NULL) {
        return original_close(fd);
    }

#ifdef SYS_close
    return (int)syscall(SYS_close, fd);
#else
    errno = ENOSYS;
    return -1;
#endif
}

int
syscall_interceptor_attach(void)
{
    syscall_interceptor_initialize();
    if (atomic_load_explicit(&peak_original_close, memory_order_acquire) == NULL) {
        g_printerr("[peak] close interposition could not resolve the next close implementation\n");
        return -1;
    }

    atomic_store_explicit(&peak_close_interceptor_active,
                          true,
                          memory_order_release);
    return 0;
}

void
syscall_interceptor_dettach(void)
{
    atomic_store_explicit(&peak_close_interceptor_active,
                          false,
                          memory_order_release);
}
