#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>

typedef int (*peak_test_execve_fn)(const char*,
                                   char* const[],
                                   char* const[]);

static peak_test_execve_fn peak_test_real_execve;
static int peak_test_execve_calls;

__attribute__((constructor))
static void
peak_test_execve_observer_init(void)
{
    peak_test_real_execve =
        (peak_test_execve_fn)dlsym(RTLD_NEXT, "execve");
}

__attribute__((visibility("default")))
int
execve(const char* path, char* const argv[], char* const envp[])
{
    peak_test_execve_calls++;
    if (peak_test_real_execve == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return peak_test_real_execve(path, argv, envp);
}

__attribute__((visibility("default")))
int
peak_test_execve_observer_count(void)
{
    return peak_test_execve_calls;
}
