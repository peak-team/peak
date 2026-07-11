#ifndef PEAK_INTERNAL_EXEC_RAW_SYSCALL_H
#define PEAK_INTERNAL_EXEC_RAW_SYSCALL_H

#include <errno.h>

int peak_exec_raw_syscall_execve(const char* path,
                                 char* const argv[],
                                 char* const envp[]);

int peak_exec_raw_syscall_handle(long number,
                                 long arg1,
                                 long arg2,
                                 long arg3,
                                 long arg4,
                                 long arg5,
                                 long arg6,
                                 long* result);

#if defined(__linux__)
int peak_exec_raw_syscall_execveat(int dirfd,
                                   const char* pathname,
                                   char* const argv[],
                                   char* const envp[],
                                   int flags);
#endif

static inline long
peak_exec_raw_syscall6(long number,
                       long arg1,
                       long arg2,
                       long arg3,
                       long arg4,
                       long arg5,
                       long arg6)
{
#if defined(__linux__) && defined(__x86_64__)
    long result;
    register long r10 __asm__("r10") = arg4;
    register long r8 __asm__("r8") = arg5;
    register long r9 __asm__("r9") = arg6;

    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number),
                       "D"(arg1),
                       "S"(arg2),
                       "d"(arg3),
                       "r"(r10),
                       "r"(r8),
                       "r"(r9)
                     : "rcx", "r11", "memory");
    if (result < 0 && result >= -4095) {
        errno = (int)-result;
        return -1;
    }
    return result;
#elif defined(__linux__) && defined(__aarch64__)
    register long x0 __asm__("x0") = arg1;
    register long x1 __asm__("x1") = arg2;
    register long x2 __asm__("x2") = arg3;
    register long x3 __asm__("x3") = arg4;
    register long x4 __asm__("x4") = arg5;
    register long x5 __asm__("x5") = arg6;
    register long x8 __asm__("x8") = number;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1),
                       "r"(x2),
                       "r"(x3),
                       "r"(x4),
                       "r"(x5),
                       "r"(x8)
                     : "cc", "memory");
    if (x0 < 0 && x0 >= -4095) {
        errno = (int)-x0;
        return -1;
    }
    return x0;
#else
    (void)number;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    (void)arg6;
    errno = ENOSYS;
    return -1;
#endif
}

#endif
