#define _GNU_SOURCE
#include <dlfcn.h>
#include <mpi.h>

#include "general_listener.h"
#include "pthread_listener.h"
#include "utils/env_parser.h"
#include "utils/mpi_utils.h"

#define PEAK_TARGET_ENV "PEAK_TARGET"
#define PEAK_TARGET_DELIM ','

size_t hook_address_count;
char** hook_strings;
gulong max_num_threads;

int MPI_Finalize(void) 
{
    return 0;
}

void  MPI_Finalize_(int *ierr) 
{
    ierr=0;
    return ;
}

int (*original_pmpi_finalize)(void);
int peak_is_done = 0;
int PMPI_Finalize(void) 
{
    if (!original_pmpi_finalize) {
        original_pmpi_finalize = dlsym(RTLD_NEXT, "PMPI_Finalize");
    }
    if(peak_is_done)
        return original_pmpi_finalize();
    else
        return 0;
}

void libprof_init()
{

    max_num_threads = sysconf(_SC_NPROCESSORS_ONLN) * 2;
    hook_address_count = parse_env_w_delim(PEAK_TARGET_ENV, PEAK_TARGET_DELIM, &hook_strings);

    gum_init_embedded();

    pthread_listener_attach();
    peak_general_listener_attach();
}

void libprof_fini()
{
    int is_MPI = check_MPI();
    peak_general_listener_print(is_MPI);

    peak_general_listener_dettach();
    pthread_listener_dettach();
    gum_deinit_embedded();
    free_parsed_result(hook_strings, hook_address_count);
}

__attribute__((section(".init_array"))) void* __init = libprof_init;
__attribute__((section(".fini_array"))) void* __fini = libprof_fini;