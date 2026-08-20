#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static const char reused_marker[] = "application reused descriptor 2\n";

__attribute__((noinline))
static int
peak_close_semantics_helper(int value)
{
    return value + 1;
}

__attribute__((noinline, used, visibility("default")))
int
peak_close_semantics_target(int value)
{
    int first = peak_close_semantics_helper(value);
    return peak_close_semantics_helper(first);
}

int
main(int argc, char** argv)
{
    int close_result;
    int close_error;
    int output_fd;
    int target_result = peak_close_semantics_target(40);

    if (argc != 3) {
        return 2;
    }
    errno = 0;
    if (strcmp(argv[1], "close") == 0) {
        close_result = close(STDERR_FILENO);
    } else if (strcmp(argv[1], "fclose") == 0) {
        close_result = fclose(stderr);
    } else if (strcmp(argv[1], "raw") == 0) {
        close_result = (int)syscall(SYS_close, STDERR_FILENO);
    } else {
        return 3;
    }
    close_error = errno;
    output_fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0600);
    printf("mode=%s close_result=%d close_errno=%d opened_fd=%d target=%d\n",
           argv[1], close_result, close_error, output_fd, target_result);
    fflush(stdout);
    if (close_result != 0 || close_error != 0 ||
        output_fd != STDERR_FILENO || target_result != 42) {
        return 4;
    }
    if (write(output_fd, reused_marker, sizeof(reused_marker) - 1) !=
        (ssize_t)(sizeof(reused_marker) - 1)) {
        return 5;
    }
    return 0;
}
