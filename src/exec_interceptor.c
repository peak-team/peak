#define _GNU_SOURCE
#include "exec_interceptor.h"
#include "internal/exec_interceptor_internal.h"
#include "internal/exec_memory.h"
#include "internal/exec_raw_syscall.h"
#include "utils/utils.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#if defined(__linux__)
#include <sys/auxv.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef PEAK_EXEC_MAX_VARARGS
#define PEAK_EXEC_MAX_VARARGS 4096U
#endif

#ifndef PEAK_EXEC_DEFAULT_PATH
#define PEAK_EXEC_DEFAULT_PATH "/bin:/usr/bin"
#endif

#define PEAK_EXEC_CHAIN_ENV "PEAK_EXEC_CHAIN"
#define PEAK_EXEC_CHECKPOINT_ENV "PEAK_EXEC_CHECKPOINT"
#define PEAK_EXEC_PROPAGATE_PEAK_ENV_ENV "PEAK_EXEC_PROPAGATE_PEAK_ENV"
#define PEAK_EXEC_TRACE_PATH_ENV "PEAK_EXEC_TRACE_PATH"

/*
 * Post-fork execution is deliberately bounded.  These buffers live in the
 * exec wrapper's frame so a child never allocates or changes shared runtime
 * state before it reaches libc execve.
 */
#define PEAK_EXEC_CHILD_ENV_SLOTS 256U
#define PEAK_EXEC_CHILD_LD_PRELOAD_CAPACITY (PATH_MAX * 2U)
#define PEAK_EXEC_CHILD_NAME_CAPACITY 256U

extern char** environ;

typedef int (*peak_execve_fn)(const char*, char* const[], char* const[]);
typedef int (*peak_execvp_fn)(const char*, char* const[]);
typedef int (*peak_execvpe_fn)(const char*, char* const[], char* const[]);
typedef int (*peak_fexecve_fn)(int, char* const[], char* const[]);
typedef int (*peak_execveat_fn)(int,
                                const char*,
                                char* const[],
                                char* const[],
                                int);
typedef int (*peak_posix_spawn_fn)(pid_t*,
                                   const char*,
                                   const posix_spawn_file_actions_t*,
                                   const posix_spawnattr_t*,
                                   char* const[],
                                   char* const[]);
typedef int (*peak_posix_spawnp_fn)(pid_t*,
                                    const char*,
                                    const posix_spawn_file_actions_t*,
                                    const posix_spawnattr_t*,
                                    char* const[],
                                    char* const[]);

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#define PEAK_EXEC_DIRECT_RAW_SUPPORTED 1
#else
#define PEAK_EXEC_DIRECT_RAW_SUPPORTED 0
#endif

static int peak_call_real_execve(const char*,
                                 char* const[],
                                 char* const[]);
#if !PEAK_EXEC_DIRECT_RAW_SUPPORTED
static int peak_call_real_execvp(const char*, char* const[]);
static int peak_call_real_execvpe(const char*,
                                  char* const[],
                                  char* const[]);
#endif
static int peak_call_real_fexecve(int, char* const[], char* const[]);
#if defined(__linux__)
static int peak_call_real_execveat(int,
                                   const char*,
                                   char* const[],
                                   char* const[],
                                   int);
#endif

typedef struct {
    char** envp;
    char** owned_strings;
    size_t owned_count;
    size_t owned_capacity;
    int changed;
} PeakExecEnv;

static __thread int peak_exec_wrapper_depth;
static __thread int peak_exec_spawn_depth;
static pid_t peak_exec_owner_pid;
static char peak_exec_cached_libpeak_path[PATH_MAX];
static int peak_exec_cached_libpeak_path_ready;
static char* peak_exec_cached_ld_library_path;
static int peak_exec_cached_ld_library_path_ready;
static int peak_exec_cached_ld_library_path_failed;
static int peak_exec_cached_secure_mode;
static int peak_exec_cached_secure_mode_ready;
/* Immutable after constructor priming; wrappers read these without libc calls. */
static int peak_exec_startup_chain_enabled;
static int peak_exec_startup_checkpoint_enabled;
static int peak_exec_startup_policy_enabled;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "exec policy publication must always be lock-free");
_Static_assert(ATOMIC_LONG_LOCK_FREE == 2,
               "exec resolver ownership must always be lock-free");
static atomic_int peak_exec_priming_complete = ATOMIC_VAR_INIT(0);

PEAK_EXEC_INTERNAL int
peak_exec_checkpoint_enabled_at_startup(void)
{
    return peak_exec_startup_checkpoint_enabled;
}
#if !PEAK_EXEC_DIRECT_RAW_SUPPORTED
/* This guard only breaks loader-time resolver recursion; it is not signal-safe. */
static __thread int peak_exec_prepublication_execve_resolving;
#endif
#if defined(PEAK_ENABLE_TEST_HOOKS)
static int peak_exec_test_force_execvpe_fallback;
extern void peak_test_exec_resolver_reentry_hook(void)
    __attribute__((weak));
#endif

static int
peak_exec_priming_is_complete(void)
{
    return atomic_load_explicit(&peak_exec_priming_complete,
                                memory_order_acquire) != 0;
}

static int
peak_exec_prepublication_execve(const char* path,
                                char* const argv[],
                                char* const envp[])
{
#if PEAK_EXEC_DIRECT_RAW_SUPPORTED && defined(SYS_execve)
    return (int)peak_exec_raw_syscall6(
        SYS_execve,
        (long)(uintptr_t)path,
        (long)(uintptr_t)argv,
        (long)(uintptr_t)envp,
        0,
        0,
        0);
#else
    peak_execve_fn fn;

    if (peak_exec_prepublication_execve_resolving) {
        errno = ENOSYS;
        return -1;
    }
    peak_exec_prepublication_execve_resolving = 1;
    fn = (peak_execve_fn)dlsym(RTLD_NEXT, "execve");
    peak_exec_prepublication_execve_resolving = 0;
    if (fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return fn(path, argv, envp);
#endif
}

static size_t
peak_exec_prepublication_strlen(const char* value, size_t limit)
{
    size_t length = 0;

    if (value == NULL) {
        return 0;
    }
    while (length < limit && value[length] != '\0') {
        length++;
    }
    return length;
}

static const char*
peak_exec_prepublication_path(void)
{
    static const char prefix[] = "PATH=";

    if (environ != NULL) {
        for (size_t index = 0; environ[index] != NULL; index++) {
            const char* entry = environ[index];
            size_t offset = 0;

            while (offset + 1 < sizeof(prefix) &&
                   entry[offset] == prefix[offset]) {
                offset++;
            }
            if (offset + 1 == sizeof(prefix)) {
                return entry + offset;
            }
        }
    }
    return PEAK_EXEC_DEFAULT_PATH;
}

static int
peak_exec_prepublication_shell(const char* path,
                               char* const argv[],
                               char* const envp[])
{
    size_t argc = 0;

    if (argv != NULL) {
        while (argv[argc] != NULL) {
            argc++;
            if (argc >= PEAK_EXEC_MAX_VARARGS) {
                errno = E2BIG;
                return -1;
            }
        }
    }
    {
        char* shell_argv[argc + 3U];

        shell_argv[0] = (char*)"/bin/sh";
        shell_argv[1] = (char*)path;
        for (size_t index = 1; index < argc; index++) {
            shell_argv[index + 1] = argv[index];
        }
        shell_argv[argc > 0 ? argc + 1 : 2] = NULL;
        return peak_exec_prepublication_execve("/bin/sh", shell_argv, envp);
    }
}

static int
peak_exec_prepublication_path_search(const char* file,
                                     char* const argv[],
                                     char* const envp[])
{
    char candidate[PATH_MAX];
    const char* path;
    const char* cursor;
    size_t file_length;
    int saved_errno = ENOENT;
    int saw_eacces = 0;

    if (file == NULL || file[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    file_length = peak_exec_prepublication_strlen(file, PATH_MAX);
    if (file_length == PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    for (size_t index = 0; index < file_length; index++) {
        if (file[index] == '/') {
            int result = peak_exec_prepublication_execve(file, argv, envp);

            if (result < 0 && errno == ENOEXEC) {
                return peak_exec_prepublication_shell(file, argv, envp);
            }
            return result;
        }
    }

    path = peak_exec_prepublication_path();
    cursor = path;
    for (;;) {
        const char* start = cursor;
        size_t directory_length;
        size_t candidate_length;

        while (*cursor != '\0' && *cursor != ':') {
            cursor++;
        }
        directory_length = (size_t)(cursor - start);
        candidate_length = directory_length +
                           (directory_length != 0 ? 1U : 0U) + file_length;
        if (candidate_length + 1U <= sizeof(candidate)) {
            size_t output = 0;

            for (size_t index = 0; index < directory_length; index++) {
                candidate[output++] = start[index];
            }
            if (directory_length != 0) {
                candidate[output++] = '/';
            }
            for (size_t index = 0; index < file_length; index++) {
                candidate[output++] = file[index];
            }
            candidate[output] = '\0';
            (void)peak_exec_prepublication_execve(candidate, argv, envp);
            saved_errno = errno;
            if (saved_errno == ENOEXEC) {
                return peak_exec_prepublication_shell(candidate, argv, envp);
            }
            if (saved_errno == EACCES) {
                saw_eacces = 1;
            } else if (saved_errno != ENOENT && saved_errno != ENOTDIR &&
                       saved_errno != ESTALE && saved_errno != ENODEV &&
                       saved_errno != ETIMEDOUT) {
                errno = saved_errno;
                return -1;
            }
        } else {
            saved_errno = ENAMETOOLONG;
        }
        if (*cursor == '\0') {
            break;
        }
        cursor++;
    }
    errno = saw_eacces ? EACCES : saved_errno;
    return -1;
}

static int
peak_exec_prepublication_execvp(const char* file, char* const argv[])
{
#if PEAK_EXEC_DIRECT_RAW_SUPPORTED
    return peak_exec_prepublication_path_search(file, argv, environ);
#else
    return peak_call_real_execvp(file, argv);
#endif
}

static int
peak_exec_prepublication_execvpe(const char* file,
                                 char* const argv[],
                                 char* const envp[])
{
#if PEAK_EXEC_DIRECT_RAW_SUPPORTED
    return peak_exec_prepublication_path_search(file, argv, envp);
#else
    return peak_call_real_execvpe(file, argv, envp);
#endif
}

static int
peak_exec_prepublication_fexecve(int fd,
                                 char* const argv[],
                                 char* const envp[])
{
#if PEAK_EXEC_DIRECT_RAW_SUPPORTED && defined(SYS_execveat) && \
    defined(AT_EMPTY_PATH)
    return (int)peak_exec_raw_syscall6(
        SYS_execveat,
        fd,
        (long)(uintptr_t)"",
        (long)(uintptr_t)argv,
        (long)(uintptr_t)envp,
        AT_EMPTY_PATH,
        0);
#else
    return peak_call_real_fexecve(fd, argv, envp);
#endif
}

#if defined(__linux__)
static int
peak_exec_prepublication_execveat(int dirfd,
                                  const char* pathname,
                                  char* const argv[],
                                  char* const envp[],
                                  int flags)
{
#if PEAK_EXEC_DIRECT_RAW_SUPPORTED && defined(SYS_execveat)
    return (int)peak_exec_raw_syscall6(
        SYS_execveat,
        dirfd,
        (long)(uintptr_t)pathname,
        (long)(uintptr_t)argv,
        (long)(uintptr_t)envp,
        flags,
        0);
#else
    return peak_call_real_execveat(dirfd, pathname, argv, envp, flags);
#endif
}
#endif

static int
peak_exec_in_fork_child(void)
{
    return peak_exec_owner_pid > 0 && getpid() != peak_exec_owner_pid;
}

static long
peak_exec_child_pid(void)
{
#if defined(SYS_getpid) && defined(__x86_64__)
    long pid;

    __asm__ volatile("syscall"
                     : "=a"(pid)
                     : "a"((long)SYS_getpid)
                     : "rcx", "r11", "memory");
    return pid;
#elif defined(SYS_getpid) && defined(__aarch64__)
    register long x0 __asm__("x0");
    register long x8 __asm__("x8") = (long)SYS_getpid;

    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "cc", "memory");
    return x0;
#else
    return (long)getpid();
#endif
}

static int
peak_exec_child_read(const void* remote, void* local, size_t length)
{
    struct iovec local_iov = {local, length};
    struct iovec remote_iov = {(void*)remote, length};
    long copied;

#if defined(SYS_process_vm_readv) && defined(__x86_64__)
    register long r10 __asm__("r10") = (long)&remote_iov;
    register long r8 __asm__("r8") = 1L;
    register long r9 __asm__("r9") = 0L;

    /* Do not let a failed probe update vfork-shared errno. */
    __asm__ volatile("syscall"
                     : "=a"(copied)
                     : "a"((long)SYS_process_vm_readv),
                       "D"(peak_exec_child_pid()),
                       "S"((long)&local_iov),
                       "d"(1L),
                       "r"(r10),
                       "r"(r8),
                       "r"(r9)
                     : "rcx", "r11", "memory");
#elif defined(SYS_process_vm_readv) && defined(__aarch64__)
    register long x0 __asm__("x0") = peak_exec_child_pid();
    register long x1 __asm__("x1") = (long)&local_iov;
    register long x2 __asm__("x2") = 1L;
    register long x3 __asm__("x3") = (long)&remote_iov;
    register long x4 __asm__("x4") = 1L;
    register long x5 __asm__("x5") = 0L;
    register long x8 __asm__("x8") = (long)SYS_process_vm_readv;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "cc", "memory");
    copied = x0;
#else
    copied = -1;
#endif
    return copied == (ssize_t)length ? 0 : -1;
}

static int
peak_exec_child_read_pointer(char* const envp[], size_t index, const char** entry)
{
    uintptr_t base = (uintptr_t)envp;
    uintptr_t offset;

    if (entry == NULL || index > (UINTPTR_MAX - base) / sizeof(*envp)) {
        return -1;
    }
    offset = base + index * sizeof(*envp);
    return peak_exec_child_read((const void*)offset, entry, sizeof(*entry));
}

static int
peak_exec_child_read_byte(const char* source, size_t index, char* value)
{
    uintptr_t address = (uintptr_t)source;

    if (value == NULL || index > UINTPTR_MAX - address) {
        return -1;
    }
    return peak_exec_child_read((const void*)(address + index), value, 1);
}

/* Returns 1 for a match, 0 for a non-match, and -1 for unreadable memory. */
static int
peak_exec_child_entry_matches(const char* entry, const char* name)
{
    size_t index = 0;

    if (entry == NULL || name == NULL) {
        return 0;
    }
    while (name[index] != '\0') {
        char actual;

        if (peak_exec_child_read_byte(entry, index, &actual) != 0) {
            return -1;
        }
        if (actual != name[index]) {
            return 0;
        }
        index++;
    }
    {
        char actual;

        if (peak_exec_child_read_byte(entry, index, &actual) != 0) {
            return -1;
        }
        return actual == '=';
    }
}

/* Returns 0 when found, 1 when absent, and -1 when unreadable or unterminated. */
static int
peak_exec_child_env_get(char* const envp[], const char* name, const char** value)
{
    if (envp == NULL) {
        return 1;
    }
    for (size_t index = 0; index < PEAK_EXEC_CHILD_ENV_SLOTS; index++) {
        const char* entry;
        int matches;

        if (peak_exec_child_read_pointer(envp, index, &entry) != 0) {
            return -1;
        }
        if (entry == NULL) {
            return 1;
        }
        matches = peak_exec_child_entry_matches(entry, name);
        if (matches < 0) {
            return -1;
        }
        if (matches != 0) {
            *value = entry + strlen(name) + 1;
            return 0;
        }
    }
    return -1;
}

static int
peak_exec_child_value_false(const char* value, int* result)
{
    static const char* const false_values[] = {"0", "false", "no", "off"};

    for (size_t candidate_index = 0;
         candidate_index < sizeof(false_values) / sizeof(false_values[0]);
         candidate_index++) {
        const char* candidate = false_values[candidate_index];
        size_t index = 0;

        for (;;) {
            char actual;

            if (index >= PEAK_EXEC_CHILD_NAME_CAPACITY ||
                peak_exec_child_read_byte(value, index, &actual) != 0) {
                return -1;
            }
            if (actual >= 'A' && actual <= 'Z') {
                actual = (char)(actual - 'A' + 'a');
            }
            if (actual != candidate[index]) {
                break;
            }
            if (actual == '\0') {
                *result = 1;
                return 0;
            }
            index++;
        }
    }
    *result = 0;
    return 0;
}

static int
peak_exec_child_control_enabled(char* const child_envp[], const char* name, int* enabled)
{
    const char* value = NULL;
    int found = peak_exec_child_env_get(child_envp, name, &value);
    int is_false;

    if (found < 0) {
        return -1;
    }
    if (found != 0) {
        found = peak_exec_child_env_get(environ, name, &value);
        if (found < 0) {
            return -1;
        }
    }
    if (found != 0) {
        *enabled = 1;
        return 0;
    }
    if (peak_exec_child_value_false(value, &is_false) != 0) {
        return -1;
    }
    *enabled = !is_false;
    return 0;
}

static int
peak_exec_child_has_name(char* const envp[], const char* name, int* has_name)
{
    const char* value = NULL;
    int found = peak_exec_child_env_get(envp, name, &value);

    if (found < 0) {
        return -1;
    }
    *has_name = found == 0;
    return 0;
}

static int
peak_exec_child_is_separator(char value)
{
    return value == ':' || value == ' ' || value == '\t' || value == '\n';
}

static int
peak_exec_child_token_is_cached_libpeak(const char* token, size_t token_len)
{
    size_t libpeak_len = 0;

    if (token == NULL || !peak_exec_cached_libpeak_path_ready) {
        return 0;
    }
    while (peak_exec_cached_libpeak_path[libpeak_len] != '\0') {
        libpeak_len++;
    }
    if (token_len == libpeak_len) {
        for (size_t index = 0; index < token_len; index++) {
            char actual;

            if (peak_exec_child_read_byte(token, index, &actual) != 0 ||
                actual != peak_exec_cached_libpeak_path[index]) {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

static int
peak_exec_child_append_preload(char* output[],
                               size_t* count,
                               const char* entry)
{
    if (*count + 1 >= PEAK_EXEC_CHILD_ENV_SLOTS) {
        return -1;
    }
    output[(*count)++] = (char*)entry;
    return 0;
}

static char* const*
peak_exec_child_build_env(char* const envp[],
                          char* output[PEAK_EXEC_CHILD_ENV_SLOTS],
                          char ld_preload[PEAK_EXEC_CHILD_LD_PRELOAD_CAPACITY])
{
    size_t out = 0;
    size_t ld_len = sizeof("LD_PRELOAD=") - 1;
    size_t libpeak_len = 0;
    int child_has_ld_library_path = 0;
    int libpeak_already_present = 0;
    int env_terminated = envp == NULL;
    int propagate_peak_env;
    int chain_enabled;

    if (!peak_exec_startup_chain_enabled ||
        !peak_exec_cached_secure_mode_ready || peak_exec_cached_secure_mode ||
        !peak_exec_cached_libpeak_path_ready ||
        peak_exec_child_control_enabled(envp,
                                        PEAK_EXEC_CHAIN_ENV,
                                        &chain_enabled) != 0 ||
        !chain_enabled) {
        return envp;
    }
    while (peak_exec_cached_libpeak_path[libpeak_len] != '\0') {
        libpeak_len++;
    }
    if (ld_len + libpeak_len + 1 > PEAK_EXEC_CHILD_LD_PRELOAD_CAPACITY) {
        return envp;
    }
    memcpy(ld_preload, "LD_PRELOAD=", ld_len);
    memcpy(ld_preload + ld_len, peak_exec_cached_libpeak_path, libpeak_len);
    ld_len += libpeak_len;
    ld_preload[ld_len] = '\0';

    if (envp != NULL) {
        for (size_t index = 0; index < PEAK_EXEC_CHILD_ENV_SLOTS; index++) {
            const char* entry;
            int preload_matches;
            int library_path_matches;

            if (peak_exec_child_read_pointer(envp, index, &entry) != 0) {
                return envp;
            }
            if (entry == NULL) {
                env_terminated = 1;
                break;
            }
            preload_matches = peak_exec_child_entry_matches(entry, "LD_PRELOAD");
            if (preload_matches < 0) {
                return envp;
            }
            if (preload_matches == 0) {
                library_path_matches = peak_exec_child_entry_matches(
                    entry, "LD_LIBRARY_PATH");
                if (library_path_matches < 0) {
                    return envp;
                }
                if (library_path_matches != 0) {
                    child_has_ld_library_path = 1;
                }
            }
            if (preload_matches != 0) {
                const char* value = entry + sizeof("LD_PRELOAD=") - 1;
                const char* cursor = value;
                size_t cursor_index = 0;

                for (;;) {
                    char current;
                    size_t token_start;
                    size_t token_len = 0;

                    if (peak_exec_child_read_byte(cursor, cursor_index, &current) != 0) {
                        return envp;
                    }
                    while (current != '\0' && peak_exec_child_is_separator(current)) {
                        if (++cursor_index >= PEAK_EXEC_CHILD_LD_PRELOAD_CAPACITY ||
                            peak_exec_child_read_byte(cursor, cursor_index, &current) != 0) {
                            return envp;
                        }
                    }
                    if (current == '\0') {
                        break;
                    }
                    token_start = cursor_index;
                    while (current != '\0' && !peak_exec_child_is_separator(current)) {
                        if (++token_len >= PEAK_EXEC_CHILD_LD_PRELOAD_CAPACITY ||
                            ++cursor_index >= PEAK_EXEC_CHILD_LD_PRELOAD_CAPACITY ||
                            peak_exec_child_read_byte(cursor, cursor_index, &current) != 0) {
                            return envp;
                        }
                    }
                    if (token_len == 0 ||
                        peak_exec_child_token_is_cached_libpeak(
                            (const char*)((uintptr_t)cursor + token_start),
                            token_len)) {
                        if (token_len != 0) {
                            libpeak_already_present = 1;
                        }
                        continue;
                    }
                    if (ld_len + 1 + token_len + 1 >
                        PEAK_EXEC_CHILD_LD_PRELOAD_CAPACITY) {
                        return envp;
                    }
                    ld_preload[ld_len++] = ':';
                    for (size_t copy = 0; copy < token_len; copy++) {
                        if (peak_exec_child_read_byte(cursor,
                                                      token_start + copy,
                                                      ld_preload + ld_len++) != 0) {
                            return envp;
                        }
                    }
                    ld_preload[ld_len] = '\0';
                }
                continue;
            }
            if (peak_exec_child_append_preload(output, &out, entry) != 0) {
                return envp;
            }
        }
        if (!env_terminated) {
            return envp;
        }
    }
    if (peak_exec_child_append_preload(output, &out, ld_preload) != 0) {
        return envp;
    }
    if (!libpeak_already_present && !child_has_ld_library_path) {
        if (!peak_exec_cached_ld_library_path_ready ||
            peak_exec_cached_ld_library_path_failed) {
            return envp;
        }
        if (peak_exec_cached_ld_library_path != NULL &&
            peak_exec_child_append_preload(output,
                                           &out,
                                           peak_exec_cached_ld_library_path) != 0) {
            return envp;
        }
    }

    if (peak_exec_child_control_enabled(envp,
                                        PEAK_EXEC_PROPAGATE_PEAK_ENV_ENV,
                                        &propagate_peak_env) != 0) {
        return envp;
    }
    if (propagate_peak_env && environ != NULL) {
        int parent_env_terminated = 0;

        for (size_t index = 0; index < PEAK_EXEC_CHILD_ENV_SLOTS; index++) {
            const char* entry;
            char prefix[5];
            size_t name_len = 0;
            char name[PEAK_EXEC_CHILD_NAME_CAPACITY];
            int has_name;

            if (peak_exec_child_read_pointer(environ, index, &entry) != 0) {
                return envp;
            }
            if (entry == NULL) {
                parent_env_terminated = 1;
                break;
            }
            if (peak_exec_child_read(entry, prefix, sizeof(prefix)) != 0 ||
                memcmp(prefix, "PEAK_", 5) != 0) {
                continue;
            }
            while (name_len + 1 < sizeof(name)) {
                if (peak_exec_child_read_byte(entry, name_len, name + name_len) != 0) {
                    return envp;
                }
                if (name[name_len] == '=') {
                    break;
                }
                if (name[name_len] == '\0') {
                    name_len = 0;
                    break;
                }
                name_len++;
            }
            if (name_len == 0 || name_len + 1 >= sizeof(name) ||
                name[name_len] != '=') {
                continue;
            }
            name[name_len] = '\0';
            if (peak_exec_child_has_name(envp, name, &has_name) != 0) {
                return envp;
            }
            if (!has_name) {
                if (peak_exec_child_append_preload(output, &out, entry) != 0) {
                    return envp;
                }
            }
        }
        if (!parent_env_terminated) {
            return envp;
        }
    }
    output[out] = NULL;
    return output;
}

static int
peak_exec_size_add(size_t left, size_t right, size_t* result)
{
    if (result == NULL || left > SIZE_MAX - right) {
        errno = E2BIG;
        return -1;
    }
    *result = left + right;
    return 0;
}

static int
peak_exec_size_mul(size_t left, size_t right, size_t* result)
{
    if (result == NULL || (left != 0 && right > SIZE_MAX / left)) {
        errno = E2BIG;
        return -1;
    }
    *result = left * right;
    return 0;
}

static void
peak_exec_trace_event(const char* event,
                      const char* path,
                      const char* detail,
                      int event_errno)
{
    const char* trace_path = getenv(PEAK_EXEC_TRACE_PATH_ENV);
    int saved_errno = errno;
    int fd;

    if (trace_path == NULL || trace_path[0] == '\0') {
        return;
    }

    fd = open(trace_path,
              O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NONBLOCK,
              0600);
    if (fd >= 0) {
        char row[1024];
        int n = snprintf(row,
                         sizeof(row),
                         "pid=%ld event=%s path=%s detail=%s errno=%d\n",
                         (long)getpid(),
                         event != NULL ? event : "",
                         path != NULL ? path : "",
                         detail != NULL ? detail : "",
                         event_errno);
        if (n > 0) {
            size_t length = (size_t)n < sizeof(row) ?
                (size_t)n : sizeof(row) - 1;
            (void)write(fd, row, length);
        }
        (void)close(fd);
    }
    errno = saved_errno;
}

static int
peak_exec_env_false(const char* value)
{
    return value != NULL &&
           (strcasecmp(value, "0") == 0 ||
            strcasecmp(value, "false") == 0 ||
            strcasecmp(value, "no") == 0 ||
            strcasecmp(value, "off") == 0);
}

static int
peak_exec_env_default_true(const char* name)
{
    return !peak_exec_env_false(getenv(name));
}

static int
peak_exec_is_separator(char ch)
{
    return ch == ':' || ch == ' ' || ch == '\t' || ch == '\n';
}

static int
peak_exec_name_len(const char* entry)
{
    const char* equals;

    if (entry == NULL) {
        return -1;
    }
    equals = strchr(entry, '=');
    if (equals == NULL) {
        return -1;
    }
    return (int)(equals - entry);
}

static int
peak_exec_env_entry_matches(const char* entry, const char* name)
{
    int name_len = peak_exec_name_len(entry);

    return name_len >= 0 &&
           strlen(name) == (size_t)name_len &&
           strncmp(entry, name, (size_t)name_len) == 0;
}

static const char*
peak_exec_env_get(char* const envp[], const char* name)
{
    if (envp == NULL) {
        return NULL;
    }
    for (char* const* scan = envp; *scan != NULL; scan++) {
        if (peak_exec_env_entry_matches(*scan, name)) {
            return strchr(*scan, '=') + 1;
        }
    }
    return NULL;
}

static int
peak_exec_env_default_true_for_child(char* const envp[], const char* name)
{
    const char* child_value = peak_exec_env_get(envp, name);

    return child_value != NULL ? !peak_exec_env_false(child_value) :
                                 peak_exec_env_default_true(name);
}

static int
peak_exec_env_has_name(char* const envp[], const char* name)
{
    return peak_exec_env_get(envp, name) != NULL;
}

static int
peak_exec_storage_has_name(char** envp, size_t count, const char* name)
{
    for (size_t i = 0; i < count; i++) {
        if (peak_exec_env_entry_matches(envp[i], name)) {
            return 1;
        }
    }
    return 0;
}

static int
peak_exec_env_count_bounded(char* const envp[], size_t* count_out)
{
    size_t count = 0;

    if (count_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (envp == NULL) {
        *count_out = 0;
        return 0;
    }
    while (envp[count] != NULL) {
        count++;
        if (count > PEAK_EXEC_MAX_ENV_ENTRIES) {
            errno = E2BIG;
            return -1;
        }
    }
    *count_out = count;
    return 0;
}

static size_t
peak_exec_env_name_count(char* const envp[], const char* name)
{
    size_t count = 0;

    if (envp == NULL) {
        return 0;
    }
    for (char* const* scan = envp; *scan != NULL; scan++) {
        if (peak_exec_env_entry_matches(*scan, name)) {
            count++;
        }
    }
    return count;
}

static int
peak_exec_current_env_has_peak_entry(void)
{
    if (environ == NULL) {
        return 0;
    }
    for (char** scan = environ; *scan != NULL; scan++) {
        if (strncmp(*scan, "PEAK_", 5) == 0) {
            return 1;
        }
    }
    return 0;
}

static int
peak_exec_env_has_peak_entry(char* const envp[])
{
    if (envp == NULL) {
        return 0;
    }
    for (char* const* scan = envp; *scan != NULL; scan++) {
        if (strncmp(*scan, "PEAK_", 5) == 0) {
            return 1;
        }
    }
    return 0;
}

static char*
peak_exec_env_join_values(char* const envp[], const char* name)
{
    size_t total = 0;
    size_t value_count = 0;
    size_t out = 0;
    char* joined;

    if (envp == NULL) {
        return NULL;
    }
    for (char* const* scan = envp; *scan != NULL; scan++) {
        const char* value;

        if (!peak_exec_env_entry_matches(*scan, name)) {
            continue;
        }
        value = strchr(*scan, '=') + 1;
        if (value[0] == '\0') {
            continue;
        }
        if (peak_exec_size_add(total, strlen(value), &total) != 0) {
            return NULL;
        }
        if (value_count == SIZE_MAX) {
            errno = E2BIG;
            return NULL;
        }
        value_count++;
    }
    if (peak_exec_env_name_count(envp, name) == 0) {
        return NULL;
    }

    size_t allocation_size;
    if (peak_exec_size_add(total,
                           value_count > 0 ? value_count : 1,
                           &allocation_size) != 0) {
        return NULL;
    }
    joined = malloc(allocation_size);
    if (joined == NULL) {
        return NULL;
    }
    for (char* const* scan = envp; *scan != NULL; scan++) {
        const char* value;
        size_t len;

        if (!peak_exec_env_entry_matches(*scan, name)) {
            continue;
        }
        value = strchr(*scan, '=') + 1;
        len = strlen(value);
        if (len == 0) {
            continue;
        }
        if (out != 0) {
            if (out + 1 >= allocation_size) {
                free(joined);
                errno = E2BIG;
                return NULL;
            }
            joined[out++] = ':';
        }
        if (out >= allocation_size ||
            len >= allocation_size - out) {
            free(joined);
            errno = E2BIG;
            return NULL;
        }
        memcpy(joined + out, value, len);
        out += len;
    }
    if (out >= allocation_size) {
        free(joined);
        errno = E2BIG;
        return NULL;
    }
    joined[out] = '\0';
    return joined;
}

static int
peak_exec_secure_mode(void)
{
#if defined(PEAK_ENABLE_TEST_HOOKS)
    const char* forced_secure = getenv("PEAK_TEST_EXEC_AT_SECURE");
    if (forced_secure != NULL && !peak_exec_env_false(forced_secure)) {
        return 1;
    }
#endif
#if defined(__linux__) && defined(AT_SECURE)
    return getauxval(AT_SECURE) != 0;
#else
    return 0;
#endif
}

static char*
peak_exec_realpath_or_dup(const char* path)
{
    char resolved[PATH_MAX];

    if (path != NULL && realpath(path, resolved) != NULL) {
        return strdup(resolved);
    }
    return path != NULL ? strdup(path) : NULL;
}

static char*
peak_exec_libpeak_path(void)
{
    Dl_info info;

    memset(&info, 0, sizeof(info));
    if (dladdr((void*)&peak_runtime_is_active_for_checkpoint, &info) == 0 ||
        info.dli_fname == NULL ||
        info.dli_fname[0] == '\0') {
        return NULL;
    }
    return peak_exec_realpath_or_dup(info.dli_fname);
}

static int
peak_exec_token_is_same_file(const char* token,
                             size_t token_len,
                             const char* libpeak_path)
{
    char* token_copy;
    char* token_real;
    struct stat token_stat;
    struct stat lib_stat;
    int same = 0;

    if (token == NULL || token_len == 0 || libpeak_path == NULL) {
        return 0;
    }
    if (token_len == strlen(libpeak_path) &&
        strncmp(token, libpeak_path, token_len) == 0) {
        return 1;
    }

    token_copy = strndup(token, token_len);
    if (token_copy == NULL) {
        return 0;
    }
    token_real = peak_exec_realpath_or_dup(token_copy);
    if (token_real != NULL && strcmp(token_real, libpeak_path) == 0) {
        same = 1;
    }
    free(token_real);
    if (!same &&
        stat(token_copy, &token_stat) == 0 &&
        stat(libpeak_path, &lib_stat) == 0 &&
        token_stat.st_dev == lib_stat.st_dev &&
        token_stat.st_ino == lib_stat.st_ino) {
        same = 1;
    }
    free(token_copy);
    return same;
}

static int
peak_exec_preload_contains_libpeak(const char* value,
                                   const char* libpeak_path)
{
    const char* cursor = value;

    if (value == NULL || libpeak_path == NULL) {
        return 0;
    }
    while (*cursor != '\0') {
        const char* start;
        size_t token_len;

        while (*cursor != '\0' && peak_exec_is_separator(*cursor)) {
            cursor++;
        }
        start = cursor;
        while (*cursor != '\0' && !peak_exec_is_separator(*cursor)) {
            cursor++;
        }
        token_len = (size_t)(cursor - start);
        if (token_len != 0 &&
            peak_exec_token_is_same_file(start, token_len, libpeak_path)) {
            return 1;
        }
    }
    return 0;
}

static char*
peak_exec_build_ld_preload(const char* existing,
                           const char* libpeak_path,
                           int* changed)
{
    size_t capacity;
    size_t length;
    size_t lib_len;
    char* output;
    const char* cursor;

    if (changed == NULL || libpeak_path == NULL || libpeak_path[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    *changed = 0;
    lib_len = strlen(libpeak_path);
    size_t existing_len = existing != NULL ? strlen(existing) : 0;
    if (peak_exec_size_add(lib_len, existing_len, &capacity) != 0 ||
        peak_exec_size_add(capacity, 2, &capacity) != 0) {
        return NULL;
    }
    output = malloc(capacity);
    if (output == NULL) {
        return NULL;
    }

    memcpy(output, libpeak_path, lib_len);
    length = lib_len;
    output[length] = '\0';

    cursor = existing;
    while (cursor != NULL && *cursor != '\0') {
        const char* start;
        size_t token_len;

        while (*cursor != '\0' && peak_exec_is_separator(*cursor)) {
            cursor++;
        }
        start = cursor;
        while (*cursor != '\0' && !peak_exec_is_separator(*cursor)) {
            cursor++;
        }
        token_len = (size_t)(cursor - start);
        if (token_len == 0 ||
            peak_exec_token_is_same_file(start, token_len, libpeak_path)) {
            continue;
        }
        size_t required;
        if (peak_exec_size_add(length, token_len, &required) != 0 ||
            peak_exec_size_add(required, 2, &required) != 0) {
            free(output);
            return NULL;
        }
        if (required > capacity) {
            size_t doubled;
            size_t new_capacity;

            if (peak_exec_size_mul(capacity, 2, &doubled) != 0 ||
                peak_exec_size_add(doubled, token_len, &new_capacity) != 0 ||
                peak_exec_size_add(new_capacity, 2, &new_capacity) != 0) {
                free(output);
                return NULL;
            }
            if (new_capacity < required) {
                new_capacity = required;
            }
            char* grown = realloc(output, new_capacity);
            if (grown == NULL) {
                free(output);
                return NULL;
            }
            output = grown;
            capacity = new_capacity;
        }
        output[length++] = ':';
        memcpy(output + length, start, token_len);
        length += token_len;
        output[length] = '\0';
    }

    if (existing == NULL || strcmp(existing, output) != 0) {
        *changed = 1;
    }
    return output;
}

static int
peak_exec_add_owned(PeakExecEnv* built, char* value)
{
    if (built == NULL || value == NULL) {
        free(value);
        errno = EINVAL;
        return -1;
    }
    if (built->owned_count >= built->owned_capacity ||
        built->owned_strings == NULL) {
        free(value);
        errno = E2BIG;
        return -1;
    }
    built->owned_strings[built->owned_count++] = value;
    return 0;
}

static size_t
peak_exec_count_missing_peak_env(char* const envp[])
{
    size_t count = 0;

    if (environ == NULL) {
        return 0;
    }
    for (char** scan = environ; *scan != NULL; scan++) {
        int name_len;
        char name[256];

        if (strncmp(*scan, "PEAK_", 5) != 0) {
            continue;
        }
        name_len = peak_exec_name_len(*scan);
        if (name_len <= 0 || (size_t)name_len >= sizeof(name)) {
            continue;
        }
        memcpy(name, *scan, (size_t)name_len);
        name[name_len] = '\0';
        if (!peak_exec_env_has_name(envp, name)) {
            count++;
        }
    }
    return count;
}

static int
peak_exec_append_missing_peak_env(PeakExecEnv* built,
                                  char* const base_env[],
                                  size_t capacity,
                                  size_t* out_index)
{
    if (environ == NULL) {
        return 0;
    }
    for (char** scan = environ; *scan != NULL; scan++) {
        int name_len;
        char name[256];
        char* copy;

        if (strncmp(*scan, "PEAK_", 5) != 0) {
            continue;
        }
        name_len = peak_exec_name_len(*scan);
        if (name_len <= 0 || (size_t)name_len >= sizeof(name)) {
            continue;
        }
        memcpy(name, *scan, (size_t)name_len);
        name[name_len] = '\0';
        if (peak_exec_env_has_name(base_env, name) ||
            peak_exec_storage_has_name(built->envp, *out_index, name)) {
            continue;
        }
        copy = strdup(*scan);
        if (peak_exec_add_owned(built, copy) != 0) {
            return -1;
        }
        if (*out_index + 1 >= capacity) {
            errno = E2BIG;
            return -1;
        }
        built->envp[(*out_index)++] = copy;
        built->changed = 1;
    }
    return 0;
}

static void
peak_exec_env_clear(PeakExecEnv* built)
{
    if (built == NULL) {
        return;
    }
    for (size_t i = 0; i < built->owned_count; i++) {
        free(built->owned_strings[i]);
    }
    free(built->owned_strings);
    free(built->envp);
    memset(built, 0, sizeof(*built));
}

static int
peak_exec_should_inject_libpeak(const char* libpeak_path,
                                char* const child_envp[])
{
    const char* current_preload = getenv("LD_PRELOAD");

    if (libpeak_path == NULL || libpeak_path[0] == '\0') {
        return 0;
    }
    return peak_runtime_is_active_for_checkpoint() ||
           peak_process_requests_work() ||
           peak_exec_current_env_has_peak_entry() ||
           peak_exec_env_has_peak_entry(child_envp) ||
           peak_exec_preload_contains_libpeak(current_preload, libpeak_path);
}

static int
peak_exec_build_env(char* const envp[],
                    int chain_enabled,
                    int propagate_peak_env,
                    PeakExecEnv* built)
{
    char* const* base_env = envp;
    size_t base_count = 0;
    size_t ld_entry_count;
    size_t missing_peak_count = 0;
    size_t extra_count;
    size_t env_capacity;
    size_t owned_capacity;
    size_t out = 0;
    int secure;
    int should_touch_ld = 0;
    int new_ld_changed = 0;
    int libpeak_already_present = 0;
    int add_ld_library_path = 0;
    int ld_written = 0;
    char* libpeak_path = NULL;
    char* existing_ld_joined = NULL;
    char* new_ld_value = NULL;

    memset(built, 0, sizeof(*built));
#if defined(PEAK_ENABLE_TEST_HOOKS)
    const char* forced_build_fail = getenv("PEAK_TEST_EXEC_ENV_BUILD_FAIL");
    if (forced_build_fail != NULL &&
        !peak_exec_env_false(forced_build_fail)) {
        errno = ENOMEM;
        return -1;
    }
#endif
    if (!chain_enabled) {
        return 0;
    }
    if (peak_exec_env_count_bounded(base_env, &base_count) != 0) {
        return -1;
    }
    ld_entry_count = peak_exec_env_name_count(base_env, "LD_PRELOAD");
    secure = peak_exec_secure_mode();
    libpeak_path = peak_exec_libpeak_path();

    if (!secure && peak_exec_should_inject_libpeak(libpeak_path, envp)) {
        should_touch_ld = 1;
        existing_ld_joined = peak_exec_env_join_values(base_env, "LD_PRELOAD");
        if (ld_entry_count > 0 && existing_ld_joined == NULL) {
            free(libpeak_path);
            errno = ENOMEM;
            return -1;
        }
        libpeak_already_present = peak_exec_preload_contains_libpeak(
            existing_ld_joined, libpeak_path);
        new_ld_value = peak_exec_build_ld_preload(existing_ld_joined,
                                                  libpeak_path,
                                                  &new_ld_changed);
        if (new_ld_value == NULL) {
            free(existing_ld_joined);
            free(libpeak_path);
            return -1;
        }
    }
    if (propagate_peak_env) {
        missing_peak_count = peak_exec_count_missing_peak_env(base_env);
    }

    if (new_ld_changed && !libpeak_already_present &&
        !peak_exec_env_has_name(base_env, "LD_LIBRARY_PATH")) {
        if (!peak_exec_cached_ld_library_path_ready ||
            peak_exec_cached_ld_library_path_failed) {
            free(new_ld_value);
            free(existing_ld_joined);
            free(libpeak_path);
            errno = ENOMEM;
            return -1;
        }
        add_ld_library_path = peak_exec_cached_ld_library_path != NULL;
    }

    if (!new_ld_changed && !add_ld_library_path && missing_peak_count == 0 &&
        ld_entry_count <= 1) {
        free(new_ld_value);
        free(existing_ld_joined);
        free(libpeak_path);
        return 0;
    }

    if (peak_exec_size_add(
            missing_peak_count,
            ((should_touch_ld && ld_entry_count == 0) ? 1 : 0) +
                (add_ld_library_path ? 1 : 0),
            &extra_count) != 0 ||
        peak_exec_size_add(base_count, extra_count, &env_capacity) != 0 ||
        peak_exec_size_add(env_capacity, 1, &env_capacity) != 0) {
        free(new_ld_value);
        free(existing_ld_joined);
        free(libpeak_path);
        return -1;
    }
    built->envp = calloc(env_capacity, sizeof(char*));
    if (built->envp == NULL) {
        free(new_ld_value);
        free(existing_ld_joined);
        free(libpeak_path);
        return -1;
    }
    if (peak_exec_size_add(missing_peak_count,
                           (should_touch_ld && new_ld_changed) ? 1 : 0,
                           &owned_capacity) != 0) {
        free(new_ld_value);
        free(existing_ld_joined);
        free(libpeak_path);
        return -1;
    }
    if (owned_capacity != 0) {
        built->owned_strings = calloc(owned_capacity, sizeof(char*));
        if (built->owned_strings == NULL) {
            free(new_ld_value);
            free(existing_ld_joined);
            free(libpeak_path);
            return -1;
        }
        built->owned_capacity = owned_capacity;
    }

    for (size_t i = 0; i < base_count; i++) {
        if (should_touch_ld &&
            peak_exec_env_entry_matches(base_env[i], "LD_PRELOAD")) {
            if (ld_written) {
                built->changed = 1;
                continue;
            }
            if (new_ld_changed) {
                size_t value_len = strlen(new_ld_value);
                size_t entry_size;
                char* entry;

                if (peak_exec_size_add(strlen("LD_PRELOAD="),
                                       value_len,
                                       &entry_size) != 0 ||
                    peak_exec_size_add(entry_size, 1, &entry_size) != 0) {
                    free(new_ld_value);
                    free(existing_ld_joined);
                    free(libpeak_path);
                    return -1;
                }
                entry = malloc(entry_size);
                if (entry == NULL) {
                    free(new_ld_value);
                    free(existing_ld_joined);
                    free(libpeak_path);
                    return -1;
                }
                sprintf(entry, "LD_PRELOAD=%s", new_ld_value);
                if (out + 1 >= env_capacity) {
                    free(entry);
                    free(new_ld_value);
                    free(existing_ld_joined);
                    free(libpeak_path);
                    errno = E2BIG;
                    return -1;
                }
                if (peak_exec_add_owned(built, entry) != 0) {
                    free(new_ld_value);
                    free(existing_ld_joined);
                    free(libpeak_path);
                    return -1;
                }
                built->envp[out++] = entry;
                built->changed = 1;
            } else {
                if (out + 1 >= env_capacity) {
                    free(new_ld_value);
                    free(existing_ld_joined);
                    free(libpeak_path);
                    errno = E2BIG;
                    return -1;
                }
                built->envp[out++] = base_env[i];
            }
            ld_written = 1;
        } else {
            if (out + 1 >= env_capacity) {
                free(new_ld_value);
                free(existing_ld_joined);
                free(libpeak_path);
                errno = E2BIG;
                return -1;
            }
            built->envp[out++] = base_env[i];
        }
    }

    if (should_touch_ld && ld_entry_count == 0) {
        size_t value_len = strlen(new_ld_value);
        size_t entry_size;
        char* entry;

        if (peak_exec_size_add(strlen("LD_PRELOAD="),
                               value_len,
                               &entry_size) != 0 ||
            peak_exec_size_add(entry_size, 1, &entry_size) != 0) {
            free(new_ld_value);
            free(existing_ld_joined);
            free(libpeak_path);
            return -1;
        }
        entry = malloc(entry_size);
        if (entry == NULL) {
            free(new_ld_value);
            free(existing_ld_joined);
            free(libpeak_path);
            return -1;
        }
        sprintf(entry, "LD_PRELOAD=%s", new_ld_value);
        if (out + 1 >= env_capacity) {
            free(entry);
            free(new_ld_value);
            free(existing_ld_joined);
            free(libpeak_path);
            errno = E2BIG;
            return -1;
        }
        if (peak_exec_add_owned(built, entry) != 0) {
            free(new_ld_value);
            free(existing_ld_joined);
            free(libpeak_path);
            return -1;
        }
        built->envp[out++] = entry;
        built->changed = 1;
    }

    if (add_ld_library_path) {
        if (out + 1 >= env_capacity) {
            free(new_ld_value);
            free(existing_ld_joined);
            free(libpeak_path);
            errno = E2BIG;
            return -1;
        }
        built->envp[out++] = peak_exec_cached_ld_library_path;
        built->changed = 1;
    }

    if (propagate_peak_env &&
        peak_exec_append_missing_peak_env(built,
                                          base_env,
                                          env_capacity,
                                          &out) != 0) {
        free(new_ld_value);
        free(existing_ld_joined);
        free(libpeak_path);
        return -1;
    }

    if (out >= env_capacity) {
        free(new_ld_value);
        free(existing_ld_joined);
        free(libpeak_path);
        errno = E2BIG;
        return -1;
    }
    built->envp[out] = NULL;
    free(new_ld_value);
    free(existing_ld_joined);
    free(libpeak_path);
    return 0;
}

static char* const*
peak_exec_env_to_use(PeakExecEnv* built_env, char* const original_envp[])
{
    return built_env != NULL && built_env->envp != NULL ?
        built_env->envp : original_envp;
}

static int
peak_exec_prepare(const char* path,
                  char* const argv[],
                  char* const envp[],
                  PeakExecEnv* built_env,
                  int allow_checkpoint)
{
    int saved_errno = errno;
    int chain_enabled = peak_exec_startup_chain_enabled &&
                        peak_exec_env_default_true_for_child(
                            envp, PEAK_EXEC_CHAIN_ENV);
    int propagate_peak_env =
        peak_exec_env_default_true_for_child(
            envp,
            PEAK_EXEC_PROPAGATE_PEAK_ENV_ENV);
    memset(built_env, 0, sizeof(*built_env));

    peak_exec_trace_event("exec-before", path, "", 0);
    if (allow_checkpoint &&
        peak_exec_spawn_depth == 0 &&
        peak_exec_startup_checkpoint_enabled &&
        peak_exec_env_default_true_for_child(envp,
                                             PEAK_EXEC_CHECKPOINT_ENV)) {
        int checkpoint_result = peak_checkpoint_for_exec(path, argv);
        int checkpoint_errno = errno;
        peak_exec_trace_event(checkpoint_result == 0 ?
                                  "exec-checkpoint-ok" :
                                  "exec-checkpoint-skipped",
                              path,
                              "",
                              checkpoint_result == 0 ? 0 : checkpoint_errno);
        errno = saved_errno;
    }

    if (peak_exec_build_env(envp,
                            chain_enabled,
                            propagate_peak_env,
                            built_env) != 0) {
        int build_errno = errno;
        peak_exec_trace_event("exec-env-build-failed",
                              path,
                              "using-original-env",
                              build_errno);
        errno = build_errno;
        return -1;
    }
    peak_exec_trace_event(built_env->changed ? "exec-env-injected" :
                                               "exec-env-unchanged",
                          path,
                          "",
                          0);
    errno = saved_errno;
    return 0;
}

static pthread_once_t peak_real_execve_once = PTHREAD_ONCE_INIT;
static pthread_once_t peak_real_execvp_once = PTHREAD_ONCE_INIT;
static pthread_once_t peak_real_execvpe_once = PTHREAD_ONCE_INIT;
static pthread_once_t peak_real_fexecve_once = PTHREAD_ONCE_INIT;
static pthread_once_t peak_real_execveat_once = PTHREAD_ONCE_INIT;
static peak_execve_fn peak_real_execve_ptr;
static peak_execvp_fn peak_real_execvp_ptr;
static peak_execvpe_fn peak_real_execvpe_ptr;
static peak_fexecve_fn peak_real_fexecve_ptr;
static peak_execveat_fn peak_real_execveat_ptr;
static peak_posix_spawn_fn peak_real_posix_spawn_ptr;
static peak_posix_spawnp_fn peak_real_posix_spawnp_ptr;
enum {
    PEAK_SPAWN_RESOLVER_UNRESOLVED = 0,
    PEAK_SPAWN_RESOLVER_IN_PROGRESS = 1,
    PEAK_SPAWN_RESOLVER_READY = 2,
};
static atomic_int peak_real_posix_spawn_state = ATOMIC_VAR_INIT(0);
static atomic_int peak_real_posix_spawnp_state = ATOMIC_VAR_INIT(0);
static atomic_long peak_real_posix_spawn_owner_pid = ATOMIC_VAR_INIT(0);
static atomic_long peak_real_posix_spawnp_owner_pid = ATOMIC_VAR_INIT(0);
static __thread int peak_posix_spawn_resolving;

static void peak_lookup_execve(void)
{
#if defined(PEAK_ENABLE_TEST_HOOKS)
    if (peak_test_exec_resolver_reentry_hook != NULL) {
        peak_test_exec_resolver_reentry_hook();
    }
#endif
    peak_real_execve_ptr = (peak_execve_fn)dlsym(RTLD_NEXT, "execve");
}

static void peak_lookup_execvp(void)
{
    peak_real_execvp_ptr = (peak_execvp_fn)dlsym(RTLD_NEXT, "execvp");
}

static void peak_lookup_execvpe(void)
{
    peak_real_execvpe_ptr = (peak_execvpe_fn)dlsym(RTLD_NEXT, "execvpe");
}

static void peak_lookup_fexecve(void)
{
    peak_real_fexecve_ptr = (peak_fexecve_fn)dlsym(RTLD_NEXT, "fexecve");
}

static void peak_lookup_execveat(void)
{
    peak_real_execveat_ptr = (peak_execveat_fn)dlsym(RTLD_NEXT, "execveat");
}

static peak_posix_spawn_fn
peak_lookup_posix_spawn(void)
{
#if defined(PEAK_ENABLE_TEST_HOOKS)
    extern void peak_test_exec_spawn_resolver_reentry_hook(int)
        __attribute__((weak));

    if (peak_test_exec_spawn_resolver_reentry_hook != NULL) {
        peak_test_exec_spawn_resolver_reentry_hook(0);
    }
#endif
    return (peak_posix_spawn_fn)dlsym(RTLD_NEXT, "posix_spawn");
}

static peak_posix_spawnp_fn
peak_lookup_posix_spawnp(void)
{
#if defined(PEAK_ENABLE_TEST_HOOKS)
    extern void peak_test_exec_spawn_resolver_reentry_hook(int)
        __attribute__((weak));

    if (peak_test_exec_spawn_resolver_reentry_hook != NULL) {
        peak_test_exec_spawn_resolver_reentry_hook(1);
    }
#endif
    return (peak_posix_spawnp_fn)dlsym(RTLD_NEXT, "posix_spawnp");
}

static peak_execve_fn
peak_real_execve(void)
{
    (void)pthread_once(&peak_real_execve_once, peak_lookup_execve);
    return peak_real_execve_ptr;
}

static peak_execvp_fn
peak_real_execvp(void)
{
    (void)pthread_once(&peak_real_execvp_once, peak_lookup_execvp);
    return peak_real_execvp_ptr;
}

static peak_execvpe_fn
peak_real_execvpe(void)
{
    (void)pthread_once(&peak_real_execvpe_once, peak_lookup_execvpe);
#if defined(PEAK_ENABLE_TEST_HOOKS)
    if (peak_exec_test_force_execvpe_fallback) {
        return NULL;
    }
#endif
    return peak_real_execvpe_ptr;
}

static peak_fexecve_fn
peak_real_fexecve(void)
{
    (void)pthread_once(&peak_real_fexecve_once, peak_lookup_fexecve);
    return peak_real_fexecve_ptr;
}

static peak_execveat_fn
peak_real_execveat(void)
{
    (void)pthread_once(&peak_real_execveat_once, peak_lookup_execveat);
    return peak_real_execveat_ptr;
}

static peak_posix_spawn_fn
peak_real_posix_spawn(void)
{
    peak_posix_spawn_fn resolved;
    const long self = (long)getpid();

    for (;;) {
        int state = atomic_load_explicit(&peak_real_posix_spawn_state,
                                         memory_order_acquire);
        long owner;

        if (state == PEAK_SPAWN_RESOLVER_READY) {
            return peak_real_posix_spawn_ptr;
        }
        if (peak_posix_spawn_resolving) {
            return NULL;
        }
        owner = atomic_load_explicit(&peak_real_posix_spawn_owner_pid,
                                     memory_order_acquire);
        if (owner == 0) {
            long expected = 0;

            peak_posix_spawn_resolving = 1;
            if (atomic_compare_exchange_strong_explicit(
                    &peak_real_posix_spawn_owner_pid,
                    &expected,
                    self,
                    memory_order_acq_rel,
                    memory_order_acquire)) {
                atomic_store_explicit(&peak_real_posix_spawn_state,
                                      PEAK_SPAWN_RESOLVER_IN_PROGRESS,
                                      memory_order_release);
                resolved = peak_lookup_posix_spawn();
                peak_real_posix_spawn_ptr = resolved;
#if defined(PEAK_ENABLE_TEST_HOOKS)
                {
                    extern void peak_test_exec_spawn_publication_hook(int)
                        __attribute__((weak));

                    if (peak_test_exec_spawn_publication_hook != NULL) {
                        peak_test_exec_spawn_publication_hook(0);
                    }
                }
#endif
                peak_posix_spawn_resolving = 0;
                atomic_store_explicit(&peak_real_posix_spawn_state,
                                      PEAK_SPAWN_RESOLVER_READY,
                                      memory_order_release);
                return resolved;
            }
            peak_posix_spawn_resolving = 0;
            continue;
        }
        if (owner != self) {
            return NULL;
        }
#if defined(PEAK_ENABLE_TEST_HOOKS)
        {
            extern void peak_test_exec_spawn_waiter_hook(int)
                __attribute__((weak));

            if (peak_test_exec_spawn_waiter_hook != NULL) {
                peak_test_exec_spawn_waiter_hook(0);
            }
        }
#endif
        do {
            state = atomic_load_explicit(&peak_real_posix_spawn_state,
                                         memory_order_acquire);
            if (state == PEAK_SPAWN_RESOLVER_READY) {
                return peak_real_posix_spawn_ptr;
            }
            if (peak_posix_spawn_resolving ||
                atomic_load_explicit(&peak_real_posix_spawn_owner_pid,
                                     memory_order_acquire) != self) {
                return NULL;
            }
        } while (state != PEAK_SPAWN_RESOLVER_READY);
    }
}

static peak_posix_spawnp_fn
peak_real_posix_spawnp(void)
{
    peak_posix_spawnp_fn resolved;
    const long self = (long)getpid();

    for (;;) {
        int state = atomic_load_explicit(&peak_real_posix_spawnp_state,
                                         memory_order_acquire);
        long owner;

        if (state == PEAK_SPAWN_RESOLVER_READY) {
            return peak_real_posix_spawnp_ptr;
        }
        if (peak_posix_spawn_resolving) {
            return NULL;
        }
        owner = atomic_load_explicit(&peak_real_posix_spawnp_owner_pid,
                                     memory_order_acquire);
        if (owner == 0) {
            long expected = 0;

            peak_posix_spawn_resolving = 1;
            if (atomic_compare_exchange_strong_explicit(
                    &peak_real_posix_spawnp_owner_pid,
                    &expected,
                    self,
                    memory_order_acq_rel,
                    memory_order_acquire)) {
                atomic_store_explicit(&peak_real_posix_spawnp_state,
                                      PEAK_SPAWN_RESOLVER_IN_PROGRESS,
                                      memory_order_release);
                resolved = peak_lookup_posix_spawnp();
                peak_real_posix_spawnp_ptr = resolved;
#if defined(PEAK_ENABLE_TEST_HOOKS)
                {
                    extern void peak_test_exec_spawn_publication_hook(int)
                        __attribute__((weak));

                    if (peak_test_exec_spawn_publication_hook != NULL) {
                        peak_test_exec_spawn_publication_hook(1);
                    }
                }
#endif
                peak_posix_spawn_resolving = 0;
                atomic_store_explicit(&peak_real_posix_spawnp_state,
                                      PEAK_SPAWN_RESOLVER_READY,
                                      memory_order_release);
                return resolved;
            }
            peak_posix_spawn_resolving = 0;
            continue;
        }
        if (owner != self) {
            return NULL;
        }
#if defined(PEAK_ENABLE_TEST_HOOKS)
        {
            extern void peak_test_exec_spawn_waiter_hook(int)
                __attribute__((weak));

            if (peak_test_exec_spawn_waiter_hook != NULL) {
                peak_test_exec_spawn_waiter_hook(1);
            }
        }
#endif
        do {
            state = atomic_load_explicit(&peak_real_posix_spawnp_state,
                                         memory_order_acquire);
            if (state == PEAK_SPAWN_RESOLVER_READY) {
                return peak_real_posix_spawnp_ptr;
            }
            if (peak_posix_spawn_resolving ||
                atomic_load_explicit(&peak_real_posix_spawnp_owner_pid,
                                     memory_order_acquire) != self) {
                return NULL;
            }
        } while (state != PEAK_SPAWN_RESOLVER_READY);
    }
}

static void
peak_exec_cache_libpeak_path(void)
{
    char* libpeak_path = peak_exec_libpeak_path();
    size_t length;

    if (libpeak_path == NULL) {
        return;
    }
    length = strlen(libpeak_path);
    if (length != 0 && length < sizeof(peak_exec_cached_libpeak_path)) {
        memcpy(peak_exec_cached_libpeak_path, libpeak_path, length + 1);
        peak_exec_cached_libpeak_path_ready = 1;
    }
    free(libpeak_path);
}

static void
peak_exec_cache_ld_library_path(void)
{
    const char* value = getenv("LD_LIBRARY_PATH");
    const size_t prefix_length = sizeof("LD_LIBRARY_PATH=") - 1;
    size_t value_length;
    char* entry;

    if (value == NULL || value[0] == '\0') {
        peak_exec_cached_ld_library_path_ready = 1;
        return;
    }
    value_length = strlen(value);
    if (value_length > SIZE_MAX - prefix_length - 1) {
        peak_exec_cached_ld_library_path_failed = 1;
        return;
    }
    entry = malloc(prefix_length + value_length + 1);
    if (entry == NULL) {
        peak_exec_cached_ld_library_path_failed = 1;
        return;
    }
    memcpy(entry, "LD_LIBRARY_PATH=", prefix_length);
    memcpy(entry + prefix_length, value, value_length + 1);
    peak_exec_cached_ld_library_path = entry;
    peak_exec_cached_ld_library_path_ready = 1;
}

static void
peak_exec_cache_secure_mode(void)
{
    peak_exec_cached_secure_mode = peak_exec_secure_mode();
    peak_exec_cached_secure_mode_ready = 1;
}

__attribute__((constructor))
static void
peak_exec_prime_real_symbols(void)
{
    (void)peak_real_execve();
    (void)peak_real_execvp();
    (void)peak_real_execvpe();
    (void)peak_real_fexecve();
    (void)peak_real_execveat();
    (void)peak_real_posix_spawn();
    (void)peak_real_posix_spawnp();
    peak_exec_owner_pid = getpid();
    peak_exec_cache_libpeak_path();
    peak_exec_cache_ld_library_path();
    peak_exec_cache_secure_mode();
#if defined(PEAK_ENABLE_TEST_HOOKS)
    {
        const char* forced_fallback = getenv("PEAK_TEST_EXECVPE_FALLBACK");

        peak_exec_test_force_execvpe_fallback =
            forced_fallback != NULL && !peak_exec_env_false(forced_fallback);
    }
#endif
    peak_exec_startup_chain_enabled =
        getenv(PEAK_EXEC_CHAIN_ENV) != NULL &&
        !peak_exec_env_false(getenv(PEAK_EXEC_CHAIN_ENV));
    peak_exec_startup_checkpoint_enabled =
        getenv(PEAK_EXEC_CHECKPOINT_ENV) != NULL &&
        !peak_exec_env_false(getenv(PEAK_EXEC_CHECKPOINT_ENV));
    peak_exec_startup_policy_enabled = peak_exec_startup_chain_enabled ||
                                       peak_exec_startup_checkpoint_enabled;
    atomic_store_explicit(&peak_exec_priming_complete,
                          1,
                          memory_order_release);
}

static int
peak_call_real_execve(const char* path,
                      char* const argv[],
                      char* const envp[])
{
    peak_execve_fn fn = peak_real_execve();

    if (fn == NULL) {
#if defined(__linux__) && defined(SYS_execve)
        return (int)peak_exec_raw_syscall6(
            SYS_execve,
            (long)(uintptr_t)path,
            (long)(uintptr_t)argv,
            (long)(uintptr_t)envp,
            0,
            0,
            0);
#else
        errno = ENOSYS;
        return -1;
#endif
    }
    return fn(path, argv, envp);
}

#if !PEAK_EXEC_DIRECT_RAW_SUPPORTED
static int
peak_call_real_execvp(const char* file, char* const argv[])
{
    peak_execvp_fn fn = peak_real_execvp();

    if (fn == NULL) {
        return peak_exec_prepublication_path_search(file, argv, environ);
    }
    return fn(file, argv);
}

static int
peak_call_real_execvpe(const char* file,
                       char* const argv[],
                       char* const envp[])
{
    peak_execvpe_fn fn = peak_real_execvpe();

    if (fn == NULL) {
        return peak_exec_prepublication_path_search(file, argv, envp);
    }
    return fn(file, argv, envp);
}
#endif

static int
peak_call_primed_execve(const char* path,
                        char* const argv[],
                        char* const envp[])
{
    if (peak_real_execve_ptr == NULL) {
#if defined(__linux__) && defined(SYS_execve)
        return (int)peak_exec_raw_syscall6(
            SYS_execve,
            (long)(uintptr_t)path,
            (long)(uintptr_t)argv,
            (long)(uintptr_t)envp,
            0,
            0,
            0);
#else
        errno = ENOSYS;
        return -1;
#endif
    }
    return peak_real_execve_ptr(path, argv, envp);
}

static int
peak_call_primed_execvp(const char* file, char* const argv[])
{
    if (peak_real_execvp_ptr == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return peak_real_execvp_ptr(file, argv);
}

static int
peak_call_primed_execvpe(const char* file,
                         char* const argv[],
                         char* const envp[])
{
#if defined(PEAK_ENABLE_TEST_HOOKS)
    if (peak_exec_test_force_execvpe_fallback) {
        errno = ENOSYS;
        return -1;
    }
#endif
    if (peak_real_execvpe_ptr == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return peak_real_execvpe_ptr(file, argv, envp);
}

static peak_execvpe_fn
peak_primed_execvpe_for_rich_path(void)
{
#if defined(PEAK_ENABLE_TEST_HOOKS)
    if (peak_exec_test_force_execvpe_fallback) {
        return NULL;
    }
#endif
    return peak_real_execvpe_ptr;
}

static int
peak_call_real_fexecve(int fd, char* const argv[], char* const envp[])
{
    peak_fexecve_fn fn = peak_real_fexecve();

    if (fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return fn(fd, argv, envp);
}

static int
peak_call_primed_fexecve(int fd, char* const argv[], char* const envp[])
{
    if (peak_real_fexecve_ptr == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return peak_real_fexecve_ptr(fd, argv, envp);
}

#if defined(__linux__)
static int
peak_call_real_execveat(int dirfd,
                        const char* pathname,
                        char* const argv[],
                        char* const envp[],
                        int flags)
{
    peak_execveat_fn fn = peak_real_execveat();

    if (fn != NULL) {
        return fn(dirfd, pathname, argv, envp, flags);
    }
#if defined(SYS_execveat)
    return (int)peak_exec_raw_syscall6(
        SYS_execveat,
        dirfd,
        (long)(uintptr_t)pathname,
        (long)(uintptr_t)argv,
        (long)(uintptr_t)envp,
        flags,
        0);
#else
    errno = ENOSYS;
    return -1;
#endif
}

static int
peak_call_primed_execveat(int dirfd,
                          const char* pathname,
                          char* const argv[],
                          char* const envp[],
                          int flags)
{
    if (peak_real_execveat_ptr != NULL) {
        return peak_real_execveat_ptr(dirfd, pathname, argv, envp, flags);
    }
#if defined(SYS_execveat)
    return (int)peak_exec_raw_syscall6(
        SYS_execveat,
        dirfd,
        (long)(uintptr_t)pathname,
        (long)(uintptr_t)argv,
        (long)(uintptr_t)envp,
        flags,
        0);
#else
    errno = ENOSYS;
    return -1;
#endif
}
#endif

PEAK_EXEC_API int
execve(const char* path, char* const argv[], char* const envp[])
{
    if (!peak_exec_priming_is_complete()) {
        return peak_exec_prepublication_execve(path, argv, envp);
    }
    if (!peak_exec_startup_policy_enabled) {
        return peak_call_primed_execve(path, argv, envp);
    }

    PeakExecEnv built_env = {0};
    char* const* exec_envp;
    int saved_errno;
    int entry_errno = errno;

    if (peak_exec_in_fork_child()) {
        char* child_env[PEAK_EXEC_CHILD_ENV_SLOTS];
        char child_ld_preload[PEAK_EXEC_CHILD_LD_PRELOAD_CAPACITY];
        char* const* child_envp = peak_exec_child_build_env(envp,
                                                             child_env,
                                                             child_ld_preload);

        return peak_call_primed_execve(
            path, argv, (char* const*)child_envp);
    }
    if (peak_exec_wrapper_depth > 0 || peak_exec_spawn_depth > 0) {
        return peak_call_real_execve(path, argv, envp);
    }
    if (peak_exec_args_readable(path, argv, envp) !=
        PEAK_EXEC_PREFLIGHT_VALID) {
        return peak_call_real_execve(path, argv, envp);
    }

    peak_exec_wrapper_depth++;
    if (peak_exec_prepare(path, argv, envp, &built_env, 1) != 0) {
        exec_envp = envp;
        errno = entry_errno;
    } else {
        exec_envp = peak_exec_env_to_use(&built_env, envp);
    }
    (void)peak_call_real_execve(path, argv, (char* const*)exec_envp);
    saved_errno = errno;
    peak_exec_env_clear(&built_env);
    peak_exec_wrapper_depth--;
    errno = saved_errno;
    return -1;
}

int
peak_exec_raw_syscall_execve(const char* path,
                             char* const argv[],
                             char* const envp[])
{
    if (!peak_exec_priming_is_complete() ||
        !peak_exec_startup_policy_enabled) {
        return peak_exec_prepublication_execve(path, argv, envp);
    }
    return execve(path, argv, envp);
}

PEAK_EXEC_API int
execv(const char* path, char* const argv[])
{
    if (!peak_exec_priming_is_complete()) {
        return peak_exec_prepublication_execve(path, argv, environ);
    }
    if (!peak_exec_startup_policy_enabled) {
        return peak_call_primed_execve(path, argv, environ);
    }
    return execve(path, argv, environ);
}

static char**
peak_exec_build_shell_argv(const char* file, char* const argv[])
{
    size_t argc = 0;
    size_t shell_argc;
    char** shell_argv;

    if (argv != NULL) {
        while (argv[argc] != NULL) {
            argc++;
            if (argc >= PEAK_EXEC_MAX_VARARGS) {
                errno = E2BIG;
                return NULL;
            }
        }
    }

    shell_argc = argc > 0 ? argc + 1 : 2;
    shell_argv = calloc(shell_argc + 1, sizeof(char*));
    if (shell_argv == NULL) {
        return NULL;
    }
    shell_argv[0] = (char*)"/bin/sh";
    shell_argv[1] = (char*)file;
    for (size_t i = 1; i < argc; i++) {
        shell_argv[i + 1] = argv[i];
    }
    shell_argv[shell_argc] = NULL;
    return shell_argv;
}

static int
peak_exec_enoexec_shell_fallback(const char* file,
                                 char* const argv[],
                                 char* const envp[])
{
    char** shell_argv = peak_exec_build_shell_argv(file, argv);
    int saved_errno;

    if (shell_argv == NULL) {
        return -1;
    }
    (void)peak_call_real_execve("/bin/sh", shell_argv, envp);
    saved_errno = errno;
    free(shell_argv);
    errno = saved_errno;
    return -1;
}

static int
peak_execvpe_fallback(const char* file,
                      char* const argv[],
                      char* const envp[])
{
    PeakExecEnv built_env = {0};
    char* const* exec_envp;
    const char* path_value;
    int saved_errno = ENOENT;
    int saw_eacces = 0;
    int entry_errno = errno;

    if (file == NULL || file[0] == '\0') {
        errno = ENOENT;
        return -1;
    }

    peak_exec_wrapper_depth++;
    if (peak_exec_prepare(file, argv, envp, &built_env, 1) != 0) {
        exec_envp = envp;
        errno = entry_errno;
    } else {
        exec_envp = peak_exec_env_to_use(&built_env, envp);
    }

    if (strchr(file, '/') != NULL) {
        (void)peak_call_real_execve(file, argv, (char* const*)exec_envp);
        saved_errno = errno;
        if (saved_errno == ENOEXEC) {
            (void)peak_exec_enoexec_shell_fallback(file,
                                                   argv,
                                                   (char* const*)exec_envp);
            saved_errno = errno;
        }
        goto out;
    }

    path_value = getenv("PATH");
    if (path_value == NULL) {
        path_value = PEAK_EXEC_DEFAULT_PATH;
    }

    const char* cursor = path_value;
    while (1) {
        const char* start = cursor;
        size_t dir_len;
        size_t file_len = strlen(file);
        size_t candidate_len;
        char* candidate;

        while (*cursor != '\0' && *cursor != ':') {
            cursor++;
        }
        dir_len = (size_t)(cursor - start);
        candidate_len = dir_len + (dir_len > 0 ? 1 : 0) + file_len;
        if (candidate_len + 1 > PATH_MAX) {
            saved_errno = ENAMETOOLONG;
            if (*cursor == '\0') {
                break;
            }
            cursor++;
            continue;
        }
        candidate = malloc(candidate_len + 1);
        if (candidate == NULL) {
            saved_errno = errno;
            break;
        }
        if (dir_len == 0) {
            memcpy(candidate, file, file_len + 1);
        } else {
            memcpy(candidate, start, dir_len);
            candidate[dir_len] = '/';
            memcpy(candidate + dir_len + 1, file, file_len + 1);
        }

        (void)peak_call_real_execve(candidate, argv, (char* const*)exec_envp);
        saved_errno = errno;
        if (saved_errno == ENOEXEC) {
            (void)peak_exec_enoexec_shell_fallback(
                candidate,
                argv,
                (char* const*)exec_envp);
            saved_errno = errno;
        }
        free(candidate);

        if (saved_errno == EACCES) {
            saw_eacces = 1;
        } else if (saved_errno != ENOENT && saved_errno != ENOTDIR &&
                   saved_errno != ESTALE && saved_errno != ENODEV &&
                   saved_errno != ETIMEDOUT) {
            break;
        }
        if (*cursor == '\0') {
            saved_errno = saw_eacces ? EACCES : saved_errno;
            break;
        }
        cursor++;
    }

out:
    peak_exec_env_clear(&built_env);
    peak_exec_wrapper_depth--;
    errno = saved_errno != 0 ? saved_errno : entry_errno;
    return -1;
}

PEAK_EXEC_API int
execvpe(const char* file, char* const argv[], char* const envp[])
{
    if (!peak_exec_priming_is_complete()) {
        return peak_exec_prepublication_execvpe(file, argv, envp);
    }
    if (!peak_exec_startup_policy_enabled) {
        return peak_call_primed_execvpe(file, argv, envp);
    }

    peak_execvpe_fn fn;
    PeakExecEnv built_env = {0};
    char* const* exec_envp;
    int saved_errno;
    int entry_errno = errno;

    if (peak_exec_in_fork_child()) {
        char* child_env[PEAK_EXEC_CHILD_ENV_SLOTS];
        char child_ld_preload[PEAK_EXEC_CHILD_LD_PRELOAD_CAPACITY];
        char* const* child_envp = peak_exec_child_build_env(envp,
                                                             child_env,
                                                             child_ld_preload);

        fn = peak_primed_execvpe_for_rich_path();
        if (fn == NULL) {
            errno = ENOSYS;
            return -1;
        }
        return fn(file, argv, (char* const*)child_envp);
    }
    if (peak_exec_wrapper_depth > 0 || peak_exec_spawn_depth > 0) {
        fn = peak_real_execvpe();
        if (fn != NULL) {
            return fn(file, argv, envp);
        }
        return peak_execvpe_fallback(file, argv, envp);
    }

    fn = peak_real_execvpe();
    PeakExecPreflightResult preflight_result =
        peak_exec_args_readable(file, argv, envp);
    if (preflight_result != PEAK_EXEC_PREFLIGHT_VALID) {
        int preflight_errno = errno;

        if (fn != NULL) {
            return fn(file, argv, envp);
        }
        errno = preflight_result == PEAK_EXEC_PREFLIGHT_INVALID ?
            preflight_errno : ENOSYS;
        return -1;
    }
    if (fn == NULL) {
        return peak_execvpe_fallback(file, argv, envp);
    }

    peak_exec_wrapper_depth++;
    if (peak_exec_prepare(file, argv, envp, &built_env, 1) != 0) {
        exec_envp = envp;
        errno = entry_errno;
    } else {
        exec_envp = peak_exec_env_to_use(&built_env, envp);
    }
    (void)fn(file, argv, (char* const*)exec_envp);
    saved_errno = errno;
    peak_exec_env_clear(&built_env);
    peak_exec_wrapper_depth--;
    errno = saved_errno;
    return -1;
}

PEAK_EXEC_API int
execvp(const char* file, char* const argv[])
{
    if (!peak_exec_priming_is_complete()) {
        return peak_exec_prepublication_execvp(file, argv);
    }
    if (!peak_exec_startup_policy_enabled) {
        return peak_call_primed_execvp(file, argv);
    }
    if (peak_exec_in_fork_child()) {
        char* child_env[PEAK_EXEC_CHILD_ENV_SLOTS];
        char child_ld_preload[PEAK_EXEC_CHILD_LD_PRELOAD_CAPACITY];
        char* const* child_envp = peak_exec_child_build_env(
            environ, child_env, child_ld_preload);
        peak_execvpe_fn execvpe_fn = peak_primed_execvpe_for_rich_path();

        if (child_envp != environ && execvpe_fn != NULL) {
            return execvpe_fn(file, argv, (char* const*)child_envp);
        }
        return peak_call_primed_execvp(file, argv);
    }
    return execvpe(file, argv, environ);
}

static int
peak_exec_collect_argv(const char* arg, va_list* ap, char*** out_argv)
{
    va_list count_ap;
    const char* value;
    size_t argc = 0;
    char** argv;

    if (arg != NULL) {
        argc = 1;
        va_copy(count_ap, *ap);
        while ((value = va_arg(count_ap, const char*)) != NULL) {
            (void)value;
            argc++;
            if (argc >= PEAK_EXEC_MAX_VARARGS) {
                va_end(count_ap);
                errno = E2BIG;
                return -1;
            }
        }
        va_end(count_ap);
    }

    argv = calloc(argc + 1, sizeof(char*));
    if (argv == NULL) {
        return -1;
    }
    if (arg != NULL) {
        argv[0] = (char*)arg;
        for (size_t i = 1; i < argc; i++) {
            argv[i] = va_arg(*ap, char*);
        }
        (void)va_arg(*ap, char*);
    }
    argv[argc] = NULL;
    *out_argv = argv;
    return 0;
}

static int
peak_exec_collect_argv_noalloc(const char* arg,
                               va_list* ap,
                               char** argv,
                               size_t capacity)
{
    size_t argc = 0;

    if (argv == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }
    if (arg != NULL) {
        argv[argc++] = (char*)arg;
        for (;;) {
            char* value = va_arg(*ap, char*);

            if (value == NULL) {
                break;
            }
            if (argc + 1 >= capacity) {
                errno = E2BIG;
                return -1;
            }
            argv[argc++] = value;
        }
    }
    argv[argc] = NULL;
    return 0;
}

static int
peak_exec_count_argv(const char* arg, va_list* ap, size_t* argc)
{
    va_list count_ap;
    size_t count = 0;

    if (argc == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (arg != NULL) {
        va_copy(count_ap, *ap);
        do {
            count++;
            if (count >= PEAK_EXEC_MAX_VARARGS) {
                va_end(count_ap);
                errno = E2BIG;
                return -1;
            }
        } while (va_arg(count_ap, const char*) != NULL);
        va_end(count_ap);
    }
    *argc = count;
    return 0;
}

PEAK_EXEC_API int
execl(const char* path, const char* arg, ...)
{
    va_list ap;
    char** argv = NULL;
    int priming_complete = peak_exec_priming_is_complete();
    int direct_bypass = !priming_complete ||
                        !peak_exec_startup_policy_enabled;
    int noalloc_child = direct_bypass || peak_exec_in_fork_child();
    int result;
    int saved_errno;
    size_t child_argc = 0;

    va_start(ap, arg);
    if (noalloc_child && peak_exec_count_argv(arg, &ap, &child_argc) != 0) {
        va_end(ap);
        return -1;
    }
    if (!noalloc_child) {
        result = peak_exec_collect_argv(arg, &ap, &argv);
    } else {
        char* child_argv[child_argc + 1U];

        result = peak_exec_collect_argv_noalloc(
            arg, &ap, child_argv, child_argc + 1U);
        if (result == 0) {
            if (priming_complete && !direct_bypass) {
                result = execve(path, child_argv, environ);
            } else if (priming_complete) {
                result = peak_call_primed_execve(path, child_argv, environ);
            } else {
                result = peak_exec_prepublication_execve(path,
                                                         child_argv,
                                                         environ);
            }
        }
        saved_errno = errno;
        va_end(ap);
        errno = saved_errno;
        return result;
    }
    va_end(ap);
    if (result != 0) {
        return -1;
    }
    if (!direct_bypass) {
        result = execv(path, argv);
    }
    saved_errno = errno;
    if (!noalloc_child) {
        free(argv);
    }
    errno = saved_errno;
    return result;
}

PEAK_EXEC_API int
execlp(const char* file, const char* arg, ...)
{
    va_list ap;
    char** argv = NULL;
    int priming_complete = peak_exec_priming_is_complete();
    int direct_bypass = !priming_complete ||
                        !peak_exec_startup_policy_enabled;
    int noalloc_child = direct_bypass || peak_exec_in_fork_child();
    int result;
    int saved_errno;
    size_t child_argc = 0;

    va_start(ap, arg);
    if (noalloc_child && peak_exec_count_argv(arg, &ap, &child_argc) != 0) {
        va_end(ap);
        return -1;
    }
    if (!noalloc_child) {
        result = peak_exec_collect_argv(arg, &ap, &argv);
    } else {
        char* child_argv[child_argc + 1U];

        result = peak_exec_collect_argv_noalloc(
            arg, &ap, child_argv, child_argc + 1U);
        if (result == 0) {
            if (priming_complete && !direct_bypass) {
                result = execvp(file, child_argv);
            } else if (priming_complete) {
                result = peak_call_primed_execvp(file, child_argv);
            } else {
                result = peak_exec_prepublication_execvp(file, child_argv);
            }
        }
        saved_errno = errno;
        va_end(ap);
        errno = saved_errno;
        return result;
    }
    va_end(ap);
    if (result != 0) {
        return -1;
    }
    if (!direct_bypass) {
        result = execvp(file, argv);
    }
    saved_errno = errno;
    if (!noalloc_child) {
        free(argv);
    }
    errno = saved_errno;
    return result;
}

PEAK_EXEC_API int
execle(const char* path, const char* arg, ...)
{
    va_list ap;
    char** argv = NULL;
    char* const* envp;
    int priming_complete = peak_exec_priming_is_complete();
    int direct_bypass = !priming_complete ||
                        !peak_exec_startup_policy_enabled;
    int noalloc_child = direct_bypass || peak_exec_in_fork_child();
    int result;
    int saved_errno;
    size_t child_argc = 0;

    va_start(ap, arg);
    if (noalloc_child && peak_exec_count_argv(arg, &ap, &child_argc) != 0) {
        va_end(ap);
        return -1;
    }
    if (!noalloc_child) {
        result = peak_exec_collect_argv(arg, &ap, &argv);
    } else {
        char* child_argv[child_argc + 1U];

        result = peak_exec_collect_argv_noalloc(
            arg, &ap, child_argv, child_argc + 1U);
        if (result == 0) {
            envp = va_arg(ap, char* const*);
            if (priming_complete && !direct_bypass) {
                result = execve(path, child_argv, (char* const*)envp);
            } else if (priming_complete) {
                result = peak_call_primed_execve(path,
                                                 child_argv,
                                                 (char* const*)envp);
            } else {
                result = peak_exec_prepublication_execve(
                    path, child_argv, (char* const*)envp);
            }
        }
        saved_errno = errno;
        va_end(ap);
        errno = saved_errno;
        return result;
    }
    if (result == 0) {
        envp = va_arg(ap, char* const*);
    }
    va_end(ap);
    if (result != 0) {
        return -1;
    }
    if (!direct_bypass) {
        result = execve(path, argv, (char* const*)envp);
    }
    saved_errno = errno;
    if (!noalloc_child) {
        free(argv);
    }
    errno = saved_errno;
    return result;
}

PEAK_EXEC_API int
fexecve(int fd, char* const argv[], char* const envp[])
{
    if (!peak_exec_priming_is_complete()) {
        return peak_exec_prepublication_fexecve(fd, argv, envp);
    }
    if (!peak_exec_startup_policy_enabled) {
        return peak_call_primed_fexecve(fd, argv, envp);
    }

    peak_fexecve_fn fn;
    PeakExecEnv built_env = {0};
    char* const* exec_envp;
    char label[64];
    int saved_errno;
    int entry_errno = errno;

    if (peak_exec_in_fork_child()) {
        char* child_env[PEAK_EXEC_CHILD_ENV_SLOTS];
        char child_ld_preload[PEAK_EXEC_CHILD_LD_PRELOAD_CAPACITY];
        char* const* child_envp = peak_exec_child_build_env(envp,
                                                             child_env,
                                                             child_ld_preload);

        return peak_call_primed_fexecve(
            fd, argv, (char* const*)child_envp);
    }
    if (peak_exec_wrapper_depth > 0 || peak_exec_spawn_depth > 0) {
        return peak_call_real_fexecve(fd, argv, envp);
    }
    if (peak_exec_argv_envp_readable(argv, envp) !=
        PEAK_EXEC_PREFLIGHT_VALID) {
        return peak_call_real_fexecve(fd, argv, envp);
    }

    snprintf(label, sizeof(label), "<fd:%d>", fd);
    peak_exec_wrapper_depth++;
    if (peak_exec_prepare(label, argv, envp, &built_env, 1) != 0) {
        exec_envp = envp;
        errno = entry_errno;
    } else {
        exec_envp = peak_exec_env_to_use(&built_env, envp);
    }
    fn = peak_real_fexecve();
    if (fn == NULL) {
        saved_errno = ENOSYS;
    } else {
        (void)fn(fd, argv, (char* const*)exec_envp);
        saved_errno = errno;
    }
    peak_exec_env_clear(&built_env);
    peak_exec_wrapper_depth--;
    errno = saved_errno;
    return -1;
}

#if defined(__linux__)
PEAK_EXEC_API int
execveat(int dirfd,
         const char* pathname,
         char* const argv[],
         char* const envp[],
         int flags)
{
    if (!peak_exec_priming_is_complete()) {
        return peak_exec_prepublication_execveat(
            dirfd, pathname, argv, envp, flags);
    }
    if (!peak_exec_startup_policy_enabled) {
        return peak_call_primed_execveat(
            dirfd, pathname, argv, envp, flags);
    }

    PeakExecEnv built_env = {0};
    char* const* exec_envp;
    int saved_errno;
    int entry_errno = errno;

    if (peak_exec_in_fork_child()) {
        char* child_env[PEAK_EXEC_CHILD_ENV_SLOTS];
        char child_ld_preload[PEAK_EXEC_CHILD_LD_PRELOAD_CAPACITY];
        char* const* child_envp = peak_exec_child_build_env(envp,
                                                             child_env,
                                                             child_ld_preload);

        return peak_call_primed_execveat(
            dirfd, pathname, argv, (char* const*)child_envp, flags);
    }
    if (peak_exec_wrapper_depth > 0 || peak_exec_spawn_depth > 0) {
        return peak_call_real_execveat(
            dirfd, pathname, argv, envp, flags);
    }
    if (peak_exec_args_readable(pathname, argv, envp) !=
        PEAK_EXEC_PREFLIGHT_VALID) {
        return peak_call_real_execveat(
            dirfd, pathname, argv, envp, flags);
    }

    peak_exec_wrapper_depth++;
    if (peak_exec_prepare(pathname, argv, envp, &built_env, 1) != 0) {
        exec_envp = envp;
        errno = entry_errno;
    } else {
        exec_envp = peak_exec_env_to_use(&built_env, envp);
    }
    (void)peak_call_real_execveat(
        dirfd, pathname, argv, (char* const*)exec_envp, flags);
    saved_errno = errno;
    peak_exec_env_clear(&built_env);
    peak_exec_wrapper_depth--;
    errno = saved_errno;
    return -1;
}
#endif

#if defined(__linux__)
int
peak_exec_raw_syscall_execveat(int dirfd,
                               const char* pathname,
                               char* const argv[],
                               char* const envp[],
                               int flags)
{
    if (!peak_exec_priming_is_complete() ||
        !peak_exec_startup_policy_enabled) {
        return peak_exec_prepublication_execveat(
            dirfd, pathname, argv, envp, flags);
    }
    return execveat(dirfd, pathname, argv, envp, flags);
}
#endif

PEAK_EXEC_API int
posix_spawn(pid_t* pid,
            const char* path,
            const posix_spawn_file_actions_t* file_actions,
            const posix_spawnattr_t* attrp,
            char* const argv[],
            char* const envp[])
{
    if (!peak_exec_priming_is_complete()) {
        int entry_errno = errno;
        peak_posix_spawn_fn fn = peak_real_posix_spawn();

        if (fn == NULL) {
            errno = entry_errno;
            return ENOSYS;
        }
        errno = entry_errno;
        return fn(pid, path, file_actions, attrp, argv, envp);
    }
    if (!peak_exec_startup_policy_enabled) {
        if (peak_real_posix_spawn_ptr == NULL) {
            return ENOSYS;
        }
        return peak_real_posix_spawn_ptr(
            pid, path, file_actions, attrp, argv, envp);
    }

    int entry_errno = errno;
    peak_posix_spawn_fn fn = peak_real_posix_spawn();
    PeakExecEnv built_env = {0};
    char* const* spawn_envp;
    int result;
    int spawn_errno;

#if defined(PEAK_ENABLE_TEST_HOOKS)
    if (getenv("PEAK_TEST_EXEC_SPAWN_RESOLVER_NULL") != NULL) {
        errno = EIO;
        fn = NULL;
    }
#endif

    if (fn == NULL) {
        errno = entry_errno;
        return ENOSYS;
    }
    PeakExecPreflightResult preflight_result =
        peak_exec_args_readable(path, argv, envp);
    if (preflight_result != PEAK_EXEC_PREFLIGHT_VALID) {
        errno = entry_errno;
        return fn(pid, path, file_actions, attrp, argv, envp);
    }
    if (peak_exec_prepare(path, argv, envp, &built_env, 0) != 0) {
        peak_exec_env_clear(&built_env);
        errno = entry_errno;
        return fn(pid, path, file_actions, attrp, argv, envp);
    } else {
        spawn_envp = peak_exec_env_to_use(&built_env, envp);
    }
    errno = entry_errno;
    peak_exec_spawn_depth++;
    result = fn(pid,
                path,
                file_actions,
                attrp,
                argv,
                (char* const*)spawn_envp);
    spawn_errno = errno;
    peak_exec_spawn_depth--;
    peak_exec_env_clear(&built_env);
    errno = spawn_errno;
    return result;
}

PEAK_EXEC_API int
posix_spawnp(pid_t* pid,
             const char* file,
             const posix_spawn_file_actions_t* file_actions,
             const posix_spawnattr_t* attrp,
             char* const argv[],
             char* const envp[])
{
    if (!peak_exec_priming_is_complete()) {
        int entry_errno = errno;
        peak_posix_spawnp_fn fn = peak_real_posix_spawnp();

        if (fn == NULL) {
            errno = entry_errno;
            return ENOSYS;
        }
        errno = entry_errno;
        return fn(pid, file, file_actions, attrp, argv, envp);
    }
    if (!peak_exec_startup_policy_enabled) {
        if (peak_real_posix_spawnp_ptr == NULL) {
            return ENOSYS;
        }
        return peak_real_posix_spawnp_ptr(
            pid, file, file_actions, attrp, argv, envp);
    }

    int entry_errno = errno;
    peak_posix_spawnp_fn fn = peak_real_posix_spawnp();
    PeakExecEnv built_env = {0};
    char* const* spawn_envp;
    int result;
    int spawn_errno;

#if defined(PEAK_ENABLE_TEST_HOOKS)
    if (getenv("PEAK_TEST_EXEC_SPAWN_RESOLVER_NULL") != NULL) {
        errno = EIO;
        fn = NULL;
    }
#endif

    if (fn == NULL) {
        errno = entry_errno;
        return ENOSYS;
    }
    PeakExecPreflightResult preflight_result =
        peak_exec_args_readable(file, argv, envp);
    if (preflight_result != PEAK_EXEC_PREFLIGHT_VALID) {
        errno = entry_errno;
        return fn(pid, file, file_actions, attrp, argv, envp);
    }
    if (peak_exec_prepare(file, argv, envp, &built_env, 0) != 0) {
        peak_exec_env_clear(&built_env);
        errno = entry_errno;
        return fn(pid, file, file_actions, attrp, argv, envp);
    } else {
        spawn_envp = peak_exec_env_to_use(&built_env, envp);
    }
    errno = entry_errno;
    peak_exec_spawn_depth++;
    result = fn(pid,
                file,
                file_actions,
                attrp,
                argv,
                (char* const*)spawn_envp);
    spawn_errno = errno;
    peak_exec_spawn_depth--;
    peak_exec_env_clear(&built_env);
    errno = spawn_errno;
    return result;
}
