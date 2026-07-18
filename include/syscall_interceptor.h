#ifndef PEAK_SYSCALL_INTERCEPTOR_H
#define PEAK_SYSCALL_INTERCEPTOR_H

/**
 * @file syscall_interceptor.h
 * @brief Process `close()` interception used to keep standard error open.
 *
 * The installed wrapper returns success without closing `STDERR_FILENO`; all
 * other descriptors are passed to the original `close()` and preserve its
 * return value.  The Gum interceptor and hook state remain module-owned.
 */

#include "frida-gum.h"

#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @name Interceptor lifecycle
 * @{ */

/**
 * @brief Attempts to replace the supported `close()` entry point.
 *
 * PEAK obtains a module-owned Gum interceptor, resolves `close`, rejects an
 * unsafe overlap with `__close_nocancel`, checks support-hook policy, and then
 * attempts a fast replacement.  A rejected or missing entry point is left
 * unmodified.
 *
 * @return The `GumReplaceReturn` value, as an `int`, when replacement is
 *         attempted; -1 when `close` is missing, overlaps the no-cancel entry,
 *         or is not a supported target.  Any obtained interceptor reference
 *         remains owned and retained by this module.
 */
int syscall_interceptor_attach(void);

/**
 * @brief Reverts and flushes the installed `close()` replacement.
 *
 * The function is a no-op if no interceptor or hook address is present.  A
 * failed Gum flush is logged and leaves the hook address and interceptor state
 * alive for safety.  A successful flush clears the hook address; the Gum
 * interceptor reference remains retained for process lifetime.
 */
void syscall_interceptor_dettach(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* PEAK_SYSCALL_INTERCEPTOR_H */
