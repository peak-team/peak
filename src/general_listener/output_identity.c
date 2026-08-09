#define _POSIX_C_SOURCE 200809L

#include "internal/general_listener/output_identity.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/random.h>
#endif

enum { PEAK_OUTPUT_HOST_CAPACITY = 256, PEAK_OUTPUT_SESSION_CAPACITY = 17 };
enum peak_output_checkpoint_state {
    PEAK_OUTPUT_CHECKPOINT_UNINITIALIZED,
    PEAK_OUTPUT_CHECKPOINT_READY_STATE,
    PEAK_OUTPUT_CHECKPOINT_INVALID,
    PEAK_OUTPUT_CHECKPOINT_INITIALIZING,
};

static uint64_t peak_output_session;
static _Atomic unsigned long peak_output_fallback_counter;
static _Atomic int peak_output_session_ready;
static pthread_once_t peak_output_session_once = PTHREAD_ONCE_INIT;
static char peak_output_jobid[128];
static char peak_output_stepid[128];
static char peak_output_rank[32];
static char peak_output_host[PEAK_OUTPUT_HOST_CAPACITY];
static char peak_output_checkpoint_prefix[PATH_MAX];
static _Atomic unsigned int peak_output_checkpoint_prefix_length;
static _Atomic int peak_output_checkpoint_state;
static pthread_once_t peak_output_checkpoint_once = PTHREAD_ONCE_INIT;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "exec checkpoint state requires lock-free atomics");

static void peak_output_identity_host(char out[PEAK_OUTPUT_HOST_CAPACITY]);
static void peak_output_identity_metadata(char* out,
                                          size_t out_size,
                                          const char* value);

static void
peak_output_identity_fallback_session(void)
{
    struct timespec now = {0};
    uint64_t mix;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    mix = ((uint64_t)(unsigned long)getpid() << 32) ^ (uint64_t)now.tv_nsec ^
          ((uint64_t)now.tv_sec << 1) ^
          (uint64_t)atomic_fetch_add_explicit(&peak_output_fallback_counter, 1UL,
                                               memory_order_relaxed) ^
          (uintptr_t)&peak_output_session;
    peak_output_session = mix == 0 ? 1 : mix;
}

static void
peak_output_identity_session_once(void)
{
    unsigned char* cursor = (unsigned char*)&peak_output_session;
    size_t remaining = sizeof(peak_output_session);

#ifdef PEAK_ENABLE_TEST_HOOKS
    if (getenv("PEAK_TEST_OUTPUT_IDENTITY_ENTROPY_FAIL") != NULL) {
        remaining = sizeof(peak_output_session);
    } else {
#endif
#ifdef __linux__
    while (remaining != 0) {
        ssize_t read_bytes;

        do {
            read_bytes = getrandom(cursor, remaining, GRND_NONBLOCK);
        } while (read_bytes < 0 && errno == EINTR);
        if (read_bytes <= 0) {
            break;
        }
        cursor += read_bytes;
        remaining -= (size_t)read_bytes;
    }
#else
    (void)cursor;
#endif
#ifdef PEAK_ENABLE_TEST_HOOKS
    }
#endif
    if (remaining != 0) {
        int fd = -1;

#ifdef PEAK_ENABLE_TEST_HOOKS
        if (getenv("PEAK_TEST_OUTPUT_IDENTITY_ENTROPY_FAIL") == NULL)
#endif
            fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NONBLOCK);

        if (fd < 0) {
            peak_output_identity_fallback_session();
            remaining = 0;
        } else {
            while (remaining != 0) {
                ssize_t read_bytes;
                do {
                    read_bytes = read(fd, cursor, remaining);
                } while (read_bytes < 0 && errno == EINTR);
                if (read_bytes <= 0) {
                    (void)close(fd);
                    peak_output_identity_fallback_session();
                    remaining = 0;
                    break;
                }
                cursor += read_bytes;
                remaining -= (size_t)read_bytes;
            }
            (void)close(fd);
        }
    }
    peak_output_identity_host(peak_output_host);
    peak_output_identity_metadata(peak_output_jobid,
                                  sizeof(peak_output_jobid),
                                  getenv("SLURM_JOB_ID"));
    peak_output_identity_metadata(peak_output_stepid,
                                  sizeof(peak_output_stepid),
                                  getenv("SLURM_STEP_ID") != NULL ?
                                      getenv("SLURM_STEP_ID") :
                                      getenv("SLURM_STEPID"));
    {
        static const char* const rank_names[] = {
            "PMI_RANK", "PMIX_RANK", "OMPI_COMM_WORLD_RANK",
            "MV2_COMM_WORLD_RANK", "I_MPI_RANK", "SLURM_PROCID", NULL,
        };
        const char* value = NULL;
        for (size_t i = 0; rank_names[i] != NULL && value == NULL; i++) {
            const char* candidate = getenv(rank_names[i]);
            if (candidate != NULL && candidate[0] != '\0') {
                value = candidate;
            }
        }
        peak_output_identity_metadata(peak_output_rank,
                                      sizeof(peak_output_rank), value);
    }
    atomic_store_explicit(&peak_output_session_ready, 1,
                          memory_order_release);
}

int
peak_output_identity_checkpoint_path(char* out,
                                     size_t out_size,
                                     unsigned long long checkpoint_index)
{
    unsigned int length;
    size_t output = 0;
    char digits[32];
    size_t count = 0;

    int state = atomic_load_explicit(&peak_output_checkpoint_state,
                                     memory_order_acquire);
    if (state != PEAK_OUTPUT_CHECKPOINT_READY_STATE) {
        return state == PEAK_OUTPUT_CHECKPOINT_UNINITIALIZED
                   ? PEAK_OUTPUT_CHECKPOINT_PREINIT
                   : PEAK_OUTPUT_CHECKPOINT_UNAVAILABLE;
    }
    length = atomic_load_explicit(&peak_output_checkpoint_prefix_length,
                                  memory_order_acquire);
    if (length == 0 || out == NULL || out_size == 0 || length >= out_size) {
        return PEAK_OUTPUT_CHECKPOINT_UNAVAILABLE;
    }
    for (size_t i = 0; i < length; i++) out[i] = peak_output_checkpoint_prefix[i];
    output = length;
    do {
        digits[count++] = (char)('0' + checkpoint_index % 10U);
        checkpoint_index /= 10U;
    } while (checkpoint_index != 0 && count < sizeof(digits));
    if (output + 5 + count + 4 >= out_size) {
        return PEAK_OUTPUT_CHECKPOINT_UNAVAILABLE;
    }
    out[output++] = '-'; out[output++] = 'e'; out[output++] = 'x';
    out[output++] = 'e'; out[output++] = 'c';
    while (count != 0) out[output++] = digits[--count];
    out[output++] = '.'; out[output++] = 'c'; out[output++] = 's';
    out[output++] = 'v'; out[output] = '\0';
    return PEAK_OUTPUT_CHECKPOINT_READY;
}

static void
peak_output_identity_checkpoint_once(void)
{
    char final_path[PATH_MAX];
    const char* base = getenv("PEAK_STATSLOG_PATH");
    const char* template_value = getenv("PEAK_STATSLOG_TEMPLATE");
    size_t length;

    atomic_store_explicit(&peak_output_checkpoint_state,
                          PEAK_OUTPUT_CHECKPOINT_INITIALIZING,
                          memory_order_release);
    if (!atomic_load_explicit(&peak_output_session_ready, memory_order_acquire)) {
        atomic_store_explicit(&peak_output_checkpoint_state,
                              PEAK_OUTPUT_CHECKPOINT_INVALID,
                              memory_order_release);
        return;
    }
    if (base == NULL || base[0] == '\0') {
        base = "./peak_statslog";
    }
    if (template_value == NULL) {
        template_value = "";
    }
    if (!peak_output_identity_path(final_path, sizeof(final_path), base,
                                   template_value,
                                   ".csv", -1)) {
        atomic_store_explicit(&peak_output_checkpoint_state,
                              PEAK_OUTPUT_CHECKPOINT_INVALID, memory_order_release);
        return;
    }
    if (!peak_output_identity_make_parent(final_path)) {
        atomic_store_explicit(&peak_output_checkpoint_state,
                              PEAK_OUTPUT_CHECKPOINT_INVALID, memory_order_release);
        return;
    }
    length = strlen(final_path);
    if (length >= 4 && strcmp(final_path + length - 4, ".csv") == 0) length -= 4;
    if (length == 0 || length >= sizeof(peak_output_checkpoint_prefix)) {
        atomic_store_explicit(&peak_output_checkpoint_state,
                              PEAK_OUTPUT_CHECKPOINT_INVALID, memory_order_release);
        return;
    }
    memcpy(peak_output_checkpoint_prefix, final_path, length);
    peak_output_checkpoint_prefix[length] = '\0';
    atomic_store_explicit(&peak_output_checkpoint_prefix_length, (unsigned int)length,
                          memory_order_release);
    atomic_store_explicit(&peak_output_checkpoint_state,
                          PEAK_OUTPUT_CHECKPOINT_READY_STATE, memory_order_release);
}

void
peak_output_identity_initialize(void)
{
    int expected = PEAK_OUTPUT_CHECKPOINT_UNINITIALIZED;

    (void)atomic_compare_exchange_strong_explicit(
        &peak_output_checkpoint_state, &expected,
        PEAK_OUTPUT_CHECKPOINT_INITIALIZING,
        memory_order_release, memory_order_relaxed);
    (void)pthread_once(&peak_output_session_once,
                       peak_output_identity_session_once);
    (void)pthread_once(&peak_output_checkpoint_once,
                       peak_output_identity_checkpoint_once);
}

bool
peak_output_identity_make_parent(const char* path)
{
    char parent[PATH_MAX];
    char* slash;

    if (path == NULL || strlen(path) >= sizeof(parent)) {
        return false;
    }
    (void)snprintf(parent, sizeof(parent), "%s", path);
    slash = strrchr(parent, '/');
    if (slash == NULL) {
        return true;
    }
    *slash = '\0';
    if (parent[0] == '\0') {
        return true;
    }
    for (char* component = parent + (parent[0] == '/' ? 1 : 0);
         ; ) {
        char* next = strchr(component, '/');

        if (next == NULL) {
            break;
        }
        *next = '\0';
        if (component[0] != '\0' && mkdir(parent, 0777) != 0 && errno != EEXIST) {
            return false;
        }
        if (component[0] != '\0') {
            struct stat status;
            if (stat(parent, &status) != 0 || !S_ISDIR(status.st_mode)) {
                return false;
            }
        }
        *next = '/';
        component = next + 1;
    }
    if (parent[0] != '\0' && mkdir(parent, 0777) != 0 && errno != EEXIST) {
        return false;
    }
    if (parent[0] != '\0') {
        struct stat status;
        if (stat(parent, &status) != 0 || !S_ISDIR(status.st_mode)) {
            return false;
        }
    }
    return true;
}

static bool
peak_output_identity_session(char out[PEAK_OUTPUT_SESSION_CAPACITY])
{
    if (!atomic_load_explicit(&peak_output_session_ready, memory_order_acquire)) {
        peak_output_identity_initialize();
    }
    if (!atomic_load_explicit(&peak_output_session_ready, memory_order_acquire)) {
        return false;
    }
    return snprintf(out, PEAK_OUTPUT_SESSION_CAPACITY,
                    "%016llx", (unsigned long long)peak_output_session) ==
           PEAK_OUTPUT_SESSION_CAPACITY - 1;
}

static void
peak_output_identity_host(char out[PEAK_OUTPUT_HOST_CAPACITY])
{
    char raw[PEAK_OUTPUT_HOST_CAPACITY] = {0};
    size_t used = 0;

    if (gethostname(raw, sizeof(raw) - 1) != 0 || raw[0] == '\0') {
        (void)snprintf(raw, sizeof(raw), "unknown");
    }
    for (size_t i = 0; raw[i] != '\0' && used + 1 < PEAK_OUTPUT_HOST_CAPACITY;
         i++) {
        unsigned char byte = (unsigned char)raw[i];

        out[used++] = (byte >= 'a' && byte <= 'z') ||
                            (byte >= 'A' && byte <= 'Z') ||
                            (byte >= '0' && byte <= '9') ||
                            byte == '-' || byte == '_' || byte == '.'
                        ? (char)byte
                        : '_';
    }
    if (used == 0) {
        (void)snprintf(out, PEAK_OUTPUT_HOST_CAPACITY, "unknown");
    } else {
        out[used] = '\0';
    }
}

static void
peak_output_identity_sanitize(char* out, size_t out_size, const char* value)
{
    size_t used = 0;

    if (value == NULL || value[0] == '\0') {
        value = "none";
    }
    for (; *value != '\0' && used + 1 < out_size; value++) {
        unsigned char byte = (unsigned char)*value;

        out[used++] = (byte >= 'a' && byte <= 'z') ||
                            (byte >= 'A' && byte <= 'Z') ||
                            (byte >= '0' && byte <= '9') ||
                            byte == '-' || byte == '_' || byte == '.'
                        ? (char)byte
                        : '_';
    }
    if (used == 0 && out_size != 0) {
        (void)snprintf(out, out_size, "none");
    } else if (out_size != 0) {
        out[used] = '\0';
    }
}

static void
peak_output_identity_metadata(char* out, size_t out_size, const char* value)
{
    peak_output_identity_sanitize(out, out_size, value);
    for (size_t i = 0; out[i] != '\0'; i++) {
        if (out[i] == '.') {
            out[i] = '_';
        }
    }
}

static bool
peak_output_identity_append(char* out, size_t out_size, size_t* used,
                            const char* text)
{
    size_t length = strlen(text);

    if (*used > out_size || length >= out_size - *used) {
        return false;
    }
    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = '\0';
    return true;
}

bool
peak_output_identity_path(char* out,
                          size_t out_size,
                          const char* base,
                          const char* template_value,
                          const char* extension,
                          long rank)
{
    char host[PEAK_OUTPUT_HOST_CAPACITY];
    char pid[32];
    char rank_text[32];
    char session[PEAK_OUTPUT_SESSION_CAPACITY];
    char jobid[128];
    char stepid[128];
    size_t used = 0;

    if (out == NULL || out_size == 0 || base == NULL || extension == NULL ||
        !peak_output_identity_session(session)) {
        return false;
    }
    (void)snprintf(host, sizeof(host), "%s", peak_output_host);
    (void)snprintf(jobid, sizeof(jobid), "%s", peak_output_jobid);
    (void)snprintf(stepid, sizeof(stepid), "%s", peak_output_stepid);
    if (rank < 0) {
        (void)snprintf(rank_text, sizeof(rank_text), "%s", peak_output_rank);
    } else {
        (void)snprintf(rank_text, sizeof(rank_text), "%ld", rank);
    }
    (void)snprintf(pid, sizeof(pid), "%ld", (long)getpid());
    out[0] = '\0';
    if (template_value == NULL || template_value[0] == '\0') {
        return peak_output_identity_append(out, out_size, &used, base) &&
               peak_output_identity_append(out, out_size, &used, "-j") &&
               peak_output_identity_append(out, out_size, &used, jobid) &&
               peak_output_identity_append(out, out_size, &used, "-s") &&
               peak_output_identity_append(out, out_size, &used, stepid) &&
               peak_output_identity_append(out, out_size, &used, "-h") &&
               peak_output_identity_append(out, out_size, &used, host) &&
               peak_output_identity_append(out, out_size, &used, "-r") &&
               peak_output_identity_append(out, out_size, &used, rank_text) &&
               peak_output_identity_append(out, out_size, &used, "-p") &&
               peak_output_identity_append(out, out_size, &used, pid) &&
               peak_output_identity_append(out, out_size, &used, "-q") &&
               peak_output_identity_append(out, out_size, &used, session) &&
               peak_output_identity_append(out, out_size, &used, extension);
    }
    for (const char* cursor = template_value; *cursor != '\0';) {
        const char* replacement = NULL;

        if (*cursor != '{') {
            char literal[2] = {*cursor++, '\0'};
            if (!peak_output_identity_append(out, out_size, &used, literal)) {
                return false;
            }
            continue;
        }
        if (strncmp(cursor, "{jobid}", 7) == 0) { replacement = jobid; cursor += 7; }
        else if (strncmp(cursor, "{stepid}", 8) == 0) { replacement = stepid; cursor += 8; }
        else if (strncmp(cursor, "{host}", 6) == 0) { replacement = host; cursor += 6; }
        else if (strncmp(cursor, "{rank}", 6) == 0) { replacement = rank_text; cursor += 6; }
        else if (strncmp(cursor, "{pid}", 5) == 0) { replacement = pid; cursor += 5; }
        else if (strncmp(cursor, "{session}", 9) == 0) { replacement = session; cursor += 9; }
        else { return false; }
        if (!peak_output_identity_append(out, out_size, &used, replacement)) {
            return false;
        }
    }
    return true;
}
