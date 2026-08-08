#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static _Atomic int keep_running = 1;
static _Atomic unsigned long allocation_count = 0;

static void*
allocation_worker(void* unused)
{
    (void)unused;
    while (atomic_load_explicit(&keep_running, memory_order_acquire)) {
        volatile unsigned char* block = malloc(128);
        if (block != NULL) {
            block[0] = 0xa5;
            atomic_fetch_add_explicit(&allocation_count, 1,
                                      memory_order_relaxed);
            free((void*)block);
        }
    }
    return NULL;
}

static pthread_t worker;

__attribute__((constructor)) static void
start_pre_main_worker(void)
{
    if (pthread_create(&worker, NULL, allocation_worker, NULL) != 0) {
        _Exit(2);
    }
}

int
main(void)
{
    atomic_store_explicit(&keep_running, 0, memory_order_release);
    if (pthread_join(worker, NULL) != 0) {
        return 3;
    }
    unsigned long completed = atomic_load_explicit(&allocation_count,
                                                   memory_order_acquire);
    if (completed == 0) {
        return 4;
    }
    printf("rollback worker done allocations=%lu\n", completed);
    return 0;
}
