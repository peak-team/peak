#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "internal/exec_raw_syscall.h"

extern long peak_aarch64_raw_syscall6_raw(long number,
                                          long arg1,
                                          long arg2,
                                          long arg3,
                                          long arg4,
                                          long arg5,
                                          long arg6);

int
main(void)
{
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    uint64_t written = 1;
    uint64_t observed = 0;

    if (fd == -1) {
        perror("eventfd");
        return EXIT_FAILURE;
    }
    long result = peak_aarch64_raw_syscall6_raw((long)SYS_write,
                                                (long)fd,
                                                (long)&written,
                                                (long)sizeof(written),
                                                0,
                                                0,
                                                0);
    if (result != (long)sizeof(written) ||
        read(fd, &observed, sizeof(observed)) != (ssize_t)sizeof(observed) ||
        observed != 1) {
        fprintf(stderr,
                "raw eventfd write failed: result=%ld observed=%lu\n",
                result,
                (unsigned long)observed);
        close(fd);
        return EXIT_FAILURE;
    }

    result = peak_aarch64_raw_syscall6_raw((long)SYS_close,
                                           (long)fd,
                                           0,
                                           0,
                                           0,
                                           0,
                                           0);
    if (result != 0) {
        fprintf(stderr, "raw close failed: result=%ld\n", result);
        return EXIT_FAILURE;
    }
    result = peak_aarch64_raw_syscall6_raw((long)SYS_close,
                                           (long)fd,
                                           0,
                                           0,
                                           0,
                                           0,
                                           0);
    if (result != -(long)EBADF) {
        fprintf(stderr,
                "second raw close did not return -EBADF: result=%ld\n",
                result);
        return EXIT_FAILURE;
    }

    errno = EALREADY;
    result = peak_exec_raw_syscall6((long)SYS_getpid, 0, 0, 0, 0, 0, 0);
    if (result != (long)getpid() || errno != EALREADY) {
        fprintf(stderr,
                "wrapped raw getpid changed result/errno: result=%ld errno=%d\n",
                result,
                errno);
        return EXIT_FAILURE;
    }
    errno = 0;
    result = peak_exec_raw_syscall6((long)SYS_close, -1, 0, 0, 0, 0, 0);
    if (result != -1 || errno != EBADF) {
        fprintf(stderr,
                "wrapped raw close did not map -EBADF: result=%ld errno=%d\n",
                result,
                errno);
        return EXIT_FAILURE;
    }

    printf("gum_raw_syscall_aarch64_ok\n");
    return EXIT_SUCCESS;
}
