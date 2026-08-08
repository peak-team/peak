#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

__attribute__((noinline)) float my_sleep_func()
{
    struct timespec ts = { 0, 100000 }; // Sleep for 0.0001 seconds (10,000,000 nanoseconds)
    nanosleep(&ts, NULL);
    return 0.01;
}

static void*
application_worker(void* unused)
{
    (void)unused;
    (void)my_sleep_func();
    return NULL;
}

int main()
{
    float total = 0.0;
    pthread_t worker;

    if (pthread_create(&worker, NULL, application_worker, NULL) != 0) {
        return 2;
    }
    if (pthread_join(worker, NULL) != 0) {
        return 3;
    }
#pragma omp parallel for reduction(+ : total)
    for (int i = 0; i < 1000; i++) {
        total += my_sleep_func();
    }
    printf("Sleep is done: %f\n", total);
    return 0;
}
