#define _GNU_SOURCE
#include "logging.h"

#include <errno.h>
#include <unistd.h>

static int interposed_syscall_called;

/*
 * A loader-time close fallback must not call libc's exported syscall symbol:
 * libpeak interposes that symbol itself, before its process decision cache is
 * initialized. This hostile definition makes such a recursive call visible.
 */
long
syscall(long number, ...)
{
    (void)number;
    interposed_syscall_called = 1;
    errno = ENOSYS;
    return -1;
}

void
peak_log_message(PeakVerbosity level, const char* format, ...)
{
    (void)level;
    (void)format;
}

int
main(void)
{
    static const char marker[] = "close_raw_fallback_ok\n";

    errno = 0;
    if (close(-1) != -1 || errno != EBADF || interposed_syscall_called) {
        return 1;
    }
    return write(STDOUT_FILENO, marker, sizeof(marker) - 1) ==
                   (ssize_t)(sizeof(marker) - 1)
               ? 0
               : 1;
}
