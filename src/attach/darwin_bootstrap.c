/* Frida's Darwin mapper cannot open an unloaded shared-cache dependency from
 * disk. Keep this bridge dependent only on libSystem and let native dyld load
 * the complete runtime and its dependencies. */
#include "internal/attach_protocol.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int transfer(int fd, void *buffer, size_t size, int writing,
                    int64_t deadline)
{
    char *cursor = buffer;
    while (size) {
        int64_t left = deadline - now_ms();
        if (left <= 0)
            return 0;
        struct pollfd pfd = {fd, writing ? POLLOUT : POLLIN, 0};
        int ready = poll(&pfd, 1, left > 100 ? 100 : (int)left);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            return 0;
        if (!ready)
            continue;
        ssize_t n = writing ? send(fd, cursor, size, MSG_NOSIGNAL)
                            : recv(fd, cursor, size, 0);
        if (n < 0 &&
            (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0)
            return 0;
        cursor += n;
        size -= (size_t)n;
    }
    return 1;
}

static void report_loader_error(const char *socket_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        close(fd);
        return;
    }
#if defined(SO_NOSIGPIPE)
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled))) {
        close(fd);
        return;
    }
#endif
    struct sockaddr_un address = {.sun_family = AF_UNIX};
#if defined(__APPLE__)
    address.sun_len = sizeof(address);
#endif
    size_t length = strlen(socket_path);
    if (length >= sizeof(address.sun_path)) {
        close(fd);
        return;
    }
    memcpy(address.sun_path, socket_path, length + 1);
    int64_t deadline = now_ms() + 5000;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            return;
        }
        struct pollfd pfd = {fd, POLLOUT, 0};
        int ready = 0;
        while (!ready && now_ms() < deadline) {
            int64_t left = deadline - now_ms();
            ready = poll(&pfd, 1, left > 100 ? 100 : (int)left);
            if (ready < 0 && errno == EINTR)
                ready = 0;
            else if (ready < 0)
                break;
        }
        int error = 0;
        socklen_t size = sizeof(error);
        if (ready <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) ||
            error) {
            close(fd);
            return;
        }
    }
    /* Consume the complete request before replying, so close cannot discard
     * the acknowledgement due to unread incoming socket data. */
    uint32_t frame;
    if (!transfer(fd, &frame, sizeof(frame), 0, deadline)) {
        close(fd);
        return;
    }
    size_t remaining = ntohl(frame);
    if (remaining > PEAK_ATTACH_MAX_FRAME) {
        close(fd);
        return;
    }
    char discarded[1024];
    while (remaining) {
        size_t chunk =
            remaining < sizeof(discarded) ? remaining : sizeof(discarded);
        if (!transfer(fd, discarded, chunk, 0, deadline)) {
            close(fd);
            return;
        }
        remaining -= chunk;
    }
    static const char ack[] = "('error', 'Darwin loader could not load the "
                              "PEAK runtime; see target stderr')";
    frame = htonl(sizeof(ack) - 1);
    if (transfer(fd, &frame, sizeof(frame), 1, deadline))
        (void)transfer(fd, (void *)ack, sizeof(ack) - 1, 1, deadline);
    close(fd);
}

static int parse_length(const char *text, size_t *length)
{
    size_t value = 0;
    for (unsigned int i = 0; i < 8; ++i) {
        unsigned int digit;
        unsigned char c = (unsigned char)text[i];
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            return 0;
        value = (value << 4) | digit;
    }
    *length = value;
    return value != 0 && value <= PEAK_ATTACH_MAX_FRAME;
}

__attribute__((visibility("default"))) void
peak_attach_bootstrap_main(const char *data, int *unload_policy)
{
    /* Frida UnloadPolicy.RESIDENT is 1. The bootstrap and native dlopen
     * handle remain valid for the process lifetime, including callbacks. */
    *unload_policy = 1;
    size_t agent_length, socket_length;
    if (!data || strlen(data) < 20 ||
        strlen(data) >= PEAK_ATTACH_DARWIN_DATA_CAPACITY ||
        memcmp(data, "PAB1", 4) || !parse_length(data + 4, &agent_length) ||
        !parse_length(data + 12, &socket_length) ||
        strlen(data) - 20 != agent_length + socket_length)
        return;
    char *agent = malloc(agent_length + 1);
    char *socket_path = malloc(socket_length + 1);
    if (!agent || !socket_path) {
        free(agent);
        free(socket_path);
        return;
    }
    memcpy(agent, data + 20, agent_length);
    agent[agent_length] = 0;
    memcpy(socket_path, data + 20 + agent_length, socket_length);
    socket_path[socket_length] = 0;
    if (agent[0] != '/' || socket_path[0] != '/')
        goto done;
    void *handle = dlopen(agent, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *error = dlerror();
        fprintf(stderr, "peak-attach: Darwin dlopen failed: %s\n",
                error ? error : "unknown loader error");
        report_loader_error(socket_path);
        goto done;
    }
    dlerror();
    typedef void (*AgentEntry)(const char *, int *);
    AgentEntry entry = (AgentEntry)dlsym(handle, PEAK_ATTACH_ENTRYPOINT);
    const char *error = dlerror();
    if (!entry || error) {
        fprintf(stderr, "peak-attach: Darwin agent entry lookup failed: %s\n",
                error ? error : "entry missing");
        report_loader_error(socket_path);
        /* Retain the handle even on failure: constructors may have created
         * process-owned state and the runtime's residency policy is strict. */
        goto done;
    }
    int runtime_policy = 1;
    entry(socket_path, &runtime_policy);
done:
    free(agent);
    free(socket_path);
}
