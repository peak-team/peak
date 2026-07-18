#ifndef PEAK_INTERNAL_EXEC_RAW_SYSCALL_H
#define PEAK_INTERNAL_EXEC_RAW_SYSCALL_H

/**
 * @file exec_raw_syscall.h
 * @brief Exec-aware raw syscall dispatch and direct syscall primitive.
 *
 * Pointer arguments are borrowed for the duration of a call.  No function in
 * this interface takes ownership of path, argv, envp, or result storage.
 */

#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @name Exec syscall adapters and dispatcher
 * @{ */

/**
 * @brief Routes a raw `execve` request through the initialized exec policy.
 *
 * Before policy priming, or when exec policy was disabled at startup, this
 * falls back to the prepublication exec path.  As with `execve()`, success
 * replaces the process image and never returns; failure returns -1 and leaves
 * the operating-system error in `errno`.
 *
 * @param[in] path Borrowed executable path.
 * @param[in] argv Borrowed argument vector.
 * @param[in] envp Borrowed environment vector.
 * @return -1 on failure; a successful exec does not return.
 */
int peak_exec_raw_syscall_execve(const char* path,
                                 char* const argv[],
                                 char* const envp[]);

/**
 * @brief Dispatches an intercepted syscall number when it is an exec syscall.
 *
 * Recognized Linux `SYS_execve` and `SYS_execveat` requests are routed through
 * their adapters and write the syscall result through @p result.  In a
 * `PEAK_EXEC_RAW_SYSCALL_STANDALONE` build, a missing weak adapter falls back
 * to `peak_exec_raw_syscall6()`.  Unrecognized requests, and every call with a
 * null @p result, are left for the caller to execute.
 *
 * @param[in] number Raw syscall number.
 * @param[in] arg1 First raw syscall argument.
 * @param[in] arg2 Second raw syscall argument.
 * @param[in] arg3 Third raw syscall argument.
 * @param[in] arg4 Fourth raw syscall argument.
 * @param[in] arg5 Fifth raw syscall argument.
 * @param[in] arg6 Sixth raw syscall argument; unused by recognized exec calls.
 * @param[out] result Caller-owned destination written only for a recognized
 *                    request; may be null.
 * @retval 1 The request was recognized and dispatched.  A successful exec
 *           does not return to report this value.
 * @retval 0 The request was not handled; @p result is unchanged.
 */
int peak_exec_raw_syscall_handle(long number,
                                 long arg1,
                                 long arg2,
                                 long arg3,
                                 long arg4,
                                 long arg5,
                                 long arg6,
                                 long* result);

#if defined(__linux__)
/**
 * @brief Routes a raw Linux `execveat` request through initialized exec policy.
 *
 * The arguments are borrowed.  As with `execveat()`, success replaces the
 * process image and never returns; failure returns -1 and sets `errno`.
 *
 * @param[in] dirfd Directory descriptor interpreted by `execveat()`.
 * @param[in] pathname Borrowed executable path.
 * @param[in] argv Borrowed argument vector.
 * @param[in] envp Borrowed environment vector.
 * @param[in] flags Linux `execveat()` flags.
 * @return -1 on failure; a successful exec does not return.
 */
int peak_exec_raw_syscall_execveat(int dirfd,
                                   const char* pathname,
                                   char* const argv[],
                                   char* const envp[],
                                   int flags);
#endif

/** @} */

#ifdef __cplusplus
}
#endif

/** @name Direct raw syscall primitive
 * @{ */

/**
 * @brief Issues a six-argument syscall without a libc syscall wrapper.
 *
 * Linux x86-64 and AArch64 use the architecture syscall instruction directly.
 * Kernel error results in [-4095, -1] are converted to -1 with `errno` set to
 * the positive error number.  Successful calls return the raw non-error result
 * and leave the existing `errno` value unchanged.  On unsupported platforms
 * the function returns -1 and sets `errno` to `ENOSYS`.
 *
 * @param[in] number Raw syscall number.
 * @param[in] arg1 First raw syscall argument.
 * @param[in] arg2 Second raw syscall argument.
 * @param[in] arg3 Third raw syscall argument.
 * @param[in] arg4 Fourth raw syscall argument.
 * @param[in] arg5 Fifth raw syscall argument.
 * @param[in] arg6 Sixth raw syscall argument.
 * @return The raw non-error syscall result, or -1 on error.
 */
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

/** @} */

#endif /* PEAK_INTERNAL_EXEC_RAW_SYSCALL_H */
