#define _GNU_SOURCE
#include "internal/attach_protocol.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <frida-core.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

extern char **environ;
static gboolean string_equal(gconstpointer a, gconstpointer b)
{
    return strcmp(a, b) == 0;
}
static volatile sig_atomic_t interrupted;
static void on_signal(int sig)
{
    (void)sig;
    interrupted = 1;
}
static gint64 deadline_ms(void) { return g_get_monotonic_time() / 1000; }
static gboolean transfer(int fd, void *data, size_t size, gboolean writing,
                         gint64 deadline)
{
    char *p = data;
    while (size) {
        if (interrupted)
            return FALSE;
        struct pollfd f = {fd, writing ? POLLOUT : POLLIN, 0};
        int remaining = (int)MAX(0, deadline - deadline_ms());
        int r = poll(&f, 1, MIN(remaining, 100));
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 || deadline_ms() >= deadline)
            return FALSE;
        if (!r)
            continue;
        ssize_t n =
            writing ? send(fd, p, size, MSG_NOSIGNAL) : recv(fd, p, size, 0);
        if (n < 0 &&
            (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0)
            return FALSE;
        p += n;
        size -= n;
    }
    return TRUE;
}
static gboolean send_variant(int fd, GVariant *v)
{
    char *s = g_variant_print(v, TRUE);
    size_t size = strlen(s);
    if (size > PEAK_ATTACH_MAX_FRAME) {
        fprintf(stderr,
                "peak-attach: configuration exceeds protocol frame limit\n");
        g_free(s);
        g_variant_unref(v);
        return FALSE;
    }
    uint32_t n = htonl(size);
    gint64 until = deadline_ms() + 5000;
    gboolean ok = transfer(fd, &n, 4, TRUE, until) &&
                  transfer(fd, s, strlen(s), TRUE, until);
    g_free(s);
    g_variant_unref(v);
    return ok;
}
static GVariant *receive_variant(int fd, const GVariantType *type,
                                 gint64 deadline)
{
    uint32_t n;
    if (!transfer(fd, &n, 4, FALSE, deadline))
        return NULL;
    n = ntohl(n);
    if (!n || n > PEAK_ATTACH_MAX_FRAME)
        return NULL;
    char *s = g_malloc(n + 1);
    if (!transfer(fd, s, n, FALSE, deadline)) {
        g_free(s);
        return NULL;
    }
    s[n] = 0;
    GError *error = NULL;
    GVariant *v = g_variant_parse(type, s, s + n, NULL, &error);
    if (error) {
        fprintf(stderr, "peak-attach: invalid agent response: %s\n",
                error->message);
        g_error_free(error);
    }
    g_free(s);
    return v;
}
/* Core runs in an exec-created process group so an unresponsive backend can
 * be terminated without touching the profiled process or blocking this CLI. */
typedef struct {
    pid_t pid;
    int error_fd;
    gboolean done;
    gboolean failed;
    gboolean leader_lost;
    char error[4096];
    size_t error_length;
} InjectionWorker;

static int run_injector(guint pid, const char *agent, const char *socket_path)
{
    FridaInjector *injector = frida_injector_new();
    GError *error = NULL;
    frida_injector_inject_library_file_sync(injector, pid, agent,
                                            PEAK_ATTACH_ENTRYPOINT, socket_path,
                                            NULL, &error);
    int result = error != NULL;
    if (error) {
        fprintf(stderr, "peak-attach: injection failed: %s\n", error->message);
        g_clear_error(&error);
    }
    frida_injector_close_sync(injector, NULL, &error);
    if (error) {
        fprintf(stderr, "peak-attach: injector cleanup failed: %s\n",
                error->message);
        g_clear_error(&error);
        result = 1;
    }
    g_object_unref(injector);
    return result;
}

static void inspect_worker(InjectionWorker *worker)
{
    char discarded[512];
    size_t drained = 0;
    while (drained < 65536) {
        size_t available = sizeof(worker->error) - 1 - worker->error_length;
        char *buffer =
            available ? worker->error + worker->error_length : discarded;
        ssize_t n = read(worker->error_fd, buffer,
                         available ? available : sizeof(discarded));
        if (n <= 0)
            break;
        drained += (size_t)n;
        if (available)
            worker->error_length += n;
    }
    worker->error[worker->error_length] = 0;
    if (!worker->done) {
        siginfo_t info = {0};
        if (waitid(P_PID, (id_t)worker->pid, &info,
                   WEXITED | WNOHANG | WNOWAIT) == 0) {
            if (info.si_pid == worker->pid) {
                worker->done = TRUE;
                worker->failed =
                    info.si_code != CLD_EXITED || info.si_status != 0;
            }
        } else if (errno == ECHILD) {
            worker->done = TRUE;
            worker->failed = TRUE;
            worker->leader_lost = TRUE;
        }
    }
}

static void finish_worker(InjectionWorker *worker, gboolean activated)
{
    if (worker->pid <= 0)
        return;
    gint64 deadline = deadline_ms() + (activated ? 3000 : 0);
    while (!worker->done && deadline_ms() < deadline) {
        inspect_worker(worker);
        struct pollfd pfd = {worker->error_fd, POLLIN, 0};
        if (!worker->done)
            poll(&pfd, 1, 20);
    }
    if (!worker->done) {
        kill(-worker->pid, SIGTERM);
        deadline = deadline_ms() + 1000;
        while (!worker->done && deadline_ms() < deadline) {
            inspect_worker(worker);
            struct pollfd pfd = {worker->error_fd, POLLIN, 0};
            if (!worker->done)
                poll(&pfd, 1, 20);
        }
    }
    if (!worker->leader_lost) {
        /* WNOWAIT retains the leader until after the final group signal,
         * preventing its PID/PGID from being reused during teardown. */
        kill(-worker->pid, SIGKILL);
        int status;
        while (waitpid(worker->pid, &status, 0) < 0 && errno == EINTR) {
        }
        worker->done = TRUE;
    }
    inspect_worker(worker);
    if (worker->error_length)
        fputs(worker->error, stderr);
    close(worker->error_fd);
}
static void usage(FILE *f)
{
    fprintf(
        f, "Usage: peak-attach PID [--target SYMBOL[,SYMBOL...]] [options]\n"
           "Profile an already running process with the full PEAK "
           "runtime.\n\n"
           "  --target SELECTORS  Repeatable function selectors (module!symbol "
           "accepted)\n"
           "  --output FILE       Set PEAK_STATSLOG_PATH (report path prefix)\n"
           "  --agent PATH        Override the profiling agent shared library\n"
           "  --help              Show this help\n  --version           Show "
           "version\n");
}
static char *executable_path(void)
{
    char exe[PATH_MAX];
#if defined(__APPLE__)
    uint32_t size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) != 0)
        return NULL;
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0)
        return NULL;
    exe[n] = 0;
#endif
    char *canonical = realpath(exe, NULL);
    if (!canonical)
        return NULL;
    return canonical;
}
static char *executable_directory(void)
{
    char *path = executable_path();
    if (!path)
        return NULL;
    char *dir = g_path_get_dirname(path);
    free(path);
    return dir;
}
static char *default_agent(void)
{
    char *dir = executable_directory();
    if (!dir)
        return NULL;
    char *path = g_build_filename(dir, PEAK_ATTACH_AGENT_NAME, NULL);
    if (access(path, R_OK)) {
        g_free(path);
        path = g_build_filename(dir, PEAK_ATTACH_AGENT_RELATIVE_PATH, NULL);
    }
    g_free(dir);
    return path;
}
static char *default_helper(void)
{
    char *dir = executable_directory();
    if (!dir)
        return NULL;
    char *path = g_build_filename(dir, "peak_detach_helper", NULL);
    g_free(dir);
    if (access(path, X_OK)) {
        g_free(path);
        return NULL;
    }
    return path;
}
static gboolean configure_socket(int fd)
{
    if (fd < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
        return FALSE;
#if defined(SO_NOSIGPIPE)
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)))
        return FALSE;
#endif
    return TRUE;
}
static gboolean verify_peer(int fd, pid_t pid)
{
#if defined(__APPLE__)
    pid_t peer_pid = 0;
    socklen_t size = sizeof(peer_pid);
    uid_t uid;
    gid_t gid;
    return !getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &peer_pid, &size) &&
           peer_pid == pid && !getpeereid(fd, &uid, &gid) && uid == geteuid();
#else
    struct ucred cred;
    socklen_t size = sizeof(cred);
    return !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) &&
           cred.pid == pid;
#endif
}
static gboolean start_worker(InjectionWorker *worker, guint pid,
                             const char *agent, const char *socket_path)
{
    int errors[2];
    if (pipe(errors))
        return FALSE;
    if (fcntl(errors[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(errors[0], F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(errors[1], F_SETFD, FD_CLOEXEC) < 0) {
        int error = errno;
        close(errors[0]);
        close(errors[1]);
        errno = error;
        return FALSE;
    }
    char *exe = executable_path();
    if (!exe) {
        close(errors[0]);
        close(errors[1]);
        return FALSE;
    }
    char pid_text[32];
    snprintf(pid_text, sizeof(pid_text), "%u", pid);
    char *arguments[] = {exe,           "--internal-inject", pid_text,
                         (char *)agent, (char *)socket_path, NULL};
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_t actions;
    gboolean attributes_ready = FALSE, actions_ready = FALSE;
    int error = posix_spawnattr_init(&attributes);
    if (error)
        goto spawn_cleanup;
    attributes_ready = TRUE;
    error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    if (error)
        goto spawn_cleanup;
    error = posix_spawnattr_setpgroup(&attributes, 0);
    if (error)
        goto spawn_cleanup;
    error = posix_spawn_file_actions_init(&actions);
    if (error)
        goto spawn_cleanup;
    actions_ready = TRUE;
    error =
        posix_spawn_file_actions_adddup2(&actions, errors[1], STDERR_FILENO);
    if (error)
        goto spawn_cleanup;
    error = posix_spawn_file_actions_addclose(&actions, errors[0]);
    if (error)
        goto spawn_cleanup;
    error = posix_spawn_file_actions_addclose(&actions, errors[1]);
    if (error)
        goto spawn_cleanup;
    error = posix_spawn(&worker->pid, exe, &actions, &attributes, arguments,
                        environ);
spawn_cleanup:
    if (actions_ready)
        posix_spawn_file_actions_destroy(&actions);
    if (attributes_ready)
        posix_spawnattr_destroy(&attributes);
    free(exe);
    close(errors[1]);
    if (error) {
        worker->pid = 0;
        close(errors[0]);
        errno = error;
        return FALSE;
    }
    worker->error_fd = errors[0];
    return TRUE;
}
static GVariant *build_configuration(GPtrArray *targets, const char *output)
{
    GHashTable *config =
        g_hash_table_new_full(g_str_hash, string_equal, g_free, g_free);
    for (char **entry = environ; *entry; ++entry) {
        if (strncmp(*entry, "PEAK_", 5))
            continue;
        const char *equal = strchr(*entry, '=');
        if (equal) {
            gboolean valid =
                equal - *entry <= 256 && strlen(equal + 1) <= 65536;
            for (const char *p = *entry; valid && p < equal; ++p)
                valid = g_ascii_isupper(*p) || g_ascii_isdigit(*p) || *p == '_';
            if (!valid) {
                fprintf(stderr, "peak-attach: invalid PEAK configuration key "
                                "or value length\n");
                g_hash_table_unref(config);
                return NULL;
            }
            g_hash_table_replace(config, g_strndup(*entry, equal - *entry),
                                 g_strdup(equal + 1));
        }
    }
    if (targets->len) {
        g_ptr_array_add(targets, NULL);
        char *joined = g_strjoinv(",", (char **)targets->pdata);
        g_hash_table_replace(config, g_strdup("PEAK_TARGET"), joined);
    }
    if (output)
        g_hash_table_replace(config, g_strdup("PEAK_STATSLOG_PATH"),
                             g_canonicalize_filename(output, NULL));
    if (!g_hash_table_contains(config, "PEAK_DETACH_HELPER")) {
        char *helper = default_helper();
        if (helper)
            g_hash_table_replace(config, g_strdup("PEAK_DETACH_HELPER"),
                                 helper);
    }
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{ss}"));
    GHashTableIter config_iter;
    gpointer key, value;
    g_hash_table_iter_init(&config_iter, config);
    while (g_hash_table_iter_next(&config_iter, &key, &value))
        g_variant_builder_add(&options, "{ss}", (char *)key, (char *)value);
    GVariant *configuration = g_variant_ref_sink(g_variant_new(
        PEAK_ATTACH_CONFIG_TYPE, &options, PEAK_ATTACH_PROTOCOL_VERSION));
    g_hash_table_unref(config);
    char *serialized = g_variant_print(configuration, TRUE);
    gboolean valid = strlen(serialized) <= PEAK_ATTACH_MAX_FRAME;
    g_free(serialized);
    if (!valid) {
        fprintf(stderr,
                "peak-attach: configuration exceeds protocol frame limit\n");
        g_variant_unref(configuration);
        return NULL;
    }
    return configuration;
}
int main(int argc, char **argv)
{
    frida_init();
    if (argc == 5 && !strcmp(argv[1], "--internal-inject")) {
        char *end;
        unsigned long pid = strtoul(argv[2], &end, 10);
        int result = !*argv[2] || *end || !pid || pid > INT_MAX
                         ? 1
                         : run_injector((guint)pid, argv[3], argv[4]);
        frida_deinit();
        return result;
    }
    GVariant *configuration = NULL;
    guint pid = 0;
    char *agent = NULL;
    const char *output = NULL;
    GPtrArray *targets = g_ptr_array_new_with_free_func(g_free);
    int result = 1;
    for (int k = 1; k < argc; ++k) {
        const char *a = argv[k];
        if (!strcmp(a, "--help")) {
            usage(stdout);
            result = 0;
            goto args_done;
        }
        if (!strcmp(a, "--version")) {
            puts("peak-attach " PEAK_ATTACH_VERSION);
            result = 0;
            goto args_done;
        }
        if ((!strcmp(a, "--target") || !strcmp(a, "--output") ||
             !strcmp(a, "--agent")) &&
            k + 1 < argc) {
            const char *v = argv[++k];
            if (!strcmp(a, "--target")) {
                char **items = g_strsplit(v, ",", -1);
                for (int j = 0; items[j]; ++j) {
                    if (!*items[j]) {
                        g_strfreev(items);
                        goto invalid;
                    }
                    g_ptr_array_add(targets, g_strdup(items[j]));
                }
                g_strfreev(items);
            } else if (!strcmp(a, "--output"))
                output = v;
            else {
                g_free(agent);
                agent = g_strdup(v);
            }
        } else if (!pid && a[0] != '-') {
            char *end;
            errno = 0;
            unsigned long value = strtoul(a, &end, 10);
            if (errno || *end || !*a || !value || value > INT_MAX)
                goto invalid;
            pid = value;
        } else
            goto invalid;
    }
    if (!pid || pid == (guint)getpid())
        goto invalid;
    if (!agent)
        agent = default_agent();
    if (!agent || access(agent, R_OK)) {
        fprintf(stderr, "peak-attach: agent is unreadable; use --agent PATH\n");
        goto args_done;
    }
    char *absolute = realpath(agent, NULL);
    g_free(agent);
    agent = absolute;
    if (!agent) {
        perror("peak-attach: agent path");
        goto args_done;
    }
    configuration = build_configuration(targets, output);
    if (!configuration)
        goto args_done;
    char directory[] = "/tmp/peak-attach-XXXXXX";
    char socket_path[108] = {0};
    int listener = -1, peer = -1;
    InjectionWorker worker = {.error_fd = -1};
    if (!mkdtemp(directory)) {
        perror("peak-attach: mkdtemp");
        goto cleanup;
    }
    snprintf(socket_path, sizeof(socket_path), "%s/control", directory);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
#if defined(__APPLE__)
    address.sun_len = sizeof(address);
#endif
    strcpy(address.sun_path, socket_path);
    if (!configure_socket(listener) ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
        listen(listener, 4)) {
        perror("peak-attach: socket");
        goto cleanup;
    }
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    if (!start_worker(&worker, pid, agent, socket_path)) {
        perror("peak-attach: start injector");
        goto cleanup;
    }
    gint64 until = deadline_ms() + 15000;
    while (!interrupted && deadline_ms() < until) {
        struct pollfd f = {listener, POLLIN, 0};
        if (poll(&f, 1, 100) > 0) {
            int candidate = accept(listener, NULL, NULL);
            if (configure_socket(candidate) &&
                verify_peer(candidate, (pid_t)pid)) {
                peer = candidate;
                break;
            }
            if (candidate >= 0)
                close(candidate);
        }
        inspect_worker(&worker);
        if (worker.done && worker.failed)
            break;
    }
    if (peer < 0) {
        fprintf(stderr, "peak-attach: agent connection failed, timed out, or "
                        "was interrupted\n");
        goto cleanup;
    }
    gboolean configured = send_variant(peer, configuration);
    configuration = NULL;
    if (!configured)
        goto disconnected;
    GVariant *ready = receive_variant(
        peer, G_VARIANT_TYPE(PEAK_ATTACH_ACK_TYPE), deadline_ms() + 15000);
    if (!ready)
        goto disconnected;
    const char *state, *message;
    g_variant_get(ready, "(&s&s)", &state, &message);
    gboolean ok = !strcmp(state, "ready");
    if (*message)
        fprintf(stderr, "peak-attach: %s\n", message);
    g_variant_unref(ready);
    if (!ok)
        goto cleanup;
    fprintf(stderr,
            "peak-attach: activated profiling for PID %u; PEAK reports will be "
            "written when the process exits\n",
            pid);
    result = 0;
    goto cleanup;
disconnected:
    fprintf(stderr, "peak-attach: target exited or agent connection was lost; "
                    "activation was not confirmed; PEAK may still be active\n");
    result = 2;
cleanup:
    if (peer >= 0) {
        shutdown(peer, SHUT_RDWR);
        close(peer);
    }
    if (listener >= 0)
        close(listener);
    finish_worker(&worker, result == 0);
    if (*socket_path)
        unlink(socket_path);
    rmdir(directory);
    goto args_done;
invalid:
    usage(stderr);
    fprintf(stderr, "peak-attach: invalid arguments\n");
args_done:
    if (configuration)
        g_variant_unref(configuration);
    g_free(agent);
    g_ptr_array_unref(targets);
    frida_deinit();
    return result;
}
