#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int
main(void)
{
    void* handle = dlopen("./libForkConstructorTarget.so",
                          RTLD_NOW | RTLD_LOCAL);
    pid_t* child_pid;
    int status;

    if (handle == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return EXIT_FAILURE;
    }
    child_pid = dlsym(handle, "peak_fork_constructor_child_pid");
    if (child_pid == NULL) {
        fputs("constructor child PID lookup failed\n", stderr);
        return EXIT_FAILURE;
    }

    if (*child_pid == 0) {
        fputs("fork_constructor_child_returned\n", stderr);
        _exit(EXIT_SUCCESS);
    }
    if (*child_pid < 0) {
        fputs("constructor fork failed\n", stderr);
        return EXIT_FAILURE;
    }
    if (waitpid(*child_pid, &status, 0) != *child_pid ||
        !WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
        fputs("constructor child did not return from dlopen\n", stderr);
        return EXIT_FAILURE;
    }

    fputs("fork_constructor_parent_ok\n", stderr);
    return EXIT_SUCCESS;
}
