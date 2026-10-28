#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "internal/general_listener/attach_policy.h"

#include <pthread.h>
#if defined(__linux__)
#include <dlfcn.h>
#include <errno.h>
#include <sys/auxv.h>
#endif
#include <stdio.h>
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
main(void)
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

    puts("attach_policy_test_ok");
    return EXIT_SUCCESS;
}
