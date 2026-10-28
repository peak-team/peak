#define _GNU_SOURCE
#include "internal/attach_protocol.h"
#include "internal/attach_runtime_config.h"
#include "frida-gum.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <dlfcn.h>
#else
#include <link.h>
#endif
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

gboolean peak_attach_configure(GVariant *dictionary, gchar **message);

/* An attach runtime is process-owned after the first activation attempt.
 * Keeping this library resident preserves callbacks, TLS destructors, and the
 * ordinary PEAK process-exit reporting path. */
static atomic_flag activation_claimed = ATOMIC_FLAG_INIT;

static gboolean
transfer_bytes(int fd, void *buffer, size_t size, gboolean writing)
{
    unsigned char *cursor = buffer;
    while (size != 0) {
        struct pollfd pfd = {fd, writing ? POLLOUT : POLLIN, 0};
        int ready;
        ssize_t transferred;
        do { ready = poll(&pfd, 1, 10000); } while (ready < 0 && errno == EINTR);
        if (ready <= 0)
            return FALSE;
        #if defined(__APPLE__)
        transferred = writing ? send(fd, cursor, size, 0) :
#else
        transferred = writing ? send(fd, cursor, size, MSG_NOSIGNAL) :
#endif
                                recv(fd, cursor, size, 0);
        if (transferred < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (transferred <= 0)
            return FALSE;
        cursor += transferred;
        size -= (size_t) transferred;
    }
    return TRUE;
}

static void
send_literal_ack(int fd, const char *text)
{
    size_t length = strlen(text);
    uint32_t frame = htonl((uint32_t) length);
    if (transfer_bytes(fd, &frame, sizeof(frame), TRUE))
        (void) transfer_bytes(fd, (void *) text, length, TRUE);
}

static void
send_ack(int fd, const gchar *status, const gchar *message)
{
    GVariant *value = g_variant_ref_sink(g_variant_new("(ss)", status,
                                                      message == NULL ? "" : message));
    gchar *text = g_variant_print(value, TRUE);
    size_t length = strlen(text);
    uint32_t frame = htonl((uint32_t) length);
    if (length <= PEAK_ATTACH_MAX_FRAME &&
        transfer_bytes(fd, &frame, sizeof(frame), TRUE))
        (void) transfer_bytes(fd, text, length, TRUE);
    g_free(text);
    g_variant_unref(value);
}

static GVariant *
read_config(int fd)
{
    uint32_t frame;
    gchar *text;
    GVariant *value = NULL;
    GError *error = NULL;
    if (!transfer_bytes(fd, &frame, sizeof(frame), FALSE))
        return NULL;
    frame = ntohl(frame);
    if (frame == 0 || frame > PEAK_ATTACH_MAX_FRAME)
        return NULL;
    text = g_malloc((size_t) frame + 1);
    if (transfer_bytes(fd, text, frame, FALSE)) {
        text[frame] = '\0';
        if (memchr(text, '\0', frame) == NULL)
            value = g_variant_parse(G_VARIANT_TYPE(PEAK_ATTACH_CONFIG_TYPE),
                                    text, text + frame, NULL, &error);
    }
    g_clear_error(&error);
    g_free(text);
    return value;
}

static int
connect_controller(const gchar *path)
{
    struct sockaddr_un address = {0};
    int fd;
    if (path == NULL || path[0] != '/' ||
        strlen(path) >= sizeof(address.sun_path))
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
#if defined(__APPLE__)
    int no_sigpipe = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
                   sizeof(no_sigpipe)) != 0) {
        close(fd);
        return -1;
    }
#endif
    address.sun_family = AF_UNIX;
#if defined(__APPLE__)
    address.sun_len = sizeof(address);
#endif
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (connect(fd, (struct sockaddr *) &address, sizeof(address)) != 0) {
        struct pollfd pfd = {fd, POLLOUT, 0};
        int socket_error = 0, ready;
        socklen_t error_size = sizeof(socket_error);
        if (errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        do { ready = poll(&pfd, 1, 10000); } while (ready < 0 && errno == EINTR);
        if (ready <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                                    &error_size) != 0 || socket_error != 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static gboolean
is_peak_runtime_path(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base == NULL ? path : base + 1;
#if defined(__APPLE__)
    return strncmp(base, "libpeak.", 8) == 0;
#else
    return strcmp(base, "libpeak.so") == 0 ||
           strncmp(base, "libpeak.so.", 11) == 0;
#endif
}

#if !defined(__APPLE__)
static int
find_existing_peak(struct dl_phdr_info *info, size_t size, void *data)
{
    (void) size;
    (void) data;
    return is_peak_runtime_path(info->dlpi_name);
}
#endif

#if defined(__APPLE__)
static _Atomic gboolean existing_peak_image;

static void
inspect_loaded_image(const struct mach_header *image, intptr_t slide)
{
    Dl_info info;
    (void) slide;
    /* dyld invokes registration callbacks for its initial image set and future
     * additions under its image lifecycle protection. A raw image-count loop
     * would race concurrent dlclose in an already-running process. */
    if (dladdr(image, &info) != 0 && info.dli_fname != NULL &&
        is_peak_runtime_path(info.dli_fname))
        atomic_store_explicit(&existing_peak_image, TRUE, memory_order_release);
}
#endif

static gboolean
has_existing_runtime(void)
{
#if defined(__APPLE__)
    _dyld_register_func_for_add_image(inspect_loaded_image);
    return atomic_load_explicit(&existing_peak_image, memory_order_acquire);
#else
    return dl_iterate_phdr(find_existing_peak, NULL) != 0;
#endif
}

static void *
activate_runtime(void *data)
{
    gchar *socket_path = data;
    int fd = connect_controller(socket_path);
    GVariant *config = NULL, *dictionary = NULL;
    gchar *message = NULL;
    guint version;
    free(socket_path);
    if (fd < 0)
        return NULL;
    if (atomic_flag_test_and_set_explicit(&activation_claimed, memory_order_acquire)) {
        send_literal_ack(fd, "('error', 'PEAK attach has already been requested for this process')");
        close(fd);
        return NULL;
    }
    if (has_existing_runtime()) {
        send_literal_ack(fd, "('error', 'a preloaded PEAK runtime is already present')");
        close(fd);
        return NULL;
    }
    int preparation = peak_attach_runtime_prepare();
    if (preparation <= 0) {
        send_literal_ack(fd, preparation < 0 ?
            "('error', 'this macOS Gum provider has no safe staged attach bootstrap')" :
            "('error', 'Gum lifecycle initialization was already claimed')");
        close(fd);
        return NULL;
    }
    config = read_config(fd);
    if (config == NULL) {
        message = g_strdup("invalid attach configuration frame");
        goto failed;
    }
    g_variant_get(config, "(@a{ss}u)", &dictionary, &version);
    if (version != PEAK_ATTACH_PROTOCOL_VERSION) {
        message = g_strdup("unsupported attach protocol version");
        goto failed;
    }
    if (!peak_attach_configure(dictionary, &message))
        goto failed;
    if (!peak_attach_runtime_start(&message))
        goto failed;
    send_ack(fd, "ready", message != NULL ? message :
        "PEAK is profiling until this process exits; normal PEAK reporting runs at exit");
    goto done;
failed:
    send_ack(fd, "error", message);
done:
    g_clear_pointer(&dictionary, g_variant_unref);
    g_clear_pointer(&config, g_variant_unref);
    g_free(message);
    close(fd);
    return NULL;
}

/* Frida Core inject_library_file entrypoint ABI. Return promptly to the
 * injector; initialization runs outside its loader/injection thread. */
__attribute__((visibility("default"))) void
peak_attach_agent_main(const gchar *data, gboolean *stay_resident)
{
    pthread_t thread;
    char *path;
    *stay_resident = TRUE;
    if (data == NULL || strlen(data) >= sizeof(((struct sockaddr_un *) 0)->sun_path))
        return;
    path = strdup(data);
    if (path == NULL)
        return;
    if (pthread_create(&thread, NULL, activate_runtime, path) == 0)
        (void) pthread_detach(thread);
    else
        free(path);
}
