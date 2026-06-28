#define _GNU_SOURCE
#include "exec_interceptor.h"
#include "utils/utils.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#if defined(__linux__)
#include <sched.h>
#endif
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/auxv.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
extern char** environ;
#ifdef __cplusplus
}
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

#ifndef PEAK_EXEC_MAX_ENV_ENTRIES
#define PEAK_EXEC_MAX_ENV_ENTRIES 32768U
#endif

#ifndef PEAK_EXEC_STACK_LD_PRELOAD_MAX
#define PEAK_EXEC_STACK_LD_PRELOAD_MAX 32768U
#endif

#ifndef PEAK_EXEC_USER_STRING_MAX
#define PEAK_EXEC_USER_STRING_MAX (1024U * 1024U)
#endif

#if defined(__linux__) && !defined(CLONE_PIDFD)
#define CLONE_PIDFD 0
#endif

#if defined(__linux__) && defined(__aarch64__) && \
    (defined(__NVCOMPILER) || defined(__PGI))
#define PEAK_EXEC_USE_LIBC_RAW_SYSCALL 1
#endif

#define PEAK_EXEC_CHAIN_ENV "PEAK_EXEC_CHAIN"
#define PEAK_EXEC_CHECKPOINT_ENV "PEAK_EXEC_CHECKPOINT"
#define PEAK_EXEC_TRACE_PATH_ENV "PEAK_EXEC_TRACE_PATH"
#define PEAK_EXEC_PROPAGATE_PEAK_ENV_ENV "PEAK_EXEC_PROPAGATE_PEAK_ENV"
#define PEAK_TARGET_ENV "PEAK_TARGET"
#define PEAK_TARGET_FILE_ENV "PEAK_TARGET_FILE"
#define PEAK_TARGET_GROUP_ENV "PEAK_TARGET_GROUP"
#define PEAK_GPU_TARGET_ENV "PEAK_GPU_TARGET"
#define PEAK_GPU_TARGET_FILE_ENV "PEAK_GPU_TARGET_FILE"
#define PEAK_GPU_MONITOR_ALL_ENV "PEAK_GPU_MONITOR_ALL"
#define PEAK_MEMORY_PROFILE_ENV "PEAK_MEMORY_PROFILE"

typedef int (*peak_execve_fn)(const char*, char* const[], char* const[]);
typedef int (*peak_execvpe_fn)(const char*, char* const[], char* const[]);
typedef int (*peak_fexecve_fn)(int, char* const[], char* const[]);
typedef pid_t (*peak_fork_fn)(void);
typedef int (*peak_clone_fn)(int (*)(void*), void*, int, void*, ...);
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
typedef long (*peak_syscall_fn)(long, ...);
#if defined(__linux__)
typedef int (*peak_execveat_fn)(int,
                                const char*,
                                char* const[],
                                char* const[],
                                int);
#endif

typedef struct {
    char** envp;
    char** owned_strings;
    size_t owned_count;
    const char* ld_preload_action;
    int changed;
    int stack_backed;
} PeakExecEnv;

typedef struct {
    int checkpoint_enabled;
    int checkpoint_result;
    int chain_enabled;
    const char* ld_preload_action;
} PeakExecAttempt;

typedef struct {
    int (*fn)(void*);
    void* arg;
} PeakCloneStart;

static __thread int peak_exec_wrapper_depth;
static __thread pid_t peak_exec_safe_child_pid;
static __thread char* peak_exec_noalloc_envp[PEAK_EXEC_MAX_ENV_ENTRIES];
static __thread char peak_exec_noalloc_ld_entry[PEAK_EXEC_STACK_LD_PRELOAD_MAX];
static __thread char* peak_exec_noalloc_argv[PEAK_EXEC_MAX_VARARGS + 1];
static __thread char* peak_exec_noalloc_shell_argv[PEAK_EXEC_MAX_VARARGS + 2];
static __thread char peak_exec_noalloc_trace_row[8192];
static pid_t peak_exec_owner_pid;
static int peak_exec_cached_secure_mode = -1;
static char peak_exec_cached_libpeak_path[PATH_MAX];
static char peak_exec_cached_libpeak_original_path[PATH_MAX];
static dev_t peak_exec_cached_libpeak_dev;
static ino_t peak_exec_cached_libpeak_ino;
static int peak_exec_cached_libpeak_identity_valid;
static size_t peak_exec_cached_page_size;
#if defined(PEAK_EXEC_USE_LIBC_RAW_SYSCALL)
static peak_syscall_fn peak_cached_syscall_fn;
static int peak_cached_syscall_looked_up;
#endif
static peak_posix_spawn_fn peak_cached_posix_spawn_fn;
static int peak_cached_posix_spawn_looked_up;
static peak_posix_spawnp_fn peak_cached_posix_spawnp_fn;
static int peak_cached_posix_spawnp_looked_up;

static int peak_exec_in_fork_like_child(void);
static int peak_exec_in_shared_vm_child(void);
static int peak_exec_env_entry_matches(const char* entry, const char* name);
static int peak_exec_peak_name_is_priority(const char* name);
static peak_clone_fn peak_real_clone(void);
static peak_posix_spawn_fn peak_real_posix_spawn(void);
static peak_posix_spawnp_fn peak_real_posix_spawnp(void);

#if defined(PEAK_EXEC_USE_LIBC_RAW_SYSCALL)
static peak_syscall_fn
peak_exec_real_syscall(void)
{
    if (!peak_cached_syscall_looked_up) {
        peak_cached_syscall_fn = (peak_syscall_fn)dlsym(RTLD_NEXT, "syscall");
        peak_cached_syscall_looked_up = 1;
    }
    return peak_cached_syscall_fn;
}

static long
peak_call_libc_syscall6(long number,
                        long arg1,
                        long arg2,
                        long arg3,
                        long arg4,
                        long arg5,
                        long arg6)
{
    peak_syscall_fn fn = peak_exec_real_syscall();
    if (fn == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return fn(number, arg1, arg2, arg3, arg4, arg5, arg6);
}

__attribute__((constructor))
static void
peak_exec_prime_real_syscall(void)
{
    (void)peak_exec_real_syscall();
}
#endif /* PEAK_EXEC_USE_LIBC_RAW_SYSCALL */

long
peak_exec_call_raw_syscall6(long number,
                            long arg1,
                            long arg2,
                            long arg3,
                            long arg4,
                            long arg5,
                            long arg6)
{
#if defined(__linux__) && defined(__x86_64__)
    long result;
    register long r10 __asm__("r10") = arg4;
    register long r8 __asm__("r8") = arg5;
    register long r9 __asm__("r9") = arg6;

    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number),
                       "D"(arg1),
                       "S"(arg2),
                       "d"(arg3),
                       "r"(r10),
                       "r"(r8),
                       "r"(r9)
                     : "rcx", "r11", "memory");
    if (result < 0 && result >= -4095) {
        errno = (int)-result;
        return -1;
    }
    return result;
#elif defined(PEAK_EXEC_USE_LIBC_RAW_SYSCALL)
    /*
     * NVHPC's AArch64 C frontend has miscompiled the register-variable inline
     * syscall sequence by leaving x0 unchanged across svc #0.  That turns raw
     * execve failures into pointer-looking positive returns and makes preflight
     * probes report EFAULT for valid user buffers.  Prefer libc's syscall
     * entrypoint when available; it is resolved with RTLD_NEXT so PEAK's own
     * syscall interposer is bypassed.
     */
    return peak_call_libc_syscall6(number,
                                   arg1,
                                   arg2,
                                   arg3,
                                   arg4,
                                   arg5,
                                   arg6);
#elif defined(__linux__) && defined(__aarch64__)
    register long x0 __asm__("x0") = arg1;
    register long x1 __asm__("x1") = arg2;
    register long x2 __asm__("x2") = arg3;
    register long x3 __asm__("x3") = arg4;
    register long x4 __asm__("x4") = arg5;
    register long x5 __asm__("x5") = arg6;
    register long x8 __asm__("x8") = number;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1),
                       "r"(x2),
                       "r"(x3),
                       "r"(x4),
                       "r"(x5),
                       "r"(x8)
                     : "cc", "memory");
    if (x0 < 0 && x0 >= -4095) {
        errno = (int)-x0;
        return -1;
    }
    return x0;
#else
    (void)number;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;
    (void)arg6;
    errno = ENOSYS;
    return -1;
#endif
}

#if defined(PEAK_ENABLE_TEST_HOOKS)
static int
peak_exec_test_env_present(const char* name)
{
    size_t name_len = strlen(name);

    if (environ == NULL) {
        return 0;
    }
    for (char** scan = environ; *scan != NULL; scan++) {
        if (strncmp(*scan, name, name_len) == 0 &&
            (*scan)[name_len] == '=') {
            return 1;
        }
    }
    return 0;
}
#endif

static int
peak_exec_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static const char*
peak_exec_parse_maps_hex(const char* p, const char* end, uintptr_t* value_out)
{
    uintptr_t value = 0;
    int digits = 0;

    while (p < end) {
        int digit = peak_exec_hex_value(*p);
        if (digit < 0) {
            break;
        }
        value = (value << 4) | (uintptr_t)digit;
        digits++;
        p++;
    }

    if (digits == 0) {
        return NULL;
    }
    *value_out = value;
    return p;
}

static int
peak_exec_parse_maps_line(const char* line,
                          size_t length,
                          uintptr_t* start_out,
                          uintptr_t* end_out,
                          int* readable_out)
{
    const char* p = line;
    const char* end = line + length;
    uintptr_t start = 0;
    uintptr_t stop = 0;

    p = peak_exec_parse_maps_hex(p, end, &start);
    if (p == NULL || p >= end || *p != '-') {
        return -1;
    }
    p++;
    p = peak_exec_parse_maps_hex(p, end, &stop);
    if (p == NULL || p >= end || *p != ' ') {
        return -1;
    }
    p++;
    if (p >= end) {
        return -1;
    }

    *start_out = start;
    *end_out = stop;
    *readable_out = *p == 'r';
    return 0;
}

static int
peak_exec_maps_range_readable(const void* remote, size_t length)
{
#if defined(__linux__) && defined(SYS_openat) && defined(SYS_read) && \
    defined(SYS_close)
    uintptr_t start = (uintptr_t)remote;
    uintptr_t stop;
    uintptr_t cursor;
    char read_buffer[4096];
    char line[1024];
    size_t line_len = 0;
    int skipping_long_line = 0;
    long fd;

    if (length == 0) {
        return 1;
    }
    if (remote == NULL || start > UINTPTR_MAX - length) {
        return 0;
    }
    stop = start + length;
    cursor = start;

    fd = peak_exec_call_raw_syscall6(SYS_openat,
                                AT_FDCWD,
                                (long)"/proc/self/maps",
                                O_RDONLY | O_CLOEXEC,
                                0,
                                0,
                                0);
    if (fd < 0) {
        return 0;
    }

    while (1) {
        long nread = peak_exec_call_raw_syscall6(SYS_read,
                                            fd,
                                            (long)read_buffer,
                                            sizeof(read_buffer),
                                            0,
                                            0,
                                            0);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)peak_exec_call_raw_syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
            return 0;
        }
        if (nread == 0) {
            break;
        }

        for (long i = 0; i < nread; i++) {
            char ch = read_buffer[i];

            if (skipping_long_line) {
                if (ch == '\n') {
                    skipping_long_line = 0;
                    line_len = 0;
                }
                continue;
            }

            if (ch != '\n' && line_len + 1 < sizeof(line)) {
                line[line_len++] = ch;
                continue;
            }

            if (ch != '\n') {
                skipping_long_line = 1;
                line_len = 0;
                continue;
            }

            uintptr_t map_start = 0;
            uintptr_t map_end = 0;
            int readable = 0;
            if (peak_exec_parse_maps_line(line,
                                          line_len,
                                          &map_start,
                                          &map_end,
                                          &readable) == 0) {
                if (map_end <= cursor) {
                    line_len = 0;
                    continue;
                }
                if (map_start > cursor || !readable) {
                    (void)peak_exec_call_raw_syscall6(SYS_close,
                                                 fd,
                                                 0,
                                                 0,
                                                 0,
                                                 0,
                                                 0);
                    return 0;
                }
                if (map_end > cursor) {
                    cursor = map_end < stop ? map_end : stop;
                    if (cursor >= stop) {
                        (void)peak_exec_call_raw_syscall6(SYS_close,
                                                     fd,
                                                     0,
                                                     0,
                                                     0,
                                                     0,
                                                     0);
                        return 1;
                    }
                }
            }
            line_len = 0;
        }
    }

    (void)peak_exec_call_raw_syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
    return 0;
#else
    (void)remote;
    (void)length;
    return 0;
#endif
}

static int
peak_exec_user_read_bytes(const void* remote, void* local, size_t length)
{
    if (length == 0) {
        return 0;
    }
    if (remote == NULL || local == NULL) {
        errno = EFAULT;
        return -1;
    }

#if defined(__linux__) && defined(SYS_process_vm_readv)
    struct iovec local_iov;
    struct iovec remote_iov;
    int saved_errno;
    int skip_proc_mem = 0;
    long nread;

    local_iov.iov_base = local;
    local_iov.iov_len = length;
    remote_iov.iov_base = (void*)remote;
    remote_iov.iov_len = length;

#if defined(PEAK_ENABLE_TEST_HOOKS)
    if (peak_exec_test_env_present("PEAK_TEST_EXEC_PREFLIGHT_UNAVAILABLE")) {
        nread = -1;
        errno = EPERM;
    } else
#endif
    {
        nread = peak_exec_call_raw_syscall6(SYS_process_vm_readv,
                                       (long)getpid(),
                                       (long)&local_iov,
                                       1,
                                       (long)&remote_iov,
                                       1,
                                       0);
    }
    if (nread == (long)length) {
        return 0;
    }
    if (nread >= 0) {
        errno = EFAULT;
        return -1;
    }
    saved_errno = errno;

#if defined(SYS_openat) && defined(SYS_pread64)
    /*
     * Some confined environments reject process_vm_readv() even for self
     * reads. Fall back to a maps-checked local copy (and then /proc/self/mem
     * when enabled) so syscall-exec preflight remains a pointer validation
     * step instead of silently bypassing exec-chain logic.
     */
#if defined(PEAK_ENABLE_TEST_HOOKS)
    if (peak_exec_test_env_present("PEAK_TEST_EXEC_PREFLIGHT_NO_PROC_MEM")) {
        skip_proc_mem = 1;
    }
#endif
    if (!peak_exec_maps_range_readable(remote, length)) {
        errno = EFAULT;
        return -1;
    }
    if (skip_proc_mem) {
        memcpy(local, remote, length);
        return 0;
    }
    long fd = peak_exec_call_raw_syscall6(SYS_openat,
                                     AT_FDCWD,
                                     (long)"/proc/self/mem",
                                     O_RDONLY | O_CLOEXEC,
                                     0,
                                     0,
                                     0);
    if (fd >= 0) {
        long mem_read = peak_exec_call_raw_syscall6(SYS_pread64,
                                               fd,
                                               (long)local,
                                               (long)length,
                                               (long)(uintptr_t)remote,
                                               0,
                                               0);
        int mem_errno = errno;
        (void)peak_exec_call_raw_syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
        if (mem_read == (long)length) {
            return 0;
        }
        errno = mem_read >= 0 ? EFAULT :
            (mem_errno == EIO ? EFAULT : mem_errno);
        return -1;
    }
    memcpy(local, remote, length);
    return 0;
#endif
    errno = saved_errno;
    return -1;
#else
    errno = ENOSYS;
    return -1;
#endif
}

static int
peak_exec_user_cstr_readable(const char* value, size_t max_length)
{
    char buffer[256];
    size_t offset = 0;
    size_t page_size = peak_exec_cached_page_size != 0 ?
        peak_exec_cached_page_size : 4096U;

    if (value == NULL) {
        errno = EFAULT;
        return -1;
    }

    while (offset < max_length) {
        size_t chunk = sizeof(buffer);
        uintptr_t address = (uintptr_t)(value + offset);
        size_t page_remaining = page_size - (address % page_size);

        if (chunk > max_length - offset) {
            chunk = max_length - offset;
        }
        if (chunk > page_remaining) {
            chunk = page_remaining;
        }
        if (peak_exec_user_read_bytes(value + offset, buffer, chunk) != 0) {
            return -1;
        }
        if (memchr(buffer, '\0', chunk) != NULL) {
            return 0;
        }
        offset += chunk;
    }

    errno = ENAMETOOLONG;
    return -1;
}

static int
peak_exec_user_cstr_array_readable(char* const values[],
                                   size_t max_entries,
                                   size_t max_string_length)
{
    if (values == NULL) {
        return 0;
    }

    for (size_t i = 0; i < max_entries; i++) {
        char* entry = NULL;
        if (peak_exec_user_read_bytes(&values[i], &entry, sizeof(entry)) != 0) {
            return -1;
        }
        if (entry == NULL) {
            return 0;
        }
        if (peak_exec_user_cstr_readable(entry, max_string_length) != 0) {
            return -1;
        }
    }

    errno = E2BIG;
    return -1;
}

static int
peak_exec_syscall_args_readable(const char* path,
                                char* const argv[],
                                char* const envp[])
{
    if (peak_exec_user_cstr_readable(path, PATH_MAX) != 0) {
        return -1;
    }
    if (peak_exec_user_cstr_array_readable(argv,
                                           PEAK_EXEC_MAX_ENV_ENTRIES,
                                           PEAK_EXEC_USER_STRING_MAX) != 0) {
        return -1;
    }
    if (peak_exec_user_cstr_array_readable(envp,
                                           PEAK_EXEC_MAX_ENV_ENTRIES,
                                           PEAK_EXEC_USER_STRING_MAX) != 0) {
        return -1;
    }
    return 0;
}

static int
peak_exec_argv_envp_readable(char* const argv[], char* const envp[])
{
    if (peak_exec_user_cstr_array_readable(argv,
                                           PEAK_EXEC_MAX_ENV_ENTRIES,
                                           PEAK_EXEC_USER_STRING_MAX) != 0) {
        return -1;
    }
    if (peak_exec_user_cstr_array_readable(envp,
                                           PEAK_EXEC_MAX_ENV_ENTRIES,
                                           PEAK_EXEC_USER_STRING_MAX) != 0) {
        return -1;
    }
    return 0;
}

static int
peak_exec_preflight_error_is_conclusive(int err)
{
    return err == EFAULT || err == E2BIG || err == ENAMETOOLONG;
}

static int
peak_exec_clone3_flags_safe(const void* clone_args, uint64_t* flags_out)
{
    if (clone_args == NULL || flags_out == NULL) {
        errno = EFAULT;
        return -1;
    }

    uint64_t flags = 0;

    if (peak_exec_user_read_bytes(clone_args, &flags, sizeof(flags)) == 0) {
        *flags_out = flags;
        return 0;
    }
    return -1;
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

static const char*
peak_exec_current_env_get_direct(const char* name)
{
    if (environ == NULL) {
        return NULL;
    }
    for (char** scan = environ; *scan != NULL; scan++) {
        if (peak_exec_env_entry_matches(*scan, name)) {
            return strchr(*scan, '=') + 1;
        }
    }
    return NULL;
}

static int
peak_exec_current_env_default_true_direct(const char* name)
{
    return !peak_exec_env_false(peak_exec_current_env_get_direct(name));
}

static int
peak_exec_env_truthy(const char* value)
{
    return value != NULL &&
           (strcasecmp(value, "1") == 0 ||
            strcasecmp(value, "true") == 0 ||
            strcasecmp(value, "yes") == 0 ||
            strcasecmp(value, "on") == 0);
}

static int
peak_exec_current_env_nonempty_direct(const char* name)
{
    const char* value = peak_exec_current_env_get_direct(name);
    return value != NULL && value[0] != '\0';
}

static int
peak_exec_current_env_requests_work_direct(void)
{
    return peak_exec_current_env_nonempty_direct(PEAK_TARGET_ENV) ||
           peak_exec_current_env_nonempty_direct(PEAK_TARGET_FILE_ENV) ||
           peak_exec_current_env_nonempty_direct(PEAK_TARGET_GROUP_ENV) ||
           peak_exec_current_env_nonempty_direct(PEAK_GPU_TARGET_ENV) ||
           peak_exec_current_env_nonempty_direct(PEAK_GPU_TARGET_FILE_ENV) ||
           peak_exec_env_truthy(
               peak_exec_current_env_get_direct(PEAK_GPU_MONITOR_ALL_ENV)) ||
           peak_exec_env_truthy(
               peak_exec_current_env_get_direct(PEAK_MEMORY_PROFILE_ENV));
}

static int
peak_exec_is_separator(char ch)
{
    return ch == ':' || isspace((unsigned char)ch);
}

static int
peak_exec_name_len(const char* entry)
{
    const char* equals = strchr(entry, '=');
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
    char* const* scan = envp;

    if (scan == NULL) {
        return NULL;
    }

    for (; *scan != NULL; scan++) {
        if (peak_exec_env_entry_matches(*scan, name)) {
            return strchr(*scan, '=') + 1;
        }
    }
    return NULL;
}

static int
peak_exec_env_has_name(char* const envp[], const char* name)
{
    return peak_exec_env_get(envp, name) != NULL;
}

static int
peak_exec_env_storage_has_name(char** envp, size_t count, const char* name)
{
    for (size_t i = 0; i < count; i++) {
        if (peak_exec_env_entry_matches(envp[i], name)) {
            return 1;
        }
    }
    return 0;
}

static size_t
peak_exec_env_count(char* const envp[])
{
    size_t count = 0;
    if (envp == NULL) {
        return 0;
    }
    while (envp[count] != NULL) {
        count++;
    }
    return count;
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

static char*
peak_exec_env_join_values(char* const envp[], const char* name)
{
    size_t total = 0;
    size_t entry_count = 0;
    size_t value_count = 0;
    size_t out = 0;
    char* joined;

    if (envp == NULL || name == NULL) {
        return NULL;
    }

    for (char* const* scan = envp; *scan != NULL; scan++) {
        if (!peak_exec_env_entry_matches(*scan, name)) {
            continue;
        }
        entry_count++;
        const char* value = strchr(*scan, '=') + 1;
        if (value[0] == '\0') {
            continue;
        }
        total += strlen(value);
        value_count++;
    }
    if (entry_count == 0) {
        return NULL;
    }

    joined = malloc(total + (value_count > 0 ? value_count : 1));
    if (joined == NULL) {
        return NULL;
    }

    for (char* const* scan = envp; *scan != NULL; scan++) {
        if (!peak_exec_env_entry_matches(*scan, name)) {
            continue;
        }
        const char* value = strchr(*scan, '=') + 1;
        size_t len = strlen(value);
        if (len == 0) {
            continue;
        }
        if (out != 0) {
            joined[out++] = ':';
        }
        memcpy(joined + out, value, len);
        out += len;
    }
    joined[out] = '\0';
    return joined;
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

static int
peak_exec_secure_mode(void)
{
    if (peak_exec_cached_secure_mode >= 0) {
        return peak_exec_cached_secure_mode;
    }
#if defined(PEAK_ENABLE_TEST_HOOKS)
    const char* forced_secure = getenv("PEAK_TEST_EXEC_AT_SECURE");
    if (forced_secure != NULL && !peak_exec_env_false(forced_secure)) {
        peak_exec_cached_secure_mode = 1;
        return peak_exec_cached_secure_mode;
    }
#endif
#if defined(__linux__) && defined(AT_SECURE)
    errno = 0;
    peak_exec_cached_secure_mode = getauxval(AT_SECURE) != 0;
    return peak_exec_cached_secure_mode;
#else
    peak_exec_cached_secure_mode = 0;
    return peak_exec_cached_secure_mode;
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
    if (dladdr((void*)&peak_checkpoint_for_exec, &info) == 0 ||
        info.dli_fname == NULL ||
        info.dli_fname[0] == '\0') {
        return NULL;
    }

    return peak_exec_realpath_or_dup(info.dli_fname);
}

static int
peak_exec_same_path_token(const char* token,
                          size_t token_len,
                          const char* libpeak_path)
{
    char* token_copy;
    char* token_real;
    int same = 0;

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
    free(token_copy);
    return same;
}

static int
peak_exec_preload_contains_libpeak(const char* value, const char* libpeak_path)
{
    const char* cursor = value;

    if (value == NULL || libpeak_path == NULL) {
        return 0;
    }

    while (*cursor != '\0') {
        while (*cursor != '\0' && peak_exec_is_separator(*cursor)) {
            cursor++;
        }
        const char* start = cursor;
        while (*cursor != '\0' && !peak_exec_is_separator(*cursor)) {
            cursor++;
        }
        if (cursor > start &&
            peak_exec_same_path_token(start,
                                      (size_t)(cursor - start),
                                      libpeak_path)) {
            return 1;
        }
    }
    return 0;
}

static int
peak_exec_preload_contains_libpeak_literal(const char* value,
                                           const char* libpeak_path)
{
    const char* cursor = value;
    size_t lib_len;

    if (value == NULL || libpeak_path == NULL || libpeak_path[0] == '\0') {
        return 0;
    }

    lib_len = strlen(libpeak_path);
    while (*cursor != '\0') {
        while (*cursor != '\0' && peak_exec_is_separator(*cursor)) {
            cursor++;
        }
        const char* start = cursor;
        while (*cursor != '\0' && !peak_exec_is_separator(*cursor)) {
            cursor++;
        }
        if ((size_t)(cursor - start) == lib_len &&
            strncmp(start, libpeak_path, lib_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int
peak_exec_preload_contains_cached_libpeak_literal(const char* value)
{
    if (peak_exec_preload_contains_libpeak_literal(
            value,
            peak_exec_cached_libpeak_path[0] != '\0' ?
                peak_exec_cached_libpeak_path : NULL)) {
        return 1;
    }
    if (peak_exec_cached_libpeak_original_path[0] != '\0' &&
        strcmp(peak_exec_cached_libpeak_original_path,
               peak_exec_cached_libpeak_path) != 0 &&
        peak_exec_preload_contains_libpeak_literal(
            value,
            peak_exec_cached_libpeak_original_path)) {
        return 1;
    }
    return 0;
}

static char*
peak_exec_build_ld_preload(const char* existing,
                           const char* libpeak_path,
                           int* changed)
{
    size_t lib_len;
    size_t capacity;
    size_t length;
    char* output;
    const char* cursor;

    *changed = 0;
    if (libpeak_path == NULL || libpeak_path[0] == '\0') {
        return NULL;
    }

    lib_len = strlen(libpeak_path);
    capacity = lib_len + 1 + (existing != NULL ? strlen(existing) : 0) + 1;
    output = malloc(capacity);
    if (output == NULL) {
        return NULL;
    }

    memcpy(output, libpeak_path, lib_len);
    length = lib_len;
    output[length] = '\0';

    cursor = existing;
    while (cursor != NULL && *cursor != '\0') {
        while (*cursor != '\0' && peak_exec_is_separator(*cursor)) {
            cursor++;
        }
        const char* start = cursor;
        while (*cursor != '\0' && !peak_exec_is_separator(*cursor)) {
            cursor++;
        }
        size_t token_len = (size_t)(cursor - start);
        if (token_len == 0 ||
            peak_exec_same_path_token(start, token_len, libpeak_path)) {
            continue;
        }
        if (length + 1 + token_len + 1 > capacity) {
            size_t new_capacity = capacity * 2 + token_len + 2;
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
    char** grown;

    if (value == NULL) {
        return -1;
    }
    grown = realloc(built->owned_strings,
                    sizeof(char*) * (built->owned_count + 1));
    if (grown == NULL) {
        free(value);
        return -1;
    }
    built->owned_strings = grown;
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
            peak_exec_env_storage_has_name(built->envp, *out_index, name)) {
            continue;
        }

        copy = strdup(*scan);
        if (peak_exec_add_owned(built, copy) != 0) {
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
    if (!built->stack_backed) {
        free(built->envp);
    }
    memset(built, 0, sizeof(*built));
}

static int
peak_exec_should_preserve_chain(const char* libpeak_path, char* const child_envp[])
{
    const char* current_preload = getenv("LD_PRELOAD");

    return peak_process_requests_work() ||
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
    size_t base_count = peak_exec_env_count(base_env);
    char* libpeak_path = NULL;
    char* new_ld_value = NULL;
    char* existing_ld_joined = NULL;
    int new_ld_changed = 0;
    int ld_written = 0;
    int secure = peak_exec_secure_mode();
    int should_touch_ld = 0;
    const char* existing_ld = NULL;
    size_t ld_entry_count = peak_exec_env_name_count(base_env, "LD_PRELOAD");
    size_t missing_peak_count = 0;
    size_t extra_count;
    size_t out = 0;

    memset(built, 0, sizeof(*built));
    built->ld_preload_action = chain_enabled ? "unchanged" : "disabled";

    if (!chain_enabled) {
        return 0;
    }

    libpeak_path = peak_exec_libpeak_path();
    if (libpeak_path != NULL && !secure &&
        peak_exec_should_preserve_chain(libpeak_path, envp)) {
        should_touch_ld = 1;
        existing_ld_joined = peak_exec_env_join_values(base_env, "LD_PRELOAD");
        if (ld_entry_count > 0 && existing_ld_joined == NULL) {
            built->ld_preload_action = "alloc-failed";
            free(libpeak_path);
            return -1;
        }
        existing_ld = existing_ld_joined;
        new_ld_value = peak_exec_build_ld_preload(existing_ld,
                                                  libpeak_path,
                                                  &new_ld_changed);
        if (new_ld_value == NULL) {
            built->ld_preload_action = "alloc-failed";
            free(existing_ld_joined);
            free(libpeak_path);
            return -1;
        }
        built->ld_preload_action = new_ld_changed ? "prepended" : "present";
    } else if (secure) {
        built->ld_preload_action = "secure-skip";
    } else {
        built->ld_preload_action = "unchanged";
    }

    if (propagate_peak_env) {
        missing_peak_count = peak_exec_count_missing_peak_env(base_env);
    }

    if (!new_ld_changed && missing_peak_count == 0 && ld_entry_count <= 1) {
        free(new_ld_value);
        free(existing_ld_joined);
        free(libpeak_path);
        return 0;
    }

    extra_count = missing_peak_count + ((should_touch_ld && ld_entry_count == 0) ? 1 : 0);
    built->envp = calloc(base_count + extra_count + 1, sizeof(char*));
    if (built->envp == NULL) {
        free(new_ld_value);
        free(existing_ld_joined);
        free(libpeak_path);
        built->ld_preload_action = "alloc-failed";
        return -1;
    }

    for (size_t i = 0; i < base_count; i++) {
        if (should_touch_ld && peak_exec_env_entry_matches(base_env[i], "LD_PRELOAD")) {
            if (ld_written) {
                built->changed = 1;
                continue;
            }
            size_t value_len = strlen(new_ld_value);
            char* entry = malloc(strlen("LD_PRELOAD=") + value_len + 1);
            if (entry == NULL) {
                free(new_ld_value);
                free(existing_ld_joined);
                free(libpeak_path);
                built->ld_preload_action = "alloc-failed";
                return -1;
            }
            sprintf(entry, "LD_PRELOAD=%s", new_ld_value);
            if (peak_exec_add_owned(built, entry) != 0) {
                free(new_ld_value);
                free(existing_ld_joined);
                free(libpeak_path);
                built->ld_preload_action = "alloc-failed";
                return -1;
            }
            built->envp[out++] = entry;
            built->changed = 1;
            ld_written = 1;
        } else {
            built->envp[out++] = base_env[i];
        }
    }

    if (should_touch_ld && ld_entry_count == 0) {
        size_t value_len = strlen(new_ld_value);
        char* entry = malloc(strlen("LD_PRELOAD=") + value_len + 1);
        if (entry == NULL) {
            free(new_ld_value);
            free(existing_ld_joined);
            free(libpeak_path);
            built->ld_preload_action = "alloc-failed";
            return -1;
        }
        sprintf(entry, "LD_PRELOAD=%s", new_ld_value);
        if (peak_exec_add_owned(built, entry) != 0) {
            free(new_ld_value);
            free(existing_ld_joined);
            free(libpeak_path);
            built->ld_preload_action = "alloc-failed";
            return -1;
        }
        built->envp[out++] = entry;
        built->changed = 1;
    }

    if (propagate_peak_env &&
        peak_exec_append_missing_peak_env(built, base_env, &out) != 0) {
        free(new_ld_value);
        free(existing_ld_joined);
        free(libpeak_path);
        built->ld_preload_action = "alloc-failed";
        return -1;
    }

    built->envp[out] = NULL;
    free(new_ld_value);
    free(existing_ld_joined);
    free(libpeak_path);
    return 0;
}

static int
peak_exec_token_is_cached_libpeak_literal(const char* token, size_t token_len)
{
    const char* paths[] = {
        peak_exec_cached_libpeak_path,
        peak_exec_cached_libpeak_original_path,
    };

    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        size_t path_len;
        if (paths[i][0] == '\0') {
            continue;
        }
        path_len = strlen(paths[i]);
        if (token_len == path_len && strncmp(token, paths[i], token_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int
peak_exec_token_is_cached_libpeak_same_file(const char* token, size_t token_len)
{
#if defined(__linux__) && defined(SYS_newfstatat)
    char path[PATH_MAX];
    struct stat st;

    if (!peak_exec_cached_libpeak_identity_valid ||
        token == NULL ||
        token_len == 0 ||
        token_len >= sizeof(path)) {
        return 0;
    }

    memcpy(path, token, token_len);
    path[token_len] = '\0';
    memset(&st, 0, sizeof(st));
    if (peak_exec_call_raw_syscall6(SYS_newfstatat,
                               AT_FDCWD,
                               (long)path,
                               (long)&st,
                               0,
                               0,
                               0) != 0) {
        return 0;
    }

    return st.st_dev == peak_exec_cached_libpeak_dev &&
           st.st_ino == peak_exec_cached_libpeak_ino;
#else
    (void)token;
    (void)token_len;
    return 0;
#endif
}

static int
peak_exec_make_stack_ld_preload(char* const envp[],
                                const char* libpeak_path,
                                char* buffer,
                                size_t buffer_size,
                                int* changed)
{
    const char prefix[] = "LD_PRELOAD=";
    size_t prefix_len = strlen(prefix);
    size_t lib_len;
    size_t out;
    const char* existing = peak_exec_env_get(envp, "LD_PRELOAD");
    size_t ld_entry_count = peak_exec_env_name_count(envp, "LD_PRELOAD");

    *changed = 0;
    if (libpeak_path == NULL || libpeak_path[0] == '\0') {
        return -1;
    }

    lib_len = strlen(libpeak_path);
    if (prefix_len + lib_len + 1 > buffer_size) {
        return -1;
    }

    memcpy(buffer, prefix, prefix_len);
    memcpy(buffer + prefix_len, libpeak_path, lib_len);
    out = prefix_len + lib_len;
    buffer[out] = '\0';

    if (envp != NULL) {
        for (char* const* scan = envp; *scan != NULL; scan++) {
            if (!peak_exec_env_entry_matches(*scan, "LD_PRELOAD")) {
                continue;
            }
            const char* cursor = strchr(*scan, '=') + 1;
            while (*cursor != '\0') {
                while (*cursor != '\0' && peak_exec_is_separator(*cursor)) {
                    cursor++;
                }
                const char* start = cursor;
                while (*cursor != '\0' && !peak_exec_is_separator(*cursor)) {
                    cursor++;
                }
                size_t token_len = (size_t)(cursor - start);
                if (token_len == 0 ||
                    peak_exec_token_is_cached_libpeak_literal(start, token_len) ||
                    peak_exec_token_is_cached_libpeak_same_file(start, token_len)) {
                    continue;
                }
                if (out + 1 + token_len + 1 > buffer_size) {
                    return -1;
                }
                buffer[out++] = ':';
                memcpy(buffer + out, start, token_len);
                out += token_len;
                buffer[out] = '\0';
            }
        }
    }

    if (ld_entry_count != 1 || existing == NULL ||
        strcmp(existing, buffer + prefix_len) != 0) {
        *changed = 1;
    }
    return 0;
}

static int
peak_exec_ensure_env_storage_capacity(PeakExecEnv* built,
                                      char*** env_storage,
                                      size_t* env_storage_count,
                                      size_t needed_count)
{
    if (needed_count <= *env_storage_count) {
        return 0;
    }
    (void)built;
    (void)env_storage;
    errno = E2BIG;
    return -1;
}

static int
peak_exec_peak_name_is_priority(const char* name)
{
    static const char* const priority_names[] = {
        "PEAK_TARGET",
        "PEAK_TARGET_FILE",
        "PEAK_TARGET_GROUP",
        "PEAK_GPU_TARGET",
        "PEAK_GPU_TARGET_FILE",
        "PEAK_GPU_MONITOR_ALL",
        "PEAK_MEMORY_PROFILE",
        "PEAK_TARGET_CONFIG_PATH",
        "PEAK_STATSLOG_PATH",
        "PEAK_OUTPUT_FORMAT",
        "PEAK_TEXT_OUTPUT",
        "PEAK_PROFILE_INTERPRETERS",
        "PEAK_HEARTBEAT_INTERVAL",
        "PEAK_EXEC_CHAIN",
        "PEAK_EXEC_CHECKPOINT",
        "PEAK_EXEC_PROPAGATE_PEAK_ENV",
        "PEAK_EXEC_TRACE_PATH",
    };
    static const char* const priority_prefixes[] = {
        "PEAK_EXEC_",
        "PEAK_MPI_",
        "PEAK_OUTPUT_",
        "PEAK_STATS",
        "PEAK_TEXT_",
        "PEAK_DLOPEN_",
        "PEAK_MEM",
        "PEAK_MEMORY_",
        "PEAK_DETACH_",
        "PEAK_REATTACH_",
        "PEAK_SIGNAL_",
        "PEAK_CUDA_",
        "PEAK_GPU_",
    };

    for (size_t i = 0; i < sizeof(priority_names) / sizeof(priority_names[0]); i++) {
        if (strcmp(name, priority_names[i]) == 0) {
            return 1;
        }
    }
    for (size_t i = 0; i < sizeof(priority_prefixes) / sizeof(priority_prefixes[0]); i++) {
        if (strncmp(name, priority_prefixes[i], strlen(priority_prefixes[i])) == 0) {
            return 1;
        }
    }
    return 0;
}

static int
peak_exec_build_env_noalloc(char* const envp[],
                            int chain_enabled,
                            int propagate_peak_env,
                            PeakExecEnv* built,
                            char** env_storage,
                            size_t env_storage_count,
                            char* ld_storage,
                            size_t ld_storage_size)
{
    char* const* base_env = envp;
    const char* libpeak_path = peak_exec_cached_libpeak_path[0] != '\0' ?
        peak_exec_cached_libpeak_path : NULL;
    size_t base_count = peak_exec_env_count(base_env);
    size_t ld_entry_count = peak_exec_env_name_count(base_env, "LD_PRELOAD");
    size_t out = 0;
    int secure = peak_exec_secure_mode();
    int should_touch_ld = 0;
    int new_ld_changed = 0;
    int ld_written = 0;
    size_t needed_count;
    size_t ld_insert_count = 0;
    size_t required_without_peak;
    int partial_peak_env = 0;

    memset(built, 0, sizeof(*built));
    built->stack_backed = 1;
    built->ld_preload_action = chain_enabled ? "unchanged" : "disabled";

    if (!chain_enabled) {
        return 0;
    }

    if (libpeak_path != NULL && !secure &&
        (peak_exec_current_env_requests_work_direct() ||
         peak_exec_current_env_has_peak_entry() ||
         peak_exec_env_has_peak_entry(envp) ||
         peak_exec_preload_contains_cached_libpeak_literal(
             peak_exec_current_env_get_direct("LD_PRELOAD")))) {
        should_touch_ld = 1;
        if (peak_exec_make_stack_ld_preload(base_env,
                                            libpeak_path,
                                            ld_storage,
                                            ld_storage_size,
                                            &new_ld_changed) != 0) {
            built->ld_preload_action = "stack-too-small";
            return -1;
        }
        built->ld_preload_action = new_ld_changed ? "prepended" : "present";
        if (ld_entry_count == 0 && new_ld_changed &&
            base_count + 2 > env_storage_count) {
            built->ld_preload_action = "env-too-large";
            return -1;
        }
    } else if (secure) {
        built->ld_preload_action = "secure-skip";
    }

    ld_insert_count =
        should_touch_ld && ld_entry_count == 0 && new_ld_changed ? 1 : 0;
    required_without_peak = base_count + ld_insert_count + 1;
    if (required_without_peak > env_storage_count) {
        built->ld_preload_action = "env-too-large";
        return -1;
    }

    needed_count = base_count + ld_insert_count + 1;

    if (!new_ld_changed && !propagate_peak_env && ld_entry_count <= 1) {
        return 0;
    }

    if (peak_exec_ensure_env_storage_capacity(built,
                                              &env_storage,
                                              &env_storage_count,
                                              needed_count) != 0) {
        built->ld_preload_action = "env-too-large";
        return -1;
    }

    for (size_t i = 0; i < base_count; i++) {
        if (should_touch_ld && peak_exec_env_entry_matches(base_env[i], "LD_PRELOAD")) {
            if (ld_written) {
                built->changed = 1;
                continue;
            }
            env_storage[out++] = new_ld_changed ? ld_storage : base_env[i];
            if (new_ld_changed) {
                built->changed = 1;
            }
            ld_written = 1;
        } else {
            env_storage[out++] = base_env[i];
        }
    }

    if (should_touch_ld && ld_entry_count == 0 && new_ld_changed) {
        env_storage[out++] = ld_storage;
        built->changed = 1;
    }

    if (propagate_peak_env && environ != NULL) {
        for (int priority_pass = 1; priority_pass >= 0; priority_pass--) {
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
                if (peak_exec_peak_name_is_priority(name) != priority_pass) {
                    continue;
                }
                if (peak_exec_env_has_name(base_env, name) ||
                    peak_exec_env_storage_has_name(env_storage, out, name)) {
                    continue;
                }
                if (out + 1 >= env_storage_count) {
                    if (priority_pass == 0) {
                        partial_peak_env = 1;
                        goto peak_env_done;
                    }
                    built->ld_preload_action = "env-too-large";
                    errno = E2BIG;
                    return -1;
                }
                env_storage[out++] = *scan;
                built->changed = 1;
            }
        }
    }

peak_env_done:
    if (partial_peak_env) {
        built->ld_preload_action = "partial-peak-env";
    }
    env_storage[out] = NULL;
    built->envp = env_storage;
    return 0;
}

static void
peak_exec_mark_after_fork_child(void)
{
    peak_exec_safe_child_pid = getpid();
}

static int
peak_exec_in_fork_like_child(void)
{
    pid_t self = getpid();
    if (peak_exec_owner_pid > 0 && self != peak_exec_owner_pid) {
        return 1;
    }
    if (peak_exec_safe_child_pid == self) {
        return 1;
    }
    return 0;
}

static int
peak_exec_in_shared_vm_child(void)
{
    pid_t self = getpid();

    /*
     * pthread_atfork() and the syscall fork/clone bridges mark ordinary fork
     * children with that child's PID. A later vfork/CLONE_VM child inherits the
     * marker but gets a different PID, so it is still treated as shared-VM.
     */
    return peak_exec_owner_pid > 0 &&
           self != peak_exec_owner_pid &&
           peak_exec_safe_child_pid != self;
}

__attribute__((constructor)) static void
peak_exec_register_atfork(void)
{
    char* libpeak_path;
    Dl_info info;

    peak_exec_owner_pid = getpid();
    long page_size = sysconf(_SC_PAGESIZE);
    peak_exec_cached_page_size = page_size > 0 ? (size_t)page_size : 4096U;
    (void)peak_exec_secure_mode();
    memset(&info, 0, sizeof(info));
    if (dladdr((void*)&peak_checkpoint_for_exec, &info) != 0 &&
        info.dli_fname != NULL &&
        info.dli_fname[0] != '\0') {
        snprintf(peak_exec_cached_libpeak_original_path,
                 sizeof(peak_exec_cached_libpeak_original_path),
                 "%s",
                 info.dli_fname);
        libpeak_path = peak_exec_realpath_or_dup(info.dli_fname);
    } else {
        libpeak_path = peak_exec_libpeak_path();
    }
    if (libpeak_path != NULL) {
        struct stat st;
        snprintf(peak_exec_cached_libpeak_path,
                 sizeof(peak_exec_cached_libpeak_path),
                 "%s",
                 libpeak_path);
        if (stat(libpeak_path, &st) == 0) {
            peak_exec_cached_libpeak_dev = st.st_dev;
            peak_exec_cached_libpeak_ino = st.st_ino;
            peak_exec_cached_libpeak_identity_valid = 1;
        }
        free(libpeak_path);
    }
    (void)peak_real_posix_spawn();
    (void)peak_real_posix_spawnp();
    (void)peak_real_clone();
    (void)pthread_atfork(NULL, NULL, peak_exec_mark_after_fork_child);
}

static void
peak_exec_trace_csv_field(FILE* fp, const char* value)
{
    fputc('"', fp);
    if (value != NULL) {
        for (const char* p = value; *p != '\0'; p++) {
            if (*p == '"') {
                fputc('"', fp);
            }
            fputc(*p, fp);
        }
    }
    fputc('"', fp);
}

static FILE*
peak_exec_trace_open_file(const char* trace_path, int* out_fd, off_t* out_size)
{
    int open_flags = O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK;
    int fd;
    struct stat st;
    FILE* fp;

    if (out_fd != NULL) {
        *out_fd = -1;
    }
    if (out_size != NULL) {
        *out_size = 0;
    }

#if defined(O_CLOEXEC)
    open_flags |= O_CLOEXEC;
#endif
    fd = open(trace_path, open_flags, 0600);
    if (fd < 0) {
        return NULL;
    }

    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return NULL;
    }

    fp = fdopen(fd, "a");
    if (fp == NULL) {
        (void)flock(fd, LOCK_UN);
        close(fd);
        return NULL;
    }

    if (out_fd != NULL) {
        *out_fd = fd;
    }
    if (out_size != NULL) {
        *out_size = st.st_size;
    }
    return fp;
}

static void
peak_exec_trace(const char* event,
                const char* path,
                const PeakExecAttempt* attempt,
                const char* exec_result,
                int exec_errno)
{
    int saved_errno = errno;
    const char* trace_path = getenv(PEAK_EXEC_TRACE_PATH_ENV);
    FILE* fp;
    int trace_fd = -1;
    off_t trace_size = 0;
    struct timespec ts;
    long nsec = 0;
    time_t sec;

    if (trace_path == NULL || trace_path[0] == '\0') {
        errno = saved_errno;
        return;
    }

    fp = peak_exec_trace_open_file(trace_path, &trace_fd, &trace_size);
    if (fp == NULL) {
        errno = saved_errno;
        return;
    }
    if (trace_size == 0) {
        fputs("time,event,path,pid,checkpoint_enabled,checkpoint_result,"
              "chain_enabled,ld_preload_action,exec_result,errno\n",
              fp);
    }

    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        sec = ts.tv_sec;
        nsec = ts.tv_nsec;
    } else {
        sec = time(NULL);
    }

    fprintf(fp, "%lld.%09ld,", (long long)sec, nsec);
    peak_exec_trace_csv_field(fp, event);
    fputc(',', fp);
    peak_exec_trace_csv_field(fp, path != NULL ? path : "");
    fprintf(fp,
            ",%ld,%d,%d,%d,",
            (long)getpid(),
            attempt != NULL ? attempt->checkpoint_enabled : 0,
            attempt != NULL ? attempt->checkpoint_result : 0,
            attempt != NULL ? attempt->chain_enabled : 0);
    peak_exec_trace_csv_field(fp,
                              attempt != NULL ? attempt->ld_preload_action
                                               : "unknown");
    fputc(',', fp);
    peak_exec_trace_csv_field(fp, exec_result != NULL ? exec_result : "");
    fprintf(fp, ",%d\n", exec_errno);
    fflush(fp);
    if (trace_fd >= 0) {
        (void)flock(trace_fd, LOCK_UN);
    }
    fclose(fp);
    errno = saved_errno;
}

static void
peak_exec_trace_append_char(char* buffer, size_t capacity, size_t* out, char ch)
{
    if (*out + 1 < capacity) {
        buffer[*out] = ch;
        (*out)++;
        buffer[*out] = '\0';
    }
}

static void
peak_exec_trace_append_long(char* buffer,
                            size_t capacity,
                            size_t* out,
                            long value)
{
    char digits[32];
    size_t used = 0;
    unsigned long magnitude;

    if (value < 0) {
        peak_exec_trace_append_char(buffer, capacity, out, '-');
        magnitude = (unsigned long)(-(value + 1)) + 1UL;
    } else {
        magnitude = (unsigned long)value;
    }

    do {
        digits[used++] = (char)('0' + (magnitude % 10UL));
        magnitude /= 10UL;
    } while (magnitude != 0 && used < sizeof(digits));

    while (used > 0) {
        peak_exec_trace_append_char(buffer, capacity, out, digits[--used]);
    }
}

static void
peak_exec_trace_append_csv_field(char* buffer,
                                 size_t capacity,
                                 size_t* out,
                                 const char* value)
{
    peak_exec_trace_append_char(buffer, capacity, out, '"');
    if (value != NULL) {
        for (const char* p = value; *p != '\0'; p++) {
            if (*p == '"') {
                peak_exec_trace_append_char(buffer, capacity, out, '"');
            }
            peak_exec_trace_append_char(buffer, capacity, out, *p);
        }
    }
    peak_exec_trace_append_char(buffer, capacity, out, '"');
}

static ssize_t
peak_exec_raw_write_all(int fd, const char* buffer, size_t length)
{
    size_t written = 0;

    while (written < length) {
        long result = peak_exec_call_raw_syscall6(SYS_write,
                                             fd,
                                             (long)(buffer + written),
                                             (long)(length - written),
                                             0,
                                             0,
                                             0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            errno = EIO;
            return -1;
        }
        written += (size_t)result;
    }
    return (ssize_t)written;
}

static int
peak_exec_raw_fd_stat(int fd, struct stat* st)
{
    if (st == NULL) {
        return -1;
    }
    memset(st, 0, sizeof(*st));
#if defined(SYS_newfstatat) && defined(AT_EMPTY_PATH)
    if (peak_exec_call_raw_syscall6(SYS_newfstatat,
                               fd,
                               (long)"",
                               (long)st,
                               AT_EMPTY_PATH,
                               0,
                               0) == 0) {
        return 0;
    }
#elif defined(SYS_fstat)
    if (peak_exec_call_raw_syscall6(SYS_fstat,
                               fd,
                               (long)st,
                               0,
                               0,
                               0,
                               0) == 0) {
        return 0;
    }
#endif
    return -1;
}

static void
peak_exec_trace_noalloc(const char* event,
                        const char* path,
                        const PeakExecAttempt* attempt,
                        const char* exec_result,
                        int exec_errno)
{
#if defined(__linux__) && defined(SYS_openat) && defined(SYS_write) && \
    defined(SYS_close)
    int saved_errno = errno;
    const char* trace_path =
        peak_exec_current_env_get_direct(PEAK_EXEC_TRACE_PATH_ENV);
    int fd;
    int open_flags = O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK;
    struct timespec ts;
    struct stat st;
    long sec = 0;
    long nsec = 0;
    size_t out = 0;
    static const char header[] =
        "time,event,path,pid,checkpoint_enabled,checkpoint_result,"
        "chain_enabled,ld_preload_action,exec_result,errno\n";

    if (trace_path == NULL || trace_path[0] == '\0') {
        return;
    }

#if defined(O_CLOEXEC)
    open_flags |= O_CLOEXEC;
#endif
    fd = (int)peak_exec_call_raw_syscall6(SYS_openat,
                                     AT_FDCWD,
                                     (long)trace_path,
                                     open_flags,
                                     0600,
                                     0,
                                     0);
    if (fd < 0) {
        errno = saved_errno;
        return;
    }

    if (peak_exec_raw_fd_stat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)peak_exec_call_raw_syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
        errno = saved_errno;
        return;
    }

#if defined(SYS_flock)
    if (peak_exec_call_raw_syscall6(SYS_flock,
                               fd,
                               LOCK_EX | LOCK_NB,
                               0,
                               0,
                               0,
                               0) != 0) {
        (void)peak_exec_call_raw_syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
        errno = saved_errno;
        return;
    }
#endif
    if (st.st_size == 0) {
        (void)peak_exec_raw_write_all(fd, header, sizeof(header) - 1);
    }

#if defined(SYS_clock_gettime)
    if (peak_exec_call_raw_syscall6(SYS_clock_gettime,
                               CLOCK_REALTIME,
                               (long)&ts,
                               0,
                               0,
                               0,
                               0) == 0) {
        sec = (long)ts.tv_sec;
        nsec = ts.tv_nsec;
    }
#endif

    peak_exec_noalloc_trace_row[0] = '\0';
    peak_exec_trace_append_long(peak_exec_noalloc_trace_row,
                                sizeof(peak_exec_noalloc_trace_row),
                                &out,
                                sec);
    peak_exec_trace_append_char(peak_exec_noalloc_trace_row,
                                sizeof(peak_exec_noalloc_trace_row),
                                &out,
                                '.');
    for (long div = 100000000L; div > 0; div /= 10) {
        peak_exec_trace_append_char(
            peak_exec_noalloc_trace_row,
            sizeof(peak_exec_noalloc_trace_row),
            &out,
            (char)('0' + ((nsec / div) % 10)));
    }
    peak_exec_trace_append_char(peak_exec_noalloc_trace_row,
                                sizeof(peak_exec_noalloc_trace_row),
                                &out,
                                ',');
    peak_exec_trace_append_csv_field(peak_exec_noalloc_trace_row,
                                     sizeof(peak_exec_noalloc_trace_row),
                                     &out,
                                     event);
    peak_exec_trace_append_char(peak_exec_noalloc_trace_row,
                                sizeof(peak_exec_noalloc_trace_row),
                                &out,
                                ',');
    peak_exec_trace_append_csv_field(peak_exec_noalloc_trace_row,
                                     sizeof(peak_exec_noalloc_trace_row),
                                     &out,
                                     path != NULL ? path : "");
    peak_exec_trace_append_char(peak_exec_noalloc_trace_row,
                                sizeof(peak_exec_noalloc_trace_row),
                                &out,
                                ',');
    peak_exec_trace_append_long(peak_exec_noalloc_trace_row,
                                sizeof(peak_exec_noalloc_trace_row),
                                &out,
                                (long)getpid());
    peak_exec_trace_append_char(peak_exec_noalloc_trace_row,
                                sizeof(peak_exec_noalloc_trace_row),
                                &out,
                                ',');
    peak_exec_trace_append_long(
        peak_exec_noalloc_trace_row,
        sizeof(peak_exec_noalloc_trace_row),
        &out,
        attempt != NULL ? attempt->checkpoint_enabled : 0);
    peak_exec_trace_append_char(peak_exec_noalloc_trace_row,
                                sizeof(peak_exec_noalloc_trace_row),
                                &out,
                                ',');
    peak_exec_trace_append_long(
        peak_exec_noalloc_trace_row,
        sizeof(peak_exec_noalloc_trace_row),
        &out,
        attempt != NULL ? attempt->checkpoint_result : 0);
    peak_exec_trace_append_char(peak_exec_noalloc_trace_row,
                                sizeof(peak_exec_noalloc_trace_row),
                                &out,
                                ',');
    peak_exec_trace_append_long(
        peak_exec_noalloc_trace_row,
        sizeof(peak_exec_noalloc_trace_row),
        &out,
        attempt != NULL ? attempt->chain_enabled : 0);
    peak_exec_trace_append_char(peak_exec_noalloc_trace_row,
                                sizeof(peak_exec_noalloc_trace_row),
                                &out,
                                ',');
    peak_exec_trace_append_csv_field(
        peak_exec_noalloc_trace_row,
        sizeof(peak_exec_noalloc_trace_row),
        &out,
        attempt != NULL ? attempt->ld_preload_action : "unknown");
    peak_exec_trace_append_char(peak_exec_noalloc_trace_row,
                                sizeof(peak_exec_noalloc_trace_row),
                                &out,
                                ',');
    peak_exec_trace_append_csv_field(peak_exec_noalloc_trace_row,
                                     sizeof(peak_exec_noalloc_trace_row),
                                     &out,
                                     exec_result != NULL ? exec_result : "");
    peak_exec_trace_append_char(peak_exec_noalloc_trace_row,
                                sizeof(peak_exec_noalloc_trace_row),
                                &out,
                                ',');
    peak_exec_trace_append_long(peak_exec_noalloc_trace_row,
                                sizeof(peak_exec_noalloc_trace_row),
                                &out,
                                exec_errno);
    peak_exec_trace_append_char(peak_exec_noalloc_trace_row,
                                sizeof(peak_exec_noalloc_trace_row),
                                &out,
                                '\n');
    (void)peak_exec_raw_write_all(fd, peak_exec_noalloc_trace_row, out);
#if defined(SYS_flock)
    (void)peak_exec_call_raw_syscall6(SYS_flock, fd, LOCK_UN, 0, 0, 0, 0);
#endif
    (void)peak_exec_call_raw_syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
    errno = saved_errno;
#else
    (void)event;
    (void)path;
    (void)attempt;
    (void)exec_result;
    (void)exec_errno;
#endif
}

static void
peak_exec_trace_if_safe(const char* event,
                        const char* path,
                        const PeakExecAttempt* attempt,
                        const char* exec_result,
                        int exec_errno)
{
    if (peak_exec_in_shared_vm_child() || peak_exec_in_fork_like_child()) {
        peak_exec_trace_noalloc(event, path, attempt, exec_result, exec_errno);
    } else {
        peak_exec_trace(event, path, attempt, exec_result, exec_errno);
    }
}

static void
peak_exec_trace_preflight_failed(const char* safe_path, int exec_errno)
{
    PeakExecAttempt attempt;

    memset(&attempt, 0, sizeof(attempt));
    attempt.checkpoint_enabled = 0;
    attempt.chain_enabled =
        (peak_exec_in_shared_vm_child() || peak_exec_in_fork_like_child())
            ? peak_exec_current_env_default_true_direct(PEAK_EXEC_CHAIN_ENV)
            : peak_exec_env_default_true(PEAK_EXEC_CHAIN_ENV);
    attempt.ld_preload_action =
        attempt.chain_enabled ? "preflight-failed" : "disabled";
    attempt.checkpoint_result = 0;

    peak_exec_trace_if_safe("exec-before", safe_path, &attempt, "", 0);
    peak_exec_trace_if_safe("exec-failed",
                            safe_path,
                            &attempt,
                            "failed",
                            exec_errno);
}

static peak_execve_fn
peak_real_execve(void)
{
    static peak_execve_fn fn;
    if (fn == NULL) {
        fn = (peak_execve_fn)dlsym(RTLD_NEXT, "execve");
    }
    return fn;
}

static peak_fork_fn
peak_real_fork(void)
{
    static peak_fork_fn fn;
    if (fn == NULL) {
        fn = (peak_fork_fn)dlsym(RTLD_NEXT, "fork");
    }
    return fn;
}

static peak_clone_fn
peak_real_clone(void)
{
    static peak_clone_fn fn;
    static int looked_up;
    if (!looked_up) {
        fn = (peak_clone_fn)dlsym(RTLD_NEXT, "clone");
        looked_up = 1;
    }
    return fn;
}

static int
peak_clone_needs_parent_tid(int flags)
{
#if defined(CLONE_PARENT_SETTID) || defined(CLONE_PIDFD)
    int mask = 0;
#if defined(CLONE_PARENT_SETTID)
    mask |= CLONE_PARENT_SETTID;
#endif
#if defined(CLONE_PIDFD)
    mask |= CLONE_PIDFD;
#endif
    return (flags & mask) != 0;
#else
    (void)flags;
    return 0;
#endif
}

static int
peak_clone_needs_tls(int flags)
{
#if defined(CLONE_SETTLS)
    return (flags & CLONE_SETTLS) != 0;
#else
    (void)flags;
    return 0;
#endif
}

static int
peak_clone_needs_child_tid(int flags)
{
#if defined(CLONE_CHILD_SETTID) || defined(CLONE_CHILD_CLEARTID)
    int mask = 0;
#if defined(CLONE_CHILD_SETTID)
    mask |= CLONE_CHILD_SETTID;
#endif
#if defined(CLONE_CHILD_CLEARTID)
    mask |= CLONE_CHILD_CLEARTID;
#endif
    return (flags & mask) != 0;
#else
    (void)flags;
    return 0;
#endif
}

static int
peak_clone_start_trampoline(void* arg)
{
    PeakCloneStart* start = (PeakCloneStart*)arg;
    int (*fn)(void*) = start->fn;
    void* user_arg = start->arg;

    peak_exec_mark_after_fork_child();
    return fn(user_arg);
}

static int
peak_call_clone_forward(peak_clone_fn fn,
                        int (*child_fn)(void*),
                        void* child_stack,
                        int flags,
                        void* arg,
                        void* parent_tid,
                        void* tls,
                        void* child_tid,
                        int optional_count)
{
    if (optional_count >= 3) {
        return fn(child_fn, child_stack, flags, arg, parent_tid, tls, child_tid);
    }
    if (optional_count >= 2) {
        return fn(child_fn, child_stack, flags, arg, parent_tid, tls);
    }
    if (optional_count >= 1) {
        return fn(child_fn, child_stack, flags, arg, parent_tid);
    }
    return fn(child_fn, child_stack, flags, arg);
}

static pid_t
peak_call_fork_raw(void)
{
    peak_fork_fn fn = peak_real_fork();
    if (fn != NULL) {
        return fn();
    }
#if defined(__linux__) && defined(SYS_fork)
    return (pid_t)peak_exec_call_raw_syscall6(SYS_fork, 0, 0, 0, 0, 0, 0);
#else
    errno = ENOSYS;
    return -1;
#endif
}

static int
peak_call_execve_raw(const char* path, char* const argv[], char* const envp[])
{
#if defined(__linux__) && defined(SYS_execve)
    if (peak_exec_in_fork_like_child() || peak_exec_in_shared_vm_child()) {
        return (int)peak_exec_call_raw_syscall6(SYS_execve,
                                           (long)path,
                                           (long)argv,
                                           (long)envp,
                                           0,
                                           0,
                                           0);
    }
#endif
    peak_execve_fn fn = peak_real_execve();
    if (fn != NULL) {
        return fn(path, argv, envp);
    }
#if defined(__linux__) && defined(SYS_execve)
    return (int)peak_exec_call_raw_syscall6(SYS_execve,
                                       (long)path,
                                       (long)argv,
                                       (long)envp,
                                       0,
                                       0,
                                       0);
#else
    errno = ENOSYS;
    return -1;
#endif
}

__attribute__((visibility("default"))) pid_t
fork(void)
{
    pid_t result = peak_call_fork_raw();
    if (result == 0) {
        peak_exec_mark_after_fork_child();
    }
    return result;
}

__attribute__((visibility("default"))) int
clone(int (*fn)(void*), void* child_stack, int flags, void* arg, ...)
{
    peak_clone_fn real_clone = peak_real_clone();
    void* parent_tid = NULL;
    void* tls = NULL;
    void* child_tid = NULL;
    int optional_count = 0;
    PeakCloneStart* start = NULL;
    PeakCloneStart stack_start;
    int in_fork_like_child = peak_exec_in_fork_like_child();
    int result;
    int saved_errno;

    if (real_clone == NULL) {
        errno = ENOSYS;
        return -1;
    }

    if (peak_clone_needs_parent_tid(flags) ||
        peak_clone_needs_tls(flags) ||
        peak_clone_needs_child_tid(flags)) {
        optional_count = 1;
    }
    if (peak_clone_needs_tls(flags) || peak_clone_needs_child_tid(flags)) {
        optional_count = 2;
    }
    if (peak_clone_needs_child_tid(flags)) {
        optional_count = 3;
    }

    va_list ap;
    va_start(ap, arg);
    if (optional_count >= 1) {
        parent_tid = va_arg(ap, void*);
    }
    if (optional_count >= 2) {
        tls = va_arg(ap, void*);
    }
    if (optional_count >= 3) {
        child_tid = va_arg(ap, void*);
    }
    va_end(ap);

#if defined(CLONE_VM)
    if ((flags & CLONE_VM) != 0) {
        return peak_call_clone_forward(real_clone,
                                       fn,
                                       child_stack,
                                       flags,
                                       arg,
                                       parent_tid,
                                       tls,
                                       child_tid,
                                       optional_count);
    }
#endif

    if (fn == NULL || child_stack == NULL) {
        return peak_call_clone_forward(real_clone,
                                       fn,
                                       child_stack,
                                       flags,
                                       arg,
                                       parent_tid,
                                       tls,
                                       child_tid,
                                       optional_count);
    }

    if (in_fork_like_child) {
        stack_start.fn = fn;
        stack_start.arg = arg;
        return peak_call_clone_forward(real_clone,
                                       peak_clone_start_trampoline,
                                       child_stack,
                                       flags,
                                       &stack_start,
                                       parent_tid,
                                       tls,
                                       child_tid,
                                       optional_count);
    }

    start = malloc(sizeof(*start));
    if (start == NULL) {
        return peak_call_clone_forward(real_clone,
                                       fn,
                                       child_stack,
                                       flags,
                                       arg,
                                       parent_tid,
                                       tls,
                                       child_tid,
                                       optional_count);
    }
    start->fn = fn;
    start->arg = arg;
    result = peak_call_clone_forward(real_clone,
                                     peak_clone_start_trampoline,
                                     child_stack,
                                     flags,
                                     start,
                                     parent_tid,
                                     tls,
                                     child_tid,
                                     optional_count);
    saved_errno = errno;
    if (result != 0) {
        free(start);
    }
    errno = saved_errno;
    return result;
}

static peak_execvpe_fn
peak_real_execvpe(void)
{
    static peak_execvpe_fn fn;
    static int looked_up;
    if (!looked_up) {
        fn = (peak_execvpe_fn)dlsym(RTLD_NEXT, "execvpe");
        looked_up = 1;
    }
#if defined(PEAK_ENABLE_TEST_HOOKS)
    if (peak_exec_env_default_true("PEAK_TEST_EXECVPE_FALLBACK") &&
        getenv("PEAK_TEST_EXECVPE_FALLBACK") != NULL) {
        return NULL;
    }
#endif
    return fn;
}

static int
peak_exec_prepare(const char* trace_path,
                  char* const argv[],
                  char* const envp[],
                  PeakExecEnv* built_env,
                  PeakExecAttempt* attempt,
                  int allow_checkpoint)
{
    int build_result;

    memset(attempt, 0, sizeof(*attempt));
    attempt->checkpoint_enabled =
        allow_checkpoint &&
        !peak_exec_in_shared_vm_child() &&
        peak_exec_env_default_true(PEAK_EXEC_CHECKPOINT_ENV);
    attempt->chain_enabled =
        peak_exec_env_default_true(PEAK_EXEC_CHAIN_ENV);
    attempt->ld_preload_action =
        attempt->chain_enabled ? "unchanged" : "disabled";
    attempt->checkpoint_result = 0;

    peak_exec_trace("exec-before", trace_path, attempt, "", 0);
    if (attempt->checkpoint_enabled &&
        peak_runtime_is_active_for_checkpoint()) {
        attempt->checkpoint_result = peak_checkpoint_for_exec(trace_path, argv);
        peak_exec_trace(attempt->checkpoint_result == 0 ?
                            "exec-checkpoint-ok" :
                            "exec-checkpoint-failed",
                        trace_path,
                        attempt,
                        "",
                        attempt->checkpoint_result == 0 ? 0 : errno);
    }

    build_result =
        peak_exec_build_env(envp,
                            attempt->chain_enabled,
                            peak_exec_env_default_true(
                                PEAK_EXEC_PROPAGATE_PEAK_ENV_ENV),
                            built_env);
    attempt->ld_preload_action = built_env->ld_preload_action;
    if (build_result != 0) {
        peak_exec_trace("exec-env-unchanged", trace_path, attempt, "", errno);
        peak_exec_env_clear(built_env);
        return -1;
    }

    peak_exec_trace(built_env->changed ? "exec-env-injected"
                                       : "exec-env-unchanged",
                    trace_path,
                    attempt,
                    "",
                    0);
    return 0;
}

static int
peak_exec_prepare_checked(const char* trace_path,
                          char* const argv[],
                          char* const envp[],
                          PeakExecEnv* built_env,
                          PeakExecAttempt* attempt,
                          int allow_checkpoint,
                          char** stack_envp,
                          size_t stack_envp_count,
                          char* stack_ld_entry,
                          size_t stack_ld_entry_size)
{
    int in_shared_vm_child = peak_exec_in_shared_vm_child();
    int in_fork_like_child = peak_exec_in_fork_like_child();
    int use_noalloc_path = in_shared_vm_child || in_fork_like_child;

    if (!use_noalloc_path) {
        return peak_exec_prepare(trace_path,
                                 argv,
                                 envp,
                                 built_env,
                                 attempt,
                                 allow_checkpoint);
    }

    memset(attempt, 0, sizeof(*attempt));
    attempt->checkpoint_enabled =
        allow_checkpoint &&
        !in_shared_vm_child &&
        peak_exec_current_env_default_true_direct(PEAK_EXEC_CHECKPOINT_ENV);
    attempt->chain_enabled =
        peak_exec_current_env_default_true_direct(PEAK_EXEC_CHAIN_ENV);
    attempt->ld_preload_action =
        attempt->chain_enabled ? "unchanged" : "disabled";
    attempt->checkpoint_result = 0;

    peak_exec_trace_noalloc("exec-before", trace_path, attempt, "", 0);
    if (attempt->checkpoint_enabled &&
        peak_runtime_is_active_for_checkpoint()) {
        attempt->checkpoint_result =
            peak_checkpoint_for_exec_trylock(trace_path, argv);
        peak_exec_trace_noalloc(attempt->checkpoint_result == 0 ?
                                    "exec-checkpoint-ok" :
                                    "exec-checkpoint-failed",
                                trace_path,
                                attempt,
                                "",
                                attempt->checkpoint_result == 0 ? 0 : errno);
    }

    int build_result = peak_exec_build_env_noalloc(
        envp,
        attempt->chain_enabled,
        peak_exec_current_env_default_true_direct(
            PEAK_EXEC_PROPAGATE_PEAK_ENV_ENV),
        built_env,
        stack_envp,
        stack_envp_count,
        stack_ld_entry,
        stack_ld_entry_size);
    attempt->ld_preload_action = built_env->ld_preload_action;
    if (build_result != 0) {
        peak_exec_trace_noalloc("exec-env-unchanged",
                                trace_path,
                                attempt,
                                "",
                                errno);
        peak_exec_env_clear(built_env);
        return -1;
    }
    peak_exec_trace_noalloc(built_env->changed ? "exec-env-injected"
                                               : "exec-env-unchanged",
                            trace_path,
                            attempt,
                            "",
                            0);
    return 0;
}

static char* const*
peak_exec_env_to_use(PeakExecEnv* built_env, char* const original_envp[])
{
    if (built_env != NULL && built_env->envp != NULL) {
        return built_env->envp;
    }
    return original_envp;
}

static int
peak_execve_prepared(const char* path,
                     char* const argv[],
                     char* const envp[],
                     const char* trace_path)
{
    PeakExecEnv built_env;
    PeakExecAttempt attempt;
    char* const* exec_envp;
    int saved_errno;
    int use_wrapper_depth = !peak_exec_in_shared_vm_child();
    int entry_errno = errno;

    if (use_wrapper_depth && peak_exec_wrapper_depth > 0) {
        return peak_call_execve_raw(path, argv, envp);
    }
    if (peak_exec_syscall_args_readable(path, argv, envp) != 0) {
#if defined(__linux__) && defined(SYS_execve)
        saved_errno = errno;
        if (peak_exec_preflight_error_is_conclusive(saved_errno)) {
            int result = (int)peak_exec_call_raw_syscall6(SYS_execve,
                                                     (long)path,
                                                     (long)argv,
                                                     (long)envp,
                                                     0,
                                                     0,
                                                     0);
            saved_errno = errno;
            peak_exec_trace_preflight_failed("", saved_errno);
            errno = saved_errno;
            return result;
        }
#else
        saved_errno = errno;
        if (peak_exec_preflight_error_is_conclusive(saved_errno)) {
            peak_exec_trace_preflight_failed("", saved_errno);
            errno = saved_errno;
            return -1;
        }
#endif
        errno = entry_errno;
        int result = peak_call_execve_raw(path, argv, envp);
        saved_errno = errno;
        peak_exec_trace_preflight_failed("", saved_errno);
        errno = saved_errno;
        return result;
    }

    if (use_wrapper_depth) {
        peak_exec_wrapper_depth++;
    }
    if (peak_exec_prepare_checked(trace_path,
                                  argv,
                                  envp,
                                  &built_env,
                                  &attempt,
                                  1,
                                  peak_exec_noalloc_envp,
                                  PEAK_EXEC_MAX_ENV_ENTRIES,
                                  peak_exec_noalloc_ld_entry,
                                  sizeof(peak_exec_noalloc_ld_entry)) != 0) {
        exec_envp = envp;
    } else {
        exec_envp = peak_exec_env_to_use(&built_env, envp);
    }

    (void)peak_call_execve_raw(path, argv, (char* const*)exec_envp);
    saved_errno = errno;
    peak_exec_trace_if_safe("exec-failed",
                            trace_path,
                            &attempt,
                            "failed",
                            saved_errno);
    peak_exec_env_clear(&built_env);
    if (use_wrapper_depth) {
        peak_exec_wrapper_depth--;
    }
    errno = saved_errno;
    return -1;
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
peak_execve_handle_enoexec(const char* file,
                           char* const argv[],
                           char* const envp[])
{
    char** shell_argv = peak_exec_build_shell_argv(file, argv);
    int saved_errno;

    if (shell_argv == NULL) {
        errno = ENOMEM;
        return -1;
    }
    (void)peak_call_execve_raw("/bin/sh", shell_argv, envp);
    saved_errno = errno;
    free(shell_argv);
    errno = saved_errno;
    return -1;
}

static int
peak_execve_handle_enoexec_noalloc(const char* file,
                                   char* const argv[],
                                   char* const envp[])
{
    size_t argc = 0;
    size_t shell_argc;

    if (argv != NULL) {
        while (argv[argc] != NULL) {
            argc++;
            if (argc >= PEAK_EXEC_MAX_VARARGS) {
                errno = E2BIG;
                return -1;
            }
        }
    }

    peak_exec_noalloc_shell_argv[0] = (char*)"/bin/sh";
    peak_exec_noalloc_shell_argv[1] = (char*)file;
    for (size_t i = 1; i < argc; i++) {
        peak_exec_noalloc_shell_argv[i + 1] = argv[i];
    }
    shell_argc = argc > 0 ? argc + 1 : 2;
    peak_exec_noalloc_shell_argv[shell_argc] = NULL;
    return peak_call_execve_raw("/bin/sh", peak_exec_noalloc_shell_argv, envp);
}

static int
peak_execvpe_fallback_prepared(const char* file,
                               char* const argv[],
                               char* const envp[],
                               const char* trace_path)
{
    PeakExecEnv built_env = { 0 };
    PeakExecAttempt attempt = { 0 };
    char* const* exec_envp;
    const char* path_value;
    int saved_errno = ENOENT;
    int saw_eacces = 0;
    int use_wrapper_depth = !peak_exec_in_shared_vm_child();
    int allow_allocation = !peak_exec_in_shared_vm_child() &&
        !peak_exec_in_fork_like_child();
    int entry_errno = errno;
    int preflight_unknown = 0;
    char candidate_stack[PATH_MAX];

    if (use_wrapper_depth && peak_exec_wrapper_depth > 0) {
        return peak_call_execve_raw(file, argv, envp);
    }
    if (peak_exec_syscall_args_readable(file, argv, envp) != 0) {
        saved_errno = errno;
        if (peak_exec_preflight_error_is_conclusive(saved_errno)) {
            peak_exec_trace_preflight_failed("", saved_errno);
            errno = saved_errno;
            return -1;
        }
        errno = entry_errno;
        preflight_unknown = 1;
        peak_exec_trace_preflight_failed("", saved_errno);
    }
    if (file == NULL || file[0] == '\0') {
        errno = ENOENT;
        return -1;
    }

    if (use_wrapper_depth) {
        peak_exec_wrapper_depth++;
    }
    if (preflight_unknown) {
        exec_envp = envp;
    } else if (peak_exec_prepare_checked(trace_path,
                                         argv,
                                         envp,
                                         &built_env,
                                         &attempt,
                                         1,
                                         peak_exec_noalloc_envp,
                                         PEAK_EXEC_MAX_ENV_ENTRIES,
                                         peak_exec_noalloc_ld_entry,
                                         sizeof(peak_exec_noalloc_ld_entry)) != 0) {
        exec_envp = envp;
    } else {
        exec_envp = peak_exec_env_to_use(&built_env, envp);
    }

    if (strchr(file, '/') != NULL) {
        (void)peak_call_execve_raw(file, argv, (char* const*)exec_envp);
        saved_errno = errno;
        if (saved_errno == ENOEXEC) {
            if (allow_allocation) {
                (void)peak_execve_handle_enoexec(file,
                                                 argv,
                                                 (char* const*)exec_envp);
            } else {
                (void)peak_execve_handle_enoexec_noalloc(
                    file,
                    argv,
                    (char* const*)exec_envp);
            }
            saved_errno = errno;
        }
        goto failed;
    }

    /*
     * The fallback path is used only when libc has no execvpe() symbol (or in
     * tests). PEAK's exec-chain contract searches the environment that will be
     * passed to the child after PEAK injection/propagation, rather than the
     * caller's current environ.
     */
    path_value = preflight_unknown ?
        peak_exec_current_env_get_direct("PATH") :
        peak_exec_env_get((char* const*)exec_envp, "PATH");
    if (path_value == NULL) {
        path_value = PEAK_EXEC_DEFAULT_PATH;
    }

    const char* cursor = path_value;
    while (1) {
        const char* start = cursor;
        size_t dir_len;
        size_t file_len = strlen(file);
        char* candidate;
        int candidate_allocated = 0;

        while (*cursor != '\0' && *cursor != ':') {
            cursor++;
        }
        dir_len = (size_t)(cursor - start);
        if (dir_len + (dir_len > 0 ? 1 : 0) + file_len + 1 >
            sizeof(candidate_stack)) {
            saved_errno = ENAMETOOLONG;
            if (*cursor == '\0') {
                break;
            }
            cursor++;
            continue;
        }
        if (allow_allocation) {
            candidate = malloc(dir_len + 1 + file_len + 1);
            if (candidate == NULL) {
                saved_errno = ENOMEM;
                break;
            }
            candidate_allocated = 1;
        } else {
            candidate = candidate_stack;
        }
        if (dir_len == 0) {
            memcpy(candidate, file, file_len + 1);
        } else {
            memcpy(candidate, start, dir_len);
            candidate[dir_len] = '/';
            memcpy(candidate + dir_len + 1, file, file_len + 1);
        }

        (void)peak_call_execve_raw(candidate, argv, (char* const*)exec_envp);
        saved_errno = errno;
        if (saved_errno == ENOEXEC) {
            if (allow_allocation) {
                (void)peak_execve_handle_enoexec(candidate,
                                                 argv,
                                                 (char* const*)exec_envp);
            } else {
                (void)peak_execve_handle_enoexec_noalloc(
                    candidate,
                    argv,
                    (char* const*)exec_envp);
            }
            saved_errno = errno;
        }
        if (candidate_allocated) {
            free(candidate);
        }

        if (saved_errno == EACCES) {
            saw_eacces = 1;
        } else if (saved_errno != ENOENT && saved_errno != ENOTDIR) {
            break;
        }

        if (*cursor == '\0') {
            saved_errno = saw_eacces ? EACCES : saved_errno;
            break;
        }
        cursor++;
    }

failed:
    peak_exec_trace_if_safe("exec-failed",
                            trace_path,
                            &attempt,
                            "failed",
                            saved_errno);
    peak_exec_env_clear(&built_env);
    if (use_wrapper_depth) {
        peak_exec_wrapper_depth--;
    }
    errno = saved_errno;
    return -1;
}

static int
peak_execvpe_prepared(const char* file,
                      char* const argv[],
                      char* const envp[],
                      const char* trace_path)
{
    peak_execvpe_fn fn = NULL;
    PeakExecEnv built_env;
    PeakExecAttempt attempt;
    char* const* exec_envp;
    int saved_errno;
    int use_wrapper_depth = !peak_exec_in_shared_vm_child();
    int noalloc_child = peak_exec_in_shared_vm_child() ||
        peak_exec_in_fork_like_child();
    int entry_errno = errno;

    if (noalloc_child) {
        return peak_execvpe_fallback_prepared(file, argv, envp, trace_path);
    }

    fn = peak_real_execvpe();
    if (fn == NULL) {
        return peak_execvpe_fallback_prepared(file, argv, envp, trace_path);
    }
    if (use_wrapper_depth && peak_exec_wrapper_depth > 0) {
        return fn(file, argv, envp);
    }
    if (peak_exec_syscall_args_readable(file, argv, envp) != 0) {
        saved_errno = errno;
        if (peak_exec_preflight_error_is_conclusive(saved_errno)) {
            peak_exec_trace_preflight_failed("", saved_errno);
            errno = saved_errno;
            return -1;
        }
        errno = entry_errno;
        int result = fn(file, argv, envp);
        saved_errno = errno;
        peak_exec_trace_preflight_failed("", saved_errno);
        errno = saved_errno;
        return result;
    }

    if (use_wrapper_depth) {
        peak_exec_wrapper_depth++;
    }
    if (peak_exec_prepare_checked(trace_path,
                                  argv,
                                  envp,
                                  &built_env,
                                  &attempt,
                                  1,
                                  peak_exec_noalloc_envp,
                                  PEAK_EXEC_MAX_ENV_ENTRIES,
                                  peak_exec_noalloc_ld_entry,
                                  sizeof(peak_exec_noalloc_ld_entry)) != 0) {
        exec_envp = envp;
    } else {
        exec_envp = peak_exec_env_to_use(&built_env, envp);
    }

    (void)fn(file, argv, (char* const*)exec_envp);
    saved_errno = errno;
    peak_exec_trace_if_safe("exec-failed",
                            trace_path,
                            &attempt,
                            "failed",
                            saved_errno);
    peak_exec_env_clear(&built_env);
    if (use_wrapper_depth) {
        peak_exec_wrapper_depth--;
    }
    errno = saved_errno;
    return -1;
}

static int
peak_collect_execl_argv(const char* arg, va_list ap, char*** out_argv)
{
    const char* value;
    size_t argc = 0;
    char** argv;
    va_list count_ap;

    if (arg != NULL) {
        argc = 1;
        va_copy(count_ap, ap);
        while ((value = va_arg(count_ap, const char*)) != NULL) {
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
        errno = ENOMEM;
        return -1;
    }

    if (arg != NULL) {
        argv[0] = (char*)arg;
        for (size_t i = 1; i < argc; i++) {
            argv[i] = va_arg(ap, char*);
        }
        (void)va_arg(ap, char*);
    }
    argv[argc] = NULL;
    *out_argv = argv;
    return 0;
}

static int
peak_collect_execl_argv_noalloc(const char* arg,
                                va_list ap,
                                char** storage,
                                size_t storage_count,
                                char*** out_argv)
{
    const char* value;
    size_t argc = 0;
    va_list count_ap;

    if (arg != NULL) {
        argc = 1;
        va_copy(count_ap, ap);
        while ((value = va_arg(count_ap, const char*)) != NULL) {
            argc++;
            if (argc >= storage_count || argc >= PEAK_EXEC_MAX_VARARGS) {
                va_end(count_ap);
                errno = E2BIG;
                return -1;
            }
        }
        va_end(count_ap);
    }

    if (arg != NULL) {
        storage[0] = (char*)arg;
        for (size_t i = 1; i < argc; i++) {
            storage[i] = va_arg(ap, char*);
        }
        (void)va_arg(ap, char*);
    }
    storage[argc] = NULL;
    *out_argv = storage;
    return 0;
}

static int
peak_collect_execle_argv_envp(const char* arg,
                              va_list ap,
                              char*** out_argv,
                              char* const** out_envp)
{
    const char* value;
    size_t argc = 0;
    char** argv;
    va_list count_ap;

    if (arg != NULL) {
        argc = 1;
        va_copy(count_ap, ap);
        while ((value = va_arg(count_ap, const char*)) != NULL) {
            argc++;
            if (argc >= PEAK_EXEC_MAX_VARARGS) {
                va_end(count_ap);
                errno = E2BIG;
                return -1;
            }
        }
        (void)va_arg(count_ap, char* const*);
        va_end(count_ap);
    }

    argv = calloc(argc + 1, sizeof(char*));
    if (argv == NULL) {
        errno = ENOMEM;
        return -1;
    }

    if (arg != NULL) {
        argv[0] = (char*)arg;
        for (size_t i = 1; i < argc; i++) {
            argv[i] = va_arg(ap, char*);
        }
        (void)va_arg(ap, char*);
    }
    argv[argc] = NULL;
    *out_envp = va_arg(ap, char* const*);
    *out_argv = argv;
    return 0;
}

static int
peak_collect_execle_argv_envp_noalloc(const char* arg,
                                      va_list ap,
                                      char** storage,
                                      size_t storage_count,
                                      char*** out_argv,
                                      char* const** out_envp)
{
    const char* value;
    size_t argc = 0;
    va_list count_ap;

    if (arg != NULL) {
        argc = 1;
        va_copy(count_ap, ap);
        while ((value = va_arg(count_ap, const char*)) != NULL) {
            argc++;
            if (argc >= storage_count || argc >= PEAK_EXEC_MAX_VARARGS) {
                va_end(count_ap);
                errno = E2BIG;
                return -1;
            }
        }
        (void)va_arg(count_ap, char* const*);
        va_end(count_ap);
    }

    if (arg != NULL) {
        storage[0] = (char*)arg;
        for (size_t i = 1; i < argc; i++) {
            storage[i] = va_arg(ap, char*);
        }
        (void)va_arg(ap, char*);
    }
    storage[argc] = NULL;
    *out_envp = va_arg(ap, char* const*);
    *out_argv = storage;
    return 0;
}

__attribute__((visibility("default"))) int
execve(const char* pathname, char* const argv[], char* const envp[])
{
    return peak_execve_prepared(pathname, argv, envp, pathname);
}

__attribute__((visibility("default"))) int
execv(const char* pathname, char* const argv[])
{
    return peak_execve_prepared(pathname, argv, environ, pathname);
}

__attribute__((visibility("default"))) int
execvp(const char* file, char* const argv[])
{
    return peak_execvpe_prepared(file, argv, environ, file);
}

#if defined(__linux__)
__attribute__((visibility("default"))) int
execvpe(const char* file, char* const argv[], char* const envp[])
{
    return peak_execvpe_prepared(file, argv, envp, file);
}
#endif

__attribute__((visibility("default"))) int
execl(const char* pathname, const char* arg, ...)
{
    va_list ap;
    char** argv = NULL;
    int result;
    int saved_errno;
    int noalloc_child = peak_exec_in_shared_vm_child() ||
        peak_exec_in_fork_like_child();

    va_start(ap, arg);
    if (noalloc_child) {
        result = peak_collect_execl_argv_noalloc(arg,
                                                 ap,
                                                 peak_exec_noalloc_argv,
                                                 PEAK_EXEC_MAX_VARARGS + 1,
                                                 &argv);
    } else {
        result = peak_collect_execl_argv(arg, ap, &argv);
    }
    va_end(ap);
    if (result != 0) {
        return -1;
    }

    result = peak_execve_prepared(pathname, argv, environ, pathname);
    saved_errno = errno;
    if (!noalloc_child) {
        free(argv);
    }
    errno = saved_errno;
    return result;
}

__attribute__((visibility("default"))) int
execlp(const char* file, const char* arg, ...)
{
    va_list ap;
    char** argv = NULL;
    int result;
    int saved_errno;
    int noalloc_child = peak_exec_in_shared_vm_child() ||
        peak_exec_in_fork_like_child();

    va_start(ap, arg);
    if (noalloc_child) {
        result = peak_collect_execl_argv_noalloc(arg,
                                                 ap,
                                                 peak_exec_noalloc_argv,
                                                 PEAK_EXEC_MAX_VARARGS + 1,
                                                 &argv);
    } else {
        result = peak_collect_execl_argv(arg, ap, &argv);
    }
    va_end(ap);
    if (result != 0) {
        return -1;
    }

    result = peak_execvpe_prepared(file, argv, environ, file);
    saved_errno = errno;
    if (!noalloc_child) {
        free(argv);
    }
    errno = saved_errno;
    return result;
}

__attribute__((visibility("default"))) int
execle(const char* pathname, const char* arg, ...)
{
    va_list ap;
    char** argv = NULL;
    char* const* envp = NULL;
    int result;
    int saved_errno;
    int noalloc_child = peak_exec_in_shared_vm_child() ||
        peak_exec_in_fork_like_child();

    va_start(ap, arg);
    if (noalloc_child) {
        result = peak_collect_execle_argv_envp_noalloc(
            arg,
            ap,
            peak_exec_noalloc_argv,
            PEAK_EXEC_MAX_VARARGS + 1,
            &argv,
            &envp);
    } else {
        result = peak_collect_execle_argv_envp(arg, ap, &argv, &envp);
    }
    va_end(ap);
    if (result != 0) {
        return -1;
    }

    result = peak_execve_prepared(pathname, argv, (char* const*)envp, pathname);
    saved_errno = errno;
    if (!noalloc_child) {
        free(argv);
    }
    errno = saved_errno;
    return result;
}

static peak_fexecve_fn
peak_real_fexecve(void)
{
    static peak_fexecve_fn fn;
    static int looked_up;
    if (!looked_up) {
        fn = (peak_fexecve_fn)dlsym(RTLD_NEXT, "fexecve");
        looked_up = 1;
    }
    return fn;
}

static peak_posix_spawn_fn
peak_real_posix_spawn(void)
{
    if (!peak_cached_posix_spawn_looked_up) {
        peak_cached_posix_spawn_fn =
            (peak_posix_spawn_fn)dlsym(RTLD_NEXT, "posix_spawn");
        peak_cached_posix_spawn_looked_up = 1;
    }
    return peak_cached_posix_spawn_fn;
}

static peak_posix_spawnp_fn
peak_real_posix_spawnp(void)
{
    if (!peak_cached_posix_spawnp_looked_up) {
        peak_cached_posix_spawnp_fn =
            (peak_posix_spawnp_fn)dlsym(RTLD_NEXT, "posix_spawnp");
        peak_cached_posix_spawnp_looked_up = 1;
    }
    return peak_cached_posix_spawnp_fn;
}

static int
peak_posix_spawn_prepare_env(const char* trace_path,
                             char* const argv[],
                             char* const envp[],
                             PeakExecEnv* built_env,
                             PeakExecAttempt* attempt,
                             char** stack_envp,
                             size_t stack_envp_count,
                             char* stack_ld_entry,
                             size_t stack_ld_entry_size)
{
    if (peak_exec_prepare_checked(trace_path,
                                  argv,
                                  envp,
                                  built_env,
                                  attempt,
                                  0,
                                  stack_envp,
                                  stack_envp_count,
                                  stack_ld_entry,
                                  stack_ld_entry_size) != 0) {
        peak_exec_env_clear(built_env);
        return -1;
    }
    return 0;
}

__attribute__((visibility("default"))) int
posix_spawn(pid_t* pid,
            const char* path,
            const posix_spawn_file_actions_t* file_actions,
            const posix_spawnattr_t* attrp,
            char* const argv[],
            char* const envp[])
{
    int use_wrapper_depth = !peak_exec_in_shared_vm_child();
    peak_posix_spawn_fn fn =
        use_wrapper_depth ? peak_real_posix_spawn() : peak_cached_posix_spawn_fn;
    PeakExecEnv built_env;
    PeakExecAttempt attempt;
    char* const* original_envp = envp;
    char* const* spawn_envp = original_envp;
    int result;
    int saved_errno;
    int entry_errno = errno;

    if (fn == NULL) {
        errno = ENOSYS;
        return ENOSYS;
    }
    if (use_wrapper_depth && peak_exec_wrapper_depth > 0) {
        return fn(pid, path, file_actions, attrp, argv, (char* const*)original_envp);
    }
    if (peak_exec_syscall_args_readable(path,
                                        argv,
                                        (char* const*)original_envp) != 0) {
        saved_errno = errno;
        if (peak_exec_preflight_error_is_conclusive(saved_errno)) {
            peak_exec_trace_preflight_failed("", saved_errno);
            errno = saved_errno;
            return saved_errno;
        }
        errno = entry_errno;
        result = fn(pid,
                    path,
                    file_actions,
                    attrp,
                    argv,
                    (char* const*)original_envp);
        saved_errno = errno;
        if (result != 0) {
            peak_exec_trace_preflight_failed("", result);
        }
        errno = saved_errno;
        return result;
    }

    if (use_wrapper_depth) {
        peak_exec_wrapper_depth++;
    }
    if (peak_posix_spawn_prepare_env(path,
                                     argv,
                                     (char* const*)original_envp,
                                     &built_env,
                                     &attempt,
                                     peak_exec_noalloc_envp,
                                     PEAK_EXEC_MAX_ENV_ENTRIES,
                                     peak_exec_noalloc_ld_entry,
                                     sizeof(peak_exec_noalloc_ld_entry)) == 0) {
        spawn_envp = peak_exec_env_to_use(&built_env,
                                          (char* const*)original_envp);
    }

    result = fn(pid, path, file_actions, attrp, argv, (char* const*)spawn_envp);
    saved_errno = errno;
    peak_exec_trace_if_safe(result == 0 ? "posix-spawn-ok" : "posix-spawn-failed",
                            path,
                            &attempt,
                            result == 0 ? "spawned" : "failed",
                            result);
    peak_exec_env_clear(&built_env);
    if (use_wrapper_depth) {
        peak_exec_wrapper_depth--;
    }
    errno = saved_errno;
    return result;
}

__attribute__((visibility("default"))) int
posix_spawnp(pid_t* pid,
             const char* file,
             const posix_spawn_file_actions_t* file_actions,
             const posix_spawnattr_t* attrp,
             char* const argv[],
             char* const envp[])
{
    int use_wrapper_depth = !peak_exec_in_shared_vm_child();
    peak_posix_spawnp_fn fn =
        use_wrapper_depth ? peak_real_posix_spawnp() : peak_cached_posix_spawnp_fn;
    PeakExecEnv built_env;
    PeakExecAttempt attempt;
    char* const* original_envp = envp;
    char* const* spawn_envp = original_envp;
    int result;
    int saved_errno;
    int entry_errno = errno;

    if (fn == NULL) {
        errno = ENOSYS;
        return ENOSYS;
    }
    if (use_wrapper_depth && peak_exec_wrapper_depth > 0) {
        return fn(pid, file, file_actions, attrp, argv, (char* const*)original_envp);
    }
    if (peak_exec_syscall_args_readable(file,
                                        argv,
                                        (char* const*)original_envp) != 0) {
        saved_errno = errno;
        if (peak_exec_preflight_error_is_conclusive(saved_errno)) {
            peak_exec_trace_preflight_failed("", saved_errno);
            errno = saved_errno;
            return saved_errno;
        }
        errno = entry_errno;
        result = fn(pid,
                    file,
                    file_actions,
                    attrp,
                    argv,
                    (char* const*)original_envp);
        saved_errno = errno;
        if (result != 0) {
            peak_exec_trace_preflight_failed("", result);
        }
        errno = saved_errno;
        return result;
    }

    if (use_wrapper_depth) {
        peak_exec_wrapper_depth++;
    }
    if (peak_posix_spawn_prepare_env(file,
                                     argv,
                                     (char* const*)original_envp,
                                     &built_env,
                                     &attempt,
                                     peak_exec_noalloc_envp,
                                     PEAK_EXEC_MAX_ENV_ENTRIES,
                                     peak_exec_noalloc_ld_entry,
                                     sizeof(peak_exec_noalloc_ld_entry)) == 0) {
        spawn_envp = peak_exec_env_to_use(&built_env,
                                          (char* const*)original_envp);
    }

    result = fn(pid, file, file_actions, attrp, argv, (char* const*)spawn_envp);
    saved_errno = errno;
    peak_exec_trace_if_safe(result == 0 ? "posix-spawn-ok" : "posix-spawn-failed",
                            file,
                            &attempt,
                            result == 0 ? "spawned" : "failed",
                            result);
    peak_exec_env_clear(&built_env);
    if (use_wrapper_depth) {
        peak_exec_wrapper_depth--;
    }
    errno = saved_errno;
    return result;
}

__attribute__((visibility("default"))) int
fexecve(int fd, char* const argv[], char* const envp[])
{
    peak_fexecve_fn fn;
    PeakExecEnv built_env;
    PeakExecAttempt attempt;
    char* const* exec_envp;
    char trace_path[64];
    const char* trace_label = "<fd>";
    int saved_errno;
    int noalloc_child = peak_exec_in_shared_vm_child() ||
        peak_exec_in_fork_like_child();
    int use_wrapper_depth = !noalloc_child;
    int entry_errno = errno;

    if (use_wrapper_depth) {
        snprintf(trace_path, sizeof(trace_path), "<fd:%d>", fd);
        trace_label = trace_path;
    }
    if (use_wrapper_depth && peak_exec_wrapper_depth > 0) {
        fn = peak_real_fexecve();
        if (fn != NULL) {
            return fn(fd, argv, envp);
        }
#if defined(__linux__) && defined(SYS_execveat) && defined(AT_EMPTY_PATH)
        return (int)peak_exec_call_raw_syscall6(SYS_execveat,
                                           fd,
                                           (long)"",
                                           (long)argv,
                                           (long)envp,
                                           AT_EMPTY_PATH,
                                           0);
#else
        errno = ENOSYS;
        return -1;
#endif
    }
    if (peak_exec_argv_envp_readable(argv, envp) != 0) {
        saved_errno = errno;
        if (!peak_exec_preflight_error_is_conclusive(saved_errno)) {
            errno = entry_errno;
            fn = use_wrapper_depth ? peak_real_fexecve() : NULL;
            if (fn != NULL) {
                (void)fn(fd, argv, envp);
            } else {
#if defined(__linux__) && defined(SYS_execveat) && defined(AT_EMPTY_PATH)
                (void)peak_exec_call_raw_syscall6(SYS_execveat,
                                             fd,
                                             (long)"",
                                             (long)argv,
                                             (long)envp,
                                             AT_EMPTY_PATH,
                                             0);
#else
                errno = ENOSYS;
#endif
            }
            saved_errno = errno;
            peak_exec_trace_preflight_failed(trace_label, saved_errno);
            errno = saved_errno;
            return -1;
        }
#if defined(__linux__) && defined(SYS_execveat) && defined(AT_EMPTY_PATH)
        int result = (int)peak_exec_call_raw_syscall6(SYS_execveat,
                                                 fd,
                                                 (long)"",
                                                 (long)argv,
                                                 (long)envp,
                                                 AT_EMPTY_PATH,
                                                 0);
        saved_errno = errno;
        peak_exec_trace_preflight_failed(trace_label, saved_errno);
        errno = saved_errno;
        return result;
#else
        peak_exec_trace_preflight_failed(trace_label, saved_errno);
        errno = saved_errno;
        return -1;
#endif
    }

    if (use_wrapper_depth) {
        peak_exec_wrapper_depth++;
    }
    fn = use_wrapper_depth ? peak_real_fexecve() : NULL;
    if (peak_exec_prepare_checked(trace_label,
                                  argv,
                                  envp,
                                  &built_env,
                                  &attempt,
                                  1,
                                  peak_exec_noalloc_envp,
                                  PEAK_EXEC_MAX_ENV_ENTRIES,
                                  peak_exec_noalloc_ld_entry,
                                  sizeof(peak_exec_noalloc_ld_entry)) != 0) {
        exec_envp = envp;
    } else {
        exec_envp = peak_exec_env_to_use(&built_env, envp);
    }

    if (fn != NULL) {
        (void)fn(fd, argv, (char* const*)exec_envp);
    } else {
#if defined(__linux__) && defined(SYS_execveat) && defined(AT_EMPTY_PATH)
        (void)peak_exec_call_raw_syscall6(SYS_execveat,
                                     fd,
                                     (long)"",
                                     (long)argv,
                                     (long)exec_envp,
                                     AT_EMPTY_PATH,
                                     0);
#else
        errno = ENOSYS;
#endif
    }
    saved_errno = errno;
    peak_exec_trace_if_safe("exec-failed",
                            trace_label,
                            &attempt,
                            "failed",
                            saved_errno);
    peak_exec_env_clear(&built_env);
    if (use_wrapper_depth) {
        peak_exec_wrapper_depth--;
    }
    errno = saved_errno;
    return -1;
}

#if defined(__linux__)
static peak_execveat_fn
peak_real_execveat(void)
{
    static peak_execveat_fn fn;
    static int looked_up;
    if (!looked_up) {
        fn = (peak_execveat_fn)dlsym(RTLD_NEXT, "execveat");
        looked_up = 1;
    }
    return fn;
}

__attribute__((visibility("default"))) int
execveat(int dirfd,
         const char* pathname,
         char* const argv[],
         char* const envp[],
         int flags)
{
    peak_execveat_fn fn;
    PeakExecEnv built_env;
    PeakExecAttempt attempt;
    char* const* exec_envp;
    int saved_errno;
    int noalloc_child = peak_exec_in_shared_vm_child() ||
        peak_exec_in_fork_like_child();
    int use_wrapper_depth = !noalloc_child;
    int entry_errno = errno;

    if (use_wrapper_depth && peak_exec_wrapper_depth > 0) {
        fn = peak_real_execveat();
        if (fn != NULL) {
            return fn(dirfd, pathname, argv, envp, flags);
        }
#if defined(SYS_execveat)
        return (int)peak_exec_call_raw_syscall6(SYS_execveat,
                                           dirfd,
                                           (long)pathname,
                                           (long)argv,
                                           (long)envp,
                                           flags,
                                           0);
#else
        errno = ENOSYS;
        return -1;
#endif
    }
    if (peak_exec_syscall_args_readable(pathname, argv, envp) != 0) {
        int preflight_errno = errno;
#if defined(SYS_execveat)
        if (peak_exec_preflight_error_is_conclusive(preflight_errno)) {
            int result = (int)peak_exec_call_raw_syscall6(SYS_execveat,
                                                     dirfd,
                                                     (long)pathname,
                                                     (long)argv,
                                                     (long)envp,
                                                     flags,
                                                     0);
            saved_errno = errno;
            peak_exec_trace_preflight_failed("", saved_errno);
            errno = saved_errno;
            return result;
        }
#else
        if (peak_exec_preflight_error_is_conclusive(preflight_errno)) {
            saved_errno = preflight_errno;
            peak_exec_trace_preflight_failed("", saved_errno);
            errno = saved_errno;
            return -1;
        }
#endif
        errno = entry_errno;
#if defined(SYS_execveat)
        int result = (int)peak_exec_call_raw_syscall6(SYS_execveat,
                                                 dirfd,
                                                 (long)pathname,
                                                 (long)argv,
                                                 (long)envp,
                                                 flags,
                                                 0);
        saved_errno = errno;
        peak_exec_trace_preflight_failed("", saved_errno);
        errno = saved_errno;
        return result;
#else
        peak_exec_trace_preflight_failed("", ENOSYS);
        errno = ENOSYS;
        return -1;
#endif
    }

    if (use_wrapper_depth) {
        peak_exec_wrapper_depth++;
    }
    fn = use_wrapper_depth ? peak_real_execveat() : NULL;
    if (peak_exec_prepare_checked(pathname,
                                  argv,
                                  envp,
                                  &built_env,
                                  &attempt,
                                  1,
                                  peak_exec_noalloc_envp,
                                  PEAK_EXEC_MAX_ENV_ENTRIES,
                                  peak_exec_noalloc_ld_entry,
                                  sizeof(peak_exec_noalloc_ld_entry)) != 0) {
        exec_envp = envp;
    } else {
        exec_envp = peak_exec_env_to_use(&built_env, envp);
    }

    if (fn != NULL) {
        (void)fn(dirfd, pathname, argv, (char* const*)exec_envp, flags);
    } else {
#if defined(SYS_execveat)
        (void)peak_exec_call_raw_syscall6(SYS_execveat,
                                     dirfd,
                                     (long)pathname,
                                     (long)argv,
                                     (long)exec_envp,
                                     flags,
                                     0);
#else
        errno = ENOSYS;
#endif
    }
    saved_errno = errno;
    peak_exec_trace_if_safe("exec-failed",
                            pathname,
                            &attempt,
                            "failed",
                            saved_errno);
    peak_exec_env_clear(&built_env);
    if (use_wrapper_depth) {
        peak_exec_wrapper_depth--;
    }
    errno = saved_errno;
    return -1;
}
#endif

int
peak_exec_handle_syscall(long number,
                         long a1,
                         long a2,
                         long a3,
                         long a4,
                         long a5,
                         long a6,
                         long* result_out)
{
    if (!peak_exec_in_shared_vm_child() && peak_exec_wrapper_depth > 0) {
        return 0;
    }

#if defined(SYS_execve)
    if (number == SYS_execve) {
        if (result_out != NULL) {
            if (peak_exec_syscall_args_readable((const char*)a1,
                                                (char* const*)a2,
                                                (char* const*)a3) != 0) {
                int preflight_errno = errno;
                if (peak_exec_preflight_error_is_conclusive(
                        preflight_errno)) {
                    *result_out = peak_exec_call_raw_syscall6(SYS_execve,
                                                         a1,
                                                         a2,
                                                         a3,
                                                         0,
                                                         0,
                                                         0);
                    int saved_errno = errno;
                    peak_exec_trace_preflight_failed("", saved_errno);
                    errno = saved_errno;
                } else {
                    *result_out = peak_exec_call_raw_syscall6(SYS_execve,
                                                         a1,
                                                         a2,
                                                         a3,
                                                         0,
                                                         0,
                                                         0);
                    int saved_errno = errno;
                    peak_exec_trace_preflight_failed("", saved_errno);
                    errno = saved_errno;
                }
            } else {
                *result_out = (long)peak_execve_prepared((const char*)a1,
                                                         (char* const*)a2,
                                                         (char* const*)a3,
                                                         (const char*)a1);
            }
        }
        return 1;
    }
#endif

#if defined(SYS_fork)
    if (number == SYS_fork) {
        if (result_out != NULL) {
            long result = peak_exec_call_raw_syscall6(SYS_fork,
                                                 0,
                                                 0,
                                                 0,
                                                 0,
                                                 0,
                                                 0);
            if (result == 0) {
                peak_exec_mark_after_fork_child();
            }
            *result_out = result;
        }
        return 1;
    }
#endif

#if defined(SYS_clone)
    if (number == SYS_clone) {
        if (result_out != NULL) {
            long flags = a1;
            long result = peak_exec_call_raw_syscall6(SYS_clone,
                                                 a1,
                                                 a2,
                                                 a3,
                                                 a4,
                                                 a5,
                                                 0);
#if defined(CLONE_VM)
            if (result == 0 && (flags & CLONE_VM) == 0) {
                peak_exec_mark_after_fork_child();
            }
#else
            if (result == 0) {
                peak_exec_mark_after_fork_child();
            }
#endif
            *result_out = result;
        }
        return 1;
    }
#endif

#if defined(SYS_clone3)
    if (number == SYS_clone3) {
        if (result_out != NULL) {
            uint64_t flags = 0;
            int flags_known =
                peak_exec_clone3_flags_safe((const void*)a1, &flags) == 0;
            long result = peak_exec_call_raw_syscall6(SYS_clone3,
                                                 a1,
                                                 a2,
                                                 0,
                                                 0,
                                                 0,
                                                 0);
            if (result == 0) {
#if defined(CLONE_VM)
                if (flags_known && (flags & CLONE_VM) == 0) {
                    peak_exec_mark_after_fork_child();
                }
#else
                if (flags_known) {
                    peak_exec_mark_after_fork_child();
                }
#endif
            }
            *result_out = result;
        }
        return 1;
    }
#endif

#if defined(SYS_vfork)
    if (number == SYS_vfork) {
        /*
         * Preserve raw syscall(SYS_vfork) semantics. Exec-chain cannot safely
         * run a no-return trampoline from the C syscall wrapper while the child
         * shares the parent's stack, so leave this syscall to the generic
         * syscall forwarder instead of emulating it as fork.
         */
        (void)result_out;
        return 0;
    }
#endif

#if defined(SYS_execveat)
    if (number == SYS_execveat) {
        if (result_out != NULL) {
            if (peak_exec_syscall_args_readable((const char*)a2,
                                                (char* const*)a3,
                                                (char* const*)a4) != 0) {
                int preflight_errno = errno;
                if (peak_exec_preflight_error_is_conclusive(
                        preflight_errno)) {
                    *result_out = peak_exec_call_raw_syscall6(SYS_execveat,
                                                         a1,
                                                         a2,
                                                         a3,
                                                         a4,
                                                         a5,
                                                         0);
                    int saved_errno = errno;
                    peak_exec_trace_preflight_failed("", saved_errno);
                    errno = saved_errno;
                } else {
                    *result_out = peak_exec_call_raw_syscall6(SYS_execveat,
                                                         a1,
                                                         a2,
                                                         a3,
                                                         a4,
                                                         a5,
                                                         0);
                    int saved_errno = errno;
                    peak_exec_trace_preflight_failed("", saved_errno);
                    errno = saved_errno;
                }
            } else {
                *result_out = (long)execveat((int)a1,
                                             (const char*)a2,
                                             (char* const*)a3,
                                             (char* const*)a4,
                                             (int)a5);
            }
        }
        return 1;
    }
#endif

    (void)a6;
    return 0;
}
