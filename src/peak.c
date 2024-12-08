#define _GNU_SOURCE
#include <dlfcn.h>
#ifdef HAVE_MPI
#include <mpi.h>
#include "mpi_interceptor.h"
#endif

#include "general_listener.h"
#include "pthread_listener.h"
#include "utils/env_parser.h"
#include "utils/mpi_utils.h"

#define PEAK_TARGET_ENV "PEAK_TARGET"
#define PEAK_TARGET_CONFIG_ENV "PEAK_TARGET_CONFIG_ENV"
#define PEAK_TARGET_CONFIG "PEAK_TARGET_CONFIG"
#define PEAK_TARGET_DELIM ','
#define PEAK_COST_ENV "PEAK_COST"
#define PPID_FILE_NAME "/tmp/lock_peak_ppid_list"

size_t peak_hook_address_count;
char** peak_hook_strings;
gulong peak_max_num_threads;
double peak_main_time;
float peak_detach_cost;
#ifdef HAVE_MPI
static int found_MPI;
static int flag_clean_fppid = 0;
#endif

void libprof_init()
{

    peak_max_num_threads = sysconf(_SC_NPROCESSORS_ONLN) * 2;
    peak_hook_address_count = parse_env_w_delim(PEAK_TARGET_ENV, PEAK_TARGET_DELIM, &peak_hook_strings);
    peak_hook_address_count += load_profiling_symbols(PEAK_TARGET_CONFIG_ENV, &peak_hook_strings, peak_hook_address_count);
    peak_hook_address_count += load_symbols_from_array(PEAK_TARGET_CONFIG, &peak_hook_strings, peak_hook_address_count);
    peak_detach_cost = parse_env_to_float(PEAK_COST_ENV);

    gum_init_embedded();

    pthread_listener_attach();
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
    // general listener needs to be after pthread and mpi ones
    peak_general_listener_attach();
    peak_main_time = peak_second();
}

void libprof_fini()
{
    peak_main_time = peak_second() - peak_main_time;
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
    free_parsed_result(peak_hook_strings, peak_hook_address_count);
}
#if defined(__APPLE__)
__attribute__((used, section("__DATA,__mod_init_func"))) void* __init = libprof_init;
__attribute__((used, section("__DATA,__mod_fini_func"))) void* __fini = libprof_fini;
#elif defined(__ELF__)
//__attribute__((section(".init_array"))) void* __init = libprof_init;
//__attribute__((section(".fini_array"))) void* __fini = libprof_fini;
typedef int (*main_fn)(int, char**, char**);
typedef int (*libc_start_main_fn)(main_fn, int, char**, 
                                  int (*)(int, char**, char**),
                                  void (*)(void), void (*)(void), void*);

static main_fn real_main = NULL;
static libc_start_main_fn real___libc_start_main = NULL;

static int main_wrapper(int argc, char** argv, char** envp) {
    // Call libprof_init before main
    // fprintf(stderr, "[LD_PRELOAD] main started. Running my code now.\n");
    libprof_init();

    int ret = real_main(argc, argv, envp);

    // Call libprof_fini after main returns
    libprof_fini();

    // fprintf(stderr, "[LD_PRELOAD] main has finished. Running my code now.\n");
    return ret;
}

__attribute__((visibility("default")))
int __libc_start_main(main_fn main, int argc, char** argv,
                      int (*init)(int, char**, char**),
                      void (*fini)(void), void (*rtld_fini)(void), void* stack_end) {
    // fprintf(stderr, "Running my code now.\n");
    if (!real___libc_start_main) {
        real___libc_start_main = (libc_start_main_fn)dlsym(RTLD_NEXT, "__libc_start_main");
        if (!real___libc_start_main) {
            fprintf(stderr, "Error: dlsym failed to find __libc_start_main\n");
            _exit(1);
        }
    }

    // Store the original main function pointer
    real_main = main;

    // Pass our main_wrapper instead of the original main
    return real___libc_start_main(main_wrapper, argc, argv, init, fini, rtld_fini, stack_end);
}
#else
#error Unsupported platform
#endif

