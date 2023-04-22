#define _GNU_SOURCE
#include <dlfcn.h>
#include <mpi.h>

#include "general_listener.h"
#include "pthread_listener.h"
#include "mpi_interceptor.h"
#include "utils/env_parser.h"
#include "utils/mpi_utils.h"

#define PEAK_TARGET_ENV "PEAK_TARGET"
#define PEAK_TARGET_DELIM ','

size_t hook_address_count;
char** hook_strings;
gulong max_num_threads;
static int found_MPI;

void libprof_init()
{

    max_num_threads = sysconf(_SC_NPROCESSORS_ONLN) * 2;
    hook_address_count = parse_env_w_delim(PEAK_TARGET_ENV, PEAK_TARGET_DELIM, &hook_strings);

    gum_init_embedded();

    pthread_listener_attach();
    peak_general_listener_attach();
    found_MPI = check_MPI();
    if (mpi_interceptor_attach() != 0) {
        found_MPI = 0;
    }
}

void libprof_fini()
{
    peak_general_listener_print(found_MPI);
    if (found_MPI)
        mpi_interceptor_dettach();
    peak_general_listener_dettach();
    pthread_listener_dettach();
    gum_deinit_embedded();
    free_parsed_result(hook_strings, hook_address_count);
}

__attribute__((section(".init_array"))) void* __init = libprof_init;
__attribute__((section(".fini_array"))) void* __fini = libprof_fini;