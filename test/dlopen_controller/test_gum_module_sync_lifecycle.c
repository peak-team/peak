#define _GNU_SOURCE

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "frida-gum.h"

typedef struct {
    const char* path;
    atomic_int* start;
    atomic_int* stop;
    atomic_uint* progress;
    atomic_int* failed;
} LoaderArgs;

typedef struct {
    atomic_int* returned;
} QuiesceArgs;

/*
 * The patched Gum object declares this optional test seam weakly. Holding the
 * owner after its worker join makes the former
 * thread_started==0/incomplete-cleanup window deterministic.
 */
static atomic_int quiesce_gate_reached;
static atomic_int quiesce_gate_release;
static atomic_int quiesce_gate_enabled;

__attribute__((visibility("default"))) void
peak_gum_module_sync_test_quiesce_cleanup_gate(void)
{
    if (atomic_load_explicit(&quiesce_gate_enabled,
                             memory_order_acquire) == 0) {
        return;
    }
    atomic_store_explicit(&quiesce_gate_reached, 1, memory_order_release);
    while (atomic_load_explicit(&quiesce_gate_release,
                                memory_order_acquire) == 0) {
        sched_yield();
    }
}

static void*
loader_main(void* data)
{
    LoaderArgs* args = data;

    while (atomic_load_explicit(args->start, memory_order_acquire) == 0) {
        sched_yield();
    }
    while (atomic_load_explicit(args->stop, memory_order_acquire) == 0) {
        void* module = dlopen(args->path, RTLD_NOW | RTLD_LOCAL);

        if (module == NULL) {
            atomic_store_explicit(args->failed, 1, memory_order_release);
            break;
        }
        atomic_fetch_add_explicit(args->progress, 1, memory_order_relaxed);
        if (dlclose(module) != 0) {
            atomic_store_explicit(args->failed, 1, memory_order_release);
            break;
        }
    }
    return NULL;
}

static void*
quiesce_main(void* data)
{
    QuiesceArgs* args = data;

    gum_interceptor_peak_quiesce_deferred_module_sync();
    atomic_fetch_add_explicit(args->returned, 1, memory_order_release);
    return NULL;
}

int
main(void)
{
    /*
     * Gum/GLib embedded teardown is process-final and does not support a
     * second gum_init_embedded() in the same process. CTest repeats this
     * executable to exercise multiple independent lifecycles.
     */
    enum { CYCLES = 1, THREADS = 2, QUIESCE_THREADS = 8 };

    for (int cycle = 0; cycle < CYCLES; cycle++) {
        pthread_t threads[THREADS];
        LoaderArgs args[THREADS];
        atomic_int start = 0;
        atomic_int stop = 0;
        atomic_uint progress = 0;
        atomic_int failed = 0;
        atomic_int quiescers_returned = 0;

        gum_init_embedded();
        (void)gum_module_registry_obtain();

        pid_t child = fork();
        if (child == -1) {
            perror("fork");
            return EXIT_FAILURE;
        }
        if (child == 0) {
            void* module =
                dlopen(PEAK_TEST_DEFERRED_MODULE_A, RTLD_NOW | RTLD_LOCAL);
            if (module == NULL || dlclose(module) != 0) {
                _exit(97);
            }
            gum_deinit_embedded();
            _exit(0);
        }
        int child_status = 0;
        if (waitpid(child, &child_status, 0) != child ||
            !WIFEXITED(child_status) ||
            WEXITSTATUS(child_status) != 0) {
            fprintf(stderr, "fork child Gum lifecycle failed\n");
            return EXIT_FAILURE;
        }

        for (int i = 0; i < THREADS; i++) {
            args[i].path = i == 0 ?
                PEAK_TEST_DEFERRED_MODULE_A :
                PEAK_TEST_DEFERRED_MODULE_B;
            args[i].start = &start;
            args[i].stop = &stop;
            args[i].progress = &progress;
            args[i].failed = &failed;
            if (pthread_create(&threads[i],
                               NULL,
                               loader_main,
                               &args[i]) != 0) {
                fprintf(stderr, "failed to create loader thread\n");
                return EXIT_FAILURE;
            }
        }

        atomic_store_explicit(&start, 1, memory_order_release);
        while (atomic_load_explicit(&progress, memory_order_acquire) < 100 &&
               atomic_load_explicit(&failed, memory_order_acquire) == 0) {
            sched_yield();
        }

        /*
         * Fork while the caller owns the dynamic-loader lock (the fixture's
         * constructor calls fork) and other threads are generating module
         * notifications.  An atfork prepare handler must not wait for the
         * module-sync worker here.
         */
        void* forking_module =
            dlopen(PEAK_TEST_FORKING_MODULE, RTLD_NOW | RTLD_LOCAL);
        if (forking_module == NULL) {
            fprintf(stderr, "failed to load forking fixture: %s\n", dlerror());
            return EXIT_FAILURE;
        }
        int (*fork_status)(void) = (int (*)(void))
            dlsym(forking_module, "peak_forking_module_status");
        if (fork_status == NULL || fork_status() != 0 ||
            dlclose(forking_module) != 0) {
            fprintf(stderr, "fork-from-constructor lifecycle failed\n");
            return EXIT_FAILURE;
        }

        /*
         * Match PEAK's process-final ordering: stop module synchronization
         * before any listener teardown can take Gum interceptor locks. Loader
         * threads continue publishing RTLD notifications to exercise the
         * quiescing admission boundary.
         */
        pthread_t owner;
        QuiesceArgs owner_args = {
            .returned = &quiescers_returned,
        };
        atomic_store_explicit(&quiesce_gate_reached, 0, memory_order_relaxed);
        atomic_store_explicit(&quiesce_gate_release, 0, memory_order_relaxed);
        atomic_store_explicit(&quiesce_gate_enabled, 1, memory_order_release);
        if (pthread_create(&owner, NULL, quiesce_main, &owner_args) != 0) {
            fprintf(stderr, "failed to create owner quiesce thread\n");
            return EXIT_FAILURE;
        }
        for (int i = 0;
             i < 50000 &&
             atomic_load_explicit(&quiesce_gate_reached,
                                  memory_order_acquire) == 0;
             i++) {
            usleep(100);
        }
        if (atomic_load_explicit(&quiesce_gate_reached,
                                 memory_order_acquire) == 0) {
            fprintf(stderr, "quiesce owner did not reach cleanup gate\n");
            atomic_store_explicit(&quiesce_gate_release,
                                  1,
                                  memory_order_release);
            pthread_join(owner, NULL);
            atomic_store_explicit(&stop, 1, memory_order_release);
            for (int i = 0; i < THREADS; i++) {
                pthread_join(threads[i], NULL);
            }
            return EXIT_FAILURE;
        }

        pthread_t quiescers[QUIESCE_THREADS];
        QuiesceArgs quiesce_args[QUIESCE_THREADS];
        int quiescers_created = 0;
        for (int i = 0; i < QUIESCE_THREADS; i++) {
            quiesce_args[i].returned = &quiescers_returned;
            if (pthread_create(&quiescers[i],
                               NULL,
                               quiesce_main,
                               &quiesce_args[i]) != 0) {
                fprintf(stderr, "failed to create quiesce thread %d\n", i);
                atomic_store_explicit(&failed, 1, memory_order_release);
                break;
            }
            quiescers_created++;
        }
        usleep(50000);
        if (atomic_load_explicit(&quiescers_returned,
                                 memory_order_acquire) != 0) {
            fprintf(stderr,
                    "quiesce caller returned before owner cleanup completed\n");
            atomic_store_explicit(&failed, 1, memory_order_release);
        }
        atomic_store_explicit(&quiesce_gate_release,
                              1,
                              memory_order_release);
        pthread_join(owner, NULL);
        for (int i = 0; i < quiescers_created; i++) {
            pthread_join(quiescers[i], NULL);
        }
        if (atomic_load_explicit(&quiescers_returned,
                                 memory_order_acquire) !=
            quiescers_created + 1) {
            fprintf(stderr, "not all quiesce callers completed\n");
            atomic_store_explicit(&failed, 1, memory_order_release);
        }
        atomic_store_explicit(&quiesce_gate_enabled, 0, memory_order_release);

        /*
         * Process-final Gum teardown may remove the RTLD interceptor.  No
         * loader may still be executing its trampoline at that point.
         */
        atomic_store_explicit(&stop, 1, memory_order_release);
        for (int i = 0; i < THREADS; i++) {
            pthread_join(threads[i], NULL);
        }
        gum_deinit_embedded();

        if (atomic_load_explicit(&failed, memory_order_acquire) != 0) {
            fprintf(stderr, "loader failed in lifecycle cycle %d\n", cycle);
            return EXIT_FAILURE;
        }
    }

    printf("gum_module_sync_lifecycle_ok\n");
    return EXIT_SUCCESS;
}
