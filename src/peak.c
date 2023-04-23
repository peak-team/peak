#define _GNU_SOURCE
#include <dlfcn.h>
#ifdef HAVE_MPI
#include <mpi.h>
#endif

#include "general_listener.h"
#include "pthread_listener.h"
#include "mpi_interceptor.h"
#include "utils/env_parser.h"
#include "utils/mpi_utils.h"

#define PEAK_TARGET_ENV "PEAK_TARGET"
#define PEAK_TARGET_DELIM ','
#define PPID_FILE_NAME "/tmp/lock_peak_ppid_list"

size_t hook_address_count;
char** hook_strings;
gulong max_num_threads;
static int found_MPI;
#ifdef HAVE_MPI
static int flag_clean_fppid = 0;
#endif

void libprof_init()
{

    max_num_threads = sysconf(_SC_NPROCESSORS_ONLN) * 2;
    hook_address_count = parse_env_w_delim(PEAK_TARGET_ENV, PEAK_TARGET_DELIM, &hook_strings);

    gum_init_embedded();

    pthread_listener_attach();
    peak_general_listener_attach();
#ifdef HAVE_MPI
    found_MPI = check_MPI();
    if (found_MPI) {
        int is_parent_MPI = check_parent_process(PPID_FILE_NAME, &flag_clean_fppid);
        if (is_parent_MPI > 0) {
            found_MPI = 0;
        } else if (mpi_interceptor_attach() != 0) {
            found_MPI = 0;
        }
    }
#endif
}

void libprof_fini()
{
#ifdef HAVE_MPI
    if (flag_clean_fppid) {
        remove_ppid_file(PPID_FILE_NAME);
    }
    peak_general_listener_print(found_MPI);
    if (found_MPI)
        mpi_interceptor_dettach();
#else
    peak_general_listener_print(0);
#endif
    peak_general_listener_dettach();
    pthread_listener_dettach();
    gum_deinit_embedded();
    free_parsed_result(hook_strings, hook_address_count);
}

__attribute__((section(".init_array"))) void* __init = libprof_init;
__attribute__((section(".fini_array"))) void* __fini = libprof_fini;