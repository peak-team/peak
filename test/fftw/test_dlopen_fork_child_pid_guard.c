#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef int (*admission_hook_fn)(void);

static void
load_function(const char* name, void* function_pointer, size_t size)
{
    void* address;
    const char* error;

    dlerror();
    address = dlsym(RTLD_DEFAULT, name);
    error = dlerror();
    if (address == NULL || error != NULL || size != sizeof(address)) {
        fprintf(stderr, "failed to resolve %s: %s\n",
                name,
                error != NULL ? error : "invalid hook address");
        exit(EXIT_FAILURE);
    }
    memcpy(function_pointer, &address, sizeof(address));
}

int
main(void)
{
    admission_hook_fn callback_is_admitted;
    pid_t child;
    int status;

    load_function("dlopen_interceptor_test_callback_is_admitted",
                  &callback_is_admitted,
                  sizeof(callback_is_admitted));

    child = fork();
    if (child < 0) {
        perror("fork");
        return EXIT_FAILURE;
    }
    if (child == 0) {
        alarm(5);
        if (callback_is_admitted()) {
            _exit(2);
        }
        _exit(0);
    }

    if (waitpid(child, &status, 0) != child) {
        fputs("failed to wait for child\n", stderr);
        return EXIT_FAILURE;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "fork child did not fail open: status=%d\n", status);
        return EXIT_FAILURE;
    }

    puts("dlopen_fork_child_pid_guard_ok");
    return EXIT_SUCCESS;
}
