#define _GNU_SOURCE
#include "internal/general_listener/socket_report_transport.h"

#include "internal/general_listener/report_maxima.h"
#include "internal/general_listener/report_model.h"
#include "internal/general_listener/runtime_config.h"
#include "logging.h"

#include <errno.h>
#include <float.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_MPI
#include <mpi.h>
#endif

#define PEAK_OUTPUT_AGGREGATION_HOST_ENV "PEAK_OUTPUT_AGGREGATION_HOST"
#define PEAK_OUTPUT_AGGREGATION_PORT_ENV "PEAK_OUTPUT_AGGREGATION_PORT"
#define PEAK_OUTPUT_AGGREGATION_TOKEN_ENV "PEAK_OUTPUT_AGGREGATION_TOKEN"
#define PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL"
#define PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_DROP_ONCE_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_DROP_ONCE"
#define PEAK_TEST_OUTPUT_AGGREGATION_RESOLVE_AGAIN_ONCE_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_RESOLVE_AGAIN_ONCE"
#define PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_FAIL_ONCE_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_FAIL_ONCE"
#define PEAK_TEST_OUTPUT_AGGREGATION_GATHER_CHUNK_BYTES_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_CHUNK_BYTES"
#define PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_MS_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_MS"
#define PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_RANK_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_RANK"
#define PEAK_TEST_OUTPUT_AGGREGATION_GATHER_PRECONNECT_DELAY_MS_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_PRECONNECT_DELAY_MS"
#define PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DISABLE_JITTER_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DISABLE_JITTER"
#define PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DROP_AFTER_BYTES_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DROP_AFTER_BYTES"
#define PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DROP_RANK_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DROP_RANK"
#define PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_DROP_AFTER_BYTES_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_DROP_AFTER_BYTES"
#define PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_DROP_RANK_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_DROP_RANK"
#define PEAK_TEST_OUTPUT_AGGREGATION_RECEIPT_FAIL_ONCE_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_RECEIPT_FAIL_ONCE"

#define PEAK_SOCKET_REDUCE_MAGIC 0x5045414b52454431ULL
#define PEAK_SOCKET_REDUCE_VERSION 11U
#define PEAK_SOCKET_REDUCE_GATHER_RECEIPT 0x41U
#define PEAK_SOCKET_REDUCE_GATHER_RECEIPT_CONFIRM 0x42U
#define PEAK_SOCKET_REDUCE_GATHER_REGISTERED 0x01U
#define PEAK_SOCKET_REDUCE_RELEASE_ACK 0x51U
#define PEAK_SOCKET_REDUCE_RELEASE_FALLBACK 0x52U
#define PEAK_SOCKET_REDUCE_RELEASE_REQUEST 0x61U
#define PEAK_SOCKET_REDUCE_RELEASE_DECISION 0x62U
#define PEAK_SOCKET_REDUCE_RELEASE_CONFIRM 0x63U
#define PEAK_SOCKET_REDUCE_DEFAULT_PORT_BASE 42000
#define PEAK_SOCKET_REDUCE_DEFAULT_PORT_SPAN 20000
#define PEAK_SOCKET_REDUCE_CONNECT_ATTEMPT_MS 500
#define PEAK_SOCKET_REDUCE_CONNECT_BACKOFF_MIN_MS 10
#define PEAK_SOCKET_REDUCE_CONNECT_BACKOFF_MAX_MS 250
#define PEAK_SOCKET_REDUCE_PEER_IO_TIMEOUT_MS 5000
#define PEAK_SOCKET_REDUCE_GATHER_ACTIVE_MAX 128U
#define PEAK_SOCKET_REDUCE_FD_RESERVE 32U
#define PEAK_SOCKET_REDUCE_GATHER_SALT 0x676174686572ULL
#define PEAK_SOCKET_REDUCE_RELEASE_SALT 0x72656c65617365ULL
#define PEAK_SOCKET_REDUCE_RESOLVE_SALT 0x7265736f6c7665ULL

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t rank;
    uint64_t hook_count;
    uint64_t session_token;
    double elapsed_seconds;
    double profile_seconds;
    double control_seconds;
    double management_seconds;
    double control_risk_seconds;
    double profile_control_risk_seconds;
    double profile_ratio;
    double control_ratio;
    double profile_control_ratio;
    double profile_control_risk_ratio;
    double control_risk_ratio;
    double management_ratio;
    uint64_t stop_window_count;
    uint64_t failed_stop_window_count;
    uint32_t local_ranks;
    uint32_t accounting_valid;
} PeakSocketReduceHeader;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t rank;
    uint64_t hook_count;
    uint64_t session_token;
    uint8_t type;
    uint8_t decision;
    uint8_t reserved[6];
} PeakSocketReduceReleaseFrame;

typedef struct {
    uint64_t identity_hash;
    uint64_t num_calls;
    double total_time;
    double max_total_time;
    double min_total_time;
    double exclusive_time;
    float max_time;
    float min_time;
    uint64_t thread_count;
    int detached;
    int reattached;
    int revisited;
} PeakSocketReduceRecord;

/*
 * Wire-v11 intentionally targets a homogeneous job: every rank must use the
 * same byte order, floating-point representation, and 64-bit Linux C ABI.
 * Lock the layouts so an accidental field or packing change cannot silently
 * corrupt a report without another wire-version bump.
 */
_Static_assert(sizeof(PeakSocketReduceHeader) == 152,
               "wire-v11 header layout changed");
_Static_assert(sizeof(PeakSocketReduceReleaseFrame) == 40,
               "wire-v11 control-frame layout changed");
_Static_assert(sizeof(PeakSocketReduceRecord) == 80,
               "wire-v11 record layout changed");
_Static_assert(sizeof(unsigned long) == sizeof(uint64_t),
               "wire-v11 requires a 64-bit unsigned long");

struct PeakSocketReportSession {
    bool* release_targets;
    int size;
    int release_port;
    int phase_timeout_ms;
    uint64_t hook_count;
    uint64_t session_token;
};

typedef enum {
    PEAK_SOCKET_RELEASE_INVALID = 0,
    PEAK_SOCKET_RELEASE_ACKNOWLEDGED,
    PEAK_SOCKET_RELEASE_FALLBACK,
} PeakSocketReleaseResult;

#ifdef PEAK_ENABLE_TEST_HOOKS
static PeakSocketReportTestTelemetry peak_socket_test_telemetry = {
    .wire_version = PEAK_SOCKET_REDUCE_VERSION,
};

void
peak_socket_report_test_telemetry_reset(void)
{
    memset(&peak_socket_test_telemetry,
           0,
           sizeof(peak_socket_test_telemetry));
    peak_socket_test_telemetry.wire_version =
        PEAK_SOCKET_REDUCE_VERSION;
}

void
peak_socket_report_test_telemetry_get(
    PeakSocketReportTestTelemetry* telemetry_out)
{
    if (telemetry_out != NULL) {
        *telemetry_out = peak_socket_test_telemetry;
    }
}
#endif

static bool
peak_socket_positive_finite(double value)
{
    return value > 0.0 && value == value && value <= DBL_MAX;
}

static uint64_t
peak_socket_add_uint64_saturated(uint64_t lhs, uint64_t rhs)
{
    const uint64_t max_published = UINT64_MAX - 1;

    if (lhs >= max_published || rhs > max_published - lhs) {
        return max_published;
    }
    return lhs + rhs;
}

static void
peak_socket_reduce_hash_text(uint64_t* hash,
                             const char* label,
                             const char* value)
{
    const unsigned char* text;

    if (hash == NULL || value == NULL || value[0] == '\0') {
        return;
    }

    for (text = (const unsigned char*)label;
         text != NULL && *text != '\0';
         text++) {
        *hash ^= (uint64_t)*text;
        *hash *= 1099511628211ULL;
    }
    *hash ^= (uint64_t)'=';
    *hash *= 1099511628211ULL;
    for (text = (const unsigned char*)value; *text != '\0'; text++) {
        *hash ^= (uint64_t)*text;
        *hash *= 1099511628211ULL;
    }
    *hash ^= (uint64_t)';';
    *hash *= 1099511628211ULL;
}

static uint64_t
peak_socket_reduce_session_token(void)
{
    static const char* shared_env_names[] = {
        "SLURM_JOB_ID",
        "SLURM_STEP_ID",
        "SLURM_STEPID",
        "SLURM_JOB_UID",
        "SLURM_CLUSTER_NAME",
        "SLURM_NODELIST",
        "SLURM_JOB_NODELIST",
        "PMI_JOBID",
        "PMI_KVS",
        "PMI_NAMESPACE",
        "PMIX_NAMESPACE",
        "OMPI_COMM_WORLD_JOBID",
        NULL,
    };
    const char* override = getenv(PEAK_OUTPUT_AGGREGATION_TOKEN_ENV);
    uint64_t hash = 1469598103934665603ULL;
    bool saw_shared_value = false;

    if (override != NULL && override[0] != '\0') {
        peak_socket_reduce_hash_text(&hash,
                                     PEAK_OUTPUT_AGGREGATION_TOKEN_ENV,
                                     override);
        return hash;
    }

    for (size_t i = 0; shared_env_names[i] != NULL; i++) {
        const char* value = getenv(shared_env_names[i]);

        if (value != NULL && value[0] != '\0') {
            peak_socket_reduce_hash_text(&hash, shared_env_names[i], value);
            saw_shared_value = true;
        }
    }

    if (!saw_shared_value) {
        peak_socket_reduce_hash_text(&hash, "fallback", "single-launcher");
    }
    return hash;
}

static void
peak_socket_reduce_header_set_report_tuple(
    PeakSocketReduceHeader* header,
    const PeakReportRankTuple* tuple)
{
    if (header == NULL || tuple == NULL) {
        return;
    }

    header->elapsed_seconds = tuple->elapsed_seconds;
    header->profile_seconds = tuple->profile_seconds;
    header->control_seconds = tuple->control_seconds;
    header->management_seconds = tuple->management_seconds;
    header->control_risk_seconds = tuple->control_risk_seconds;
    header->profile_control_risk_seconds =
        tuple->profile_control_risk_seconds;
    header->profile_ratio = tuple->profile_ratio;
    header->control_ratio = tuple->control_ratio;
    header->profile_control_ratio = tuple->ratio;
    header->profile_control_risk_ratio =
        tuple->profile_control_risk_ratio;
    header->control_risk_ratio = tuple->control_risk_ratio;
    header->management_ratio = tuple->management_ratio;
    header->stop_window_count = tuple->stop_window_count;
    header->failed_stop_window_count = tuple->failed_stop_window_count;
    header->local_ranks = tuple->local_ranks;
    header->accounting_valid = tuple->accounting_valid ? 1U : 0U;
}

static PeakReportRankTuple
peak_socket_reduce_header_report_tuple(
    const PeakSocketReduceHeader* header)
{
    PeakReportRankTuple tuple = {0};

    if (header == NULL) {
        return tuple;
    }

    tuple.accounting_valid = header->accounting_valid != 0U;
    tuple.local_ranks = header->local_ranks;
    tuple.stop_window_count = header->stop_window_count;
    tuple.failed_stop_window_count = header->failed_stop_window_count;
    tuple.elapsed_seconds = header->elapsed_seconds;
    tuple.profile_seconds = header->profile_seconds;
    tuple.control_seconds = header->control_seconds;
    tuple.management_seconds = header->management_seconds;
    tuple.control_risk_seconds = header->control_risk_seconds;
    tuple.profile_control_risk_seconds =
        header->profile_control_risk_seconds;
    tuple.profile_ratio = header->profile_ratio;
    tuple.control_ratio = header->control_ratio;
    tuple.profile_control_risk_ratio =
        header->profile_control_risk_ratio;
    tuple.control_risk_ratio = header->control_risk_ratio;
    tuple.management_ratio = header->management_ratio;
    tuple.ratio = header->profile_control_ratio;
    return tuple;
}

static int
peak_socket_reduce_parse_positive_int_env(const char* name,
                                          int default_value)
{
    const char* value = getenv(name);
    char* end = NULL;
    long parsed;

    if (value == NULL || value[0] == '\0') {
        return default_value;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0 ||
        parsed > INT_MAX) {
        peak_log_info("[peak] Ignoring invalid %s=%s\n", name, value);
        return default_value;
    }

    return (int)parsed;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
static bool
peak_socket_reduce_parse_test_size_env(const char* name,
                                       size_t* value_out)
{
    const char* value = getenv(name);
    char* end = NULL;
    unsigned long long parsed;

    if (value_out == NULL || value == NULL || value[0] == '\0') {
        return false;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed > (unsigned long long)SIZE_MAX) {
        return false;
    }
    *value_out = (size_t)parsed;
    return true;
}

static size_t
peak_socket_reduce_test_gather_chunk_bytes(void)
{
    size_t chunk_bytes;

    if (!peak_socket_reduce_parse_test_size_env(
            PEAK_TEST_OUTPUT_AGGREGATION_GATHER_CHUNK_BYTES_ENV,
            &chunk_bytes) ||
        chunk_bytes == 0) {
        return SIZE_MAX;
    }
    return chunk_bytes;
}

static bool
peak_socket_reduce_test_gather_delay_applies(uint32_t rank,
                                             int* delay_ms_out)
{
    int delay_ms = peak_socket_reduce_parse_positive_int_env(
        PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_MS_ENV, 0);
    int delay_rank = peak_socket_reduce_parse_positive_int_env(
        PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DELAY_RANK_ENV, -1);

    if (delay_ms_out != NULL) {
        *delay_ms_out = delay_ms;
    }
    return delay_ms > 0 && delay_rank >= 0 &&
           rank == (uint32_t)delay_rank;
}
#else
static size_t
peak_socket_reduce_test_gather_chunk_bytes(void)
{
    return SIZE_MAX;
}
#endif

static int
peak_socket_reduce_default_port(void)
{
    const char* job_id = getenv("SLURM_JOB_ID");
    char* end = NULL;
    long parsed = 0;

    if (job_id != NULL && job_id[0] != '\0') {
        errno = 0;
        parsed = strtol(job_id, &end, 10);
        if (errno != 0 || end == job_id) {
            parsed = 0;
        }
    }

    if (parsed < 0) {
        parsed = -parsed;
    }

    return PEAK_SOCKET_REDUCE_DEFAULT_PORT_BASE +
           (int)(parsed % PEAK_SOCKET_REDUCE_DEFAULT_PORT_SPAN);
}

static int
peak_socket_reduce_port(void)
{
    int port = peak_socket_reduce_parse_positive_int_env(
        PEAK_OUTPUT_AGGREGATION_PORT_ENV,
        peak_socket_reduce_default_port());

    if (port <= 0 || port > 65535) {
        peak_log_info("[peak] Ignoring out-of-range %s=%d\n",
                      PEAK_OUTPUT_AGGREGATION_PORT_ENV,
                      port);
        return peak_socket_reduce_default_port();
    }

    return port;
}

static int64_t
peak_socket_reduce_monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static int64_t
peak_socket_reduce_deadline_us(int timeout_ms)
{
    return peak_socket_reduce_monotonic_us() + (int64_t)timeout_ms * 1000;
}

static int
peak_socket_reduce_remaining_ms(int64_t deadline_us)
{
    int64_t now = peak_socket_reduce_monotonic_us();
    int64_t remaining = deadline_us - now;

    if (remaining <= 0) {
        return 0;
    }

    remaining = (remaining + 999) / 1000;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

static int64_t
peak_socket_reduce_capped_deadline_us(int64_t outer_deadline_us,
                                      int cap_ms)
{
    int64_t cap_deadline_us = peak_socket_reduce_deadline_us(cap_ms);

    return cap_deadline_us < outer_deadline_us
               ? cap_deadline_us
               : outer_deadline_us;
}

static uint64_t
peak_socket_reduce_jitter_value(uint32_t rank,
                                uint64_t session_token,
                                uint32_t attempt,
                                uint64_t phase_salt)
{
    uint64_t value =
        session_token ^ phase_salt ^
        ((uint64_t)rank + 1U) * 0x9e3779b97f4a7c15ULL ^
        ((uint64_t)attempt + 1U) * 0xbf58476d1ce4e5b9ULL;

    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

static void
peak_socket_reduce_sleep_before_deadline(int delay_ms, int64_t deadline_us)
{
    int remaining_ms = peak_socket_reduce_remaining_ms(deadline_us);
    struct timespec delay;

    if (delay_ms <= 0 || remaining_ms <= 0) {
        return;
    }
    if (delay_ms > remaining_ms) {
        delay_ms = remaining_ms;
    }
    delay.tv_sec = delay_ms / 1000;
    delay.tv_nsec = (long)(delay_ms % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static void
peak_socket_reduce_connect_backoff(uint32_t rank,
                                   uint64_t session_token,
                                   uint32_t attempt,
                                   uint64_t phase_salt,
                                   int64_t deadline_us)
{
    unsigned int shift = attempt < 5U ? attempt : 5U;
    int base_ms = PEAK_SOCKET_REDUCE_CONNECT_BACKOFF_MIN_MS << shift;
    uint64_t jitter;
    int delay_ms;

    if (base_ms > PEAK_SOCKET_REDUCE_CONNECT_BACKOFF_MAX_MS) {
        base_ms = PEAK_SOCKET_REDUCE_CONNECT_BACKOFF_MAX_MS;
    }
    jitter = peak_socket_reduce_jitter_value(
        rank, session_token, attempt, phase_salt);
    delay_ms = base_ms / 2 +
               (int)(jitter % (uint64_t)(base_ms - base_ms / 2 + 1));
    peak_socket_reduce_sleep_before_deadline(delay_ms, deadline_us);
}

static bool
peak_socket_reduce_poll_fd(int fd, short events, int64_t deadline_us)
{
    while (peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        struct pollfd descriptor = {
            .fd = fd,
            .events = events,
            .revents = 0,
        };
        int result = poll(&descriptor,
                          1,
                          peak_socket_reduce_remaining_ms(deadline_us));

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        return (descriptor.revents & events) != 0;
    }

    return false;
}

static bool
peak_socket_reduce_send_all(int fd,
                            const void* data,
                            size_t size,
                            int64_t deadline_us)
{
    const char* cursor = (const char*)data;

    while (size > 0 && peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        ssize_t written;

        if (!peak_socket_reduce_poll_fd(fd, POLLOUT, deadline_us)) {
            return false;
        }
        written = send(fd, cursor, size, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        cursor += written;
        size -= (size_t)written;
    }

    return size == 0;
}

static bool
peak_socket_reduce_send_gather_all(int fd,
                                   const void* data,
                                   size_t size,
                                   size_t* total_written,
                                   uint32_t rank,
                                   int64_t deadline_us)
{
    const char* cursor = (const char*)data;
    size_t chunk_limit =
        peak_socket_reduce_test_gather_chunk_bytes();
#ifdef PEAK_ENABLE_TEST_HOOKS
    size_t drop_after = 0;
    size_t drop_rank = SIZE_MAX;
    bool drop_enabled = peak_socket_reduce_parse_test_size_env(
        PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DROP_AFTER_BYTES_ENV,
        &drop_after) &&
        peak_socket_reduce_parse_test_size_env(
            PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DROP_RANK_ENV,
            &drop_rank) &&
        drop_rank == (size_t)rank;
#else
    (void)rank;
#endif

    if (total_written == NULL) {
        return false;
    }
    while (size > 0 &&
           peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        size_t request = size < chunk_limit ? size : chunk_limit;
        ssize_t written;

#ifdef PEAK_ENABLE_TEST_HOOKS
        if (drop_enabled) {
            if (*total_written >= drop_after) {
                return false;
            }
            if (request > drop_after - *total_written) {
                request = drop_after - *total_written;
            }
        }
#endif
        if (request == 0 ||
            !peak_socket_reduce_poll_fd(fd, POLLOUT, deadline_us)) {
            return false;
        }
        written = send(fd, cursor, request, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0 ||
            *total_written > SIZE_MAX - (size_t)written) {
            return false;
        }
        cursor += written;
        size -= (size_t)written;
        *total_written += (size_t)written;
    }

    return size == 0;
}

static bool
peak_socket_reduce_send_gather_confirmation(
    int fd,
    const PeakSocketReduceReleaseFrame* confirmation,
    uint32_t rank,
    int64_t deadline_us)
{
#ifdef PEAK_ENABLE_TEST_HOOKS
    size_t drop_after = 0;
    size_t drop_rank = SIZE_MAX;

    if (peak_socket_reduce_parse_test_size_env(
            PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_DROP_AFTER_BYTES_ENV,
            &drop_after) &&
        peak_socket_reduce_parse_test_size_env(
            PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_DROP_RANK_ENV,
            &drop_rank) &&
        drop_rank == (size_t)rank &&
        drop_after < sizeof(*confirmation)) {
        if (drop_after > 0) {
            (void)peak_socket_reduce_send_all(fd,
                                              confirmation,
                                              drop_after,
                                              deadline_us);
        }
        return false;
    }
#else
    (void)rank;
#endif
    return peak_socket_reduce_send_all(fd,
                                       confirmation,
                                       sizeof(*confirmation),
                                       deadline_us);
}

static bool
peak_socket_reduce_recv_all(int fd,
                            void* data,
                            size_t size,
                            int64_t deadline_us)
{
    char* cursor = (char*)data;

    while (size > 0 && peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        ssize_t read_count;

        if (!peak_socket_reduce_poll_fd(fd, POLLIN, deadline_us)) {
            return false;
        }
        read_count = recv(fd, cursor, size, MSG_WAITALL);
        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (read_count == 0) {
            return false;
        }
        cursor += read_count;
        size -= (size_t)read_count;
    }

    return size == 0;
}

static void
peak_socket_reduce_set_timeout(int fd, int timeout_ms)
{
    struct timeval timeout;

    if (timeout_ms <= 0) {
        timeout_ms = 1;
    }

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    (void)setsockopt(fd,
                     SOL_SOCKET,
                     SO_RCVTIMEO,
                     &timeout,
                     sizeof(timeout));
    (void)setsockopt(fd,
                     SOL_SOCKET,
                     SO_SNDTIMEO,
                     &timeout,
                     sizeof(timeout));
}

static bool
peak_socket_reduce_hostlist_token_is_ascending_range(
    const char* token,
    size_t token_len,
    size_t* range_prefix_len)
{
    const char* dash = memchr(token, '-', token_len);
    char* end = NULL;
    long first;
    long last;

    if (range_prefix_len != NULL) {
        *range_prefix_len = token_len;
    }
    if (dash == NULL || dash == token || dash + 1 >= token + token_len) {
        return false;
    }
    for (const char* cursor = token; cursor < token + token_len; cursor++) {
        if (cursor == dash) {
            continue;
        }
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
    }

    errno = 0;
    first = strtol(token, &end, 10);
    if (errno != 0 || end != dash) {
        return false;
    }
    errno = 0;
    last = strtol(dash + 1, &end, 10);
    if (errno != 0 || end != token + token_len || first > last) {
        return false;
    }

    if (range_prefix_len != NULL) {
        *range_prefix_len = (size_t)(dash - token);
    }
    return true;
}

static bool
peak_socket_reduce_first_host_from_slurm_nodelist(const char* nodelist,
                                                  char* out,
                                                  size_t out_size)
{
    const char* bracket;
    const char* comma;
    const char* token_end;
    size_t prefix_len;
    size_t token_len;
    size_t host_token_len;

    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    if (nodelist == NULL || nodelist[0] == '\0') {
        return false;
    }

    bracket = strchr(nodelist, '[');
    comma = strchr(nodelist, ',');
    if (bracket == NULL || (comma != NULL && comma < bracket)) {
        token_len =
            comma != NULL ? (size_t)(comma - nodelist) : strlen(nodelist);
        if (token_len == 0 || token_len >= out_size) {
            return false;
        }
        memcpy(out, nodelist, token_len);
        out[token_len] = '\0';
        return true;
    }

    prefix_len = (size_t)(bracket - nodelist);
    comma = strchr(bracket + 1, ',');
    token_end = strchr(bracket + 1, ']');
    if (token_end == NULL) {
        return false;
    }
    if (comma != NULL && comma < token_end) {
        token_end = comma;
    }
    token_len = (size_t)(token_end - (bracket + 1));
    host_token_len = token_len;
    (void)peak_socket_reduce_hostlist_token_is_ascending_range(
        bracket + 1,
        token_len,
        &host_token_len);
    if (prefix_len + host_token_len == 0 ||
        prefix_len + host_token_len >= out_size) {
        return false;
    }
    memcpy(out, nodelist, prefix_len);
    memcpy(out + prefix_len, bracket + 1, host_token_len);
    out[prefix_len + host_token_len] = '\0';
    return true;
}

static bool
peak_socket_reduce_first_slurm_host(char* out, size_t out_size)
{
    const char* nodelist = getenv("SLURM_NODELIST");

    if (nodelist == NULL || nodelist[0] == '\0') {
        nodelist = getenv("SLURM_JOB_NODELIST");
    }

    return peak_socket_reduce_first_host_from_slurm_nodelist(nodelist,
                                                             out,
                                                             out_size);
}

#ifdef PEAK_ENABLE_TEST_HOOKS
int
peak_general_listener_test_first_slurm_host(const char* nodelist,
                                            char* out,
                                            size_t out_size)
{
    return peak_socket_reduce_first_host_from_slurm_nodelist(nodelist,
                                                             out,
                                                             out_size)
               ? 1
               : 0;
}
#endif

static size_t
peak_socket_reduce_strlcpy(char* destination,
                           const char* source,
                           size_t destination_size)
{
    size_t source_length = strlen(source);

    if (destination_size != 0) {
        size_t copy_length = source_length < destination_size - 1
                                 ? source_length
                                 : destination_size - 1;

        memcpy(destination, source, copy_length);
        destination[copy_length] = '\0';
    }
    return source_length;
}

static bool
peak_socket_reduce_root_host(char* out, size_t out_size)
{
    const char* override = getenv(PEAK_OUTPUT_AGGREGATION_HOST_ENV);

    if (override != NULL && override[0] != '\0') {
        (void)peak_socket_reduce_strlcpy(out, override, out_size);
        return true;
    }

    if (peak_socket_reduce_first_slurm_host(out, out_size)) {
        return true;
    }

    (void)peak_socket_reduce_strlcpy(out, "127.0.0.1", out_size);
    return true;
}

static bool
peak_socket_reduce_record_bytes(size_t hook_count, size_t* bytes_out)
{
    if (bytes_out == NULL ||
        hook_count > SIZE_MAX / sizeof(PeakSocketReduceRecord)) {
        return false;
    }
    *bytes_out = hook_count * sizeof(PeakSocketReduceRecord);
    return true;
}

static bool
peak_socket_reduce_build_records(const PeakReportSnapshot* snapshot,
                                 PeakSocketReduceRecord** records_out)
{
    PeakSocketReduceRecord* records = NULL;
    size_t bytes;

    if (snapshot == NULL || records_out == NULL ||
        !peak_socket_reduce_record_bytes(snapshot->hook_count, &bytes)) {
        return false;
    }
    *records_out = NULL;
    if (bytes != 0) {
        records = calloc(1, bytes);
        if (records == NULL) {
            return false;
        }
    }

    for (size_t i = 0; i < snapshot->hook_count; i++) {
        records[i].identity_hash =
            peak_report_snapshot_slot_identity_hash(snapshot, i);
        records[i].num_calls = (uint64_t)snapshot->num_calls[i];
        records[i].total_time = snapshot->total_time[i];
        records[i].max_total_time = snapshot->max_total_time[i];
        records[i].min_total_time = snapshot->min_total_time[i];
        records[i].exclusive_time = snapshot->exclusive_time[i];
        records[i].max_time = snapshot->max_time[i];
        records[i].min_time = snapshot->min_time[i];
        records[i].thread_count = (uint64_t)snapshot->thread_count[i];
        records[i].detached = snapshot->detached[i] ? 1 : 0;
        records[i].reattached = snapshot->reattached[i] ? 1 : 0;
        records[i].revisited = snapshot->revisited[i] ? 1 : 0;
    }

    *records_out = records;
    return true;
}

static int
peak_socket_reduce_create_listener(int port, int expected_connections)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    int backlog = expected_connections > 0 ? expected_connections : 1;
    int flags;
    struct sockaddr_in address;

    if (fd < 0) {
        return -1;
    }

    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0 ||
        listen(fd, backlog) != 0 ||
        (flags = fcntl(fd, F_GETFL, 0)) < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return -1;
    }

    return fd;
}

static bool
peak_socket_reduce_wait_connected(int fd, int64_t deadline_us)
{
    while (peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        struct pollfd descriptor = {
            .fd = fd,
            .events = POLLOUT,
            .revents = 0,
        };
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        int poll_result = poll(
            &descriptor,
            1,
            peak_socket_reduce_remaining_ms(deadline_us));

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (poll_result == 0) {
            return false;
        }
        if (getsockopt(fd,
                       SOL_SOCKET,
                       SO_ERROR,
                       &socket_error,
                       &socket_error_size) != 0 ||
            socket_error != 0) {
            return false;
        }
        return true;
    }

    return false;
}

static bool
peak_socket_reduce_resolve_root(const char* host,
                                int64_t deadline_us,
                                uint32_t rank,
                                uint64_t session_token,
                                struct addrinfo** addresses_out)
{
    char qualified_host[NI_MAXHOST];
    struct addrinfo hints;
    const char* candidate;
    uint32_t attempt = 0;
    bool using_qualified = false;

    if (host == NULL || host[0] == '\0' || addresses_out == NULL) {
        errno = EINVAL;
        return false;
    }
    *addresses_out = NULL;
    memset(&hints, 0, sizeof(hints));
    /*
     * The root listener is IPv4. Restricting resolution to the address family
     * it can actually accept also avoids paying a connect timeout for unusable
     * IPv6 answers on every rank.
     */
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    qualified_host[0] = '\0';
    if (strchr(host, '.') == NULL) {
        char local_host[NI_MAXHOST];

        if (gethostname(local_host, sizeof(local_host)) == 0) {
            char* domain;

            local_host[sizeof(local_host) - 1] = '\0';
            domain = strchr(local_host, '.');
            if (domain != NULL &&
                strlen(host) + strlen(domain) < sizeof(qualified_host)) {
                snprintf(qualified_host,
                         sizeof(qualified_host),
                         "%s%s",
                         host,
                         domain);
            }
        }
    }

    candidate = host;
    while (peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        struct addrinfo* addresses = NULL;
        int resolve_result;
#ifdef PEAK_ENABLE_TEST_HOOKS
        static bool injected_transient_failure = false;

        if (!injected_transient_failure &&
            peak_general_listener_env_value_truthy(
                getenv(
                    PEAK_TEST_OUTPUT_AGGREGATION_RESOLVE_AGAIN_ONCE_ENV))) {
            injected_transient_failure = true;
            resolve_result = EAI_AGAIN;
        } else
#endif
        {
            resolve_result =
                getaddrinfo(candidate, NULL, &hints, &addresses);
        }
        int saved_errno = errno;
        bool transient =
            resolve_result == EAI_AGAIN ||
            (resolve_result == EAI_SYSTEM &&
             (saved_errno == EAGAIN || saved_errno == EINTR));

        if (resolve_result == 0 && addresses != NULL) {
            *addresses_out = addresses;
            return true;
        }
        if (addresses != NULL) {
            freeaddrinfo(addresses);
        }
        if (transient) {
            peak_socket_reduce_connect_backoff(
                rank,
                session_token,
                attempt++,
                PEAK_SOCKET_REDUCE_RESOLVE_SALT,
                deadline_us);
            continue;
        }
        if (!using_qualified && qualified_host[0] != '\0' &&
            strcmp(host, qualified_host) != 0) {
            candidate = qualified_host;
            using_qualified = true;
            attempt = 0;
            continue;
        }
        errno = resolve_result == EAI_SYSTEM && saved_errno != 0
                    ? saved_errno
                    : EHOSTUNREACH;
        return false;
    }

    errno = ETIMEDOUT;
    return false;
}

static bool
peak_socket_reduce_address_with_port(const struct addrinfo* entry,
                                     int port,
                                     struct sockaddr_storage* address,
                                     socklen_t* address_size)
{
    if (entry == NULL || entry->ai_addr == NULL || address == NULL ||
        address_size == NULL ||
        entry->ai_addrlen > (socklen_t)sizeof(*address)) {
        return false;
    }

    memset(address, 0, sizeof(*address));
    memcpy(address, entry->ai_addr, entry->ai_addrlen);
    if (entry->ai_family == AF_INET) {
        ((struct sockaddr_in*)address)->sin_port =
            htons((uint16_t)port);
    } else if (entry->ai_family == AF_INET6) {
        ((struct sockaddr_in6*)address)->sin6_port =
            htons((uint16_t)port);
    } else {
        return false;
    }
    *address_size = (socklen_t)entry->ai_addrlen;
    return true;
}

static int
peak_socket_reduce_connect(const struct addrinfo* addresses,
                           int port,
                           int64_t deadline_us,
                           uint32_t rank,
                           uint64_t session_token,
                           uint64_t phase_salt)
{
    uint32_t attempt = 0;

    if (addresses == NULL) {
        errno = EHOSTUNREACH;
        return -1;
    }

    while (peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        for (const struct addrinfo* entry = addresses;
             entry != NULL;
             entry = entry->ai_next) {
            struct sockaddr_storage address;
            socklen_t address_size;
            int64_t attempt_deadline_us;
            int fd;
            int flags;

            if (!peak_socket_reduce_address_with_port(
                    entry, port, &address, &address_size)) {
                continue;
            }
            attempt_deadline_us =
                peak_socket_reduce_capped_deadline_us(
                    deadline_us,
                    PEAK_SOCKET_REDUCE_CONNECT_ATTEMPT_MS);
            fd = socket(entry->ai_family,
                        entry->ai_socktype,
                        entry->ai_protocol);
            if (fd < 0) {
                continue;
            }
            flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0 ||
                fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
                close(fd);
                continue;
            }
            if (connect(fd,
                        (const struct sockaddr*)&address,
                        address_size) == 0 ||
                (errno == EINPROGRESS &&
                 peak_socket_reduce_wait_connected(
                     fd, attempt_deadline_us))) {
                (void)fcntl(fd, F_SETFL, flags);
                peak_socket_reduce_set_timeout(
                    fd,
                    peak_socket_reduce_remaining_ms(deadline_us));
                return fd;
            }
            close(fd);
        }
        peak_socket_reduce_connect_backoff(
            rank,
            session_token,
            attempt++,
            phase_salt,
            deadline_us);
    }

    errno = ETIMEDOUT;
    return -1;
}

static int
peak_socket_reduce_release_port(int port)
{
    return port < 65535 ? port + 1 : port - 1;
}

static PeakSocketReleaseResult
peak_socket_reduce_wait_for_release(const struct addrinfo* addresses,
                                    int port,
                                    const PeakSocketReduceHeader* header,
                                    int timeout_ms)
{
    int64_t deadline_us = peak_socket_reduce_deadline_us(timeout_ms);
    uint32_t handshake_attempt = 0;
    uint8_t accepted_decision = 0;

    if (header == NULL) {
        return PEAK_SOCKET_RELEASE_INVALID;
    }

    while (peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        int fd = peak_socket_reduce_connect(
            addresses,
            port,
            deadline_us,
            header->rank,
            header->session_token,
            PEAK_SOCKET_REDUCE_RELEASE_SALT ^ handshake_attempt);
        PeakSocketReduceReleaseFrame frame;
        int64_t io_deadline_us;
        bool valid;

        if (fd < 0) {
            break;
        }
        io_deadline_us = peak_socket_reduce_capped_deadline_us(
            deadline_us, PEAK_SOCKET_REDUCE_PEER_IO_TIMEOUT_MS);
        memset(&frame, 0, sizeof(frame));
        frame.magic = header->magic;
        frame.version = header->version;
        frame.rank = header->rank;
        frame.hook_count = header->hook_count;
        frame.session_token = header->session_token;
        frame.type = PEAK_SOCKET_REDUCE_RELEASE_REQUEST;
        valid = peak_socket_reduce_send_all(
                    fd, &frame, sizeof(frame), io_deadline_us) &&
                peak_socket_reduce_recv_all(
                    fd, &frame, sizeof(frame), io_deadline_us) &&
                frame.magic == header->magic &&
                frame.version == header->version &&
                frame.rank == header->rank &&
                frame.hook_count == header->hook_count &&
                frame.session_token == header->session_token &&
                frame.type == PEAK_SOCKET_REDUCE_RELEASE_DECISION &&
                (frame.decision == PEAK_SOCKET_REDUCE_RELEASE_ACK ||
                 frame.decision ==
                     PEAK_SOCKET_REDUCE_RELEASE_FALLBACK);
        if (valid) {
            if (accepted_decision == 0) {
                accepted_decision = frame.decision;
#ifdef PEAK_ENABLE_TEST_HOOKS
                peak_socket_test_telemetry
                    .peer_release_decision_received = true;
                peak_socket_test_telemetry.peer_release_decision =
                    frame.decision;
#endif
            } else if (frame.decision != accepted_decision) {
                /*
                 * A validated decision is authoritative: ACK means the
                 * aggregate is already published, while FALLBACK means it
                 * will not be. Never turn an accepted ACK into local output
                 * merely because a retry observed an inconsistent peer.
                 */
                close(fd);
                break;
            }
            frame.type = PEAK_SOCKET_REDUCE_RELEASE_CONFIRM;
#ifdef PEAK_ENABLE_TEST_HOOKS
            static bool injected_confirm_failure = false;

            if (!injected_confirm_failure &&
                peak_general_listener_env_value_truthy(
                    getenv(
                        PEAK_TEST_OUTPUT_AGGREGATION_CONFIRM_FAIL_ONCE_ENV))) {
                injected_confirm_failure = true;
                valid = false;
            } else
#endif
            {
                valid = peak_socket_reduce_send_all(
                    fd, &frame, sizeof(frame), io_deadline_us);
            }
#ifdef PEAK_ENABLE_TEST_HOOKS
            if (valid) {
                peak_socket_test_telemetry
                    .peer_release_confirmation_sent = true;
            }
#endif
        }
        close(fd);
        if (valid) {
            return accepted_decision == PEAK_SOCKET_REDUCE_RELEASE_ACK
                       ? PEAK_SOCKET_RELEASE_ACKNOWLEDGED
                       : PEAK_SOCKET_RELEASE_FALLBACK;
        }
        peak_socket_reduce_connect_backoff(
            header->rank,
            header->session_token,
            handshake_attempt++,
            PEAK_SOCKET_REDUCE_RELEASE_SALT,
            deadline_us);
    }

    if (accepted_decision == PEAK_SOCKET_REDUCE_RELEASE_ACK) {
        return PEAK_SOCKET_RELEASE_ACKNOWLEDGED;
    }
    if (accepted_decision == PEAK_SOCKET_REDUCE_RELEASE_FALLBACK) {
        return PEAK_SOCKET_RELEASE_FALLBACK;
    }
    return PEAK_SOCKET_RELEASE_INVALID;
}

static bool
peak_socket_reduce_release_peers(int port,
                                 bool* release_targets,
                                 int size,
                                 uint64_t hook_count,
                                 uint64_t session_token,
                                 int timeout_ms,
                                 uint8_t ack)
{
    int listener;
    int64_t deadline_us;
    unsigned int peer_count = 0;
    unsigned int released = 0;
#ifdef PEAK_ENABLE_TEST_HOOKS
    bool drop_first_release_response =
        peak_general_listener_env_value_truthy(
            getenv(
                PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_DROP_ONCE_ENV));
    bool dropped_release_response = false;
#endif

    if (release_targets == NULL || size <= 1) {
        return true;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    peak_socket_test_telemetry.root_release_decision = ack;
    if (ack == PEAK_SOCKET_REDUCE_RELEASE_ACK &&
        peak_general_listener_env_value_truthy(
            getenv(PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL_ENV))) {
        peak_log_warn("[peak] Socket aggregation release failure requested by test hook; peer ranks may fall back to local output\n");
        return false;
    }
#endif

    for (int rank = 1; rank < size; rank++) {
        if (release_targets[rank]) {
            peer_count++;
        }
    }
    if (peer_count == 0) {
        return true;
    }

    listener = peak_socket_reduce_create_listener(port, (int)peer_count);
    if (listener < 0) {
        peak_log_warn("[peak] Socket aggregation could not listen on release port %d for %u peers: %s; peer ranks may exit after timeout\n",
                      port,
                      peer_count,
                      strerror(errno));
        return false;
    }

    deadline_us = peak_socket_reduce_deadline_us(timeout_ms);
    while (released < peer_count &&
           peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        struct pollfd descriptor = {
            .fd = listener,
            .events = POLLIN,
            .revents = 0,
        };
        int poll_result = poll(
            &descriptor,
            1,
            peak_socket_reduce_remaining_ms(deadline_us));
        int fd;
        PeakSocketReduceReleaseFrame frame;
        int64_t io_deadline_us;
        bool valid;

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (poll_result == 0) {
            break;
        }

        fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN ||
                errno == EWOULDBLOCK) {
                continue;
            }
            break;
        }
        io_deadline_us = peak_socket_reduce_capped_deadline_us(
            deadline_us, PEAK_SOCKET_REDUCE_PEER_IO_TIMEOUT_MS);
        peak_socket_reduce_set_timeout(
            fd,
            peak_socket_reduce_remaining_ms(io_deadline_us));
        memset(&frame, 0, sizeof(frame));
        valid = peak_socket_reduce_recv_all(fd,
                                            &frame,
                                            sizeof(frame),
                                            io_deadline_us) &&
                frame.magic == PEAK_SOCKET_REDUCE_MAGIC &&
                frame.version == PEAK_SOCKET_REDUCE_VERSION &&
                frame.session_token == session_token &&
                frame.hook_count == hook_count &&
                frame.rank < (uint32_t)size && frame.rank > 0 &&
                frame.type == PEAK_SOCKET_REDUCE_RELEASE_REQUEST &&
                frame.decision == 0U &&
                release_targets[frame.rank];
#ifdef PEAK_ENABLE_TEST_HOOKS
        if (valid && drop_first_release_response &&
            !dropped_release_response) {
            dropped_release_response = true;
            close(fd);
            continue;
        }
#endif
        if (valid) {
            frame.type = PEAK_SOCKET_REDUCE_RELEASE_DECISION;
            frame.decision = ack;
            valid = peak_socket_reduce_send_all(fd,
                                                &frame,
                                                sizeof(frame),
                                                io_deadline_us);
        }
        if (valid) {
            valid = peak_socket_reduce_recv_all(fd,
                                                &frame,
                                                sizeof(frame),
                                                io_deadline_us) &&
                    frame.magic == PEAK_SOCKET_REDUCE_MAGIC &&
                    frame.version == PEAK_SOCKET_REDUCE_VERSION &&
                    frame.session_token == session_token &&
                    frame.hook_count == hook_count &&
                    frame.rank < (uint32_t)size && frame.rank > 0 &&
                    frame.type == PEAK_SOCKET_REDUCE_RELEASE_CONFIRM &&
                    frame.decision == ack &&
                    release_targets[frame.rank];
        }
        close(fd);
        if (valid) {
            release_targets[frame.rank] = false;
            released++;
#ifdef PEAK_ENABLE_TEST_HOOKS
            if (peak_socket_test_telemetry
                    .root_release_confirmed_count <
                UINT32_MAX) {
                peak_socket_test_telemetry
                    .root_release_confirmed_count++;
            }
#endif
        }
    }

    close(listener);
    if (released != peer_count) {
        peak_log_info("[peak] Socket aggregation confirmed release for %u/%u peer ranks before timeout\n",
                      released,
                      peer_count);
        return false;
    }
    return true;
}

static bool
peak_socket_reduce_nonnegative_double(double value)
{
    return value >= 0.0 && value == value && value <= DBL_MAX;
}

static bool
peak_socket_reduce_nonnegative_float(float value)
{
    return value >= 0.0f && value == value && value <= FLT_MAX;
}

static bool
peak_socket_reduce_merge_record(PeakSocketReduceRecord* aggregate,
                                const PeakSocketReduceRecord* incoming)
{
    uint64_t aggregate_thread_count;

    if (aggregate == NULL || incoming == NULL ||
        incoming->identity_hash != aggregate->identity_hash ||
        (incoming->detached != 0 && incoming->detached != 1) ||
        (incoming->reattached != 0 && incoming->reattached != 1) ||
        (incoming->revisited != 0 && incoming->revisited != 1) ||
        !peak_socket_reduce_nonnegative_double(incoming->total_time) ||
        !peak_socket_reduce_nonnegative_double(
            incoming->max_total_time) ||
        !peak_socket_reduce_nonnegative_double(
            incoming->min_total_time) ||
        !peak_socket_reduce_nonnegative_double(
            incoming->exclusive_time) ||
        !peak_socket_reduce_nonnegative_float(incoming->max_time) ||
        !peak_socket_reduce_nonnegative_float(incoming->min_time) ||
        aggregate->num_calls > UINT64_MAX - incoming->num_calls ||
        aggregate->thread_count >
            UINT64_MAX - incoming->thread_count ||
        aggregate->total_time >
            DBL_MAX - incoming->total_time ||
        aggregate->exclusive_time >
            DBL_MAX - incoming->exclusive_time) {
        return false;
    }

    aggregate_thread_count = aggregate->thread_count;
    aggregate->num_calls += incoming->num_calls;
    aggregate->total_time += incoming->total_time;
    if (incoming->max_total_time > aggregate->max_total_time) {
        aggregate->max_total_time = incoming->max_total_time;
    }
    if (incoming->thread_count > 0 &&
        (aggregate_thread_count == 0 ||
         incoming->min_total_time < aggregate->min_total_time)) {
        aggregate->min_total_time = incoming->min_total_time;
    }
    aggregate->exclusive_time += incoming->exclusive_time;
    if (incoming->max_time > aggregate->max_time) {
        aggregate->max_time = incoming->max_time;
    }
    if (incoming->thread_count > 0 &&
        (aggregate_thread_count == 0 ||
         incoming->min_time < aggregate->min_time)) {
        aggregate->min_time = incoming->min_time;
    }
    aggregate->thread_count += incoming->thread_count;
    aggregate->detached =
        aggregate->detached || incoming->detached;
    aggregate->reattached =
        aggregate->reattached || incoming->reattached;
    aggregate->revisited =
        aggregate->revisited || incoming->revisited;
    return true;
}

static void
peak_socket_reduce_records_to_snapshot(
    const PeakSocketReduceRecord* records,
    PeakReportSnapshot* snapshot)
{
    for (size_t i = 0; i < snapshot->hook_count; i++) {
        snapshot->num_calls[i] = (unsigned long)records[i].num_calls;
        snapshot->instrumented[i] =
            snapshot->instrumented[i] || records[i].num_calls != 0;
        snapshot->total_time[i] = records[i].total_time;
        snapshot->max_total_time[i] = records[i].max_total_time;
        snapshot->min_total_time[i] = records[i].min_total_time;
        snapshot->exclusive_time[i] = records[i].exclusive_time;
        snapshot->max_time[i] = records[i].max_time;
        snapshot->min_time[i] = records[i].min_time;
        snapshot->thread_count[i] =
            (unsigned long)records[i].thread_count;
        snapshot->detached[i] = records[i].detached ? 1 : 0;
        snapshot->reattached[i] = records[i].reattached ? 1 : 0;
        snapshot->revisited[i] = records[i].revisited ? 1 : 0;
    }
}

static bool
peak_socket_reduce_rank_size(int* rank_out,
                             int* size_out,
                             PeakSocketReportRankSource rank_source)
{
#ifdef HAVE_MPI
    int initialized = 0;
    int finalized = 0;

    if (rank_source == PEAK_SOCKET_REPORT_RANK_MPI_OR_ENV) {
        MPI_Initialized(&initialized);
        MPI_Finalized(&finalized);
        if (initialized && !finalized) {
            MPI_Comm_rank(MPI_COMM_WORLD, rank_out);
            MPI_Comm_size(MPI_COMM_WORLD, size_out);
            return true;
        }
    }
#else
    (void)rank_source;
#endif

    long env_size = -1;
    long env_rank = -1;

    if (!peak_general_listener_mpi_env_rank_size(&env_rank, &env_size)) {
        if (rank_source == PEAK_SOCKET_REPORT_RANK_ENV_REQUIRED ||
            peak_general_listener_mpi_env_world_metadata_present()) {
            return false;
        }
        *rank_out = 0;
        *size_out = 1;
        return true;
    }
    *rank_out = (int)env_rank;
    *size_out = (int)env_size;
    return true;
}

static bool
peak_socket_snapshot_is_complete(const PeakReportSnapshot* snapshot)
{
    if (snapshot == NULL) {
        return false;
    }
    if (snapshot->hook_count == 0) {
        return true;
    }
    return snapshot->names != NULL && snapshot->instrumented != NULL &&
           snapshot->detached != NULL && snapshot->reattached != NULL &&
           snapshot->revisited != NULL && snapshot->num_calls != NULL &&
           snapshot->total_time != NULL &&
           snapshot->max_total_time != NULL &&
           snapshot->min_total_time != NULL &&
           snapshot->exclusive_time != NULL && snapshot->max_time != NULL &&
           snapshot->min_time != NULL && snapshot->thread_count != NULL;
}

static PeakSocketReportSession*
peak_socket_report_session_create(bool* release_targets,
                                  int size,
                                  int release_port,
                                  int phase_timeout_ms,
                                  uint64_t hook_count,
                                  uint64_t session_token)
{
    PeakSocketReportSession* session = malloc(sizeof(*session));

    if (session == NULL) {
        return NULL;
    }
    session->release_targets = release_targets;
    session->size = size;
    session->release_port = release_port;
    session->phase_timeout_ms = phase_timeout_ms;
    session->hook_count = hook_count;
    session->session_token = session_token;
    return session;
}

static void
peak_socket_report_session_destroy(PeakSocketReportSession* session)
{
    if (session == NULL) {
        return;
    }
    free(session->release_targets);
    free(session);
}

static void
peak_socket_report_set_aggregate_overhead(
    PeakReportSnapshot* aggregate,
    const PeakReportMaxima* maxima,
    bool accounting_valid,
    uint64_t failed_stop_window_count,
    double profile_seconds,
    double elapsed_min_seconds,
    double elapsed_max_seconds)
{
    const PeakReportRankTuple* combined_maximum =
        &maxima->tuples[PEAK_REPORT_METRIC_COMBINED];
    PeakReportOverhead report = {0};

    report.valid = true;
    report.accounting_valid = accounting_valid;
    report.per_rank_max = peak_report_maxima_complete(maxima);
    report.local_ranks = combined_maximum->local_ranks;
    report.stop_window_count = combined_maximum->stop_window_count;
    report.failed_stop_window_count = failed_stop_window_count;
    report.elapsed_seconds = combined_maximum->elapsed_seconds;
    report.elapsed_min_seconds = elapsed_min_seconds;
    report.elapsed_max_seconds = elapsed_max_seconds;
    report.profile_seconds = profile_seconds;
    report.control_seconds = combined_maximum->control_seconds;
    report.management_seconds = combined_maximum->management_seconds;
    report.control_risk_seconds = combined_maximum->control_risk_seconds;
    report.profile_control_risk_seconds =
        combined_maximum->profile_control_risk_seconds;
    report.profile_ratio =
        maxima->tuples[PEAK_REPORT_METRIC_PROFILE].profile_ratio;
    report.control_ratio =
        maxima->tuples[PEAK_REPORT_METRIC_CONTROL].control_ratio;
    report.ratio = combined_maximum->ratio;
    report.profile_control_risk_ratio =
        maxima->tuples[PEAK_REPORT_METRIC_PROFILE_CONTROL_RISK]
            .profile_control_risk_ratio;
    report.control_risk_ratio =
        maxima->tuples[PEAK_REPORT_METRIC_CONTROL_RISK]
            .control_risk_ratio;
    report.management_ratio =
        maxima->tuples[PEAK_REPORT_METRIC_MANAGEMENT].management_ratio;
    report.per_rank_maxima = *maxima;
    aggregate->overhead = report;
}

typedef enum {
    PEAK_SOCKET_GATHER_READING_HEADER = 0,
    PEAK_SOCKET_GATHER_READING_PAYLOAD,
    PEAK_SOCKET_GATHER_SENDING_RECEIPT,
    PEAK_SOCKET_GATHER_READING_CONFIRM,
} PeakSocketGatherPhase;

typedef struct {
    int fd;
    PeakSocketGatherPhase phase;
    PeakSocketReduceHeader header;
    PeakReportRankTuple report_tuple;
    size_t header_bytes;
    size_t payload_bytes;
    size_t record_index;
    size_t record_bytes;
    PeakSocketReduceRecord record;
    PeakSocketReduceReleaseFrame receipt;
    size_t receipt_bytes;
    PeakSocketReduceReleaseFrame confirmation;
    size_t confirmation_bytes;
} PeakSocketGatherConnection;

typedef struct {
    PeakSocketReduceRecord* aggregate_records;
    PeakReportMaxima* maxima;
    size_t hook_count;
    size_t record_bytes;
    int size;
    uint64_t session_token;
    bool* claimed_ranks;
    bool* release_targets;
    double* profile_seconds;
    double* min_elapsed_seconds;
    double* max_elapsed_seconds;
    uint64_t* failed_stop_window_count;
    bool* accounting_valid;
} PeakSocketGatherAggregate;

static bool
peak_socket_reduce_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    return flags >= 0 &&
           fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void
peak_socket_gather_connection_close(PeakSocketGatherConnection* connection)
{
    if (connection == NULL) {
        return;
    }
    if (connection->fd >= 0) {
        close(connection->fd);
    }
    connection->fd = -1;
}

static bool
peak_socket_gather_prepare_receipt(
    PeakSocketGatherConnection* connection,
    PeakSocketGatherAggregate* aggregate)
{
    const PeakReportRankTuple* tuple;

    if (connection == NULL || aggregate == NULL ||
        connection->record_index != aggregate->hook_count ||
        connection->record_bytes != 0 ||
        !peak_report_maxima_consider(aggregate->maxima,
                                     &connection->report_tuple,
                                     (int)connection->header.rank)) {
        return false;
    }

    tuple = &connection->report_tuple;
    if (tuple->elapsed_seconds < *aggregate->min_elapsed_seconds) {
        *aggregate->min_elapsed_seconds = tuple->elapsed_seconds;
    }
    if (tuple->elapsed_seconds > *aggregate->max_elapsed_seconds) {
        *aggregate->max_elapsed_seconds = tuple->elapsed_seconds;
    }
    if (peak_socket_positive_finite(tuple->profile_seconds) &&
        *aggregate->profile_seconds <=
            DBL_MAX - tuple->profile_seconds) {
        *aggregate->profile_seconds += tuple->profile_seconds;
    }
    *aggregate->failed_stop_window_count =
        peak_socket_add_uint64_saturated(
            *aggregate->failed_stop_window_count,
            tuple->failed_stop_window_count);
    *aggregate->accounting_valid =
        *aggregate->accounting_valid && tuple->accounting_valid;
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (peak_socket_test_telemetry.root_payload_count <
        UINT32_MAX) {
        peak_socket_test_telemetry.root_payload_count++;
    }
#endif

    memset(&connection->receipt, 0, sizeof(connection->receipt));
    connection->receipt.magic = PEAK_SOCKET_REDUCE_MAGIC;
    connection->receipt.version = PEAK_SOCKET_REDUCE_VERSION;
    connection->receipt.rank = connection->header.rank;
    connection->receipt.hook_count = connection->header.hook_count;
    connection->receipt.session_token =
        connection->header.session_token;
    connection->receipt.type = PEAK_SOCKET_REDUCE_GATHER_RECEIPT;
    connection->receipt.decision =
        PEAK_SOCKET_REDUCE_GATHER_REGISTERED;
    connection->receipt_bytes = 0;
    connection->phase = PEAK_SOCKET_GATHER_SENDING_RECEIPT;
    return true;
}

static bool
peak_socket_gather_validate_header(
    PeakSocketGatherConnection* connection,
    PeakSocketGatherAggregate* aggregate)
{
    PeakSocketReduceHeader* header;

    if (connection == NULL || aggregate == NULL) {
        return false;
    }
    header = &connection->header;
    if (header->magic != PEAK_SOCKET_REDUCE_MAGIC ||
        header->version != PEAK_SOCKET_REDUCE_VERSION ||
        header->session_token != aggregate->session_token ||
        header->hook_count != (uint64_t)aggregate->hook_count ||
        header->rank == 0 || header->rank >= (uint32_t)aggregate->size ||
        header->accounting_valid > 1U ||
        aggregate->claimed_ranks[header->rank]) {
        return false;
    }

    connection->report_tuple =
        peak_socket_reduce_header_report_tuple(header);
    if (!peak_report_rank_tuple_is_valid(&connection->report_tuple)) {
        return false;
    }
    aggregate->claimed_ranks[header->rank] = true;
    connection->phase = PEAK_SOCKET_GATHER_READING_PAYLOAD;
    connection->payload_bytes = 0;
    connection->record_index = 0;
    connection->record_bytes = 0;
    memset(&connection->record, 0, sizeof(connection->record));
    if (aggregate->record_bytes == 0) {
        return peak_socket_gather_prepare_receipt(connection, aggregate);
    }
    return true;
}

static bool
peak_socket_gather_consume_payload(
    PeakSocketGatherConnection* connection,
    PeakSocketGatherAggregate* aggregate,
    const unsigned char* data,
    size_t data_size)
{
    size_t offset = 0;

    if (connection == NULL || aggregate == NULL ||
        (data == NULL && data_size != 0)) {
        return false;
    }
    while (offset < data_size) {
        size_t needed;
        size_t available;
        size_t copy_size;

        if (connection->record_index >= aggregate->hook_count) {
            return false;
        }
        needed = sizeof(connection->record) -
                 connection->record_bytes;
        available = data_size - offset;
        copy_size = needed < available ? needed : available;
        memcpy((unsigned char*)&connection->record +
                   connection->record_bytes,
               data + offset,
               copy_size);
        connection->record_bytes += copy_size;
        connection->payload_bytes += copy_size;
        offset += copy_size;

        if (connection->record_bytes == sizeof(connection->record)) {
            if (!peak_socket_reduce_merge_record(
                    &aggregate->aggregate_records[
                        connection->record_index],
                    &connection->record)) {
                return false;
            }
            connection->record_index++;
            connection->record_bytes = 0;
            memset(&connection->record, 0, sizeof(connection->record));
        }
    }
    if (connection->payload_bytes == aggregate->record_bytes) {
        return peak_socket_gather_prepare_receipt(connection, aggregate);
    }
    return connection->payload_bytes < aggregate->record_bytes;
}

static bool
peak_socket_gather_read_ready(PeakSocketGatherConnection* connection,
                              PeakSocketGatherAggregate* aggregate,
                              int64_t deadline_us,
                              bool* confirmed_out)
{
    unsigned char payload_buffer[64 * 1024];

    if (confirmed_out == NULL) {
        return false;
    }
    *confirmed_out = false;
    while (connection->phase !=
               PEAK_SOCKET_GATHER_SENDING_RECEIPT &&
           peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        void* destination;
        size_t request;
        ssize_t received;

        if (connection->phase ==
            PEAK_SOCKET_GATHER_READING_HEADER) {
            destination =
                (unsigned char*)&connection->header +
                connection->header_bytes;
            request =
                sizeof(connection->header) -
                connection->header_bytes;
        } else if (connection->phase ==
                   PEAK_SOCKET_GATHER_READING_PAYLOAD) {
            size_t remaining =
                aggregate->record_bytes -
                connection->payload_bytes;

            destination = payload_buffer;
            request = remaining < sizeof(payload_buffer)
                          ? remaining
                          : sizeof(payload_buffer);
        } else if (connection->phase ==
                   PEAK_SOCKET_GATHER_READING_CONFIRM) {
            destination =
                (unsigned char*)&connection->confirmation +
                connection->confirmation_bytes;
            request =
                sizeof(connection->confirmation) -
                connection->confirmation_bytes;
        } else {
            return false;
        }
        if (request == 0) {
            return false;
        }

        received = recv(connection->fd, destination, request, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }
        if (received == 0) {
            return false;
        }

        if (connection->phase ==
            PEAK_SOCKET_GATHER_READING_HEADER) {
            connection->header_bytes += (size_t)received;
            if (connection->header_bytes ==
                    sizeof(connection->header) &&
                !peak_socket_gather_validate_header(connection,
                                                    aggregate)) {
                return false;
            }
        } else if (connection->phase ==
                   PEAK_SOCKET_GATHER_READING_PAYLOAD) {
            if (!peak_socket_gather_consume_payload(
                    connection,
                    aggregate,
                    payload_buffer,
                    (size_t)received)) {
                return false;
            }
        } else {
            connection->confirmation_bytes += (size_t)received;
            if (connection->confirmation_bytes ==
                sizeof(connection->confirmation)) {
                const PeakSocketReduceReleaseFrame* confirmation =
                    &connection->confirmation;

                if (confirmation->magic !=
                        PEAK_SOCKET_REDUCE_MAGIC ||
                    confirmation->version !=
                        PEAK_SOCKET_REDUCE_VERSION ||
                    confirmation->rank != connection->header.rank ||
                    confirmation->hook_count !=
                        connection->header.hook_count ||
                    confirmation->session_token !=
                        connection->header.session_token ||
                    confirmation->type !=
                        PEAK_SOCKET_REDUCE_GATHER_RECEIPT_CONFIRM ||
                    confirmation->decision !=
                        PEAK_SOCKET_REDUCE_GATHER_REGISTERED) {
                    return false;
                }
                *confirmed_out = true;
                return true;
            }
        }
    }
    return connection->phase ==
               PEAK_SOCKET_GATHER_SENDING_RECEIPT ||
           peak_socket_reduce_remaining_ms(deadline_us) > 0;
}

static bool
peak_socket_gather_write_ready(PeakSocketGatherConnection* connection,
                               int64_t deadline_us,
                               bool* complete_out)
{
    size_t chunk_limit =
        peak_socket_reduce_test_gather_chunk_bytes();

    if (connection == NULL || complete_out == NULL ||
        connection->phase != PEAK_SOCKET_GATHER_SENDING_RECEIPT) {
        return false;
    }
    *complete_out = false;
#ifdef PEAK_ENABLE_TEST_HOOKS
    {
        static bool injected_receipt_failure = false;

        if (!injected_receipt_failure &&
            peak_general_listener_env_value_truthy(
                getenv(
                    PEAK_TEST_OUTPUT_AGGREGATION_RECEIPT_FAIL_ONCE_ENV))) {
            injected_receipt_failure = true;
            return false;
        }
    }
#endif

    while (connection->receipt_bytes <
               sizeof(connection->receipt) &&
           peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        size_t remaining =
            sizeof(connection->receipt) -
            connection->receipt_bytes;
        size_t request =
            remaining < chunk_limit ? remaining : chunk_limit;
        ssize_t written = send(
            connection->fd,
            (const unsigned char*)&connection->receipt +
                connection->receipt_bytes,
            request,
            MSG_NOSIGNAL);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }
        if (written == 0) {
            return false;
        }
        connection->receipt_bytes += (size_t)written;
    }
    *complete_out =
        connection->receipt_bytes == sizeof(connection->receipt);
    return *complete_out ||
           peak_socket_reduce_remaining_ms(deadline_us) > 0;
}

static size_t
peak_socket_reduce_gather_active_limit(size_t peer_count)
{
    struct rlimit descriptor_limit;
    size_t active_limit =
        peer_count < PEAK_SOCKET_REDUCE_GATHER_ACTIVE_MAX
            ? peer_count
            : PEAK_SOCKET_REDUCE_GATHER_ACTIVE_MAX;

    if (getrlimit(RLIMIT_NOFILE, &descriptor_limit) == 0 &&
        descriptor_limit.rlim_cur != RLIM_INFINITY) {
        rlim_t reserved =
            (rlim_t)PEAK_SOCKET_REDUCE_FD_RESERVE + 1U;

        if (descriptor_limit.rlim_cur <= reserved) {
            active_limit = 1U;
        } else {
            rlim_t descriptor_budget =
                descriptor_limit.rlim_cur - reserved;

            if (descriptor_budget < (rlim_t)active_limit) {
                active_limit = (size_t)descriptor_budget;
            }
        }
    }
    return active_limit > 0 ? active_limit : 1U;
}

static bool
peak_socket_reduce_root_gather(
    int listener,
    int size,
    int64_t deadline_us,
    PeakSocketGatherAggregate* aggregate,
    unsigned int* received_out)
{
    size_t peer_count;
    size_t active_limit;
    size_t poll_count;
    PeakSocketGatherConnection* connections = NULL;
    struct pollfd* descriptors = NULL;
    size_t accepted = 0;
    size_t active = 0;
    unsigned int completed = 0;
    bool ok = true;

    if (received_out != NULL) {
        *received_out = 0;
    }
    if (listener < 0 || size <= 1 || aggregate == NULL ||
        received_out == NULL) {
        return false;
    }
    peer_count = (size_t)(size - 1);
    active_limit =
        peak_socket_reduce_gather_active_limit(peer_count);
    if (active_limit > SIZE_MAX - 1 ||
        active_limit + 1 > (size_t)((nfds_t)-1) ||
        active_limit > SIZE_MAX / sizeof(*connections) ||
        active_limit + 1 > SIZE_MAX / sizeof(*descriptors)) {
        return false;
    }
    poll_count = active_limit + 1;
    connections = calloc(active_limit, sizeof(*connections));
    descriptors = calloc(poll_count, sizeof(*descriptors));
    if (connections == NULL || descriptors == NULL) {
        free(connections);
        free(descriptors);
        return false;
    }
    for (size_t i = 0; i < active_limit; i++) {
        connections[i].fd = -1;
    }

    while (completed < peer_count &&
           peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        int poll_result;

        descriptors[0].fd =
            accepted < peer_count && active < active_limit
                ? listener
                : -1;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        for (size_t i = 0; i < active_limit; i++) {
            descriptors[i + 1].fd = connections[i].fd;
            descriptors[i + 1].events =
                connections[i].phase ==
                        PEAK_SOCKET_GATHER_SENDING_RECEIPT
                    ? POLLOUT
                    : POLLIN;
            descriptors[i + 1].revents = 0;
        }

        poll_result = poll(descriptors,
                           (nfds_t)poll_count,
                           peak_socket_reduce_remaining_ms(deadline_us));
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        if (poll_result == 0) {
            break;
        }
        if ((descriptors[0].revents &
             (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            ok = false;
            break;
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            while (accepted < peer_count &&
                   active < active_limit &&
                   peak_socket_reduce_remaining_ms(deadline_us) > 0) {
                size_t slot = active_limit;
                int fd = accept(listener, NULL, NULL);

                if (fd < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN ||
                        errno == EWOULDBLOCK) {
                        break;
                    }
                    ok = false;
                    break;
                }
                for (size_t i = 0; i < active_limit; i++) {
                    if (connections[i].fd < 0) {
                        slot = i;
                        break;
                    }
                }
                if (slot == active_limit ||
                    !peak_socket_reduce_set_nonblocking(fd)) {
                    close(fd);
                    ok = false;
                    break;
                }
                memset(&connections[slot],
                       0,
                       sizeof(connections[slot]));
                connections[slot].fd = fd;
                connections[slot].phase =
                    PEAK_SOCKET_GATHER_READING_HEADER;
                accepted++;
                active++;
#ifdef PEAK_ENABLE_TEST_HOOKS
                if (active >
                    peak_socket_test_telemetry.root_max_active) {
                    peak_socket_test_telemetry.root_max_active =
                        active > UINT32_MAX
                            ? UINT32_MAX
                            : (uint32_t)active;
                }
#endif
            }
            if (!ok) {
                break;
            }
        }

        for (size_t i = 0; i < active_limit && ok; i++) {
            PeakSocketGatherConnection* connection =
                &connections[i];
            short revents = descriptors[i + 1].revents;
            short expected =
                connection->phase ==
                        PEAK_SOCKET_GATHER_SENDING_RECEIPT
                    ? POLLOUT
                    : POLLIN;

            if (connection->fd < 0 || revents == 0) {
                continue;
            }
            if ((revents & expected) != 0) {
                if (connection->phase ==
                    PEAK_SOCKET_GATHER_SENDING_RECEIPT) {
                    bool complete = false;

                    ok = peak_socket_gather_write_ready(
                        connection, deadline_us, &complete);
                    if (ok && complete) {
                        uint32_t rank = connection->header.rank;

                        if (rank == 0 ||
                            rank >= (uint32_t)size ||
                            aggregate->release_targets[rank]) {
                            ok = false;
                        } else {
                            aggregate->release_targets[rank] = true;
#ifdef PEAK_ENABLE_TEST_HOOKS
                            if (peak_socket_test_telemetry
                                    .root_release_target_count <
                                UINT32_MAX) {
                                peak_socket_test_telemetry
                                    .root_release_target_count++;
                            }
                            if (peak_socket_test_telemetry
                                    .root_receipt_count <
                                UINT32_MAX) {
                                peak_socket_test_telemetry
                                    .root_receipt_count++;
                            }
#endif
                            connection->phase =
                                PEAK_SOCKET_GATHER_READING_CONFIRM;
                            connection->confirmation_bytes = 0;
                            memset(&connection->confirmation,
                                   0,
                                   sizeof(connection->confirmation));
                        }
                    }
                } else {
                    bool confirmed = false;

                    ok = peak_socket_gather_read_ready(
                        connection,
                        aggregate,
                        deadline_us,
                        &confirmed);
                    if (ok && confirmed) {
#ifdef PEAK_ENABLE_TEST_HOOKS
                        if (peak_socket_test_telemetry
                                .root_confirmation_count <
                            UINT32_MAX) {
                            peak_socket_test_telemetry
                                .root_confirmation_count++;
                        }
#endif
                        completed++;
                        active--;
                        peak_socket_gather_connection_close(
                            connection);
                    }
                }
            }
            if (ok && connection->fd >= 0 &&
                (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                ok = false;
            }
        }
    }

    for (size_t i = 0; i < active_limit; i++) {
        peak_socket_gather_connection_close(&connections[i]);
    }
    free(descriptors);
    free(connections);
    *received_out = completed;
    return ok && completed == peer_count;
}

static PeakSocketReportStatus
peak_socket_report_peer_begin(const PeakReportSnapshot* local,
                              int rank,
                              int port,
                              int release_port,
                              int release_wait_timeout_ms,
                              int64_t deadline_us,
                              const PeakSocketReduceHeader* header,
                              PeakSocketReduceRecord* local_records,
                              size_t record_bytes)
{
    char root_host[256];
    struct addrinfo* root_addresses = NULL;
    int fd;
    bool sent;
    bool receipt_received;
    bool confirmation_sent;
    size_t gather_written = 0;
    PeakSocketReduceReleaseFrame receipt;
    PeakSocketReleaseResult release;

    if (!peak_socket_reduce_root_host(root_host, sizeof(root_host))) {
        peak_log_warn("[peak] Socket aggregation could not determine root host; skipping aggregate output\n");
        free(local_records);
        return PEAK_SOCKET_REPORT_FAILED;
    }

    if (rank > 0) {
        int initial_window_ms =
            peak_socket_reduce_remaining_ms(deadline_us) / 8;
        uint64_t jitter;
#ifdef PEAK_ENABLE_TEST_HOOKS
        int preconnect_delay_ms =
            peak_socket_reduce_parse_positive_int_env(
                PEAK_TEST_OUTPUT_AGGREGATION_GATHER_PRECONNECT_DELAY_MS_ENV,
                0);

        peak_socket_reduce_sleep_before_deadline(
            preconnect_delay_ms, deadline_us);
        if (peak_general_listener_env_value_truthy(
                getenv(
                    PEAK_TEST_OUTPUT_AGGREGATION_GATHER_DISABLE_JITTER_ENV))) {
            initial_window_ms = 0;
        }
#endif

        if (initial_window_ms > 1000) {
            initial_window_ms = 1000;
        }
        if (initial_window_ms > 0) {
            jitter = peak_socket_reduce_jitter_value(
                (uint32_t)rank,
                header->session_token,
                0,
                PEAK_SOCKET_REDUCE_GATHER_SALT);
            peak_socket_reduce_sleep_before_deadline(
                (int)(jitter % (uint64_t)(initial_window_ms + 1)),
                deadline_us);
        }
    }
    if (!peak_socket_reduce_resolve_root(root_host,
                                         deadline_us,
                                         (uint32_t)rank,
                                         header->session_token,
                                         &root_addresses)) {
        peak_log_warn("[peak] Socket aggregation gather could not resolve root host %s for rank %d before timeout: %s; skipping aggregate output\n",
                      root_host,
                      rank,
                      strerror(errno));
        free(local_records);
        return PEAK_SOCKET_REPORT_FAILED;
    }
    fd = peak_socket_reduce_connect(
        root_addresses,
        port,
        deadline_us,
        (uint32_t)rank,
        header->session_token,
        PEAK_SOCKET_REDUCE_GATHER_SALT);
    if (fd < 0) {
        peak_log_warn("[peak] Socket aggregation gather could not connect rank %d to %s:%d before timeout: %s; skipping aggregate output\n",
                      rank,
                      root_host,
                      port,
                      strerror(errno));
        freeaddrinfo(root_addresses);
        free(local_records);
        return PEAK_SOCKET_REPORT_FAILED;
    }

    sent = peak_socket_reduce_send_gather_all(fd,
                                              header,
                                              sizeof(*header),
                                              &gather_written,
                                              header->rank,
                                              deadline_us);
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (sent) {
        int delay_ms = 0;

        if (peak_socket_reduce_test_gather_delay_applies(
                header->rank, &delay_ms)) {
            peak_socket_reduce_sleep_before_deadline(delay_ms,
                                                     deadline_us);
        }
    }
#endif
    sent = sent &&
           peak_socket_reduce_send_gather_all(fd,
                                              local_records,
                                              record_bytes,
                                              &gather_written,
                                              header->rank,
                                              deadline_us);
    memset(&receipt, 0, sizeof(receipt));
    receipt_received =
        sent &&
        peak_socket_reduce_recv_all(fd,
                                    &receipt,
                                    sizeof(receipt),
                                    deadline_us) &&
        receipt.magic == header->magic &&
        receipt.version == header->version &&
        receipt.rank == header->rank &&
        receipt.hook_count == header->hook_count &&
        receipt.session_token == header->session_token &&
        receipt.type == PEAK_SOCKET_REDUCE_GATHER_RECEIPT &&
        receipt.decision ==
            PEAK_SOCKET_REDUCE_GATHER_REGISTERED;
    confirmation_sent = false;
    if (receipt_received) {
        receipt.type =
            PEAK_SOCKET_REDUCE_GATHER_RECEIPT_CONFIRM;
        confirmation_sent =
            peak_socket_reduce_send_gather_confirmation(
                fd, &receipt, header->rank, deadline_us);
    }
    close(fd);
    free(local_records);
    if (!receipt_received) {
        peak_log_warn("[peak] Socket aggregation gather or receipt failed for rank %d; skipping aggregate output\n",
                      rank);
        freeaddrinfo(root_addresses);
        return PEAK_SOCKET_REPORT_FAILED;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    peak_socket_test_telemetry.peer_receipt_received = true;
    peak_socket_test_telemetry.peer_confirmation_sent =
        confirmation_sent;
    peak_socket_test_telemetry.peer_release_started = true;
#endif
    if (!confirmation_sent) {
        peak_log_info("[peak] Socket aggregation gather receipt confirmation failed for rank %d; waiting for the root fallback decision\n",
                      rank);
    }

    release = peak_socket_reduce_wait_for_release(root_addresses,
                                                  release_port,
                                                  header,
                                                  release_wait_timeout_ms);
    freeaddrinfo(root_addresses);
    if (release == PEAK_SOCKET_RELEASE_FALLBACK) {
        peak_log_info("[peak] Socket aggregation root requested rank-local fallback for rank %d\n",
                      rank);
        return PEAK_SOCKET_REPORT_FAILED;
    }
    if (release != PEAK_SOCKET_RELEASE_ACKNOWLEDGED) {
        peak_log_warn("[peak] Socket aggregation release wait failed for rank %d within %d ms budget; skipping aggregate output\n",
                      rank,
                      release_wait_timeout_ms);
        return PEAK_SOCKET_REPORT_FAILED;
    }
    (void)local;
    return PEAK_SOCKET_REPORT_PEER_RELEASED;
}

PeakSocketReportStatus
peak_socket_report_transport_begin(const PeakReportSnapshot* local,
                                   PeakSocketReportRankSource rank_source,
                                   PeakSocketReportSession** session_out,
                                   PeakReportSnapshot** aggregate_out)
{
    int rank = 0;
    int size = 1;
    int port;
    int release_port;
    int timeout_ms;
    int release_wait_timeout_ms;
    int64_t deadline_us;
    uint64_t session_token;
    PeakSocketReduceRecord* local_records = NULL;
    PeakSocketReduceHeader header;
    PeakReportRankTuple local_report_tuple;
    PeakReportTimeoutBudget timeout_budget;
    size_t record_bytes;

    if (session_out != NULL) {
        *session_out = NULL;
    }
    if (aggregate_out != NULL) {
        *aggregate_out = NULL;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    peak_socket_report_test_telemetry_reset();
#endif
    if (session_out == NULL || aggregate_out == NULL) {
        return PEAK_SOCKET_REPORT_FAILED;
    }
    if (!peak_socket_snapshot_is_complete(local) ||
        (rank_source != PEAK_SOCKET_REPORT_RANK_MPI_OR_ENV &&
         rank_source != PEAK_SOCKET_REPORT_RANK_ENV_ONLY &&
         rank_source != PEAK_SOCKET_REPORT_RANK_ENV_REQUIRED)) {
        return PEAK_SOCKET_REPORT_FAILED;
    }

    port = peak_socket_reduce_port();
    release_port = peak_socket_reduce_release_port(port);
    timeout_budget = peak_general_listener_report_timeout_budget();
    timeout_ms = (int)timeout_budget.socket_phase_timeout_ms;
    release_wait_timeout_ms =
        (int)timeout_budget.socket_release_timeout_ms;
    deadline_us = peak_socket_reduce_deadline_us(timeout_ms);
    session_token = peak_socket_reduce_session_token();

    if (!peak_socket_reduce_rank_size(&rank, &size, rank_source)) {
        peak_log_warn("[peak] Socket aggregation could not determine rank/size from %s; skipping aggregate output\n",
                      rank_source == PEAK_SOCKET_REPORT_RANK_MPI_OR_ENV
                          ? "MPI or launcher metadata"
                          : "launcher metadata");
        return PEAK_SOCKET_REPORT_FAILED;
    }
    if (size <= 0 || rank < 0 || rank >= size) {
        peak_log_warn("[peak] Socket aggregation observed inconsistent rank/size metadata (%d/%d); skipping aggregate output\n",
                      rank,
                      size);
        return PEAK_SOCKET_REPORT_FAILED;
    }
    if (rank == 0 && timeout_budget.socket_release_was_raised) {
        peak_log_info("[peak] Raised socket peer release timeout to %u ms to preserve the three-phase gather/publication/release budget\n",
                      timeout_budget.socket_release_timeout_ms);
    }

    if (peak_report_snapshot_has_duplicate_names(local)) {
        if (rank == 0) {
            peak_log_warn("[peak] Socket aggregation contains duplicate hook names, likely from multiple JIT generations; skipping aggregate output\n");
        }
        return PEAK_SOCKET_REPORT_FAILED;
    }

    local_report_tuple = peak_report_overhead_rank_tuple(&local->overhead);
    if (size <= 1) {
        PeakReportSnapshot* single = peak_report_snapshot_clone(local);

        if (single == NULL) {
            return PEAK_SOCKET_REPORT_FAILED;
        }
        single->rank_count = 1;
        *aggregate_out = single;
        return PEAK_SOCKET_REPORT_SINGLE_READY;
    }
    if (!peak_socket_positive_finite(local->overhead.elapsed_seconds)) {
        peak_log_warn("[peak] Socket aggregation observed an invalid local elapsed time; skipping aggregate output\n");
        return PEAK_SOCKET_REPORT_FAILED;
    }
    if (!peak_socket_reduce_record_bytes(local->hook_count, &record_bytes) ||
        !peak_socket_reduce_build_records(local, &local_records)) {
        return PEAK_SOCKET_REPORT_FAILED;
    }

    memset(&header, 0, sizeof(header));
    header.magic = PEAK_SOCKET_REDUCE_MAGIC;
    header.version = PEAK_SOCKET_REDUCE_VERSION;
    header.rank = (uint32_t)rank;
    header.hook_count = (uint64_t)local->hook_count;
    header.session_token = session_token;
    peak_socket_reduce_header_set_report_tuple(&header, &local_report_tuple);

    if (rank != 0) {
        return peak_socket_report_peer_begin(local,
                                             rank,
                                             port,
                                             release_port,
                                             release_wait_timeout_ms,
                                             deadline_us,
                                             &header,
                                             local_records,
                                             record_bytes);
    }

    int listener =
        peak_socket_reduce_create_listener(port, size - 1);
    if (listener < 0) {
        peak_log_warn("[peak] Socket aggregation could not listen on gather port %d for %d peers: %s; skipping aggregate output\n",
                      port,
                      size - 1,
                      strerror(errno));
        free(local_records);
        return PEAK_SOCKET_REPORT_FAILED;
    }

    bool* claimed_ranks =
        calloc((size_t)size, sizeof(*claimed_ranks));
    bool* release_targets = calloc((size_t)size, sizeof(*release_targets));
    PeakSocketReduceRecord* aggregate_records = NULL;
    double socket_profile_seconds = local->overhead.profile_seconds;
    double socket_min_elapsed_seconds = local->overhead.elapsed_seconds;
    double socket_max_elapsed_seconds = local->overhead.elapsed_seconds;
    PeakReportMaxima socket_maxima;
    uint64_t socket_failed_stop_window_count =
        local->overhead.failed_stop_window_count;
    bool socket_accounting_valid = local->overhead.accounting_valid;
    unsigned int received = 0;
    bool failed;
    PeakSocketGatherAggregate gather_aggregate;

    if (record_bytes != 0) {
        aggregate_records = malloc(record_bytes);
        if (aggregate_records != NULL) {
            memcpy(aggregate_records, local_records, record_bytes);
        }
    }
    if (claimed_ranks == NULL || release_targets == NULL ||
        (record_bytes != 0 && aggregate_records == NULL) ||
        !peak_report_maxima_initialize(&socket_maxima,
                                       &local_report_tuple,
                                       0)) {
        close(listener);
        free(local_records);
        free(release_targets);
        free(claimed_ranks);
        free(aggregate_records);
        return PEAK_SOCKET_REPORT_FAILED;
    }
    claimed_ranks[0] = true;
    memset(&gather_aggregate, 0, sizeof(gather_aggregate));
    gather_aggregate.aggregate_records = aggregate_records;
    gather_aggregate.maxima = &socket_maxima;
    gather_aggregate.hook_count = local->hook_count;
    gather_aggregate.record_bytes = record_bytes;
    gather_aggregate.size = size;
    gather_aggregate.session_token = session_token;
    gather_aggregate.claimed_ranks = claimed_ranks;
    gather_aggregate.release_targets = release_targets;
    gather_aggregate.profile_seconds = &socket_profile_seconds;
    gather_aggregate.min_elapsed_seconds =
        &socket_min_elapsed_seconds;
    gather_aggregate.max_elapsed_seconds =
        &socket_max_elapsed_seconds;
    gather_aggregate.failed_stop_window_count =
        &socket_failed_stop_window_count;
    gather_aggregate.accounting_valid = &socket_accounting_valid;
    failed = !peak_socket_reduce_root_gather(listener,
                                             size,
                                             deadline_us,
                                             &gather_aggregate,
                                             &received);

    close(listener);
    free(local_records);
    free(claimed_ranks);

    if (failed || received != (unsigned int)(size - 1)) {
        peak_log_warn("[peak] Socket aggregation received %u/%d peer ranks; skipping aggregate output on root\n",
                      received,
                      size - 1);
        (void)peak_socket_reduce_release_peers(
            release_port,
            release_targets,
            size,
            (uint64_t)local->hook_count,
            session_token,
            timeout_ms,
            PEAK_SOCKET_REDUCE_RELEASE_FALLBACK);
        free(release_targets);
        free(aggregate_records);
        return PEAK_SOCKET_REPORT_FAILED;
    }

    PeakReportSnapshot* aggregate = peak_report_snapshot_clone(local);
    PeakSocketReportSession* session;

    if (aggregate == NULL) {
        (void)peak_socket_reduce_release_peers(
            release_port,
            release_targets,
            size,
            (uint64_t)local->hook_count,
            session_token,
            timeout_ms,
            PEAK_SOCKET_REDUCE_RELEASE_FALLBACK);
        free(release_targets);
        free(aggregate_records);
        return PEAK_SOCKET_REPORT_FAILED;
    }
    peak_socket_reduce_records_to_snapshot(aggregate_records, aggregate);
    free(aggregate_records);
    aggregate->rank_count = size;
    peak_socket_report_set_aggregate_overhead(
        aggregate,
        &socket_maxima,
        socket_accounting_valid,
        socket_failed_stop_window_count,
        socket_profile_seconds,
        socket_min_elapsed_seconds,
        socket_max_elapsed_seconds);

    session = peak_socket_report_session_create(
        release_targets,
        size,
        release_port,
        timeout_ms,
        (uint64_t)local->hook_count,
        session_token);
    if (session == NULL) {
        peak_report_snapshot_destroy(aggregate);
        (void)peak_socket_reduce_release_peers(
            release_port,
            release_targets,
            size,
            (uint64_t)local->hook_count,
            session_token,
            timeout_ms,
            PEAK_SOCKET_REDUCE_RELEASE_FALLBACK);
        free(release_targets);
        return PEAK_SOCKET_REPORT_FAILED;
    }

    *session_out = session;
    *aggregate_out = aggregate;
    return PEAK_SOCKET_REPORT_ROOT_PREPARED;
}

bool
peak_socket_report_transport_commit(PeakSocketReportSession* session)
{
    bool committed;

    if (session == NULL) {
        return false;
    }
    committed = peak_socket_reduce_release_peers(
        session->release_port,
        session->release_targets,
        session->size,
        session->hook_count,
        session->session_token,
        session->phase_timeout_ms,
        PEAK_SOCKET_REDUCE_RELEASE_ACK);
    peak_socket_report_session_destroy(session);
    return committed;
}

void
peak_socket_report_transport_abort(PeakSocketReportSession* session)
{
    if (session == NULL) {
        return;
    }
    (void)peak_socket_reduce_release_peers(
        session->release_port,
        session->release_targets,
        session->size,
        session->hook_count,
        session->session_token,
        session->phase_timeout_ms,
        PEAK_SOCKET_REDUCE_RELEASE_FALLBACK);
    peak_socket_report_session_destroy(session);
}
