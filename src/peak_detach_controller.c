#define _GNU_SOURCE
#include "peak_detach_controller.h"
#include "peak_detach_helper_protocol.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PEAK_DETACH_CONTROLLER_IO_TIMEOUT_MS 5000
#define PEAK_DETACH_MAX_PHYSICAL_PATCH_RECORDS 8192u
#define PEAK_DETACH_HELPER_PROTOCOL_FD 3
#define PEAK_DETACH_CONTROLLER_MAX_BATCH_REQUESTS \
    PEAK_DETACH_HELPER_MAX_BATCH_WRITES

typedef enum {
    PEAK_SAFE_DETACH_MODE_COMPATIBILITY = 0,
    PEAK_SAFE_DETACH_MODE_STRICT
} PeakSafeDetachMode;

static gsize mode_initialized = 0;
static PeakSafeDetachMode configured_mode = PEAK_SAFE_DETACH_MODE_COMPATIBILITY;
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
static gboolean warned_missing_gum_api = FALSE;
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

typedef struct {
    gboolean active;
    gboolean uses_physical_patch;
    PeakDetachOperation operation;
    PeakDetachHelperInstruction instructions[PEAK_DETACH_HELPER_MAX_INSTRUCTIONS];
    uint32_t instruction_count;
} PeakDetachHeldMutation;

typedef struct {
    gboolean used;
    size_t hook_id;
    gpointer function_address;
    uint8_t active_patch[PEAK_DETACH_HELPER_MAX_PATCH_BYTES];
    uint32_t patch_size;
} PeakDetachPhysicalPatchRecord;

typedef struct {
    gboolean has_context;
    gboolean has_patch;
    GumPeakPcDiagnostics diagnostics;
    uint8_t active_patch[PEAK_DETACH_HELPER_MAX_PATCH_BYTES];
    uint8_t original_prologue[PEAK_DETACH_HELPER_MAX_PATCH_BYTES];
    guint prologue_len;
} PeakDetachGumSnapshot;

typedef struct {
    gboolean valid;
    gboolean safe;
    PeakDetachStatus status;
    PeakDetachGumSnapshot snapshot;
    PeakDetachHeldMutation mutation;
} PeakDetachBatchCandidate;

static gboolean peak_detach_controller_wait_fd(int fd, short events);
static PeakDetachStatus peak_detach_controller_errno_status(void);
extern char** environ;

static pthread_mutex_t mutation_guard_mutex;
static pthread_once_t mutation_guard_mutex_initialized = PTHREAD_ONCE_INIT;
static pthread_once_t atfork_initialized = PTHREAD_ONCE_INIT;
static int helper_fd = -1;
static pid_t helper_pid = -1;
static pid_t helper_owner_pid = -1;
static gboolean warned_helper_unavailable = FALSE;
static gboolean warned_helper_resume_failed = FALSE;
static gboolean warned_helper_fatal = FALSE;
static gboolean helper_state_fatal = FALSE;
static char resolved_helper_path[PATH_MAX];
static PeakDetachHeldMutation held_mutation = { 0 };
static PeakDetachPhysicalPatchRecord physical_patch_records[PEAK_DETACH_MAX_PHYSICAL_PATCH_RECORDS];
static double held_mutation_started_at = 0.0;
static double last_stop_window_us = 0.0;
#endif

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
static gboolean
peak_detach_controller_write_ranges_overlap(
    const PeakDetachHelperInstruction* left,
    const PeakDetachHelperInstruction* right)
{
    uint64_t left_start = left->address;
    uint64_t right_start = right->address;
    uint64_t left_end = left_start + left->size;
    uint64_t right_end = right_start + right->size;

    if (left_end < left_start || right_end < right_start) {
        return TRUE;
    }

    return left_start < right_end && right_start < left_end;
}

static void
peak_detach_controller_init_mutation_guard(void)
{
    pthread_mutexattr_t attr;

    if (pthread_mutexattr_init(&attr) != 0) {
        _exit(128);
    }
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0 ||
        pthread_mutex_init(&mutation_guard_mutex, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        _exit(128);
    }
    pthread_mutexattr_destroy(&attr);
}

static void
peak_detach_controller_lock_mutation_guard(void)
{
    (void)pthread_once(&mutation_guard_mutex_initialized,
                       peak_detach_controller_init_mutation_guard);
    (void)pthread_mutex_lock(&mutation_guard_mutex);
}

static void
peak_detach_controller_unlock_mutation_guard(void)
{
    (void)pthread_mutex_unlock(&mutation_guard_mutex);
}

static void
peak_detach_controller_atfork_prepare(void)
{
    peak_detach_controller_lock_mutation_guard();
}

static void
peak_detach_controller_atfork_parent(void)
{
    peak_detach_controller_unlock_mutation_guard();
}

static void
peak_detach_controller_atfork_child(void)
{
    if (helper_fd >= 0) {
        close(helper_fd);
    }
    helper_fd = -1;
    helper_pid = -1;
    helper_owner_pid = -1;
    helper_state_fatal = FALSE;
    held_mutation = (PeakDetachHeldMutation){ 0 };
    memset(physical_patch_records, 0, sizeof(physical_patch_records));
    peak_detach_controller_init_mutation_guard();
}

static void
peak_detach_controller_register_atfork(void)
{
    (void)pthread_atfork(peak_detach_controller_atfork_prepare,
                         peak_detach_controller_atfork_parent,
                         peak_detach_controller_atfork_child);
}

static void
peak_detach_controller_init_atfork_once(void)
{
    (void)pthread_once(&atfork_initialized,
                       peak_detach_controller_register_atfork);
}

static void
peak_detach_controller_reset_inherited_helper_if_needed(void)
{
    pid_t current_pid = getpid();

    if (helper_owner_pid > 0 && helper_owner_pid != current_pid) {
        if (helper_fd >= 0) {
            close(helper_fd);
        }
        helper_fd = -1;
        helper_pid = -1;
        helper_owner_pid = -1;
        helper_state_fatal = FALSE;
        held_mutation = (PeakDetachHeldMutation){ 0 };
        memset(physical_patch_records, 0, sizeof(physical_patch_records));
    }
}
#endif

static gboolean
peak_detach_controller_env_truthy(const char* value)
{
    return value != NULL &&
           (g_ascii_strcasecmp(value, "1") == 0 ||
            g_ascii_strcasecmp(value, "true") == 0 ||
            g_ascii_strcasecmp(value, "yes") == 0 ||
            g_ascii_strcasecmp(value, "on") == 0);
}

static PeakSafeDetachMode
peak_detach_controller_mode(void)
{
    if (g_once_init_enter(&mode_initialized)) {
        const char* strict_env = g_getenv("PEAK_REQUIRE_SAFE_DETACH");
        const char* mode_env = g_getenv("PEAK_SAFE_DETACH_MODE");
        PeakSafeDetachMode mode = PEAK_SAFE_DETACH_MODE_COMPATIBILITY;

        if (peak_detach_controller_env_truthy(strict_env) ||
            (mode_env != NULL &&
             (g_ascii_strcasecmp(mode_env, "strict") == 0 ||
              g_ascii_strcasecmp(mode_env, "helper") == 0 ||
              g_ascii_strcasecmp(mode_env, "debugger") == 0))) {
            mode = PEAK_SAFE_DETACH_MODE_STRICT;
        }

        configured_mode = mode;
        g_once_init_leave(&mode_initialized, 1);
    }

    return configured_mode;
}

static size_t
peak_detach_controller_count_proc_threads(gboolean* ok_out)
{
#ifdef __linux__
    DIR* dir = opendir("/proc/self/task");
    size_t count = 0;

    if (ok_out != NULL) {
        *ok_out = FALSE;
    }

    if (dir == NULL) {
        return 0;
    }

    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (entry == NULL) {
            break;
        }

        if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') {
            count++;
        }
    }

    if (ok_out != NULL && errno == 0) {
        *ok_out = TRUE;
    }
    closedir(dir);
    return count;
#else
    (void)ok_out;
    return 0;
#endif
}

const char*
peak_detach_controller_operation_string(PeakDetachOperation operation)
{
    switch (operation) {
        case PEAK_DETACH_OPERATION_ATTACH:
            return "attach";
        case PEAK_DETACH_OPERATION_DETACH:
            return "detach";
        case PEAK_DETACH_OPERATION_REATTACH:
            return "reattach";
        case PEAK_DETACH_OPERATION_SHUTDOWN:
            return "shutdown";
        case PEAK_DETACH_OPERATION_REPLACE:
            return "replace";
        case PEAK_DETACH_OPERATION_REVERT:
            return "revert";
        default:
            return "unknown";
    }
}

static gboolean
peak_detach_controller_operation_is_valid(PeakDetachOperation operation)
{
    switch (operation) {
        case PEAK_DETACH_OPERATION_ATTACH:
        case PEAK_DETACH_OPERATION_DETACH:
        case PEAK_DETACH_OPERATION_REATTACH:
        case PEAK_DETACH_OPERATION_SHUTDOWN:
        case PEAK_DETACH_OPERATION_REPLACE:
        case PEAK_DETACH_OPERATION_REVERT:
            return TRUE;
        default:
            return FALSE;
    }
}

static gboolean
peak_detach_controller_validate_request(const PeakDetachRequest* request,
                                        PeakDetachStatus* status_out)
{
    if (request == NULL ||
        request->interceptor == NULL ||
        request->function_address == NULL ||
        !peak_detach_controller_operation_is_valid(request->operation)) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }
    if (request->operation != PEAK_DETACH_OPERATION_ATTACH &&
        request->operation != PEAK_DETACH_OPERATION_REPLACE &&
        request->operation != PEAK_DETACH_OPERATION_REVERT &&
        request->listener == NULL) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

const char*
peak_detach_controller_status_string(PeakDetachStatus status)
{
    switch (status) {
        case PEAK_DETACH_STATUS_SAFE:
            return "safe";
        case PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED:
            return "compatibility-allowed";
        case PEAK_DETACH_STATUS_DISABLED:
            return "disabled";
        case PEAK_DETACH_STATUS_UNSUPPORTED:
            return "unsupported";
        case PEAK_DETACH_STATUS_MISSING_GUM_API:
            return "missing-gum-api";
        case PEAK_DETACH_STATUS_PERMISSION_DENIED:
            return "permission-denied";
        case PEAK_DETACH_STATUS_TIMEOUT:
            return "timeout";
        case PEAK_DETACH_STATUS_CLASSIFY_FAILED:
            return "classify-failed";
        case PEAK_DETACH_STATUS_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
static gboolean
peak_detach_controller_read_exact(int fd, void* buffer, size_t size)
{
    char* cursor = (char*)buffer;
    size_t done = 0;

    while (done < size) {
        if (!peak_detach_controller_wait_fd(fd, POLLIN)) {
            return FALSE;
        }
        ssize_t n = read(fd, cursor + done, size - done);
        if (n == 0) {
            return FALSE;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FALSE;
        }
        done += (size_t)n;
    }

    return TRUE;
}

static gboolean
peak_detach_controller_write_exact(int fd, const void* buffer, size_t size)
{
    const char* cursor = (const char*)buffer;
    size_t done = 0;

    while (done < size) {
        if (!peak_detach_controller_wait_fd(fd, POLLOUT)) {
            return FALSE;
        }
        ssize_t n = send(fd, cursor + done, size - done, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FALSE;
        }
        done += (size_t)n;
    }

    return TRUE;
}

static gboolean
peak_detach_controller_wait_fd(int fd, short events)
{
    struct pollfd pfd = {
        .fd = fd,
        .events = events,
        .revents = 0
    };

    for (;;) {
        int ret = poll(&pfd, 1, PEAK_DETACH_CONTROLLER_IO_TIMEOUT_MS);

        if (ret > 0) {
            if ((pfd.revents & events) != 0) {
                return TRUE;
            }
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                errno = EPIPE;
                return FALSE;
            }
            errno = EAGAIN;
            return FALSE;
        }
        if (ret == 0) {
            errno = ETIMEDOUT;
            return FALSE;
        }
        if (errno == EINTR) {
            continue;
        }
        return FALSE;
    }
}

static PeakDetachStatus
peak_detach_controller_errno_status(void)
{
    if (errno == ETIMEDOUT) {
        return PEAK_DETACH_STATUS_TIMEOUT;
    }
    if (errno == EACCES || errno == EPERM) {
        return PEAK_DETACH_STATUS_PERMISSION_DENIED;
    }
    return PEAK_DETACH_STATUS_ERROR;
}

static PeakDetachStatus
peak_detach_controller_helper_status_to_detach_status(uint32_t helper_status)
{
    switch ((PeakDetachHelperStatus)helper_status) {
        case PEAK_DETACH_HELPER_STATUS_OK:
            return PEAK_DETACH_STATUS_SAFE;
        case PEAK_DETACH_HELPER_STATUS_PERMISSION_DENIED:
            return PEAK_DETACH_STATUS_PERMISSION_DENIED;
        case PEAK_DETACH_HELPER_STATUS_UNSUPPORTED:
        case PEAK_DETACH_HELPER_STATUS_THREAD_LIMIT:
            return PEAK_DETACH_STATUS_UNSUPPORTED;
        case PEAK_DETACH_HELPER_STATUS_TIMEOUT:
            return PEAK_DETACH_STATUS_TIMEOUT;
        case PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED:
            return PEAK_DETACH_STATUS_ERROR;
        case PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR:
        case PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR:
        case PEAK_DETACH_HELPER_STATUS_REGISTER_ERROR:
        default:
            return PEAK_DETACH_STATUS_ERROR;
    }
}

static void
peak_detach_controller_mark_helper_fatal(const char* reason,
                                         PeakDetachStatus status)
{
    char message[256];
    int length;

    length = snprintf(message,
                      sizeof(message),
                      "[peak] fatal safe-detach helper failure (%s: %s); terminating to avoid running with unknown stopped-thread state\n",
                      reason != NULL ? reason : "unknown",
                      peak_detach_controller_status_string(status));

    helper_state_fatal = TRUE;
    warned_helper_fatal = TRUE;
    if (length > 0) {
        size_t size = (size_t)length;
        if (size >= sizeof(message)) {
            size = sizeof(message) - 1;
        }
        int flags = fcntl(STDERR_FILENO, F_GETFL);
        if (flags >= 0) {
            (void)fcntl(STDERR_FILENO, F_SETFL, flags | O_NONBLOCK);
        }
        (void)write(STDERR_FILENO, message, size);
    }
    _exit(128);
}

static const char*
peak_detach_controller_helper_path(void)
{
    const char* path = g_getenv("PEAK_DETACH_HELPER");

    if (path != NULL && path[0] != '\0') {
        return path;
    }

    Dl_info info;
    if (dladdr((void*)peak_detach_controller_helper_path, &info) != 0 &&
        info.dli_fname != NULL) {
        gchar* lib_dir = g_path_get_dirname(info.dli_fname);
        if (lib_dir != NULL) {
            gchar* install_root = g_path_get_dirname(lib_dir);
            gchar* candidates[3] = {
                g_build_filename(install_root, "bin", "peak_detach_helper", NULL),
                g_build_filename(lib_dir, "peak_detach_helper", NULL),
                NULL
            };

            for (size_t i = 0; candidates[i] != NULL; i++) {
                if (access(candidates[i], X_OK) == 0) {
                    g_strlcpy(resolved_helper_path,
                              candidates[i],
                              sizeof(resolved_helper_path));
                    for (size_t j = 0; candidates[j] != NULL; j++) {
                        g_free(candidates[j]);
                    }
                    g_free(install_root);
                    g_free(lib_dir);
                    return resolved_helper_path;
                }
            }

            for (size_t i = 0; candidates[i] != NULL; i++) {
                g_free(candidates[i]);
            }
            g_free(install_root);
            g_free(lib_dir);
        }
    }

#ifdef PEAK_INSTALL_DETACH_HELPER_PATH
    if (access(PEAK_INSTALL_DETACH_HELPER_PATH, X_OK) == 0) {
        return PEAK_INSTALL_DETACH_HELPER_PATH;
    }
#endif

#ifdef PEAK_DEFAULT_DETACH_HELPER_PATH
    if (access(PEAK_DEFAULT_DETACH_HELPER_PATH, X_OK) == 0) {
        return PEAK_DEFAULT_DETACH_HELPER_PATH;
    }
#endif

    return NULL;
}

static gboolean
peak_detach_controller_env_should_strip(const char* entry)
{
    return g_str_has_prefix(entry, "LD_PRELOAD=") ||
           g_str_has_prefix(entry, "LD_AUDIT=") ||
           g_str_has_prefix(entry, "PEAK_");
}

static char**
peak_detach_controller_build_helper_env(void)
{
    size_t count = 0;
    size_t kept = 0;

    if (environ == NULL) {
        return NULL;
    }

    while (environ[count] != NULL) {
        count++;
    }

    char** helper_env = calloc(count + 1, sizeof(*helper_env));
    if (helper_env == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        if (!peak_detach_controller_env_should_strip(environ[i])) {
            helper_env[kept] = strdup(environ[i]);
            if (helper_env[kept] == NULL) {
                for (size_t j = 0; j < kept; j++) {
                    free(helper_env[j]);
                }
                free(helper_env);
                return NULL;
            }
            kept++;
        }
    }
    helper_env[kept] = NULL;
    return helper_env;
}

static void
peak_detach_controller_free_helper_env(char** helper_env)
{
    if (helper_env == NULL) {
        return;
    }

    for (size_t i = 0; helper_env[i] != NULL; i++) {
        free(helper_env[i]);
    }
    free(helper_env);
}

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
static void
peak_detach_controller_child_close_inherited_fds(int protocol_fd)
{
    if (protocol_fd != PEAK_DETACH_HELPER_PROTOCOL_FD) {
        if (dup2(protocol_fd, PEAK_DETACH_HELPER_PROTOCOL_FD) < 0) {
            _exit(127);
        }
        close(protocol_fd);
        protocol_fd = PEAK_DETACH_HELPER_PROTOCOL_FD;
    }

#ifdef SYS_close_range
    (void)syscall(SYS_close_range,
                  (unsigned int)(PEAK_DETACH_HELPER_PROTOCOL_FD + 1),
                  ~0U,
                  0);
#endif
    (void)protocol_fd;
}
#endif

static void
peak_detach_controller_close_helper(void)
{
    pid_t pid = helper_pid;

    if (helper_fd >= 0) {
        close(helper_fd);
        helper_fd = -1;
    }
    helper_owner_pid = -1;

    if (pid > 0) {
        int status = 0;

        for (unsigned int attempt = 0; attempt < 50; attempt++) {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid || (waited < 0 && errno == ECHILD)) {
                helper_pid = -1;
                return;
            }
            usleep(1000);
        }

        kill(pid, SIGTERM);
        for (unsigned int attempt = 0; attempt < 100; attempt++) {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid || (waited < 0 && errno == ECHILD)) {
                helper_pid = -1;
                return;
            }
            usleep(1000);
        }

        kill(pid, SIGKILL);
        (void)waitpid(pid, &status, 0);
        helper_pid = -1;
    }
}

static gboolean
peak_detach_controller_ensure_helper(PeakDetachStatus* status_out)
{
    const char* path;
    int sockets[2];
    char fd_arg[32];
    char** helper_env;
    pid_t pid;
    PeakDetachHelperResponse handshake;

    peak_detach_controller_reset_inherited_helper_if_needed();
    if (helper_fd >= 0 && helper_pid > 0) {
        return TRUE;
    }

    path = peak_detach_controller_helper_path();
    if (path == NULL || access(path, X_OK) != 0) {
        if (!warned_helper_unavailable) {
            warned_helper_unavailable = TRUE;
            g_printerr("[peak] strict safe detach requested, but detach helper is unavailable; set PEAK_DETACH_HELPER to an executable helper\n");
        }
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_UNSUPPORTED;
        }
        return FALSE;
    }

    helper_env = peak_detach_controller_build_helper_env();
    if (helper_env == NULL) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        free(helper_env);
        if (status_out != NULL) {
            *status_out = errno == EACCES || errno == EPERM ?
                PEAK_DETACH_STATUS_PERMISSION_DENIED :
                PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    snprintf(fd_arg, sizeof(fd_arg), "%d", PEAK_DETACH_HELPER_PROTOCOL_FD);
    pid = fork();
    if (pid < 0) {
        peak_detach_controller_free_helper_env(helper_env);
        close(sockets[0]);
        close(sockets[1]);
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    if (pid == 0) {
        char* const helper_argv[] = { (char*)path, fd_arg, NULL };
        close(sockets[0]);
        peak_detach_controller_child_close_inherited_fds(sockets[1]);
        execve(path, helper_argv, helper_env);
        _exit(127);
    }

    peak_detach_controller_free_helper_env(helper_env);
    close(sockets[1]);
    helper_fd = sockets[0];
    helper_pid = pid;
    helper_owner_pid = getpid();

    int flags = fcntl(helper_fd, F_GETFD);
    if (flags >= 0) {
        (void)fcntl(helper_fd, F_SETFD, flags | FD_CLOEXEC);
    }

#ifdef PR_SET_PTRACER
    if (prctl(PR_SET_PTRACER, helper_pid, 0, 0, 0) != 0 &&
        errno != EINVAL && errno != ENOSYS) {
        PeakDetachStatus status = errno == EACCES || errno == EPERM ?
            PEAK_DETACH_STATUS_PERMISSION_DENIED :
            PEAK_DETACH_STATUS_ERROR;
        peak_detach_controller_close_helper();
        if (status_out != NULL) {
            *status_out = status;
        }
        return FALSE;
    }
#endif

    if (!peak_detach_controller_read_exact(helper_fd,
                                           &handshake,
                                           sizeof(handshake)) ||
        handshake.magic != PEAK_DETACH_HELPER_MAGIC ||
        handshake.version != PEAK_DETACH_HELPER_VERSION ||
        handshake.status != PEAK_DETACH_HELPER_STATUS_OK) {
        PeakDetachStatus status = peak_detach_controller_errno_status();
        peak_detach_controller_close_helper();
        if (status_out != NULL) {
            *status_out = status;
        }
        return FALSE;
    }

    return TRUE;
}

static gboolean
peak_detach_controller_send_helper_command(PeakDetachHelperCommand command,
                                           uint32_t instruction_count,
                                           PeakDetachHelperInstruction* instructions,
                                           gboolean close_on_io_failure,
                                           PeakDetachStatus* status_out)
{
    PeakDetachHelperRequest request = {
        .magic = PEAK_DETACH_HELPER_MAGIC,
        .version = PEAK_DETACH_HELPER_VERSION,
        .command = (uint32_t)command,
        .pid = (int32_t)getpid(),
        .controller_tid = (int32_t)syscall(SYS_gettid),
        .instruction_count = instruction_count
    };
    PeakDetachHelperResponse response;

    if (helper_fd < 0 ||
        !peak_detach_controller_write_exact(helper_fd, &request, sizeof(request)) ||
        (instruction_count > 0 &&
         !peak_detach_controller_write_exact(helper_fd,
                                             instructions,
                                             sizeof(instructions[0]) * instruction_count)) ||
        !peak_detach_controller_read_exact(helper_fd, &response, sizeof(response)) ||
        response.magic != PEAK_DETACH_HELPER_MAGIC ||
        response.version != PEAK_DETACH_HELPER_VERSION) {
        PeakDetachStatus status = peak_detach_controller_errno_status();
        if (close_on_io_failure) {
            peak_detach_controller_close_helper();
        } else {
            peak_detach_controller_mark_helper_fatal("held helper protocol failure",
                                                     status);
        }
        if (status_out != NULL) {
            *status_out = status;
        }
        return FALSE;
    }

    if (response.status != PEAK_DETACH_HELPER_STATUS_OK) {
        if (response.status == PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED) {
            peak_detach_controller_mark_helper_fatal("stop cleanup failure",
                                                     PEAK_DETACH_STATUS_ERROR);
        }
        if (close_on_io_failure) {
            peak_detach_controller_close_helper();
        }
        if (status_out != NULL) {
            *status_out =
                peak_detach_controller_helper_status_to_detach_status(response.status);
        }
        return FALSE;
    }

    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

static gboolean
peak_detach_controller_send_evacuate(uint32_t instruction_count,
                                     PeakDetachHelperInstruction* instructions,
                                     PeakDetachStatus* status_out)
{
    return peak_detach_controller_send_helper_command(
        PEAK_DETACH_HELPER_CMD_EVACUATE,
        instruction_count,
        instructions,
        FALSE,
        status_out);
}

static gboolean
peak_detach_controller_send_resume(PeakDetachStatus* status_out)
{
    return peak_detach_controller_send_helper_command(
        PEAK_DETACH_HELPER_CMD_RESUME,
        0,
        NULL,
        FALSE,
        status_out);
}

static gboolean
peak_detach_controller_send_shutdown(gboolean close_on_io_failure,
                                     PeakDetachStatus* status_out)
{
    gboolean ok = peak_detach_controller_send_helper_command(
        PEAK_DETACH_HELPER_CMD_SHUTDOWN,
        0,
        NULL,
        close_on_io_failure,
        status_out);

    if (ok) {
        peak_detach_controller_close_helper();
    }
    return ok;
}

static gboolean
peak_detach_controller_stop_threads(PeakDetachHelperThreadSnapshot* snapshots,
                                    uint32_t* snapshot_count_out,
                                    PeakDetachStatus* status_out)
{
    PeakDetachHelperRequest request = {
        .magic = PEAK_DETACH_HELPER_MAGIC,
        .version = PEAK_DETACH_HELPER_VERSION,
        .command = PEAK_DETACH_HELPER_CMD_STOP,
        .pid = (int32_t)getpid(),
        .controller_tid = (int32_t)syscall(SYS_gettid),
        .instruction_count = 0
    };
    PeakDetachHelperResponse response;

    *snapshot_count_out = 0;

    if (!peak_detach_controller_write_exact(helper_fd, &request, sizeof(request))) {
        PeakDetachStatus status = peak_detach_controller_errno_status();
        peak_detach_controller_close_helper();
        if (status_out != NULL) {
            *status_out = status;
        }
        return FALSE;
    }

    if (!peak_detach_controller_read_exact(helper_fd, &response, sizeof(response)) ||
        response.magic != PEAK_DETACH_HELPER_MAGIC ||
        response.version != PEAK_DETACH_HELPER_VERSION) {
        PeakDetachStatus status = peak_detach_controller_errno_status();
        peak_detach_controller_mark_helper_fatal("stop response failure", status);
    }

    if (response.status != PEAK_DETACH_HELPER_STATUS_OK) {
        if (response.status == PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED) {
            peak_detach_controller_mark_helper_fatal("stop cleanup failure",
                                                     PEAK_DETACH_STATUS_ERROR);
        }
        if (status_out != NULL) {
            *status_out =
                peak_detach_controller_helper_status_to_detach_status(response.status);
        }
        return FALSE;
    }

    if (response.thread_count > PEAK_DETACH_HELPER_MAX_THREADS ||
        !peak_detach_controller_read_exact(helper_fd,
                                           snapshots,
                                           sizeof(snapshots[0]) * response.thread_count)) {
        PeakDetachStatus status = peak_detach_controller_errno_status();
        peak_detach_controller_mark_helper_fatal("stop snapshot failure", status);
    }

    *snapshot_count_out = response.thread_count;
    return TRUE;
}

static PeakDetachPhysicalPatchRecord*
peak_detach_controller_find_physical_patch_record(size_t hook_id,
                                                  gboolean create)
{
    PeakDetachPhysicalPatchRecord* free_record = NULL;

    for (size_t i = 0; i < PEAK_DETACH_MAX_PHYSICAL_PATCH_RECORDS; i++) {
        PeakDetachPhysicalPatchRecord* record = &physical_patch_records[i];
        if (record->used && record->hook_id == hook_id) {
            return record;
        }
        if (!record->used && free_record == NULL) {
            free_record = record;
        }
    }

    if (!create || free_record == NULL) {
        return NULL;
    }

    memset(free_record, 0, sizeof(*free_record));
    free_record->used = TRUE;
    free_record->hook_id = hook_id;
    return free_record;
}

static double
peak_detach_controller_monotonic_second(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }

    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void
peak_detach_controller_note_stop_window_started(void)
{
    held_mutation_started_at = peak_detach_controller_monotonic_second();
    last_stop_window_us = 0.0;
}

static void
peak_detach_controller_note_stop_window_finished(void)
{
    if (held_mutation_started_at > 0.0) {
        last_stop_window_us =
            (peak_detach_controller_monotonic_second() -
             held_mutation_started_at) *
            1000000.0;
    }
    held_mutation_started_at = 0.0;
}

static gboolean
peak_detach_controller_append_write_instruction(PeakDetachHeldMutation* mutation,
                                                gpointer address,
                                                const uint8_t* bytes,
                                                uint32_t size)
{
    if (address == NULL || bytes == NULL || size == 0 ||
        size > PEAK_DETACH_HELPER_MAX_PATCH_BYTES ||
        mutation->instruction_count >= PEAK_DETACH_HELPER_MAX_INSTRUCTIONS) {
        return FALSE;
    }

    PeakDetachHelperInstruction* instruction =
        &mutation->instructions[mutation->instruction_count++];
    memset(instruction, 0, sizeof(*instruction));
    instruction->tid = 0;
    instruction->action = PEAK_DETACH_HELPER_INSTRUCTION_WRITE_MEMORY;
    instruction->address = (uint64_t)(uintptr_t)address;
    instruction->size = size;
    memcpy(instruction->bytes, bytes, size);
    return TRUE;
}

static gboolean
peak_detach_controller_pointer_in_range(gpointer pointer,
                                        gpointer start,
                                        size_t size)
{
    uintptr_t value = (uintptr_t)pointer;
    uintptr_t begin = (uintptr_t)start;
    uintptr_t end;

    if (pointer == NULL || start == NULL || size == 0) {
        return FALSE;
    }

    end = begin + (uintptr_t)size;
    return end >= begin && value >= begin && value < end;
}

static gboolean
peak_detach_controller_pointer_between(gpointer pointer,
                                       gpointer start,
                                       gpointer end)
{
    uintptr_t value = (uintptr_t)pointer;
    uintptr_t begin = (uintptr_t)start;
    uintptr_t finish = (uintptr_t)end;

    if (pointer == NULL || start == NULL || end == NULL || finish <= begin) {
        return FALSE;
    }

    return value >= begin && value < finish;
}

static gboolean
peak_detach_controller_capture_gum_snapshot(const PeakDetachRequest* request,
                                            PeakDetachGumSnapshot* snapshot,
                                            PeakDetachStatus* status_out)
{
    memset(snapshot, 0, sizeof(*snapshot));

    if (gum_interceptor_peak_get_pc_diagnostics(request->interceptor,
                                                request->function_address,
                                                request->listener,
                                                &snapshot->diagnostics)) {
        snapshot->has_context = TRUE;
    }

    if (request->operation == PEAK_DETACH_OPERATION_DETACH ||
        request->operation == PEAK_DETACH_OPERATION_SHUTDOWN ||
        request->operation == PEAK_DETACH_OPERATION_REVERT) {
        if (!gum_interceptor_peak_get_function_patch(request->interceptor,
                                                     request->function_address,
                                                     request->listener,
                                                     snapshot->active_patch,
                                                     snapshot->original_prologue,
                                                     &snapshot->prologue_len)) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
            }
            return FALSE;
        }
        snapshot->has_patch = TRUE;
    }

    if (request->operation == PEAK_DETACH_OPERATION_REATTACH &&
        !snapshot->has_context) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
        }
        return FALSE;
    }

    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

static GumPeakPcState
peak_detach_controller_classify_pc_from_snapshot(
    const PeakDetachGumSnapshot* gum_snapshot,
    gpointer pc)
{
    const GumPeakPcDiagnostics* diagnostics = &gum_snapshot->diagnostics;
    uintptr_t slice_start;
    uintptr_t slice_end;

    if (pc == NULL) {
        return GUM_PEAK_PC_UNKNOWN;
    }

    if (!gum_snapshot->has_context) {
        return GUM_PEAK_PC_SAFE;
    }

    if (peak_detach_controller_pointer_in_range(
            pc,
            diagnostics->function_address,
            diagnostics->overwritten_prologue_len)) {
        return GUM_PEAK_PC_AT_PATCH_ENTRY;
    }

    if (peak_detach_controller_pointer_in_range(pc,
                                                diagnostics->enter_thunk_start,
                                                diagnostics->enter_thunk_size) ||
        peak_detach_controller_pointer_in_range(pc,
                                                diagnostics->leave_thunk_start,
                                                diagnostics->leave_thunk_size)) {
        return GUM_PEAK_PC_IN_DISPATCH;
    }

    if (diagnostics->trampoline_slice_start == NULL ||
        diagnostics->trampoline_slice_size == 0) {
        return GUM_PEAK_PC_SAFE;
    }

    if (!peak_detach_controller_pointer_in_range(
            pc,
            diagnostics->trampoline_slice_start,
            diagnostics->trampoline_slice_size)) {
        return GUM_PEAK_PC_SAFE;
    }

    if (peak_detach_controller_pointer_between(
            pc,
            diagnostics->on_enter_trampoline,
            diagnostics->on_leave_trampoline)) {
        return GUM_PEAK_PC_IN_ENTER_TRAMPOLINE;
    }

    if (peak_detach_controller_pointer_between(
            pc,
            diagnostics->on_leave_trampoline,
            diagnostics->on_invoke_trampoline)) {
        return GUM_PEAK_PC_IN_LEAVE_TRAMPOLINE;
    }

    if (diagnostics->on_invoke_trampoline == NULL) {
        return GUM_PEAK_PC_UNKNOWN;
    }

    slice_start = (uintptr_t)diagnostics->trampoline_slice_start;
    slice_end = slice_start + (uintptr_t)diagnostics->trampoline_slice_size;
    if (slice_end < slice_start ||
        (uintptr_t)diagnostics->on_invoke_trampoline >= slice_end) {
        return GUM_PEAK_PC_UNKNOWN;
    }

    if (peak_detach_controller_pointer_in_range(
            pc,
            diagnostics->on_invoke_trampoline,
            (size_t)(slice_end -
                     (uintptr_t)diagnostics->on_invoke_trampoline))) {
        return GUM_PEAK_PC_IN_INVOKE_TRAMPOLINE;
    }

    return GUM_PEAK_PC_UNKNOWN;
}

static gpointer
peak_detach_controller_safe_pc_from_snapshot(
    const PeakDetachGumSnapshot* snapshot,
    gpointer pc,
    GumPeakPcState state)
{
    if (state == GUM_PEAK_PC_IN_ENTER_TRAMPOLINE &&
        pc == snapshot->diagnostics.on_enter_trampoline) {
        return snapshot->diagnostics.function_address;
    }

    return NULL;
}

static gboolean
peak_detach_controller_append_physical_patch(const PeakDetachRequest* request,
                                             const PeakDetachGumSnapshot* snapshot,
                                             PeakDetachHeldMutation* mutation,
                                             PeakDetachStatus* status_out)
{
    if (request->operation != PEAK_DETACH_OPERATION_DETACH &&
        request->operation != PEAK_DETACH_OPERATION_REATTACH &&
        request->operation != PEAK_DETACH_OPERATION_SHUTDOWN &&
        request->operation != PEAK_DETACH_OPERATION_REVERT) {
        return TRUE;
    }

    if (request->operation == PEAK_DETACH_OPERATION_REATTACH) {
        PeakDetachPhysicalPatchRecord* record =
            peak_detach_controller_find_physical_patch_record(request->hook_id,
                                                              FALSE);
        if (record == NULL || record->patch_size == 0 ||
            record->function_address != request->function_address) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
            }
            return FALSE;
        }

        if (!peak_detach_controller_append_write_instruction(mutation,
                                                             request->function_address,
                                                             record->active_patch,
                                                             record->patch_size)) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_ERROR;
            }
            return FALSE;
        }
        mutation->uses_physical_patch = TRUE;
        return TRUE;
    }

    if (!snapshot->has_patch || snapshot->prologue_len == 0 ||
        snapshot->prologue_len > PEAK_DETACH_HELPER_MAX_PATCH_BYTES) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
        }
        return FALSE;
    }

    if (request->operation == PEAK_DETACH_OPERATION_DETACH) {
        PeakDetachPhysicalPatchRecord* record =
            peak_detach_controller_find_physical_patch_record(request->hook_id,
                                                              TRUE);
        if (record == NULL) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_ERROR;
            }
            return FALSE;
        }
        record->function_address = request->function_address;
        record->patch_size = (uint32_t)snapshot->prologue_len;
        memcpy(record->active_patch,
               snapshot->active_patch,
               snapshot->prologue_len);
    }

    if (!peak_detach_controller_append_write_instruction(mutation,
                                                         request->function_address,
                                                         snapshot->original_prologue,
                                                         (uint32_t)snapshot->prologue_len)) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    mutation->uses_physical_patch = TRUE;
    return TRUE;
}

static gboolean
peak_detach_controller_operation_patches_entry(PeakDetachOperation operation)
{
    return operation == PEAK_DETACH_OPERATION_ATTACH ||
           operation == PEAK_DETACH_OPERATION_REPLACE ||
           operation == PEAK_DETACH_OPERATION_REVERT;
}

static gboolean
peak_detach_controller_classify_stopped_threads(
    const PeakDetachRequest* request,
    const PeakDetachGumSnapshot* snapshot,
    PeakDetachHelperThreadSnapshot* snapshots,
    uint32_t snapshot_count,
    PeakDetachHeldMutation* mutation,
    PeakDetachStatus* status_out)
{
    mutation->instruction_count = 0;
    mutation->uses_physical_patch = FALSE;
    mutation->operation = request->operation;

    for (uint32_t i = 0; i < snapshot_count; i++) {
        PeakDetachHelperThreadSnapshot* thread_snapshot = &snapshots[i];
        GumPeakPcState state = GUM_PEAK_PC_UNKNOWN;
        gpointer pc = (gpointer)(uintptr_t)thread_snapshot->pc;

        if (thread_snapshot->status != PEAK_DETACH_HELPER_THREAD_OK) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_ERROR;
            }
            return FALSE;
        }

        if (peak_detach_controller_pointer_in_range(pc,
                                                    request->blocked_pc_start,
                                                    request->blocked_pc_size)) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
            }
            return FALSE;
        }

        if (peak_detach_controller_operation_patches_entry(request->operation) &&
            peak_detach_controller_pointer_in_range(
                pc, request->function_address, GUM_PEAK_MAX_PROLOGUE_SIZE)) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
            }
            return FALSE;
        }

        state = peak_detach_controller_classify_pc_from_snapshot(snapshot,
                                                                pc);

        switch (state) {
            case GUM_PEAK_PC_SAFE:
                break;

            case GUM_PEAK_PC_AT_PATCH_ENTRY:
                if (request->operation == PEAK_DETACH_OPERATION_REATTACH ||
                    request->operation == PEAK_DETACH_OPERATION_ATTACH ||
                    request->operation == PEAK_DETACH_OPERATION_REPLACE) {
                    if (status_out != NULL) {
                        *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
                    }
                    return FALSE;
                }
                break;

            case GUM_PEAK_PC_IN_ENTER_TRAMPOLINE:
            case GUM_PEAK_PC_IN_INVOKE_TRAMPOLINE:
            case GUM_PEAK_PC_IN_LEAVE_TRAMPOLINE:
            case GUM_PEAK_PC_IN_DISPATCH: {
                gpointer safe_pc =
                    peak_detach_controller_safe_pc_from_snapshot(snapshot,
                                                                 pc,
                                                                 state);

                if (safe_pc == NULL ||
                    mutation->instruction_count >= PEAK_DETACH_HELPER_MAX_INSTRUCTIONS) {
                    if (status_out != NULL) {
                        *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
                    }
                    return FALSE;
                }

                PeakDetachHelperInstruction* instruction =
                    &mutation->instructions[mutation->instruction_count++];
                instruction->tid = thread_snapshot->tid;
                instruction->action = PEAK_DETACH_HELPER_INSTRUCTION_SET_PC;
                instruction->pc = (uint64_t)(uintptr_t)safe_pc;
                break;
            }

            case GUM_PEAK_PC_UNKNOWN:
            default:
                if (status_out != NULL) {
                    *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
                }
                return FALSE;
        }
    }

    if (!peak_detach_controller_append_physical_patch(request,
                                                       snapshot,
                                                       mutation,
                                                       status_out)) {
        return FALSE;
    }

    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

static gboolean
peak_detach_controller_find_instruction_conflict(
    const PeakDetachHeldMutation* mutation,
    const PeakDetachHelperInstruction* candidate)
{
    for (uint32_t i = 0; i < mutation->instruction_count; i++) {
        const PeakDetachHelperInstruction* existing = &mutation->instructions[i];

        if (existing->action != candidate->action) {
            continue;
        }

        if (candidate->action == PEAK_DETACH_HELPER_INSTRUCTION_SET_PC &&
            existing->tid == candidate->tid) {
            return existing->pc != candidate->pc;
        }

        if (candidate->action == PEAK_DETACH_HELPER_INSTRUCTION_WRITE_MEMORY &&
            peak_detach_controller_write_ranges_overlap(existing, candidate)) {
            if (existing->address == candidate->address &&
                existing->size == candidate->size &&
                memcmp(existing->bytes, candidate->bytes, candidate->size) == 0) {
                return FALSE;
            }
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
peak_detach_controller_instruction_is_duplicate(
    const PeakDetachHeldMutation* mutation,
    const PeakDetachHelperInstruction* candidate)
{
    for (uint32_t i = 0; i < mutation->instruction_count; i++) {
        const PeakDetachHelperInstruction* existing = &mutation->instructions[i];

        if (existing->action != candidate->action) {
            continue;
        }
        if (candidate->action == PEAK_DETACH_HELPER_INSTRUCTION_SET_PC &&
            existing->tid == candidate->tid &&
            existing->pc == candidate->pc) {
            return TRUE;
        }
        if (candidate->action == PEAK_DETACH_HELPER_INSTRUCTION_WRITE_MEMORY &&
            existing->address == candidate->address &&
            existing->size == candidate->size &&
            memcmp(existing->bytes, candidate->bytes, candidate->size) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
peak_detach_controller_append_candidate_mutation(
    PeakDetachHeldMutation* aggregate,
    const PeakDetachHeldMutation* candidate,
    PeakDetachStatus* status_out)
{
    for (uint32_t i = 0; i < candidate->instruction_count; i++) {
        const PeakDetachHelperInstruction* instruction =
            &candidate->instructions[i];

        if (peak_detach_controller_find_instruction_conflict(aggregate,
                                                             instruction)) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
            }
            return FALSE;
        }
        if (peak_detach_controller_instruction_is_duplicate(aggregate,
                                                           instruction)) {
            continue;
        }
        if (aggregate->instruction_count >= PEAK_DETACH_HELPER_MAX_INSTRUCTIONS) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_ERROR;
            }
            return FALSE;
        }

        aggregate->instructions[aggregate->instruction_count++] = *instruction;
    }

    aggregate->uses_physical_patch =
        aggregate->uses_physical_patch || candidate->uses_physical_patch;
    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

static PeakDetachStatus
peak_detach_controller_empty_batch_status(const PeakDetachBatchResult* results,
                                          size_t result_count)
{
    gboolean all_unsupported = TRUE;

    for (size_t i = 0; i < result_count; i++) {
        if (results[i].status != PEAK_DETACH_STATUS_UNSUPPORTED) {
            all_unsupported = FALSE;
            break;
        }
    }

    return all_unsupported ? PEAK_DETACH_STATUS_UNSUPPORTED :
                             PEAK_DETACH_STATUS_CLASSIFY_FAILED;
}
#endif

gboolean
peak_detach_controller_prepare_hook_mutation(const PeakDetachRequest* request,
                                             PeakDetachStatus* status_out)
{
#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    peak_detach_controller_init_atfork_once();
#endif
    gboolean proc_ok = FALSE;
    size_t proc_threads = peak_detach_controller_count_proc_threads(&proc_ok);

    (void)proc_threads;
    (void)proc_ok;

    if (!peak_detach_controller_validate_request(request, status_out)) {
        return FALSE;
    }

    if (peak_detach_controller_mode() == PEAK_SAFE_DETACH_MODE_COMPATIBILITY) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED;
        }
        return TRUE;
    }

#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    if (!warned_missing_gum_api) {
        warned_missing_gum_api = TRUE;
        g_printerr("[peak] strict safe detach requested, but selected Frida Gum does not expose PEAK PC classification APIs; refusing Gum %s for hook %lu (%s)\n",
                   peak_detach_controller_operation_string(request->operation),
                   (unsigned long)request->hook_id,
                   request->symbol_name != NULL ? request->symbol_name : "<unknown>");
    }
    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_MISSING_GUM_API;
    }
    return FALSE;
#else
    PeakDetachGumSnapshot gum_snapshot;
    PeakDetachHelperThreadSnapshot snapshots[PEAK_DETACH_HELPER_MAX_THREADS];
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    uint32_t snapshot_count = 0;

    peak_detach_controller_lock_mutation_guard();

    if (held_mutation.active) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        peak_detach_controller_unlock_mutation_guard();
        return FALSE;
    }
    held_mutation_started_at = 0.0;
    last_stop_window_us = 0.0;

    if (helper_state_fatal) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        peak_detach_controller_unlock_mutation_guard();
        return FALSE;
    }

    if (!peak_detach_controller_ensure_helper(&status)) {
        if (status_out != NULL) {
            *status_out = status;
        }
        peak_detach_controller_unlock_mutation_guard();
        return FALSE;
    }

    if (!peak_detach_controller_capture_gum_snapshot(request,
                                                     &gum_snapshot,
                                                     &status)) {
        if (status_out != NULL) {
            *status_out = status;
        }
        peak_detach_controller_unlock_mutation_guard();
        return FALSE;
    }

    peak_detach_controller_note_stop_window_started();
    if (!peak_detach_controller_stop_threads(snapshots, &snapshot_count, &status)) {
        held_mutation_started_at = 0.0;
        if (status_out != NULL) {
            *status_out = status;
        }
        peak_detach_controller_unlock_mutation_guard();
        return FALSE;
    }

    if (!peak_detach_controller_classify_stopped_threads(request,
                                                        &gum_snapshot,
                                                        snapshots,
                                                        snapshot_count,
                                                        &held_mutation,
                                                        &status)) {
        PeakDetachStatus resume_status = PEAK_DETACH_STATUS_ERROR;

        if (!peak_detach_controller_send_resume(&resume_status)) {
            peak_detach_controller_mark_helper_fatal("classify abort",
                                                     resume_status);
        }
        peak_detach_controller_note_stop_window_finished();

        if (status_out != NULL) {
            *status_out = status;
        }
        peak_detach_controller_unlock_mutation_guard();
        return FALSE;
    }

    if (held_mutation.instruction_count > 0 &&
        !peak_detach_controller_send_evacuate(held_mutation.instruction_count,
                                              held_mutation.instructions,
                                              &status)) {
        held_mutation = (PeakDetachHeldMutation){ 0 };
        if (status_out != NULL) {
            *status_out = status;
        }
        peak_detach_controller_unlock_mutation_guard();
        return FALSE;
    }
    held_mutation.instruction_count = 0;
    held_mutation.active = TRUE;

    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    peak_detach_controller_unlock_mutation_guard();
    return TRUE;
#endif
}

gboolean
peak_detach_controller_strict_batch_supported(void)
{
    if (peak_detach_controller_mode() == PEAK_SAFE_DETACH_MODE_COMPATIBILITY) {
        return FALSE;
    }

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    return TRUE;
#else
    return FALSE;
#endif
}

gboolean
peak_detach_controller_prepare_hook_mutation_batch(
    const PeakDetachRequest* requests,
    size_t request_count,
    PeakDetachBatchResult* results,
    size_t* prepared_count_out,
    PeakDetachStatus* status_out)
{
#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    peak_detach_controller_init_atfork_once();
#endif
    if (prepared_count_out != NULL) {
        *prepared_count_out = 0;
    }

    if (requests == NULL || results == NULL || request_count == 0) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    for (size_t i = 0; i < request_count; i++) {
        results[i].prepared = FALSE;
        results[i].uses_physical_patch = FALSE;
        results[i].status = PEAK_DETACH_STATUS_ERROR;
    }

    if (peak_detach_controller_mode() == PEAK_SAFE_DETACH_MODE_COMPATIBILITY) {
        for (size_t i = 0; i < request_count; i++) {
            results[i].status = PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED;
        }
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED;
        }
        return FALSE;
    }

#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    if (!warned_missing_gum_api) {
        warned_missing_gum_api = TRUE;
        g_printerr("[peak] strict safe detach requested, but selected Frida Gum does not expose PEAK PC classification APIs; refusing batched Gum target mutations\n");
    }
    for (size_t i = 0; i < request_count; i++) {
        results[i].status = PEAK_DETACH_STATUS_MISSING_GUM_API;
    }
    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_MISSING_GUM_API;
    }
    return FALSE;
#else
    size_t usable_count = request_count;
    PeakDetachBatchCandidate* candidates;
    PeakDetachHelperThreadSnapshot snapshots[PEAK_DETACH_HELPER_MAX_THREADS];
    PeakDetachHeldMutation aggregate = { 0 };
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    PeakDetachPhysicalPatchRecord* physical_patch_snapshot = NULL;
    uint32_t snapshot_count = 0;
    size_t valid_count = 0;
    size_t safe_count = 0;
    gboolean ambiguous_batch = FALSE;

    if (usable_count > PEAK_DETACH_CONTROLLER_MAX_BATCH_REQUESTS) {
        usable_count = PEAK_DETACH_CONTROLLER_MAX_BATCH_REQUESTS;
        for (size_t i = usable_count; i < request_count; i++) {
            results[i].status = PEAK_DETACH_STATUS_ERROR;
        }
    }

    candidates = calloc(usable_count, sizeof(*candidates));
    if (candidates == NULL) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    peak_detach_controller_lock_mutation_guard();

    if (held_mutation.active) {
        status = PEAK_DETACH_STATUS_ERROR;
        goto fail_all_locked;
    }
    held_mutation_started_at = 0.0;
    last_stop_window_us = 0.0;
    if (helper_state_fatal) {
        status = PEAK_DETACH_STATUS_ERROR;
        goto fail_all_locked;
    }

    for (size_t i = 0; i < usable_count; i++) {
        PeakDetachStatus request_status = PEAK_DETACH_STATUS_ERROR;

        if (!peak_detach_controller_validate_request(&requests[i],
                                                     &request_status)) {
            results[i].status = request_status;
            continue;
        }
        if (requests[i].operation != PEAK_DETACH_OPERATION_DETACH &&
            requests[i].operation != PEAK_DETACH_OPERATION_REATTACH) {
            results[i].status = PEAK_DETACH_STATUS_UNSUPPORTED;
            continue;
        }

        candidates[i].valid = TRUE;
        candidates[i].status = PEAK_DETACH_STATUS_SAFE;
        results[i].status = PEAK_DETACH_STATUS_SAFE;
    }

    for (size_t i = 0; i < usable_count; i++) {
        if (!candidates[i].valid) {
            continue;
        }
        for (size_t j = i + 1; j < usable_count; j++) {
            if (!candidates[j].valid) {
                continue;
            }
            if (requests[i].hook_id == requests[j].hook_id ||
                requests[i].function_address == requests[j].function_address) {
                candidates[i].valid = FALSE;
                candidates[j].valid = FALSE;
                results[i].status = PEAK_DETACH_STATUS_UNSUPPORTED;
                results[j].status = PEAK_DETACH_STATUS_UNSUPPORTED;
            }
        }
    }

    for (size_t i = 0; i < usable_count; i++) {
        if (!candidates[i].valid) {
            continue;
        }
        if (requests[i].operation == PEAK_DETACH_OPERATION_REATTACH) {
            PeakDetachPhysicalPatchRecord* record =
                peak_detach_controller_find_physical_patch_record(
                    requests[i].hook_id,
                    FALSE);
            if (record == NULL || record->patch_size == 0 ||
                record->function_address != requests[i].function_address) {
                candidates[i].valid = FALSE;
                results[i].status = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
                continue;
            }
        }
        if (!peak_detach_controller_capture_gum_snapshot(&requests[i],
                                                         &candidates[i].snapshot,
                                                         &candidates[i].status)) {
            candidates[i].valid = FALSE;
            results[i].status = candidates[i].status;
            continue;
        }
        valid_count++;
    }

    if (valid_count == 0) {
        status = peak_detach_controller_empty_batch_status(results,
                                                           usable_count);
        goto fail_without_stop_locked;
    }

    if (!peak_detach_controller_ensure_helper(&status)) {
        goto fail_all_locked;
    }

    physical_patch_snapshot = malloc(sizeof(physical_patch_records));
    if (physical_patch_snapshot == NULL) {
        status = PEAK_DETACH_STATUS_ERROR;
        goto fail_all_locked;
    }
    memcpy(physical_patch_snapshot,
           physical_patch_records,
           sizeof(physical_patch_records));

    peak_detach_controller_note_stop_window_started();
    if (!peak_detach_controller_stop_threads(snapshots, &snapshot_count, &status)) {
        held_mutation_started_at = 0.0;
        goto fail_all_locked;
    }

    for (size_t i = 0; i < usable_count; i++) {
        PeakDetachStatus candidate_status = PEAK_DETACH_STATUS_ERROR;

        if (!candidates[i].valid) {
            continue;
        }

        if (!peak_detach_controller_classify_stopped_threads(
                &requests[i],
                &candidates[i].snapshot,
                snapshots,
                snapshot_count,
                &candidates[i].mutation,
                &candidate_status)) {
            candidates[i].status = candidate_status;
            results[i].status = candidate_status;
            continue;
        }

        if (!peak_detach_controller_append_candidate_mutation(
                &aggregate,
                &candidates[i].mutation,
                &candidate_status)) {
            ambiguous_batch = TRUE;
            status = candidate_status;
            break;
        }

        candidates[i].safe = TRUE;
        candidates[i].status = PEAK_DETACH_STATUS_SAFE;
        results[i].status = PEAK_DETACH_STATUS_SAFE;
        safe_count++;
    }

    if (ambiguous_batch) {
        PeakDetachStatus resume_status = PEAK_DETACH_STATUS_ERROR;

        if (!peak_detach_controller_send_resume(&resume_status)) {
            peak_detach_controller_mark_helper_fatal("batch classify abort",
                                                     resume_status);
        }
        peak_detach_controller_note_stop_window_finished();
        memcpy(physical_patch_records,
               physical_patch_snapshot,
               sizeof(physical_patch_records));
        for (size_t i = 0; i < usable_count; i++) {
            if (candidates[i].valid) {
                results[i].prepared = FALSE;
                results[i].uses_physical_patch = FALSE;
                results[i].status = status;
            }
        }
        goto fail_unlocked_after_resume;
    }

    if (safe_count == 0) {
        PeakDetachStatus resume_status = PEAK_DETACH_STATUS_ERROR;

        if (!peak_detach_controller_send_resume(&resume_status)) {
            peak_detach_controller_mark_helper_fatal("batch classify retry",
                                                     resume_status);
        }
        peak_detach_controller_note_stop_window_finished();
        memcpy(physical_patch_records,
               physical_patch_snapshot,
               sizeof(physical_patch_records));
        status = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
        goto fail_unlocked_after_resume;
    }

    if (aggregate.instruction_count > 0 &&
        !peak_detach_controller_send_evacuate(aggregate.instruction_count,
                                              aggregate.instructions,
                                              &status)) {
        held_mutation = (PeakDetachHeldMutation){ 0 };
        for (size_t i = 0; i < usable_count; i++) {
            if (candidates[i].safe) {
                results[i].status = status;
            }
        }
        memcpy(physical_patch_records,
               physical_patch_snapshot,
               sizeof(physical_patch_records));
        goto fail_without_resume_locked;
    }

    aggregate.instruction_count = 0;
    aggregate.active = TRUE;
    aggregate.operation = PEAK_DETACH_OPERATION_DETACH;
    held_mutation = aggregate;

    for (size_t i = 0; i < usable_count; i++) {
        if (candidates[i].safe) {
            results[i].prepared = TRUE;
            results[i].uses_physical_patch =
                candidates[i].mutation.uses_physical_patch;
        }
    }

    if (prepared_count_out != NULL) {
        *prepared_count_out = safe_count;
    }
    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    peak_detach_controller_unlock_mutation_guard();
    free(physical_patch_snapshot);
    free(candidates);
    return TRUE;

fail_all_locked:
    for (size_t i = 0; i < usable_count; i++) {
        if (candidates[i].valid || results[i].status == PEAK_DETACH_STATUS_SAFE) {
            results[i].status = status;
        }
    }
fail_without_stop_locked:
    if (status_out != NULL) {
        *status_out = status;
    }
    peak_detach_controller_unlock_mutation_guard();
    free(physical_patch_snapshot);
    free(candidates);
    return FALSE;

fail_without_resume_locked:
    if (status_out != NULL) {
        *status_out = status;
    }
    peak_detach_controller_unlock_mutation_guard();
    free(physical_patch_snapshot);
    free(candidates);
    return FALSE;

fail_unlocked_after_resume:
    if (status_out != NULL) {
        *status_out = status;
    }
    peak_detach_controller_unlock_mutation_guard();
    free(physical_patch_snapshot);
    free(candidates);
    return FALSE;
#endif
}

gboolean
peak_detach_controller_threads_are_held(void)
{
    gboolean active = FALSE;

    if (peak_detach_controller_mode() == PEAK_SAFE_DETACH_MODE_COMPATIBILITY) {
        return FALSE;
    }

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    peak_detach_controller_init_atfork_once();
    peak_detach_controller_lock_mutation_guard();
    active = held_mutation.active;
    peak_detach_controller_unlock_mutation_guard();
#endif

    return active;
}

gboolean
peak_detach_controller_current_mutation_uses_physical_patch(void)
{
    gboolean uses_physical_patch = FALSE;

    if (peak_detach_controller_mode() == PEAK_SAFE_DETACH_MODE_COMPATIBILITY) {
        return FALSE;
    }

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    peak_detach_controller_init_atfork_once();
    peak_detach_controller_lock_mutation_guard();
    uses_physical_patch = held_mutation.active &&
                          held_mutation.uses_physical_patch;
    peak_detach_controller_unlock_mutation_guard();
#endif

    return uses_physical_patch;
}

double
peak_detach_controller_last_stop_window_us(void)
{
    double value = 0.0;

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    peak_detach_controller_init_atfork_once();
    peak_detach_controller_lock_mutation_guard();
    value = last_stop_window_us;
    peak_detach_controller_unlock_mutation_guard();
#endif

    return value;
}

gboolean
peak_detach_controller_finish_hook_mutation(const PeakDetachRequest* request,
                                            PeakDetachStatus* status_out)
{
    if (request != NULL &&
        !peak_detach_controller_operation_is_valid(request->operation)) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    if (peak_detach_controller_mode() == PEAK_SAFE_DETACH_MODE_COMPATIBILITY) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED;
        }
        return TRUE;
    }

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    peak_detach_controller_init_atfork_once();
    peak_detach_controller_lock_mutation_guard();

    if (held_mutation.active) {
        PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;

        gboolean released = peak_detach_controller_send_resume(&status);

        if (!released) {
            held_mutation_started_at = 0.0;
            if (!warned_helper_resume_failed) {
                warned_helper_resume_failed = TRUE;
                g_printerr("[peak] detach helper failed to resume stopped threads after Gum mutation: %s\n",
                           peak_detach_controller_status_string(status));
            }
            peak_detach_controller_mark_helper_fatal("finish", status);
            if (status_out != NULL) {
                *status_out = status;
            }
            peak_detach_controller_unlock_mutation_guard();
            return FALSE;
        }

        peak_detach_controller_note_stop_window_finished();
        held_mutation.active = FALSE;
        held_mutation.uses_physical_patch = FALSE;
        held_mutation.operation = PEAK_DETACH_OPERATION_ATTACH;
        held_mutation.instruction_count = 0;
    }

    peak_detach_controller_unlock_mutation_guard();
#endif
    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

gboolean
peak_detach_controller_finish_hook_mutation_batch(PeakDetachStatus* status_out)
{
    return peak_detach_controller_finish_hook_mutation(NULL, status_out);
}

gboolean
peak_detach_controller_shutdown_helper(PeakDetachStatus* status_out)
{
    if (peak_detach_controller_mode() == PEAK_SAFE_DETACH_MODE_COMPATIBILITY) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED;
        }
        return TRUE;
    }

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    peak_detach_controller_init_atfork_once();
    peak_detach_controller_lock_mutation_guard();

    if (held_mutation.active) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        peak_detach_controller_unlock_mutation_guard();
        return FALSE;
    }

    if (helper_fd >= 0) {
        PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
        gboolean ok = peak_detach_controller_send_shutdown(TRUE, &status);
        if (!ok) {
            if (!warned_helper_unavailable) {
                warned_helper_unavailable = TRUE;
                g_printerr("[peak] detach helper was unavailable during idle shutdown: %s\n",
                           peak_detach_controller_status_string(status));
            }
            if (status_out != NULL) {
                *status_out = status;
            }
            peak_detach_controller_unlock_mutation_guard();
            return FALSE;
        }
    }

    peak_detach_controller_unlock_mutation_guard();
#endif

    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

void
peak_detach_controller_abort_after_failed_finish(const char* context,
                                                 PeakDetachStatus status)
{
#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    peak_detach_controller_mark_helper_fatal(context, status);
#else
    const char message[] =
        "[peak] fatal safe-detach finish failure; terminating to avoid unpublished Gum hook state\n";

    (void)context;
    (void)status;
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(128);
#endif
}
