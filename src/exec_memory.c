#define _GNU_SOURCE
#include "internal/exec_memory.h"
#include "internal/exec_raw_syscall.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int
peak_exec_test_hook_enabled(const char* name)
{
#if defined(PEAK_ENABLE_TEST_HOOKS)
    const char* value = getenv(name);

    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
#else
    (void)name;
    return 0;
#endif
}

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
peak_exec_parse_maps_hex(const char* cursor,
                         const char* end,
                         uintptr_t* value_out)
{
    uintptr_t value = 0;
    int digits = 0;

    while (cursor < end) {
        int digit = peak_exec_hex_value(*cursor);

        if (digit < 0) {
            break;
        }
        if (value > (UINTPTR_MAX - (uintptr_t)digit) / 16U) {
            return NULL;
        }
        value = value * 16U + (uintptr_t)digit;
        digits++;
        cursor++;
    }
    if (digits == 0) {
        return NULL;
    }
    *value_out = value;
    return cursor;
}

static int
peak_exec_parse_maps_line(const char* line,
                          size_t length,
                          uintptr_t* start_out,
                          uintptr_t* end_out,
                          int* readable_out)
{
    const char* cursor = line;
    const char* end = line + length;
    uintptr_t start = 0;
    uintptr_t stop = 0;

    cursor = peak_exec_parse_maps_hex(cursor, end, &start);
    if (cursor == NULL || cursor >= end || *cursor != '-') {
        return -1;
    }
    cursor = peak_exec_parse_maps_hex(cursor + 1, end, &stop);
    if (cursor == NULL || cursor >= end || *cursor != ' ') {
        return -1;
    }
    cursor++;
    if (cursor >= end || stop <= start) {
        return -1;
    }

    *start_out = start;
    *end_out = stop;
    *readable_out = *cursor == 'r';
    return 0;
}

static PeakExecPreflightResult
peak_exec_maps_range_readable(const void* remote, size_t length)
{
#if defined(__linux__) && defined(SYS_openat) && defined(SYS_read) && \
    defined(SYS_close)
    uintptr_t start = (uintptr_t)remote;
    uintptr_t stop;
    uintptr_t covered;
    char read_buffer[4096] = {0};
    char line[1024];
    size_t line_length = 0;
    int skipping_long_line = 0;
    long fd;

    if (length == 0) {
        return PEAK_EXEC_PREFLIGHT_VALID;
    }
    if (remote == NULL || start > UINTPTR_MAX - length) {
        return PEAK_EXEC_PREFLIGHT_INVALID;
    }
    stop = start + length;
    covered = start;

    if (peak_exec_test_hook_enabled(
            "PEAK_TEST_EXEC_PREFLIGHT_MAPS_UNAVAILABLE")) {
        return PEAK_EXEC_PREFLIGHT_UNKNOWN;
    }
    fd = peak_exec_raw_syscall6(SYS_openat,
                                AT_FDCWD,
                                (long)"/proc/self/maps",
                                O_RDONLY | O_CLOEXEC,
                                0,
                                0,
                                0);
    if (fd < 0) {
        return PEAK_EXEC_PREFLIGHT_UNKNOWN;
    }

    while (covered < stop) {
        long bytes_read = peak_exec_raw_syscall6(SYS_read,
                                                 fd,
                                                 (long)read_buffer,
                                                 sizeof(read_buffer),
                                                 0,
                                                 0,
                                                 0);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (bytes_read == 0) {
            break;
        }

        for (long i = 0; i < bytes_read; i++) {
            char ch = read_buffer[i];

            if (skipping_long_line) {
                if (ch == '\n') {
                    skipping_long_line = 0;
                    line_length = 0;
                }
                continue;
            }
            if (ch != '\n' && line_length + 1 < sizeof(line)) {
                line[line_length++] = ch;
                continue;
            }
            if (ch != '\n') {
                skipping_long_line = 1;
                line_length = 0;
                continue;
            }

            uintptr_t map_start = 0;
            uintptr_t map_end = 0;
            int readable = 0;

            if (peak_exec_parse_maps_line(line,
                                          line_length,
                                          &map_start,
                                          &map_end,
                                          &readable) == 0 &&
                map_end > covered) {
                if (map_start > covered || !readable) {
                    (void)peak_exec_raw_syscall6(SYS_close,
                                                 fd,
                                                 0,
                                                 0,
                                                 0,
                                                 0,
                                                 0);
                    return PEAK_EXEC_PREFLIGHT_INVALID;
                }
                covered = map_end < stop ? map_end : stop;
                if (covered >= stop) {
                    (void)peak_exec_raw_syscall6(SYS_close,
                                                 fd,
                                                 0,
                                                 0,
                                                 0,
                                                 0,
                                                 0);
                    return PEAK_EXEC_PREFLIGHT_VALID;
                }
            }
            line_length = 0;
        }
    }

    (void)peak_exec_raw_syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
    return PEAK_EXEC_PREFLIGHT_INVALID;
#else
    (void)remote;
    (void)length;
    return PEAK_EXEC_PREFLIGHT_UNKNOWN;
#endif
}

static PeakExecPreflightResult
peak_exec_user_read_bytes(const void* remote, void* local, size_t length)
{
    if (length == 0) {
        return PEAK_EXEC_PREFLIGHT_VALID;
    }
    if (remote == NULL || local == NULL) {
        errno = EFAULT;
        return PEAK_EXEC_PREFLIGHT_INVALID;
    }

#if defined(__linux__) && defined(SYS_process_vm_readv)
    struct iovec local_iov = { .iov_base = local, .iov_len = length };
    struct iovec remote_iov = {
        .iov_base = (void*)remote,
        .iov_len = length
    };
    long bytes_read;

    if (peak_exec_test_hook_enabled("PEAK_TEST_EXEC_PREFLIGHT_UNAVAILABLE")) {
        bytes_read = -1;
        errno = EPERM;
    } else {
        bytes_read = peak_exec_raw_syscall6(SYS_process_vm_readv,
                                            (long)getpid(),
                                            (long)&local_iov,
                                            1,
                                            (long)&remote_iov,
                                            1,
                                            0);
    }
    if (bytes_read == (long)length) {
        return PEAK_EXEC_PREFLIGHT_VALID;
    }
    if (bytes_read >= 0) {
        errno = EFAULT;
        return PEAK_EXEC_PREFLIGHT_INVALID;
    }

    PeakExecPreflightResult maps_result =
        peak_exec_maps_range_readable(remote, length);
    if (maps_result == PEAK_EXEC_PREFLIGHT_INVALID) {
        errno = EFAULT;
        return PEAK_EXEC_PREFLIGHT_INVALID;
    }
    if (maps_result == PEAK_EXEC_PREFLIGHT_UNKNOWN) {
        return PEAK_EXEC_PREFLIGHT_UNKNOWN;
    }

#if defined(SYS_openat) && defined(SYS_pread64) && defined(SYS_close)
    if (!peak_exec_test_hook_enabled("PEAK_TEST_EXEC_PREFLIGHT_NO_PROC_MEM")) {
        long fd = peak_exec_raw_syscall6(SYS_openat,
                                         AT_FDCWD,
                                         (long)"/proc/self/mem",
                                         O_RDONLY | O_CLOEXEC,
                                         0,
                                         0,
                                         0);
        if (fd >= 0) {
            long mem_read = peak_exec_raw_syscall6(
                SYS_pread64,
                fd,
                (long)local,
                (long)length,
                (long)(uintptr_t)remote,
                0,
                0);
            int read_errno = errno;

            (void)peak_exec_raw_syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
            if (mem_read == (long)length) {
                return PEAK_EXEC_PREFLIGHT_VALID;
            }
            if (mem_read >= 0 || read_errno == EIO) {
                errno = EFAULT;
                return PEAK_EXEC_PREFLIGHT_INVALID;
            }
        }
    }
#endif
    return PEAK_EXEC_PREFLIGHT_UNKNOWN;
#else
    return PEAK_EXEC_PREFLIGHT_UNKNOWN;
#endif
}

static PeakExecPreflightResult
peak_exec_user_cstr_readable(const char* value, size_t max_length)
{
    char buffer[256];
    size_t offset = 0;
    const size_t page_size = 4096U;
    uintptr_t base = (uintptr_t)value;

    if (value == NULL) {
        errno = EFAULT;
        return PEAK_EXEC_PREFLIGHT_INVALID;
    }
    while (offset < max_length) {
        uintptr_t address;
        size_t chunk = sizeof(buffer);
        size_t page_remaining;

        if (base > UINTPTR_MAX - offset) {
            errno = EFAULT;
            return PEAK_EXEC_PREFLIGHT_INVALID;
        }
        address = base + offset;
        page_remaining = page_size - (address % page_size);
        if (chunk > max_length - offset) {
            chunk = max_length - offset;
        }
        if (chunk > page_remaining) {
            chunk = page_remaining;
        }
        PeakExecPreflightResult read_result =
            peak_exec_user_read_bytes((const void*)address, buffer, chunk);
        if (read_result != PEAK_EXEC_PREFLIGHT_VALID) {
            return read_result;
        }
        if (memchr(buffer, '\0', chunk) != NULL) {
            return PEAK_EXEC_PREFLIGHT_VALID;
        }
        offset += chunk;
    }

    errno = ENAMETOOLONG;
    return PEAK_EXEC_PREFLIGHT_INVALID;
}

static PeakExecPreflightResult
peak_exec_user_cstr_array_readable(char* const values[],
                                   size_t max_entries,
                                   size_t max_string_length)
{
    uintptr_t base = (uintptr_t)values;

    if (values == NULL) {
        return PEAK_EXEC_PREFLIGHT_VALID;
    }
    for (size_t i = 0; i < max_entries; i++) {
        char* entry = NULL;
        uintptr_t slot;

        if (i > (UINTPTR_MAX - base) / sizeof(entry)) {
            errno = EFAULT;
            return PEAK_EXEC_PREFLIGHT_INVALID;
        }
        slot = base + i * sizeof(entry);
        PeakExecPreflightResult read_result =
            peak_exec_user_read_bytes((const void*)slot,
                                      &entry,
                                      sizeof(entry));
        if (read_result != PEAK_EXEC_PREFLIGHT_VALID) {
            return read_result;
        }
        if (entry == NULL) {
            return PEAK_EXEC_PREFLIGHT_VALID;
        }
        read_result = peak_exec_user_cstr_readable(entry, max_string_length);
        if (read_result != PEAK_EXEC_PREFLIGHT_VALID) {
            return read_result;
        }
    }

    errno = E2BIG;
    return PEAK_EXEC_PREFLIGHT_INVALID;
}

PeakExecPreflightResult
peak_exec_args_readable(const char* path,
                        char* const argv[],
                        char* const envp[])
{
    PeakExecPreflightResult result =
        peak_exec_user_cstr_readable(path, PATH_MAX);

    if (result != PEAK_EXEC_PREFLIGHT_VALID) {
        return result;
    }
    return peak_exec_argv_envp_readable(argv, envp);
}

PeakExecPreflightResult
peak_exec_argv_envp_readable(char* const argv[], char* const envp[])
{
    PeakExecPreflightResult result =
        peak_exec_user_cstr_array_readable(argv,
                                           PEAK_EXEC_MAX_ENV_ENTRIES,
                                           PEAK_EXEC_USER_STRING_MAX);

    if (result != PEAK_EXEC_PREFLIGHT_VALID) {
        return result;
    }
    return peak_exec_user_cstr_array_readable(envp,
                                              PEAK_EXEC_MAX_ENV_ENTRIES,
                                              PEAK_EXEC_USER_STRING_MAX);
}
