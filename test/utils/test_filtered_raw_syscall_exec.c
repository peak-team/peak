#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

extern char** environ;

static void
raw_exec_self(const char* path, char* const argv[], char* const envp[])
{
#ifdef SYS_execve
    (void)syscall(SYS_execve, path, argv, envp);
    fprintf(stderr, "raw syscall execve failed for %s: errno=%d\n",
            path,
            errno);
    _exit(127);
#else
    (void)path;
    (void)argv;
    (void)envp;
    printf("filtered_raw_syscall_exec_ok no_sys_execve\n");
    _exit(0);
#endif
}

static int
run_child(void)
{
    const char* preload = getenv("LD_PRELOAD");
    const char* peak_target = getenv("PEAK_TARGET");
    const char* peak_target_file = getenv("PEAK_TARGET_FILE");

    printf("filtered_raw_syscall_child ld_preload=%s peak_target=%s peak_target_file=%s\n",
           preload != NULL ? preload : "<unset>",
           peak_target != NULL ? peak_target : "<unset>",
           peak_target_file != NULL ? peak_target_file : "<unset>");

    if (preload != NULL && strstr(preload, "libpeak") != NULL) {
        fprintf(stderr, "PEAK LD_PRELOAD leaked into filtered raw syscall child\n");
        return 2;
    }
    if (peak_target != NULL || peak_target_file != NULL) {
        fprintf(stderr, "PEAK target env leaked into filtered raw syscall child\n");
        return 3;
    }

    printf("filtered_raw_syscall_exec_ok\n");
    return 0;
}

static int
run_filtered_parent(const char* self_path)
{
    char* child_argv[] = { (char*)"raw-syscall-child", (char*)"--child", NULL };
    char* child_env[] = { (char*)"PATH=/bin:/usr/bin", NULL };

    raw_exec_self(self_path, child_argv, child_env);
    return 127;
}

int
main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--child") == 0) {
        return run_child();
    }
    if (argc > 2 && strcmp(argv[1], "--filtered-parent") == 0) {
        return run_filtered_parent(argv[2]);
    }

    char* filtered_argv[] = {
        (char*)"timeout",
        (char*)"--filtered-parent",
        argv[0],
        NULL
    };
    raw_exec_self(argv[0], filtered_argv, environ);
    return 127;
}
