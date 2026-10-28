#define _GNU_SOURCE
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/vfs.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#endif
static volatile sig_atomic_t running = 1;
static _Atomic unsigned long progress;
static _Atomic unsigned long statfs_errors;
__attribute__((noinline, visibility("default")))
void peak_attach_existing_target(void) {
    struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
    atomic_fetch_add_explicit(&progress, 1, memory_order_relaxed);
}
__attribute__((noinline, visibility("default")))
void peak_attach_churn_target(void) {
    struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
}
static void *worker(void *unused) {
    (void)unused;
    while (running) {
        volatile unsigned char *allocation = malloc(256);
        if (!allocation) abort();
        allocation[0] = 17;
        free((void *)allocation);
        peak_attach_existing_target();
    }
    return NULL;
}
static void *short_worker(void *unused) {
    (void)unused;
    for (int i = 0; i < 5; ++i) peak_attach_churn_target();
    return NULL;
}
static void *filesystem_worker(void *unused) {
    (void)unused;
    int fd = open(".", O_RDONLY);
    if (fd < 0) { atomic_fetch_add(&statfs_errors, 1); return NULL; }
    while (running) {
        struct statfs info;
        if (statfs(".", &info) != 0) atomic_fetch_add(&statfs_errors, 1);
        if (fstatfs(fd, &info) != 0) atomic_fetch_add(&statfs_errors, 1);
        errno = 0;
        if (fstatfs(-1, &info) != -1 || errno != EBADF)
            atomic_fetch_add(&statfs_errors, 1);
#ifdef __linux__
        struct statfs64 wide_info;
        if (statfs64(".", &wide_info) != 0) atomic_fetch_add(&statfs_errors, 1);
        if (fstatfs64(fd, &wide_info) != 0) atomic_fetch_add(&statfs_errors, 1);
#endif
        struct timespec gap = {0, 100000};
        nanosleep(&gap, NULL);
    }
    close(fd);
    return NULL;
}
static void stop(int sig) { (void)sig; running = 0; }
int main(int argc, char **argv) {
    if (argc != 2) return 2;
#ifdef __linux__
    /* Permit this test's sibling injector under Yama without changing policy. */
    if (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) != 0) {
        perror("PR_SET_PTRACER"); return 77;
    }
#endif
    signal(SIGTERM, stop); signal(SIGINT, stop);
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    pthread_t filesystem_thread;
    if (pthread_create(&filesystem_thread, NULL, filesystem_worker, NULL)) return 2;
    pthread_t workers[3];
    for (int i = 0; i < 3; ++i)
        if (pthread_create(&workers[i], NULL, worker, NULL)) return 2;
    while (running) {
        char command;
        if (read(STDIN_FILENO, &command, 1) == 1 && command == 'Q') break;
        FILE *f = fopen(argv[1], "w");
        if (!f) return 2;
        fprintf(f, "%lu\n", atomic_load_explicit(&progress, memory_order_relaxed));
        fclose(f);
        pthread_t thread;
        if (pthread_create(&thread, NULL, short_worker, NULL)) return 2;
        pthread_join(thread, NULL);
        struct timespec delay = {0, 20000000}; nanosleep(&delay, NULL);
    }
    running = 0;
    for (int i = 0; i < 3; ++i) pthread_join(workers[i], NULL);
    pthread_join(filesystem_thread, NULL);
    printf("statfs_errors=%lu\n", atomic_load(&statfs_errors));
    return atomic_load(&statfs_errors) != 0;
}
