#include "general_listener.h"
#include "pthread_listener.h"
#include "utils/env_parser.h"

size_t hook_address_count;
char** hook_strings;
gulong max_num_threads;

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
    peak_general_listener_print();

    peak_general_listener_dettach();
    pthread_listener_dettach();
    gum_deinit_embedded();
    free_parsed_result(hook_strings, hook_address_count);
}

__attribute__((section(".init_array"))) void* __init = libprof_init;
__attribute__((section(".fini_array"))) void* __fini = libprof_fini;