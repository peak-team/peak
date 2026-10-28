#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
static _Thread_local int internal_context;

/* Model the exported MPI wrapper predicate without requiring an MPI launch. */
__attribute__((visibility("default"))) int peak_mpi_internal_context_active(void)
{
    const char* mode = getenv("PEAK_MPI_GLOBAL_BUDGET");
    return internal_context && mode != NULL && strcmp(mode, "1") == 0;
}

__attribute__((noinline, visibility("default"))) void utility_target(void)
{
    __asm__ volatile("" ::: "memory");
}

static void check_status(int status)
{
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 7);
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "child") == 0 || strcmp(argv[1], "descend") == 0 ||
        strcmp(argv[1], "global-off-child") == 0) {
        int (*active)(void) = dlsym(RTLD_DEFAULT,
                                   "peak_runtime_is_active_for_checkpoint");
        assert(active != NULL);
        printf("child active=%d marker=%d\n", active(),
               getenv("PEAK_MPI_RUNTIME_CHILD") != NULL);
        for (int i = 0; i < 10; ++i) utility_target();
        if (strcmp(argv[1], "descend") == 0 ||
            strcmp(argv[1], "global-off-child") == 0) {
            if (strcmp(argv[1], "global-off-child") == 0)
                assert(setenv("PEAK_MPI_GLOBAL_BUDGET", "1", 1) == 0);
            char nested[4096];
            assert(snprintf(nested, sizeof(nested), "exec '%s' child", argv[0]) > 0);
            check_status(system(nested));
        }
        return 7;
    }

    char command[4096];
    assert(snprintf(command, sizeof(command), "exec '%s' %s", argv[0],
                    strcmp(argv[1], "descendant") == 0 ? "descend" : "child") > 0);
    internal_context = strcmp(argv[1], "normal") != 0;
    const char original_global_mode = getenv("PEAK_MPI_GLOBAL_BUDGET")[0];
    assert(getenv("PEAK_MPI_RUNTIME_CHILD") == NULL);
    if (strcmp(argv[1], "popen") == 0) {
        FILE* pipe = popen(command, "r");
        assert(pipe != NULL);
        char payload[128];
        assert(fgets(payload, sizeof(payload), pipe) != NULL);
        fputs(payload, stdout);
        assert(fgets(payload, sizeof(payload), pipe) == NULL);
        check_status(pclose(pipe));
    } else if (strcmp(argv[1], "exec") == 0 ||
               strcmp(argv[1], "custom-global-off") == 0) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            int custom = strcmp(argv[1], "custom-global-off") == 0;
            char* args[] = {argv[0], custom ? "global-off-child" : "child", NULL};
            char* child_env[1024];
            size_t n = 0;
            for (size_t i = 0; environ[i] != NULL; ++i) {
                assert(n < 1022);
                if (strncmp(environ[i], "PEAK_MPI_GLOBAL_BUDGET=", 23) != 0)
                    child_env[n++] = environ[i];
            }
            child_env[n++] = "PEAK_MPI_GLOBAL_BUDGET=0";
            child_env[n] = NULL;
            execve(argv[0], args, custom ? child_env : environ);
            _exit(99);
        }
        int status;
        assert(waitpid(child, &status, 0) == child);
        check_status(status);
    } else if (strcmp(argv[1], "spawn") == 0) {
        pid_t child;
        char* args[] = {argv[0], "child", NULL};
        assert(posix_spawn(&child, argv[0], NULL, NULL, args, environ) == 0);
        int status;
        assert(waitpid(child, &status, 0) == child);
        check_status(status);
    } else if (strcmp(argv[1], "failure") == 0) {
        char* args[] = {"missing", NULL};
        errno = 0;
        assert(execve("/peak/no-such-utility", args, environ) == -1);
        assert(errno == ENOENT);
    } else if (strcmp(argv[1], "null") == 0) {
        assert(system(NULL) != 0);
    } else {
        check_status(system(command));
    }
    internal_context = 0;
    assert(getenv("PEAK_MPI_RUNTIME_CHILD") == NULL);
    assert(getenv("PEAK_MPI_GLOBAL_BUDGET")[0] == original_global_mode);
    utility_target();
    puts("parent environment unchanged");
    return 0;
}
