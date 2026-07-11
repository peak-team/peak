#define _GNU_SOURCE
#include "internal/exec_raw_syscall.h"

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

/* Standalone detach tests link signal_policy without the exec interceptor. */
#if defined(PEAK_EXEC_RAW_SYSCALL_STANDALONE)
extern int peak_exec_raw_syscall_execve(const char*, char* const[], char* const[])
    __attribute__((weak));
#if defined(__linux__)
extern int peak_exec_raw_syscall_execveat(int,
                                          const char*,
                                          char* const[],
                                          char* const[],
                                          int) __attribute__((weak));
#endif
#endif

int
peak_exec_raw_syscall_handle(long number,
                             long arg1,
                             long arg2,
                             long arg3,
                             long arg4,
                             long arg5,
                             long arg6,
                             long* result)
{
    (void)arg6;
    if (result == NULL) {
        return 0;
    }

#if defined(__linux__) && defined(SYS_execve)
    if (number == SYS_execve) {
#if !defined(PEAK_EXEC_RAW_SYSCALL_STANDALONE)
        *result = peak_exec_raw_syscall_execve(
            (const char*)(uintptr_t)arg1,
            (char* const*)(uintptr_t)arg2,
            (char* const*)(uintptr_t)arg3);
#else
        if (peak_exec_raw_syscall_execve != NULL) {
            *result = peak_exec_raw_syscall_execve(
                (const char*)(uintptr_t)arg1,
                (char* const*)(uintptr_t)arg2,
                (char* const*)(uintptr_t)arg3);
        } else {
            *result = peak_exec_raw_syscall6(number,
                                             arg1, arg2, arg3, 0, 0, 0);
        }
#endif
        return 1;
    }
#endif
#if defined(__linux__) && defined(SYS_execveat)
    if (number == SYS_execveat) {
#if !defined(PEAK_EXEC_RAW_SYSCALL_STANDALONE)
        *result = peak_exec_raw_syscall_execveat(
            (int)arg1,
            (const char*)(uintptr_t)arg2,
            (char* const*)(uintptr_t)arg3,
            (char* const*)(uintptr_t)arg4,
            (int)arg5);
#else
        if (peak_exec_raw_syscall_execveat != NULL) {
            *result = peak_exec_raw_syscall_execveat(
                (int)arg1,
                (const char*)(uintptr_t)arg2,
                (char* const*)(uintptr_t)arg3,
                (char* const*)(uintptr_t)arg4,
                (int)arg5);
        } else {
            *result = peak_exec_raw_syscall6(number,
                                             arg1, arg2, arg3, arg4, arg5, 0);
        }
#endif
        return 1;
    }
#endif
    return 0;
}
