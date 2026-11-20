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
static _Atomic unsigned long statfs_bootstrap_eintr;
static _Atomic unsigned long statfs_guarded_calls;
static _Atomic unsigned long statfs_phase;
static void check_filesystem_result(int result, int expected_errno,
                                    unsigned long phase_before) {
    int error = errno;
    unsigned long phase_after = atomic_load(&statfs_phase);
    int guarded = (phase_before & 1) && phase_before == phase_after;
    if (guarded) atomic_fetch_add(&statfs_guarded_calls, 1);
    if ((expected_errno == 0 && result == 0) ||
        (expected_errno != 0 && result == -1 && error == expected_errno)) return;
    if (result == -1 && error == EINTR && !guarded) {
        atomic_fetch_add(&statfs_bootstrap_eintr, 1);
        return;
    }
    atomic_fetch_add(&statfs_errors, 1);
}
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
        unsigned long phase = atomic_load(&statfs_phase);
        int result = statfs(".", &info);
        check_filesystem_result(result, 0, phase);
        phase = atomic_load(&statfs_phase);
        result = fstatfs(fd, &info);
        check_filesystem_result(result, 0, phase);
        phase = atomic_load(&statfs_phase);
        errno = 0;
        result = fstatfs(-1, &info);
        check_filesystem_result(result, EBADF, phase);
#ifdef __linux__
        struct statfs64 wide_info;
        phase = atomic_load(&statfs_phase);
        result = statfs64(".", &wide_info);
        check_filesystem_result(result, 0, phase);
        phase = atomic_load(&statfs_phase);
        result = fstatfs64(fd, &wide_info);
        check_filesystem_result(result, 0, phase);
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
        if (read(STDIN_FILENO, &command, 1) == 1) {
            if (command == 'Q') break;
            if (command == 'B' || command == 'G') {
#ifdef __linux__
                /* The stop helper narrows PR_SET_PTRACER to its own PID.
                 * Restore this fixture's explicit sibling-injector permission
                 * before acknowledging another external debugger operation. */
                if (command == 'B' &&
                    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) != 0) {
                    perror("PR_SET_PTRACER bootstrap phase");
                    return 2;
                }
#endif
                unsigned long next = (atomic_load(&statfs_phase) + 2) & ~1UL;
                atomic_store(&statfs_phase, next | (command == 'G'));
                printf("statfs_phase=%c\n", command);
                fflush(stdout);
            }
        }
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
    printf("statfs_errors=%lu\nstatfs_guarded_calls=%lu\nstatfs_bootstrap_eintr=%lu\n",
           atomic_load(&statfs_errors), atomic_load(&statfs_guarded_calls),
           atomic_load(&statfs_bootstrap_eintr));
    return atomic_load(&statfs_errors) != 0;
}
