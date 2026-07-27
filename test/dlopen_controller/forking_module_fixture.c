#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int fork_result = EXIT_FAILURE;

__attribute__((constructor))
static void
fork_from_constructor(void)
{
    pid_t child = fork();
    int status = 0;

    if (child == 0) {
        _exit(0);
    }
    if (child > 0 &&
        waitpid(child, &status, 0) == child &&
        WIFEXITED(status) &&
        WEXITSTATUS(status) == 0) {
        fork_result = EXIT_SUCCESS;
    }
}

__attribute__((visibility("default")))
int
peak_forking_module_status(void)
{
    return fork_result;
}
