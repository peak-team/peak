#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef int (*lazy_profile_target_fn)(int);

extern char** environ;

static int
env_name_is(const char* entry, const char* name)
{
    size_t len = strlen(name);
    return strncmp(entry, name, len) == 0 && entry[len] == '=';
}

static char**
make_baseline_env(void)
{
    size_t count = 0;
    size_t out = 0;

    while (environ[count] != NULL) {
        count++;
    }

    char** envp = calloc(count + 2, sizeof(char*));
    if (envp == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        if (env_name_is(environ[i], "LD_PRELOAD") ||
            env_name_is(environ[i], "PEAK_LAZY_DLOPEN_BASELINE")) {
            continue;
        }
        envp[out++] = environ[i];
    }
    envp[out++] = (char*)"PEAK_LAZY_DLOPEN_BASELINE=1";
    envp[out] = NULL;
    return envp;
}

static int
run_lazy_probe(int quiet)
{
    dlerror();
    void* handle = dlopen("./liblazy_undefined.so", RTLD_LAZY | RTLD_LOCAL);
    if (handle == NULL) {
        const char* error = dlerror();
        if (quiet) {
            fprintf(stderr,
                    "baseline lazy dlopen probe unavailable\n");
        } else {
            fprintf(stderr,
                    "RTLD_LAZY dlopen failed: %s\n",
                    error != NULL ? error : "unknown error");
        }
        return 1;
    }

    dlerror();
    lazy_profile_target_fn target =
        (lazy_profile_target_fn)dlsym(handle, "lazy_profile_target");
    const char* error = dlerror();
    if (error != NULL || target == NULL) {
        fprintf(stderr,
                "dlsym lazy_profile_target failed: %s\n",
                error != NULL ? error : "unknown error");
        return 1;
    }

    int result = 0;
    for (int i = 0; i < 1000; i++) {
        result += target(i);
        usleep(1000);
    }

    printf("lazy_dlopen_ok result=%d\n", result);
    return 0;
}

static int
run_no_peak_baseline(void)
{
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "fork baseline probe failed: %s\n", strerror(errno));
        return 1;
    }
    if (child == 0) {
        char* const argv[] = {
            (char*)"dlopen_lazy_undefined_test",
            NULL,
        };
        char** envp = make_baseline_env();
        if (envp == NULL) {
            _exit(1);
        }
        execve("/proc/self/exe", argv, envp);
        _exit(1);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        fprintf(stderr, "wait baseline probe failed: %s\n", strerror(errno));
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 1;
}

int
main(void)
{
    if (getenv("PEAK_LAZY_DLOPEN_BASELINE") != NULL) {
        return run_lazy_probe(1) == 0 ? 0 : 77;
    }

    int baseline_status = run_no_peak_baseline();
    if (baseline_status == 77) {
        fprintf(stderr,
                "skipping RTLD_LAZY unresolved-symbol probe because "
                "the no-PEAK baseline loader rejects the fixture\n");
        return 77;
    }
    if (baseline_status != 0) {
        fprintf(stderr,
                "no-PEAK baseline lazy dlopen probe failed unexpectedly\n");
        return 1;
    }

    return run_lazy_probe(0);
}
