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

#define PEAK_SOCKET_REDUCE_MAGIC 0x5045414b52454431ULL
#define PEAK_SOCKET_REDUCE_VERSION 10U
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
 * Wire-v10 intentionally targets a homogeneous job: every rank must use the
 * same byte order, floating-point representation, and 64-bit Linux C ABI.
 * Lock the layouts so an accidental field or packing change cannot silently
 * corrupt a report without another wire-version bump.
 */
_Static_assert(sizeof(PeakSocketReduceHeader) == 152,
               "wire-v10 header layout changed");
_Static_assert(sizeof(PeakSocketReduceReleaseFrame) == 40,
               "wire-v10 release layout changed");
_Static_assert(sizeof(PeakSocketReduceRecord) == 80,
               "wire-v10 record layout changed");

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
peak_socket_reduce_merge_records(PeakSocketReduceRecord* aggregate,
                                 const PeakSocketReduceRecord* incoming,
                                 size_t hook_count)
{
    for (size_t i = 0; i < hook_count; i++) {
        if (incoming[i].identity_hash != aggregate[i].identity_hash) {
            return false;
        }

        aggregate[i].num_calls += incoming[i].num_calls;
        aggregate[i].total_time += incoming[i].total_time;
        if (incoming[i].max_total_time > aggregate[i].max_total_time) {
            aggregate[i].max_total_time = incoming[i].max_total_time;
        }
        if (incoming[i].thread_count > 0 &&
            (aggregate[i].thread_count == 0 ||
             incoming[i].min_total_time < aggregate[i].min_total_time)) {
            aggregate[i].min_total_time = incoming[i].min_total_time;
        }
        aggregate[i].exclusive_time += incoming[i].exclusive_time;
        if (incoming[i].max_time > aggregate[i].max_time) {
            aggregate[i].max_time = incoming[i].max_time;
        }
        if (incoming[i].thread_count > 0 &&
            (aggregate[i].thread_count == 0 ||
             incoming[i].min_time < aggregate[i].min_time)) {
            aggregate[i].min_time = incoming[i].min_time;
        }
        aggregate[i].thread_count += incoming[i].thread_count;
        aggregate[i].detached =
            aggregate[i].detached || incoming[i].detached;
        aggregate[i].reattached =
            aggregate[i].reattached || incoming[i].reattached;
        aggregate[i].revisited =
            aggregate[i].revisited || incoming[i].revisited;
    }

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

    sent = peak_socket_reduce_send_all(fd,
                                       header,
                                       sizeof(*header),
                                       deadline_us) &&
           peak_socket_reduce_send_all(fd,
                                       local_records,
                                       record_bytes,
                                       deadline_us);
    close(fd);
    free(local_records);
    if (!sent) {
        peak_log_warn("[peak] Socket aggregation gather send failed for rank %d; skipping aggregate output\n",
                      rank);
        freeaddrinfo(root_addresses);
        return PEAK_SOCKET_REPORT_FAILED;
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

    bool* seen = calloc((size_t)size, sizeof(*seen));
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
    bool failed = false;

    if (record_bytes != 0) {
        aggregate_records = malloc(record_bytes);
        if (aggregate_records != NULL) {
            memcpy(aggregate_records, local_records, record_bytes);
        }
    }
    if (seen == NULL || release_targets == NULL ||
        (record_bytes != 0 && aggregate_records == NULL) ||
        !peak_report_maxima_initialize(&socket_maxima,
                                       &local_report_tuple,
                                       0)) {
        close(listener);
        free(local_records);
        free(release_targets);
        free(seen);
        free(aggregate_records);
        return PEAK_SOCKET_REPORT_FAILED;
    }
    seen[0] = true;

    while (received < (unsigned int)(size - 1) &&
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
        int64_t io_deadline_us;
        PeakSocketReduceHeader incoming_header = {0};
        PeakSocketReduceRecord* incoming = NULL;
        PeakReportRankTuple incoming_report_tuple;
        bool ok;

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            failed = true;
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
            failed = true;
            break;
        }
        io_deadline_us = peak_socket_reduce_capped_deadline_us(
            deadline_us, PEAK_SOCKET_REDUCE_PEER_IO_TIMEOUT_MS);
        peak_socket_reduce_set_timeout(
            fd,
            peak_socket_reduce_remaining_ms(io_deadline_us));
        ok = peak_socket_reduce_recv_all(fd,
                                         &incoming_header,
                                         sizeof(incoming_header),
                                         io_deadline_us) &&
             incoming_header.magic == PEAK_SOCKET_REDUCE_MAGIC &&
             incoming_header.version == PEAK_SOCKET_REDUCE_VERSION &&
             incoming_header.session_token == session_token &&
             incoming_header.hook_count == (uint64_t)local->hook_count &&
             incoming_header.rank < (uint32_t)size &&
             incoming_header.rank > 0 &&
             incoming_header.accounting_valid <= 1U &&
             !seen[incoming_header.rank];
        incoming_report_tuple =
            peak_socket_reduce_header_report_tuple(&incoming_header);
        ok = ok && peak_report_rank_tuple_is_valid(&incoming_report_tuple);
        if (ok) {
            release_targets[incoming_header.rank] = true;
        }
        if (ok && record_bytes != 0) {
            incoming = calloc(1, record_bytes);
            ok = incoming != NULL;
        }
        ok = ok &&
             peak_socket_reduce_recv_all(fd,
                                         incoming,
                                         record_bytes,
                                         io_deadline_us) &&
             peak_socket_reduce_merge_records(aggregate_records,
                                              incoming,
                                              local->hook_count) &&
             peak_report_maxima_consider(&socket_maxima,
                                         &incoming_report_tuple,
                                         (int)incoming_header.rank);
        if (ok) {
            if (incoming_report_tuple.elapsed_seconds <
                socket_min_elapsed_seconds) {
                socket_min_elapsed_seconds =
                    incoming_report_tuple.elapsed_seconds;
            }
            if (incoming_report_tuple.elapsed_seconds >
                socket_max_elapsed_seconds) {
                socket_max_elapsed_seconds =
                    incoming_report_tuple.elapsed_seconds;
            }
            if (peak_socket_positive_finite(
                    incoming_report_tuple.profile_seconds) &&
                socket_profile_seconds <=
                    DBL_MAX - incoming_report_tuple.profile_seconds) {
                socket_profile_seconds += incoming_report_tuple.profile_seconds;
            }
            socket_failed_stop_window_count =
                peak_socket_add_uint64_saturated(
                    socket_failed_stop_window_count,
                    incoming_report_tuple.failed_stop_window_count);
            socket_accounting_valid =
                socket_accounting_valid &&
                incoming_report_tuple.accounting_valid;
        }
        close(fd);
        free(incoming);

        if (!ok) {
            failed = true;
            break;
        }

        seen[incoming_header.rank] = true;
        received++;
    }

    close(listener);
    free(local_records);
    free(seen);

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
