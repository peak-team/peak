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
#define PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS_ENV \
    "PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS"
#define PEAK_OUTPUT_AGGREGATION_TOKEN_ENV "PEAK_OUTPUT_AGGREGATION_TOKEN"
#define PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL_ENV \
    "PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL"

#define PEAK_SOCKET_REDUCE_MAGIC 0x5045414b52454431ULL
#define PEAK_SOCKET_REDUCE_VERSION 9U
#define PEAK_SOCKET_REDUCE_RELEASE_ACK 0x51U
#define PEAK_SOCKET_REDUCE_RELEASE_FALLBACK 0x52U
#define PEAK_SOCKET_REDUCE_DEFAULT_TIMEOUT_MS 60000
#define PEAK_SOCKET_REDUCE_DEFAULT_PORT_BASE 42000
#define PEAK_SOCKET_REDUCE_DEFAULT_PORT_SPAN 20000

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
    uint8_t ack;
    uint8_t reserved[7];
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

struct PeakSocketReportSession {
    bool* release_targets;
    int size;
    int release_port;
    int timeout_ms;
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
    if (bracket == NULL) {
        comma = strchr(nodelist, ',');
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
peak_socket_reduce_create_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
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
        listen(fd, 4096) != 0) {
        close(fd);
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

static int
peak_socket_reduce_connect(const char* host,
                           int port,
                           int64_t deadline_us)
{
    char port_text[16];
    char qualified_host[NI_MAXHOST];
    struct addrinfo hints;
    struct addrinfo* result = NULL;

    snprintf(port_text, sizeof(port_text), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    qualified_host[0] = '\0';
    if (host != NULL && strchr(host, '.') == NULL) {
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

    while (peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        const char* candidates[3] = {
            host,
            qualified_host[0] != '\0' ? qualified_host : NULL,
            NULL,
        };

        for (size_t i = 0; candidates[i] != NULL; i++) {
            int gai = getaddrinfo(candidates[i], port_text, &hints, &result);

            if (gai == 0) {
                for (struct addrinfo* entry = result;
                     entry != NULL;
                     entry = entry->ai_next) {
                    int fd = socket(entry->ai_family,
                                    entry->ai_socktype,
                                    entry->ai_protocol);
                    int flags;

                    if (fd < 0) {
                        continue;
                    }
                    flags = fcntl(fd, F_GETFL, 0);
                    if (flags < 0 ||
                        fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
                        close(fd);
                        continue;
                    }
                    if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0 ||
                        (errno == EINPROGRESS &&
                         peak_socket_reduce_wait_connected(fd, deadline_us))) {
                        (void)fcntl(fd, F_SETFL, flags);
                        peak_socket_reduce_set_timeout(
                            fd,
                            peak_socket_reduce_remaining_ms(deadline_us));
                        freeaddrinfo(result);
                        return fd;
                    }
                    close(fd);
                }
                freeaddrinfo(result);
                result = NULL;
            }
        }
        usleep(10000);
    }

    return -1;
}

static int
peak_socket_reduce_release_port(int port)
{
    return port < 65535 ? port + 1 : port - 1;
}

static PeakSocketReleaseResult
peak_socket_reduce_wait_for_release(const char* host,
                                    int port,
                                    const PeakSocketReduceHeader* header,
                                    int timeout_ms)
{
    int64_t deadline_us = peak_socket_reduce_deadline_us(timeout_ms);
    int fd = peak_socket_reduce_connect(host, port, deadline_us);
    PeakSocketReduceReleaseFrame frame;
    bool valid;

    if (fd < 0 || header == NULL) {
        if (fd >= 0) {
            close(fd);
        }
        return PEAK_SOCKET_RELEASE_INVALID;
    }

    memset(&frame, 0, sizeof(frame));
    frame.magic = header->magic;
    frame.version = header->version;
    frame.rank = header->rank;
    frame.hook_count = header->hook_count;
    frame.session_token = header->session_token;
    valid = peak_socket_reduce_send_all(fd,
                                        &frame,
                                        sizeof(frame),
                                        deadline_us) &&
            peak_socket_reduce_recv_all(fd,
                                        &frame,
                                        sizeof(frame),
                                        deadline_us) &&
            frame.magic == header->magic &&
            frame.version == header->version &&
            frame.rank == header->rank &&
            frame.hook_count == header->hook_count &&
            frame.session_token == header->session_token &&
            (frame.ack == PEAK_SOCKET_REDUCE_RELEASE_ACK ||
             frame.ack == PEAK_SOCKET_REDUCE_RELEASE_FALLBACK);
    close(fd);
    if (!valid) {
        return PEAK_SOCKET_RELEASE_INVALID;
    }
    return frame.ack == PEAK_SOCKET_REDUCE_RELEASE_ACK
               ? PEAK_SOCKET_RELEASE_ACKNOWLEDGED
               : PEAK_SOCKET_RELEASE_FALLBACK;
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

    listener = peak_socket_reduce_create_listener(port);
    if (listener < 0) {
        peak_log_warn("[peak] Socket aggregation could not listen on release port %d; peer ranks may exit after timeout\n",
                      port);
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
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        peak_socket_reduce_set_timeout(
            fd,
            peak_socket_reduce_remaining_ms(deadline_us));
        memset(&frame, 0, sizeof(frame));
        valid = peak_socket_reduce_recv_all(fd,
                                            &frame,
                                            sizeof(frame),
                                            deadline_us) &&
                frame.magic == PEAK_SOCKET_REDUCE_MAGIC &&
                frame.version == PEAK_SOCKET_REDUCE_VERSION &&
                frame.session_token == session_token &&
                frame.hook_count == hook_count &&
                frame.rank < (uint32_t)size && frame.rank > 0 &&
                release_targets[frame.rank];
        if (valid) {
            frame.ack = ack;
            valid = peak_socket_reduce_send_all(fd,
                                                &frame,
                                                sizeof(frame),
                                                deadline_us);
        }
        close(fd);
        if (valid) {
            release_targets[frame.rank] = false;
            released++;
        }
    }

    close(listener);
    if (released != peer_count) {
        peak_log_info("[peak] Socket aggregation released %u/%u peer ranks before timeout\n",
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

    long env_size = peak_general_listener_mpi_env_size();
    long env_rank = peak_general_listener_mpi_env_rank();

    if (env_size > 1) {
        if (env_rank >= 0 && env_rank < env_size) {
            *rank_out = (int)env_rank;
            *size_out = (int)env_size;
            return true;
        }
        return false;
    }

    if (env_rank > 0) {
        return false;
    }

    *rank_out = 0;
    *size_out = 1;
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
                                  int timeout_ms,
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
    session->timeout_ms = timeout_ms;
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
                              int timeout_ms,
                              int64_t deadline_us,
                              const PeakSocketReduceHeader* header,
                              PeakSocketReduceRecord* local_records,
                              size_t record_bytes)
{
    char root_host[256];
    int fd;
    bool sent;
    PeakSocketReleaseResult release;

    if (!peak_socket_reduce_root_host(root_host, sizeof(root_host))) {
        peak_log_warn("[peak] Socket aggregation could not determine root host; skipping aggregate output\n");
        free(local_records);
        return PEAK_SOCKET_REPORT_FAILED;
    }

    if (rank > 0) {
        usleep((useconds_t)((rank % 1024) * 1000));
    }
    fd = peak_socket_reduce_connect(root_host, port, deadline_us);
    if (fd < 0) {
        peak_log_warn("[peak] Socket aggregation could not connect to %s:%d; skipping aggregate output\n",
                      root_host,
                      port);
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
        peak_log_warn("[peak] Socket aggregation send failed; skipping aggregate output\n");
        return PEAK_SOCKET_REPORT_FAILED;
    }

    release = peak_socket_reduce_wait_for_release(root_host,
                                                  release_port,
                                                  header,
                                                  timeout_ms);
    if (release != PEAK_SOCKET_RELEASE_ACKNOWLEDGED) {
        peak_log_warn("[peak] Socket aggregation release wait failed; skipping aggregate output\n");
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
    int64_t deadline_us;
    uint64_t session_token;
    PeakSocketReduceRecord* local_records = NULL;
    PeakSocketReduceHeader header;
    PeakReportRankTuple local_report_tuple;
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
         rank_source != PEAK_SOCKET_REPORT_RANK_ENV_ONLY)) {
        return PEAK_SOCKET_REPORT_FAILED;
    }

    port = peak_socket_reduce_port();
    release_port = peak_socket_reduce_release_port(port);
    timeout_ms = peak_socket_reduce_parse_positive_int_env(
        PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS_ENV,
        PEAK_SOCKET_REDUCE_DEFAULT_TIMEOUT_MS);
    deadline_us = peak_socket_reduce_deadline_us(timeout_ms);
    session_token = peak_socket_reduce_session_token();

    if (!peak_socket_reduce_rank_size(&rank, &size, rank_source)) {
        peak_log_warn("[peak] Socket aggregation could not determine rank/size from %s; skipping aggregate output\n",
                      rank_source == PEAK_SOCKET_REPORT_RANK_MPI_OR_ENV
                          ? "MPI or launcher metadata"
                          : "launcher metadata");
        return PEAK_SOCKET_REPORT_FAILED;
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
                                             timeout_ms,
                                             deadline_us,
                                             &header,
                                             local_records,
                                             record_bytes);
    }

    int listener = peak_socket_reduce_create_listener(port);
    if (listener < 0) {
        peak_log_warn("[peak] Socket aggregation could not listen on port %d; skipping aggregate output\n",
                      port);
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
            if (errno == EINTR) {
                continue;
            }
            failed = true;
            break;
        }
        peak_socket_reduce_set_timeout(
            fd,
            peak_socket_reduce_remaining_ms(deadline_us));
        ok = peak_socket_reduce_recv_all(fd,
                                         &incoming_header,
                                         sizeof(incoming_header),
                                         deadline_us) &&
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
                                         deadline_us) &&
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
        session->timeout_ms,
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
        session->timeout_ms,
        PEAK_SOCKET_REDUCE_RELEASE_FALLBACK);
    peak_socket_report_session_destroy(session);
}
