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
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <mach-o/dyld.h>
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

extern char **environ;
static void diagnostic_phase(const char *phase)
{
    const char *enabled = getenv("PEAK_ATTACH_DIAGNOSTICS");
    if (enabled && !strcmp(enabled, "1")) {
        fprintf(stderr, "peak-attach[%ld]: %s\n", (long)getpid(), phase);
        fflush(stderr);
    }
}
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
static gboolean send_variant(int fd, GVariant *v, gint64 deadline)
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
    gint64 until = MIN(deadline, deadline_ms() + 5000);
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
#if defined(__APPLE__)
typedef struct {
    pid_t pid;
    pid_t parent_pid;
    uint64_t start_seconds;
    uint64_t start_microseconds;
    gboolean warned;
} OwnedInjectorHelper;
#endif
typedef struct {
    pid_t pid;
    int error_fd;
    int control_fd;
    gboolean done;
    gboolean failed;
    gboolean leader_lost;
    char error[4096];
    size_t error_length;
#if defined(__APPLE__)
    OwnedInjectorHelper helpers[64];
    size_t helper_count;
    gboolean helper_scan_warned;
#endif
} InjectionWorker;

#if defined(__APPLE__)
/* Darwin Core helpers call setsid(), so process-group cleanup alone cannot
 * contain them. Discover only this unreaped worker's direct children. */
static void collect_worker_helpers(InjectionWorker *worker)
{
    if (worker->done || worker->pid <= 0)
        return;
    struct proc_bsdinfo parent_info;
    if (proc_pidinfo(worker->pid, PROC_PIDTBSDINFO, 1, &parent_info,
                     sizeof(parent_info)) != (int)sizeof(parent_info) ||
        parent_info.pbi_pid != (uint32_t)worker->pid ||
        parent_info.pbi_ppid != (uint32_t)getpid()) {
        if (!worker->helper_scan_warned) {
            fprintf(stderr, "peak-attach: injector worker ownership could not "
                            "be verified; skipping child census\n");
            worker->helper_scan_warned = TRUE;
        }
        return;
    }
    pid_t children[64];
    errno = 0;
    int count = proc_listchildpids(worker->pid, children, sizeof(children));
    if (count < 0 || (!count && errno)) {
        if (!worker->helper_scan_warned) {
            fprintf(
                stderr,
                "peak-attach: cannot inspect injector helper ownership: %s\n",
                strerror(errno));
            worker->helper_scan_warned = TRUE;
        }
        return;
    }
    for (int i = 0; i < count && i < 64; ++i) {
        struct proc_bsdinfo info;
        errno = 0;
        int received =
            proc_pidinfo(children[i], PROC_PIDTBSDINFO, 1, &info, sizeof(info));
        if (received != (int)sizeof(info)) {
            if (errno && errno != ESRCH && !worker->helper_scan_warned) {
                fprintf(
                    stderr,
                    "peak-attach: cannot inspect injector child identity: %s\n",
                    strerror(errno));
                worker->helper_scan_warned = TRUE;
            }
            continue;
        }
        if (info.pbi_pid != (uint32_t)children[i] ||
            info.pbi_ppid != (uint32_t)worker->pid)
            continue;
        gboolean known = FALSE;
        for (size_t j = 0; j < worker->helper_count; ++j) {
            OwnedInjectorHelper *helper = &worker->helpers[j];
            if (helper->pid == children[i] &&
                helper->start_seconds == info.pbi_start_tvsec &&
                helper->start_microseconds == info.pbi_start_tvusec) {
                known = TRUE;
                break;
            }
        }
        if (!known && worker->helper_count < 64) {
            worker->helpers[worker->helper_count++] = (OwnedInjectorHelper){
                .pid = children[i],
                .parent_pid = (pid_t)info.pbi_ppid,
                .start_seconds = info.pbi_start_tvsec,
                .start_microseconds = info.pbi_start_tvusec};
            char phase[192];
            snprintf(phase, sizeof(phase),
                     "tracked injector child %ld parent %ld birth %llu.%06llu "
                     "uid %u pgid %u status %u",
                     (long)children[i], (long)worker->pid,
                     (unsigned long long)info.pbi_start_tvsec,
                     (unsigned long long)info.pbi_start_tvusec,
                     (unsigned int)info.pbi_uid, info.pbi_pgid,
                     info.pbi_status);
            diagnostic_phase(phase);
        }
    }
}

static gboolean helper_identity(OwnedInjectorHelper *helper,
                                struct proc_bsdinfo *info)
{
    errno = 0;
    int size =
        proc_pidinfo(helper->pid, PROC_PIDTBSDINFO, 1, info, sizeof(*info));
    if (size != (int)sizeof(*info)) {
        if (errno && errno != ESRCH && !helper->warned) {
            fprintf(stderr,
                    "peak-attach: cannot verify injector helper %ld: %s\n",
                    (long)helper->pid, strerror(errno));
            helper->warned = TRUE;
        }
        return FALSE;
    }
    return info->pbi_pid == (uint32_t)helper->pid &&
           info->pbi_start_tvsec == helper->start_seconds &&
           info->pbi_start_tvusec == helper->start_microseconds;
}

static gboolean worker_helpers_pending(InjectionWorker *worker)
{
    for (size_t i = 0; i < worker->helper_count; ++i) {
        struct proc_bsdinfo info;
        /* Darwin p_stat SZOMB=5: terminated children are left for Core to reap.
         */
        if (helper_identity(&worker->helpers[i], &info) && info.pbi_status != 5)
            return TRUE;
    }
    return FALSE;
}

static void signal_worker_helpers(InjectionWorker *worker, int signal_number)
{
    collect_worker_helpers(worker);
    for (size_t i = 0; i < worker->helper_count; ++i) {
        OwnedInjectorHelper *helper = &worker->helpers[i];
        struct proc_bsdinfo info;
        if (!helper_identity(helper, &info) || info.pbi_status == 5)
            continue;
        /* Original direct-child discovery plus the kernel birth identity
         * remains proof of ownership if an exited worker orphaned it to init.
         */
        if (helper->parent_pid != worker->pid ||
            (info.pbi_ppid != (uint32_t)helper->parent_pid &&
             info.pbi_ppid != 1)) {
            if (!helper->warned) {
                fprintf(stderr,
                        "peak-attach: injector helper %ld changed ownership; "
                        "leaving it untouched\n",
                        (long)helper->pid);
                helper->warned = TRUE;
            }
            continue;
        }
        if (kill(helper->pid, signal_number) && errno != ESRCH &&
            !helper->warned) {
            fprintf(stderr,
                    "peak-attach: cannot stop owned injector helper %ld: %s\n",
                    (long)helper->pid, strerror(errno));
            helper->warned = TRUE;
        }
    }
}
#else
static void collect_worker_helpers(InjectionWorker *worker) { (void)worker; }
static gboolean worker_helpers_pending(InjectionWorker *worker)
{
    (void)worker;
    return FALSE;
}
static void signal_worker_helpers(InjectionWorker *worker, int signal_number)
{
    (void)worker;
    (void)signal_number;
}
#endif

#if defined(__APPLE__)
static char *default_bootstrap(void);
#endif
static int run_injector(guint pid, const char *agent, const char *socket_path,
                        int control_fd)
{
    FridaInjector *injector = frida_injector_new();
    GError *error = NULL;
    const char *injection_path = agent;
    const char *entrypoint = PEAK_ATTACH_ENTRYPOINT;
    const char *data = socket_path;
#if defined(__APPLE__)
    char *bootstrap = default_bootstrap();
    if (!bootstrap || access(bootstrap, R_OK)) {
        fprintf(stderr,
                "peak-attach: Darwin bootstrap library is unreadable\n");
        g_free(bootstrap);
        g_object_unref(injector);
        return 1;
    }
    if (strlen(agent) + strlen(socket_path) + 20 >=
        PEAK_ATTACH_DARWIN_DATA_CAPACITY) {
        fprintf(stderr, "peak-attach: agent and socket paths exceed Darwin "
                        "injection data capacity\n");
        g_free(bootstrap);
        g_object_unref(injector);
        return 1;
    }
    char *payload =
        g_strdup_printf("PAB1%08x%08x%s%s", (unsigned int)strlen(agent),
                        (unsigned int)strlen(socket_path), agent, socket_path);
    injection_path = bootstrap;
    entrypoint = "peak_attach_bootstrap_main";
    data = payload;
#endif
    diagnostic_phase("inject begin");
    frida_injector_inject_library_file_sync(injector, pid, injection_path,
                                            entrypoint, data, NULL, &error);
#if defined(__APPLE__)
    g_free(payload);
    g_free(bootstrap);
#endif
    diagnostic_phase("inject returned");
    int result = error != NULL;
    if (error) {
        fprintf(stderr, "peak-attach: injection failed: %s\n", error->message);
        g_clear_error(&error);
    }
    if (!result) {
        diagnostic_phase("inject accepted; awaiting parent release");
        gint64 until = deadline_ms() + 20000;
        while (deadline_ms() < until) {
            struct pollfd control = {control_fd, POLLIN | POLLHUP, 0};
            int ready = poll(&control, 1, 100);
            if (ready > 0 || (ready < 0 && errno != EINTR))
                break;
        }
        diagnostic_phase("parent released injector");
    }
    close(control_fd);
    diagnostic_phase("injector close begin");
    frida_injector_close_sync(injector, NULL, &error);
    diagnostic_phase("injector close returned");
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
    collect_worker_helpers(worker);
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

static void finish_worker(InjectionWorker *worker, gboolean completed_response)
{
    if (worker->pid <= 0)
        return;
    collect_worker_helpers(worker);
    if (worker->control_fd >= 0) {
        close(worker->control_fd);
        worker->control_fd = -1;
    }
    /* Error ACKs also precede the remote entrypoint return; allow Core to
     * complete injection and helper close before signaling the worker. */
    gint64 deadline =
        deadline_ms() + (completed_response && !interrupted ? 2000 : 500);
    while ((!worker->done || worker_helpers_pending(worker)) &&
           deadline_ms() < deadline) {
        inspect_worker(worker);
        struct pollfd pfd = {worker->error_fd, POLLIN, 0};
        if (worker->done)
            poll(NULL, 0, 20);
        else
            poll(&pfd, 1, 20);
    }
    if (worker_helpers_pending(worker)) {
        diagnostic_phase("stopping owned injector helpers");
        signal_worker_helpers(worker, SIGTERM);
        deadline = deadline_ms() + 500;
        while (worker_helpers_pending(worker) && deadline_ms() < deadline) {
            inspect_worker(worker);
            struct pollfd pfd = {worker->error_fd, POLLIN, 0};
            if (worker->done)
                poll(NULL, 0, 20);
            else
                poll(&pfd, 1, 20);
        }
        signal_worker_helpers(worker, SIGKILL);
        deadline = deadline_ms() + 250;
        while (worker_helpers_pending(worker) && deadline_ms() < deadline) {
            inspect_worker(worker);
            struct pollfd pfd = {worker->error_fd, POLLIN, 0};
            poll(&pfd, 1, 20);
        }
    }
    if (!worker->done) {
        /* Narrow the discovery-to-termination race with bounded repeated
         * direct-child censuses while the worker identity is still pinned. */
        for (unsigned int pass = 0; pass < 3 && !worker->done; ++pass) {
            collect_worker_helpers(worker);
            signal_worker_helpers(worker, SIGKILL);
            poll(NULL, 0, 10);
            inspect_worker(worker);
        }
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
        signal_worker_helpers(worker, SIGKILL);
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
static char *default_library(const char *name, const char *relative_path)
{
    char *dir = executable_directory();
    if (!dir)
        return NULL;
    char *path = g_build_filename(dir, name, NULL);
    if (access(path, R_OK)) {
        g_free(path);
        path = g_build_filename(dir, relative_path, NULL);
    }
    g_free(dir);
    return path;
}
static char *default_agent(void)
{
    return default_library(PEAK_ATTACH_AGENT_NAME,
                           PEAK_ATTACH_AGENT_RELATIVE_PATH);
}
#if defined(__APPLE__)
static char *default_bootstrap(void)
{
    return default_library(PEAK_ATTACH_BOOTSTRAP_NAME,
                           PEAK_ATTACH_BOOTSTRAP_RELATIVE_PATH);
}
#endif
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
static gboolean private_pipe(int descriptors[2])
{
    if (pipe(descriptors))
        return FALSE;
    for (int i = 0; i < 2; ++i) {
        int replacement = fcntl(descriptors[i], F_DUPFD_CLOEXEC, 3);
        if (replacement < 0) {
            int saved = errno;
            close(descriptors[0]);
            close(descriptors[1]);
            errno = saved;
            return FALSE;
        }
        close(descriptors[i]);
        descriptors[i] = replacement;
    }
    return TRUE;
}
static gboolean start_worker(InjectionWorker *worker, guint pid,
                             const char *agent, const char *socket_path,
                             int target_lock)
{
    int lease_copy = -1;
    int controls[2];
    if (!private_pipe(controls))
        return FALSE;
    int control_copy = fcntl(controls[0], F_DUPFD_CLOEXEC, 10);
    if (control_copy < 0 || fcntl(controls[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(controls[1], F_SETFD, FD_CLOEXEC) < 0) {
        if (control_copy >= 0)
            close(control_copy);
        if (lease_copy >= 0)
            close(lease_copy);
        close(controls[0]);
        close(controls[1]);
        return FALSE;
    }
    lease_copy = fcntl(target_lock, F_DUPFD_CLOEXEC, 10);
    if (lease_copy < 0) {
        close(control_copy);
        if (lease_copy >= 0)
            close(lease_copy);
        close(controls[0]);
        close(controls[1]);
        return FALSE;
    }
    int errors[2];
    if (!private_pipe(errors)) {
        close(control_copy);
        if (lease_copy >= 0)
            close(lease_copy);
        close(controls[0]);
        close(controls[1]);
        return FALSE;
    }
    if (fcntl(errors[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(errors[0], F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(errors[1], F_SETFD, FD_CLOEXEC) < 0) {
        int error = errno;
        close(errors[0]);
        close(errors[1]);
        errno = error;
        close(control_copy);
        if (lease_copy >= 0)
            close(lease_copy);
        close(controls[0]);
        close(controls[1]);
        return FALSE;
    }
    char *exe = executable_path();
    if (!exe) {
        close(errors[0]);
        close(errors[1]);
        close(control_copy);
        if (lease_copy >= 0)
            close(lease_copy);
        close(controls[0]);
        close(controls[1]);
        return FALSE;
    }
    int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null_fd < 0) {
        int error = errno;
        free(exe);
        close(errors[0]);
        close(errors[1]);
        errno = error;
        close(control_copy);
        if (lease_copy >= 0)
            close(lease_copy);
        close(controls[0]);
        close(controls[1]);
        return FALSE;
    }
    if (null_fd < 3) {
        int replacement = fcntl(null_fd, F_DUPFD_CLOEXEC, 3);
        int saved = errno;
        close(null_fd);
        null_fd = replacement;
        if (null_fd < 0) {
            free(exe);
            close(errors[0]);
            close(errors[1]);
            close(control_copy);
            if (lease_copy >= 0)
                close(lease_copy);
            close(controls[0]);
            close(controls[1]);
            errno = saved;
            return FALSE;
        }
    }
    char pid_text[32];
    snprintf(pid_text, sizeof(pid_text), "%u", pid);
    char lease_text[32];
    snprintf(lease_text, sizeof(lease_text), "%d", lease_copy);
    char control_text[32];
    snprintf(control_text, sizeof(control_text), "%d", control_copy);
    char *arguments[] = {exe,
                         "--internal-inject",
                         pid_text,
                         (char *)agent,
                         (char *)socket_path,
                         control_text,
                         lease_text,
                         NULL};
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
    /* Duplicate the same flock open-file description so parent death cannot
     * release serialization while Core is still cleaning up in this worker. */
    error = posix_spawn_file_actions_adddup2(&actions, target_lock, lease_copy);
    if (error)
        goto spawn_cleanup;
    error =
        posix_spawn_file_actions_adddup2(&actions, controls[0], control_copy);
    if (error)
        goto spawn_cleanup;
    error = posix_spawn_file_actions_addclose(&actions, controls[0]);
    if (error)
        goto spawn_cleanup;
    error = posix_spawn_file_actions_addclose(&actions, controls[1]);
    if (error)
        goto spawn_cleanup;
    error = posix_spawn_file_actions_adddup2(&actions, null_fd, STDIN_FILENO);
    if (error)
        goto spawn_cleanup;
    error = posix_spawn_file_actions_adddup2(&actions, null_fd, STDOUT_FILENO);
    if (error)
        goto spawn_cleanup;
    error = posix_spawn_file_actions_addclose(&actions, null_fd);
    if (error)
        goto spawn_cleanup;
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
    close(null_fd);
    close(errors[1]);
    close(controls[0]);
    close(control_copy);
    if (lease_copy >= 0)
        close(lease_copy);
    if (error) {
        close(controls[1]);
        worker->pid = 0;
        close(errors[0]);
        errno = error;
        return FALSE;
    }
    worker->control_fd = controls[1];
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
/* Keep lock inodes persistent: unlinking a held file permits a second inode
 * to admit another injector for the same target. */
static int lock_target(guint pid)
{
    char directory[128], name[64];
    snprintf(directory, sizeof(directory), "/tmp/peak-attach-locks-%lu",
             (unsigned long)getuid());
    if (mkdir(directory, 0700) && errno != EEXIST)
        return -1;
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat info;
    if (dir < 0)
        return -1;
    if (fstat(dir, &info) || !S_ISDIR(info.st_mode) ||
        info.st_uid != getuid() || (info.st_mode & 0777) != 0700) {
        close(dir);
        errno = EPERM;
        return -1;
    }
    snprintf(name, sizeof(name), "%u.lock", pid);
    int fd = openat(dir, name, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(dir);
    if (fd < 0)
        return -1;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
        (info.st_mode & 0777) != 0600 || info.st_nlink != 1) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}
int main(int argc, char **argv)
{
    diagnostic_phase("Frida init begin");
    frida_init();
    diagnostic_phase("Frida init returned");
    if (argc == 7 && !strcmp(argv[1], "--internal-inject")) {
        char *end;
        unsigned long pid = strtoul(argv[2], &end, 10);
        char *lease_end;
        long lease = strtol(argv[6], &lease_end, 10);
        char *fd_end;
        long control = strtol(argv[5], &fd_end, 10);
        int result =
            !*argv[6] || *lease_end || lease < 3 || lease > INT_MAX ||
                    fcntl((int)lease, F_SETFD, FD_CLOEXEC) < 0 || !*argv[5] ||
                    *fd_end || control < 3 || control > INT_MAX ||
                    fcntl((int)control, F_SETFD, FD_CLOEXEC) < 0 || !*argv[2] ||
                    *end || !pid || pid > INT_MAX
                ? 1
                : run_injector((guint)pid, argv[3], argv[4], (int)control);
        diagnostic_phase("worker Frida deinit begin");
        frida_deinit();
        diagnostic_phase("worker Frida deinit returned");
        if (lease >= 3 && lease <= INT_MAX)
            close((int)lease);
        return result;
    }
    GVariant *configuration = NULL;
    guint pid = 0;
    char *agent = NULL;
    const char *output = NULL;
    GPtrArray *targets = g_ptr_array_new_with_free_func(g_free);
    int result = 1;
    int target_lock = -1;
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
    target_lock = lock_target(pid);
    if (target_lock < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            fprintf(stderr, "peak-attach: another peak-attach command is "
                            "already activating this process\n");
        else
            perror("peak-attach: target activation lock");
        goto args_done;
    }
    char directory[] = "/tmp/peak-attach-XXXXXX";
    char socket_path[108] = {0};
    int listener = -1, peer = -1;
    InjectionWorker worker = {.error_fd = -1, .control_fd = -1};
    gboolean completed_response = FALSE;
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
    if (!start_worker(&worker, pid, agent, socket_path, target_lock)) {
        perror("peak-attach: start injector");
        goto cleanup;
    }
    diagnostic_phase("worker spawned; awaiting agent");
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
    diagnostic_phase("authenticated agent connected");
    gboolean configured = send_variant(peer, configuration, until);
    configuration = NULL;
    if (!configured)
        goto disconnected;
    diagnostic_phase("configuration sent; awaiting response");
    GVariant *ready =
        receive_variant(peer, G_VARIANT_TYPE(PEAK_ATTACH_ACK_TYPE), until);
    if (!ready)
        goto disconnected;
    completed_response = TRUE;
    diagnostic_phase("agent response received");
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
    diagnostic_phase("worker cleanup begin");
    finish_worker(&worker, completed_response);
    diagnostic_phase("worker cleanup returned");
    if (*socket_path)
        unlink(socket_path);
    rmdir(directory);
    goto args_done;
invalid:
    usage(stderr);
    fprintf(stderr, "peak-attach: invalid arguments\n");
args_done:
    if (target_lock >= 0)
        close(target_lock);
    if (configuration)
        g_variant_unref(configuration);
    g_free(agent);
    g_ptr_array_unref(targets);
    diagnostic_phase("parent Frida deinit begin");
    frida_deinit();
    diagnostic_phase("parent Frida deinit returned");
    return result;
}
