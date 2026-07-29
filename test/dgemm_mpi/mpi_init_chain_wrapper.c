#define _GNU_SOURCE
#include <dlfcn.h>
#include <mpi.h>
#include <stdio.h>

typedef int (*PeakInitFunction)(int*, char***);
typedef int (*PeakInitThreadFunction)(int*, char***, int, int*);
typedef int (*PeakRuntimeActiveFunction)(void);

static int
runtime_still_inactive(const char* symbol, int result)
{
    PeakRuntimeActiveFunction runtime_active =
        (PeakRuntimeActiveFunction)dlsym(
            RTLD_DEFAULT, "peak_runtime_is_active_for_checkpoint");

    if (result == MPI_SUCCESS &&
        (runtime_active == NULL || runtime_active() != 0)) {
        fprintf(stderr,
                "mpi_init_chain_wrapper_error early_activation=%s\n",
                symbol);
        return 0;
    }
    return 1;
}

static PeakInitFunction
next_init(const char* symbol)
{
    PeakInitFunction function =
        (PeakInitFunction)dlsym(RTLD_NEXT, symbol);

    if (function == NULL) {
        fprintf(stderr, "mpi_init_chain_wrapper_error symbol=%s\n", symbol);
    }
    return function;
}

static PeakInitThreadFunction
next_init_thread(const char* symbol)
{
    PeakInitThreadFunction function =
        (PeakInitThreadFunction)dlsym(RTLD_NEXT, symbol);

    if (function == NULL) {
        fprintf(stderr, "mpi_init_chain_wrapper_error symbol=%s\n", symbol);
    }
    return function;
}

__attribute__((visibility("default")))
int
MPI_Init(int* argc, char*** argv)
{
    PeakInitFunction function =
        (PeakInitFunction)dlsym(RTLD_DEFAULT, "PMPI_Init");
    int result;

    printf("mpi_init_chain_wrapper_seen symbol=MPI_Init\n");
    fflush(stdout);
    result = function != NULL ? function(argc, argv) : MPI_ERR_OTHER;
    return runtime_still_inactive("MPI_Init", result)
               ? result
               : MPI_ERR_OTHER;
}

__attribute__((visibility("default")))
int
PMPI_Init(int* argc, char*** argv)
{
    PeakInitFunction function = next_init("PMPI_Init");
    int result;

    printf("mpi_init_chain_wrapper_seen symbol=PMPI_Init\n");
    fflush(stdout);
    result = function != NULL ? function(argc, argv) : MPI_ERR_OTHER;
    return runtime_still_inactive("PMPI_Init", result)
               ? result
               : MPI_ERR_OTHER;
}

__attribute__((visibility("default")))
int
MPI_Init_thread(int* argc, char*** argv, int required, int* provided)
{
    PeakInitThreadFunction function =
        (PeakInitThreadFunction)dlsym(RTLD_DEFAULT, "PMPI_Init_thread");
    int result;

    printf("mpi_init_chain_wrapper_seen symbol=MPI_Init_thread\n");
    fflush(stdout);
    result = function != NULL
                 ? function(argc, argv, required, provided)
                 : MPI_ERR_OTHER;
    return runtime_still_inactive("MPI_Init_thread", result)
               ? result
               : MPI_ERR_OTHER;
}

__attribute__((visibility("default")))
int
PMPI_Init_thread(int* argc, char*** argv, int required, int* provided)
{
    PeakInitThreadFunction function = next_init_thread("PMPI_Init_thread");
    int result;

    printf("mpi_init_chain_wrapper_seen symbol=PMPI_Init_thread\n");
    fflush(stdout);
    result = function != NULL
                 ? function(argc, argv, required, provided)
                 : MPI_ERR_OTHER;
    return runtime_still_inactive("PMPI_Init_thread", result)
               ? result
               : MPI_ERR_OTHER;
}
