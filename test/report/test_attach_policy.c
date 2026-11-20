#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "internal/general_listener/attach_policy.h"

#include <pthread.h>
#if defined(__linux__)
#include <dlfcn.h>
#include <errno.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <link.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int ready;
    int release;
} PeakAttachPolicyTestThread;

static void*
hold_peer_thread(void* argument)
{
    PeakAttachPolicyTestThread* state = argument;

    pthread_mutex_lock(&state->mutex);
    state->ready = 1;
    pthread_cond_signal(&state->cond);
    while (!state->release) {
        pthread_cond_wait(&state->cond, &state->mutex);
    }
    pthread_mutex_unlock(&state->mutex);
    return NULL;
}

int
main(int argc, char** argv)
{
    PeakAttachPolicyTestThread state = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .ready = 0,
        .release = 0,
    };
    pthread_t thread;

    if (!peak_general_listener_startup_attach_can_skip_stop()) {
        fputs("single-thread startup probe was conservative unexpectedly\n",
              stderr);
        return EXIT_FAILURE;
    }

    if (pthread_create(&thread, NULL, hold_peer_thread, &state) != 0) {
        fputs("failed to create startup-probe peer thread\n", stderr);
        return EXIT_FAILURE;
    }
    pthread_mutex_lock(&state.mutex);
    while (!state.ready) {
        pthread_cond_wait(&state.cond, &state.mutex);
    }
    pthread_mutex_unlock(&state.mutex);

    if (peak_general_listener_startup_attach_can_skip_stop()) {
        fputs("multi-thread startup probe allowed skipping STOP\n", stderr);
        pthread_mutex_lock(&state.mutex);
        state.release = 1;
        pthread_cond_signal(&state.cond);
        pthread_mutex_unlock(&state.mutex);
        pthread_join(thread, NULL);
        return EXIT_FAILURE;
    }

    pthread_mutex_lock(&state.mutex);
    state.release = 1;
    pthread_cond_signal(&state.cond);
    pthread_mutex_unlock(&state.mutex);
    pthread_join(thread, NULL);

    gum_init_embedded();
    if (setenv("PEAK_ALLOW_UNSAFE_GUM_PROLOGUE", "1", 1) != 0 ||
        !peak_general_listener_attach_target_is_supported("test", NULL) ||
        !peak_general_listener_support_attach_target_is_supported(
            "support",
            NULL)) {
        fputs("unsafe attach override did not preserve policy behavior\n",
              stderr);
        gum_deinit_embedded();
        return EXIT_FAILURE;
    }
#if defined(__linux__)
    /* Exact entry addresses also cover aliases from a collected symbol file.
     * The unsafe-prologue override above must not disable these safeguards. */
    gpointer protected_entries[] = {
        (gpointer)__errno_location,
        (gpointer)pthread_getspecific,
        (gpointer)pthread_setspecific,
        (gpointer)dlclose,
        (gpointer)pthread_join,
        (gpointer)pthread_detach,
        (gpointer)dlopen,
    };
    for (size_t i = 0; i < G_N_ELEMENTS(protected_entries); i++) {
        if (peak_general_listener_attach_target_is_supported(
                "collector_alias", protected_entries[i])) {
            fputs("unsafe override allowed a protected runtime entry\n", stderr);
            gum_deinit_embedded();
            return EXIT_FAILURE;
        }
    }
    GumAddress loader_base = getauxval(AT_BASE);
    gboolean direct_loader = argc == 3 && strcmp(argv[1], "--direct-loader") == 0;
    if (direct_loader && loader_base != 0) {
        fputs("direct loader fixture unexpectedly has AT_BASE\n", stderr);
        return EXIT_FAILURE;
    }
    char* loader_path = NULL;
    if (loader_base != 0 || direct_loader) {
        GumModule* loader = direct_loader
            ? gum_process_find_module_by_name(argv[2])
            : gum_process_find_module_by_address(loader_base);
        if (loader == NULL) {
            fputs("could not identify the process ELF loader\n", stderr);
            gum_deinit_embedded();
            return EXIT_FAILURE;
        }
        loader_path = strdup(gum_module_get_path(loader));
        GumMemoryRange range = *gum_module_get_range(loader);
        g_object_unref(loader);
        const ElfW(Ehdr)* elf = (const void*)range.base_address;
        const ElfW(Phdr)* headers = (const void*)(range.base_address + elf->e_phoff);
        for (size_t i = 0; i < elf->e_phnum; i++) {
            if (headers[i].p_type != PT_LOAD || !(headers[i].p_flags & PF_X))
                continue;
            if (peak_general_listener_attach_target_is_supported(
                    "loader_code", (gpointer)(range.base_address + headers[i].p_vaddr))) {
                fputs("unsafe override allowed loader executable segment\n", stderr);
                return EXIT_FAILURE;
            }
        }
        void* code = mmap((void*)(range.base_address - 4096), 4096,
                          PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (code == MAP_FAILED ||
            !peak_general_listener_attach_target_is_supported("anonymous_jit", code)) {
            fputs("loader policy rejected anonymous JIT mapping\n", stderr);
            return EXIT_FAILURE;
        }
        munmap(code, 4096);
    }
    if (!peak_general_listener_attach_target_is_supported(
            "ordinary_libc", (gpointer)puts)) {
        fputs("ELF loader policy rejected an ordinary libc entry\n", stderr);
        gum_deinit_embedded();
        return EXIT_FAILURE;
    }
    const char *loader_names[] = {"_dl_find_dso_for_object", "_dl_allocate_tls"};
    for (size_t i = 0; i < G_N_ELEMENTS(loader_names); i++) {
        gpointer entry = dlsym(RTLD_DEFAULT, loader_names[i]);
        Dl_info owner = {0};
        if (entry != NULL && dladdr(entry, &owner) != 0 &&
            getauxval(AT_BASE) != 0 &&
            owner.dli_fbase == (gpointer)getauxval(AT_BASE) &&
            peak_general_listener_attach_target_is_supported("collector_alias", entry)) {
            fputs("unsafe override allowed an ELF loader entry\n", stderr);
            gum_deinit_embedded();
            return EXIT_FAILURE;
        }
    }
#endif
    gum_deinit_embedded();
#if defined(__linux__)
    if (!direct_loader && loader_path != NULL) {
        pid_t child = fork();
        if (child == 0) {
            alarm(15);
            execl(loader_path, loader_path, argv[0], "--direct-loader", loader_path,
                  (char*)NULL);
            _exit(127);
        }
        int status = 0;
        if (child < 0 || waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fputs("direct loader attach-policy regression failed\n", stderr);
            free(loader_path);
            return EXIT_FAILURE;
        }
    }
    free(loader_path);
#else
    (void)argc;
    (void)argv;
#endif

    puts("attach_policy_test_ok");
    return EXIT_SUCCESS;
}
