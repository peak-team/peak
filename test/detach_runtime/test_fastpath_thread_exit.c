#define _GNU_SOURCE

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

static atomic_int cancel_target_entered;

__attribute__((noinline, used, externally_visible, visibility("default")))
uintptr_t
peak_fastpath_thread_exit_target(uintptr_t mode)
{
    __asm__ volatile("" : "+r"(mode) :: "memory");
    if (mode == 1) {
        pthread_exit(NULL);
    }
    if (mode == 2) {
        atomic_store_explicit(&cancel_target_entered, 1,
                              memory_order_release);
        for (;;) {
            pthread_testcancel();
            sched_yield();
        }
    }
    return mode + 1;
}

static void*
exit_worker(void* arg)
{
    (void)arg;
    (void)peak_fastpath_thread_exit_target(0);
    (void)peak_fastpath_thread_exit_target(1);
    return (void*)1;
}

static void*
cancel_worker(void* arg)
{
    (void)arg;
    (void)peak_fastpath_thread_exit_target(0);
    (void)peak_fastpath_thread_exit_target(2);
    return (void*)1;
}

int
main(void)
{
    for (int iteration = 0; iteration < 128; iteration++) {
        pthread_t thread;
        void* result = (void*)1;

        if (pthread_create(&thread, NULL, exit_worker, NULL) != 0 ||
            pthread_join(thread, &result) != 0 || result != NULL) {
            fputs("pthread_exit worker failed\n", stderr);
            return 1;
        }
    }

    atomic_store_explicit(&cancel_target_entered, 0,
                          memory_order_relaxed);
    pthread_t canceled;
    if (pthread_create(&canceled, NULL, cancel_worker, NULL) != 0) {
        fputs("cancel worker create failed\n", stderr);
        return 1;
    }
    while (!atomic_load_explicit(&cancel_target_entered,
                                 memory_order_acquire)) {
        sched_yield();
    }
    if (pthread_cancel(canceled) != 0) {
        fputs("pthread_cancel failed\n", stderr);
        return 1;
    }
    void* canceled_result = NULL;
    if (pthread_join(canceled, &canceled_result) != 0 ||
        canceled_result != PTHREAD_CANCELED) {
        fputs("canceled worker join failed\n", stderr);
        return 1;
    }

    pthread_t final_worker;
    void* final_result = (void*)1;
    if (pthread_create(&final_worker, NULL, exit_worker, NULL) != 0 ||
        pthread_join(final_worker, &final_result) != 0 ||
        final_result != NULL) {
        fputs("reused-slot worker failed\n", stderr);
        return 1;
    }

    puts("fastpath_thread_exit_ok");
    return 0;
}
