#ifndef __SYSCALL_INTERCEPTOR_H
#define __SYSCALL_INTERCEPTOR_H

/**
 * @file syscall_interceptor.h
 * @brief Header file for system call interception functionality.
 *
 * Provides lifecycle control for PEAK's loader-level `close` interposition.
 */

/**
 * @brief Resolve the next `close` implementation before application startup.
 *
 * Early initialization preserves the platform's normal `close` semantics even
 * when no PEAK profiling target is requested. Calls made reentrantly during
 * symbol resolution use the raw close syscall fallback.
 */
void syscall_interceptor_initialize(void);

/**
 * @brief Profileable implementation boundary for loader-interposed close.
 *
 * The exported close wrapper forwards here so PEAK_TARGET=close can attach to
 * PEAK-owned code without rewriting libc's close entry.
 */
int peak_close(int fd);

/**
 * @brief Attaches the system call interceptor.
 *
 * This function enables stderr protection in the loader-interposed `close`
 * wrapper. It does not modify libc code.
 *
 * @return 0 on success, or an error code indicating failure.
 */
int syscall_interceptor_attach(void);

/**
 * @brief Detaches the system call interceptor.
 *
 * This function disables stderr protection. The loader-level wrapper remains
 * present and transparently forwards to the next `close` implementation.
 */
void syscall_interceptor_dettach(void);

#endif /* __SYSCALL_INTERCEPTOR_H */
