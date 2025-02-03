#ifndef __SYSCALL_INTERCEPTOR_H
#define __SYSCALL_INTERCEPTOR_H

/**
 * @file syscall_interceptor.h
 * @brief Header file for system call interception functionality.
 *
 * Provides functions to attach and detach an interceptor for 
 * system calls such as `close`.
 */

#include "frida-gum.h"

#include <unistd.h>

/**
 * @brief Attaches the system call interceptor.
 *
 * This function initializes and attaches a system call interceptor 
 * for the `close` function. It obtains a GumInterceptor instance, 
 * locates the `close` function's memory address, and replaces it with 
 * a custom implementation. 
 *
 * @return 0 on success, or an error code indicating failure.
 */
int syscall_interceptor_attach(void);

/**
 * @brief Detaches the system call interceptor.
 *
 * This function reverts the replacement of the intercepted system call
 * (e.g., `close`) and releases the resources associated with the interceptor.
 */
void syscall_interceptor_dettach(void);

#endif /* __SYSCALL_INTERCEPTOR_H */
