#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile unsigned long close_interposition_sink;

__attribute__((noinline, visibility("default")))
void
peak_close_interposition_target(void)
{
    close_interposition_sink++;
}

int
main(int argc, char** argv)
{
    static const char marker[] = "close_interposition_active_ok\n";
    static const char inactive_marker[] = "close_interposition_inactive_ok\n";
    int duplicate_stderr;

    if (argc == 2 && strcmp(argv[1], "inactive") == 0) {
        if (close(STDERR_FILENO) != 0) {
            return EXIT_FAILURE;
        }
        errno = 0;
        if (fcntl(STDERR_FILENO, F_GETFD) != -1 || errno != EBADF ||
            write(STDOUT_FILENO,
                  inactive_marker,
                  sizeof(inactive_marker) - 1) !=
                (ssize_t)(sizeof(inactive_marker) - 1)) {
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    peak_close_interposition_target();

    duplicate_stderr = dup(STDERR_FILENO);
    if (duplicate_stderr < 0 || close(duplicate_stderr) != 0) {
        return EXIT_FAILURE;
    }
    errno = 0;
    if (fcntl(duplicate_stderr, F_GETFD) != -1 || errno != EBADF) {
        return EXIT_FAILURE;
    }

    if (close(STDERR_FILENO) != 0 ||
        fcntl(STDERR_FILENO, F_GETFD) == -1 ||
        write(STDERR_FILENO, marker, sizeof(marker) - 1) !=
            (ssize_t)(sizeof(marker) - 1)) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
