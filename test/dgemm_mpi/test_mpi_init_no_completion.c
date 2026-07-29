#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*PeakRuntimeActiveFunction)(void);

__attribute__((visibility("default"), noinline))
int
peak_mpi_init_no_completion_target(int value)
{
    return value + 1;
}

int
main(int argc, char** argv)
{
    PeakRuntimeActiveFunction runtime_active =
        (PeakRuntimeActiveFunction)dlsym(
            RTLD_DEFAULT, "peak_runtime_is_active_for_checkpoint");

    if (runtime_active == NULL || runtime_active() != 0) {
        fputs("mpi_init_no_completion_error\n", stderr);
        return 2;
    }
    puts("mpi_init_no_completion_ok");
    if (argc > 1 && strcmp(argv[1], "exit") == 0) {
        exit(0);
    }
    return peak_mpi_init_no_completion_target(0) != 1;
}
