#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* This native launcher must enter with no profiler preload. Only the child
 * receives LD_PRELOAD; the measurement starts before setting its environment. */
int main(int argc, char **argv) {
    if (argc < 11 || strcmp(argv[1], "--preload") ||
        strcmp(argv[3], "--helper") || strcmp(argv[5], "--global") ||
        strcmp(argv[7], "--policy") || strcmp(argv[9], "--") ||
        (strcmp(argv[6], "0") && strcmp(argv[6], "1")) ||
        (strcmp(argv[8], "immediate") && strcmp(argv[8], "post-init") && strcmp(argv[8], "default"))) {
        fprintf(stderr, "usage: exec_meter --preload path|none --helper path|none --global 0|1 --policy immediate|post-init|default -- program [args]\n");
        return 2;
    }
    const char *preload = getenv("LD_PRELOAD"), *audit = getenv("LD_AUDIT");
    if ((preload && *preload) || (audit && *audit)) {
        fprintf(stderr, "exec_meter refuses preloaded/audited entry\n");
        return 2;
    }
    fprintf(stderr, "bench_exec_meter entry_ld_preload=empty entry_ld_audit=empty policy=%s global=%s\n", argv[8], argv[6]);
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 2;
    uint64_t begin = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    char value[32];
    snprintf(value, sizeof(value), "%" PRIu64, begin);
    if (setenv("PEAK_BENCH_EXEC_BEGIN_NS", value, 1) ||
        setenv("PEAK_MPI_GLOBAL_BUDGET", argv[6], 1)) return 2;
    if (!strcmp(argv[8], "default")) {
        if (unsetenv("PEAK_MPI_ACTIVATION_POLICY")) return 2;
    } else if (setenv("PEAK_MPI_ACTIVATION_POLICY", argv[8], 1)) return 2;
    if (strcmp(argv[2], "none")) {
        if (setenv("LD_PRELOAD", argv[2], 1)) return 2;
    } else if (unsetenv("LD_PRELOAD")) return 2;
    if (strcmp(argv[4], "none")) {
        if (setenv("PEAK_DETACH_HELPER", argv[4], 1)) return 2;
    } else if (unsetenv("PEAK_DETACH_HELPER")) return 2;
    execv(argv[10], &argv[10]);
    fprintf(stderr, "exec_meter execv failed: %s\n", strerror(errno));
    return 127;
}
