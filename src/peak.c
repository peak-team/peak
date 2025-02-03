#define _GNU_SOURCE
#include <dlfcn.h>

#ifdef HAVE_MPI
#include <mpi.h>
#include "mpi_interceptor.h"
#endif

#include "general_listener.h"
#include "pthread_listener.h"
#include "syscall_interceptor.h"
#include "utils/env_parser.h"
#include "utils/mpi_utils.h"

#define PEAK_TARGET_ENV            "PEAK_TARGET"
#define PEAK_TARGET_CONFIG_ENV     "PEAK_TARGET_CONFIG_ENV"
#define PEAK_TARGET_CONFIG         "PEAK_TARGET_CONFIG"
#define PEAK_TARGET_DELIM          ','
#define PEAK_COST_ENV              "PEAK_COST"
#define PPID_FILE_NAME             "/tmp/lock_peak_ppid_list"

size_t peak_hook_address_count;
char** peak_hook_strings;
gulong peak_max_num_threads;
double peak_main_time;
float peak_detach_cost;
#ifdef HAVE_MPI
static int found_MPI;
static int flag_clean_fppid = 0;
#endif

void peak_init()
{

    peak_max_num_threads = sysconf(_SC_NPROCESSORS_ONLN) * 2;
    peak_hook_address_count = parse_env_w_delim(PEAK_TARGET_ENV, PEAK_TARGET_DELIM, &peak_hook_strings);
    peak_hook_address_count += load_profiling_symbols(PEAK_TARGET_CONFIG_ENV, &peak_hook_strings, peak_hook_address_count);
    peak_hook_address_count += load_symbols_from_array(PEAK_TARGET_CONFIG, &peak_hook_strings, peak_hook_address_count);
    peak_detach_cost = parse_env_to_float(PEAK_COST_ENV);

    //gum_init_embedded();

    pthread_listener_attach();
    syscall_interceptor_attach();
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

void peak_fini()
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
    syscall_interceptor_dettach();
    pthread_listener_dettach();
    free_parsed_result(peak_hook_strings, peak_hook_address_count);
}

#if defined(__APPLE__)
__attribute__((used, section("__DATA,__mod_init_func"))) void* __init = peak_init;
__attribute__((used, section("__DATA,__mod_fini_func"))) void* __fini = peak_fini;
#elif defined(__ELF__)
//__attribute__((section(".init_array"))) void* __init = peak_init;
//__attribute__((section(".fini_array"))) void* __fini = peak_fini;
typedef int (*main_fn)(int, char**, char**);
typedef int (*libc_start_main_fn)(main_fn, int, char**, 
                                  int (*)(int, char**, char**),
                                  void (*)(void), void (*)(void), void*);

static main_fn real_main = NULL;
static libc_start_main_fn real___libc_start_main = NULL;

// Original function pointer for `exit`
static void (*original_exit)(int) = NULL;
static GumInterceptor* exit_interceptor = NULL;
static gpointer* exit_address = NULL;
void exit_interceptor_detach();

static void
peak_exit(int status) {
    //g_printerr("Custom exit called with status: %d\n", status);

    peak_fini();
    atexit(exit_interceptor_detach);

    // Call the original `exit` function to terminate the process
    original_exit(status);
}

/**
 * @brief Attaches the interceptor to the `exit` function.
 *
 * This function uses the Gum API to intercept calls to the `exit` function, 
 * replacing it with a custom implementation (`peak_exit`).
 *
 * @return 0 on success, -1 on failure.
 */
int exit_interceptor_attach() {
    gum_init_embedded();
    GumReplaceReturn replace_check = -1;
    exit_interceptor = gum_interceptor_obtain();

    gum_interceptor_begin_transaction(exit_interceptor);
    exit_address = gum_find_function("exit");
    if (exit_address) {
        replace_check = gum_interceptor_replace_fast(exit_interceptor,
                                      exit_address, (gpointer*)&peak_exit,
                                      (gpointer*)(&original_exit));
    }
    gum_interceptor_end_transaction(exit_interceptor);
    return replace_check;
}

/**
 * @brief Detaches the interceptor from the `exit` function.
 *
 * This function reverts the interception of the `exit` function, restoring its 
 * original behavior.
 */
void exit_interceptor_detach() {
    gum_interceptor_revert(exit_interceptor, exit_address);
    g_object_unref(exit_interceptor);
    gum_deinit_embedded();
}

static int main_wrapper(int argc, char** argv, char** envp) {
    // Call peak_init before main
    // fprintf(stderr, "[LD_PRELOAD] main started. Running my code now.\n");
    if (!exit_interceptor_attach())
        peak_init();

    int ret = real_main(argc, argv, envp);

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

    // Decide whether to use the main wrapper based on argv[0]
    if (argv[0] && !check_command(argv[0])) {
        return real___libc_start_main(main_wrapper, argc, argv, init, fini, rtld_fini, stack_end);
    } else {
        return real___libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end);
    }
}
#else
#error Unsupported platform
#endif

