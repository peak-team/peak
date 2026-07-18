#define _GNU_SOURCE
#include "internal/general_listener/exec_checkpoint_writer.h"

#include "internal/exec_raw_syscall.h"

#include <errno.h>
#include <float.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

static bool
peak_exec_checkpoint_append_char(char* buffer,
                                 size_t buffer_size,
                                 size_t* out,
                                 char value)
{
    if (*out + 1 >= buffer_size) {
        errno = ENAMETOOLONG;
        return false;
    }
    buffer[(*out)++] = value;
    buffer[*out] = '\0';
    return true;
}

static bool
peak_exec_checkpoint_append_literal(char* buffer,
                                    size_t buffer_size,
                                    size_t* out,
                                    const char* value)
{
    while (value != NULL && *value != '\0') {
        if (!peak_exec_checkpoint_append_char(buffer,
                                              buffer_size,
                                              out,
                                              *value++)) {
            return false;
        }
    }
    return true;
}

static bool
peak_exec_checkpoint_append_u64(char* buffer,
                                size_t buffer_size,
                                size_t* out,
                                unsigned long long value)
{
    char digits[32];
    size_t count = 0;

    do {
        digits[count++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    } while (value != 0ULL);

    while (count != 0) {
        if (!peak_exec_checkpoint_append_char(buffer,
                                              buffer_size,
                                              out,
                                              digits[--count])) {
            return false;
        }
    }
    return true;
}

static bool
peak_exec_checkpoint_append_padded_u64(char* buffer,
                                       size_t buffer_size,
                                       size_t* out,
                                       unsigned long long value,
                                       unsigned int width)
{
    char digits[32];
    size_t count = 0;

    do {
        digits[count++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    } while (value != 0ULL);
    while (count < width && count < sizeof(digits)) {
        digits[count++] = '0';
    }
    while (count != 0) {
        if (!peak_exec_checkpoint_append_char(buffer,
                                              buffer_size,
                                              out,
                                              digits[--count])) {
            return false;
        }
    }
    return true;
}

static int
peak_exec_checkpoint_path(char* buffer,
                          size_t buffer_size,
                          unsigned long long checkpoint_index)
{
    const char* base = getenv("PEAK_STATSLOG_PATH");
    size_t out = 0;

    if (buffer == NULL || buffer_size == 0) {
        errno = EINVAL;
        return -1;
    }
    buffer[0] = '\0';
    if (base == NULL || base[0] == '\0') {
        base = "./peak_statslog";
    }

    if (!peak_exec_checkpoint_append_literal(buffer, buffer_size, &out, base) ||
        !peak_exec_checkpoint_append_literal(buffer, buffer_size, &out, "-p") ||
        !peak_exec_checkpoint_append_u64(buffer,
                                         buffer_size,
                                         &out,
                                         (unsigned long long)getpid()) ||
        !peak_exec_checkpoint_append_literal(buffer,
                                             buffer_size,
                                             &out,
                                             "-exec") ||
        !peak_exec_checkpoint_append_u64(buffer,
                                         buffer_size,
                                         &out,
                                         checkpoint_index) ||
        !peak_exec_checkpoint_append_literal(buffer,
                                             buffer_size,
                                             &out,
                                             ".csv")) {
        return -1;
    }
    return 0;
}

static bool
peak_exec_checkpoint_write_all(int fd, const char* data, size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t rc = (ssize_t)peak_exec_raw_syscall6(
            SYS_write,
            fd,
            (long)(data + written),
            (long)(length - written),
            0,
            0,
            0);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (rc == 0) {
            errno = EIO;
            return false;
        }
        written += (size_t)rc;
    }
    return true;
}

static int
peak_exec_checkpoint_open_exclusive(const char* path)
{
    return (int)peak_exec_raw_syscall6(SYS_openat,
                                       AT_FDCWD,
                                       (long)path,
                                       O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                                       0600,
                                       0,
                                       0);
}

static bool
peak_exec_checkpoint_close_fd(int fd)
{
    return peak_exec_raw_syscall6(
               SYS_close, fd, 0, 0, 0, 0, 0) == 0;
}

static void
peak_exec_checkpoint_unlink_path(const char* path)
{
    (void)peak_exec_raw_syscall6(
        SYS_unlinkat, AT_FDCWD, (long)path, 0, 0, 0, 0);
}

static bool
peak_exec_checkpoint_write_csv_name(int fd, const char* name)
{
    const char* start;
    const char* cursor;

    if (!peak_exec_checkpoint_write_all(fd, "\"", 1)) {
        return false;
    }
    if (name == NULL) {
        return peak_exec_checkpoint_write_all(fd, "\"", 1);
    }

    start = name;
    cursor = name;
    while (*cursor != '\0') {
        if (*cursor == '"') {
            if (cursor > start &&
                !peak_exec_checkpoint_write_all(
                    fd, start, (size_t)(cursor - start))) {
                return false;
            }
            if (!peak_exec_checkpoint_write_all(fd, "\"\"", 2)) {
                return false;
            }
            start = cursor + 1;
        }
        cursor++;
    }
    if (cursor > start &&
        !peak_exec_checkpoint_write_all(
            fd, start, (size_t)(cursor - start))) {
        return false;
    }
    return peak_exec_checkpoint_write_all(fd, "\"", 1);
}

static bool
peak_exec_checkpoint_append_double(char* buffer,
                                   size_t buffer_size,
                                   size_t* out,
                                   double value)
{
    int exponent = 0;
    unsigned long long scaled;
    unsigned long long digit;
    unsigned long long frac;

    if (value != value) {
        return peak_exec_checkpoint_append_literal(
            buffer, buffer_size, out, "nan");
    }
    if (value < 0.0) {
        if (!peak_exec_checkpoint_append_char(
                buffer, buffer_size, out, '-')) {
            return false;
        }
        value = -value;
    }
    if (value > DBL_MAX) {
        return peak_exec_checkpoint_append_literal(
            buffer, buffer_size, out, "inf");
    }
    if (value == 0.0) {
        return peak_exec_checkpoint_append_literal(
            buffer, buffer_size, out, "0.000000000e+00");
    }
    while (value >= 10.0 && exponent < 308) {
        value /= 10.0;
        exponent++;
    }
    while (value < 1.0 && exponent > -308) {
        value *= 10.0;
        exponent--;
    }
    scaled = (unsigned long long)(value * 1000000000.0 + 0.5);
    if (scaled >= 10000000000ULL) {
        scaled /= 10ULL;
        exponent++;
    }
    digit = scaled / 1000000000ULL;
    frac = scaled % 1000000000ULL;
    if (!peak_exec_checkpoint_append_u64(buffer,
                                         buffer_size,
                                         out,
                                         digit) ||
        !peak_exec_checkpoint_append_char(buffer, buffer_size, out, '.') ||
        !peak_exec_checkpoint_append_padded_u64(buffer,
                                                buffer_size,
                                                out,
                                                frac,
                                                9) ||
        !peak_exec_checkpoint_append_char(buffer, buffer_size, out, 'e')) {
        return false;
    }
    if (exponent < 0) {
        if (!peak_exec_checkpoint_append_char(
                buffer, buffer_size, out, '-')) {
            return false;
        }
        exponent = -exponent;
    } else if (!peak_exec_checkpoint_append_char(
                   buffer, buffer_size, out, '+')) {
        return false;
    }
    return peak_exec_checkpoint_append_padded_u64(
        buffer,
        buffer_size,
        out,
        (unsigned long long)exponent,
        2);
}

static bool
peak_exec_checkpoint_write_row(int fd,
                               const PeakExecCheckpointRow* row,
                               double overhead_per_call)
{
    char buffer[512];
    size_t out = 0;
    unsigned long safe_thread_count =
        row->threads_seen != 0 ? row->threads_seen : 1;
    unsigned long per_thread =
        row->num_calls / safe_thread_count +
        ((row->num_calls % safe_thread_count != 0) ? 1 : 0);
    double exclusive_time = row->exclusive_time;

    if (exclusive_time < 0.0) {
        exclusive_time = 0.0;
    }
    if (row->total_time >= 0.0 && exclusive_time > row->total_time) {
        exclusive_time = row->total_time;
    }

    if (!peak_exec_checkpoint_write_csv_name(fd, row->name)) {
        return false;
    }

    if (!peak_exec_checkpoint_append_char(buffer, sizeof(buffer), &out, ',') ||
        !peak_exec_checkpoint_append_u64(buffer,
                                         sizeof(buffer),
                                         &out,
                                         row->num_calls) ||
        !peak_exec_checkpoint_append_char(buffer, sizeof(buffer), &out, ',') ||
        !peak_exec_checkpoint_append_u64(buffer,
                                         sizeof(buffer),
                                         &out,
                                         per_thread) ||
        !peak_exec_checkpoint_append_char(buffer, sizeof(buffer), &out, ',') ||
        !peak_exec_checkpoint_append_u64(buffer,
                                         sizeof(buffer),
                                         &out,
                                         row->num_calls) ||
        !peak_exec_checkpoint_append_char(buffer, sizeof(buffer), &out, ',') ||
        !peak_exec_checkpoint_append_double(buffer,
                                            sizeof(buffer),
                                            &out,
                                            (double)row->max_time) ||
        !peak_exec_checkpoint_append_char(buffer, sizeof(buffer), &out, ',') ||
        !peak_exec_checkpoint_append_double(buffer,
                                            sizeof(buffer),
                                            &out,
                                            (double)row->min_time) ||
        !peak_exec_checkpoint_append_char(buffer, sizeof(buffer), &out, ',') ||
        !peak_exec_checkpoint_append_double(buffer,
                                            sizeof(buffer),
                                            &out,
                                            row->total_time) ||
        !peak_exec_checkpoint_append_char(buffer, sizeof(buffer), &out, ',') ||
        !peak_exec_checkpoint_append_double(buffer,
                                            sizeof(buffer),
                                            &out,
                                            exclusive_time) ||
        !peak_exec_checkpoint_append_char(buffer, sizeof(buffer), &out, ',') ||
        !peak_exec_checkpoint_append_double(buffer,
                                            sizeof(buffer),
                                            &out,
                                            row->max_total_time) ||
        !peak_exec_checkpoint_append_char(buffer, sizeof(buffer), &out, ',') ||
        !peak_exec_checkpoint_append_double(buffer,
                                            sizeof(buffer),
                                            &out,
                                            row->min_total_time) ||
        !peak_exec_checkpoint_append_char(buffer, sizeof(buffer), &out, ',') ||
        !peak_exec_checkpoint_append_double(
            buffer,
            sizeof(buffer),
            &out,
            (double)row->num_calls * overhead_per_call) ||
        !peak_exec_checkpoint_append_char(buffer,
                                          sizeof(buffer),
                                          &out,
                                          '\n')) {
        return false;
    }
    return peak_exec_checkpoint_write_all(fd, buffer, out);
}

bool
peak_exec_checkpoint_write_rows(
    unsigned long long checkpoint_index,
    const PeakExecCheckpointRow* rows,
    size_t row_count,
    double overhead_per_call)
{
    static const char header[] =
        "function,"
        "count,per_thread,per_rank,call_max_s,call_min_s,"
        "total_s,exclusive_s,thread_max_s,thread_min_s,overhead_s\n";
    char path[PATH_MAX];
    int fd = -1;

    if (rows == NULL && row_count != 0) {
        errno = EINVAL;
        return false;
    }

    for (unsigned int attempt = 0; attempt < 1024; attempt++) {
        if (peak_exec_checkpoint_path(path,
                                      sizeof(path),
                                      checkpoint_index + attempt) != 0) {
            return false;
        }
        fd = peak_exec_checkpoint_open_exclusive(path);
        if (fd >= 0) {
            break;
        }
        if (errno != EEXIST) {
            return false;
        }
    }
    if (fd < 0) {
        errno = EEXIST;
        return false;
    }

    if (!peak_exec_checkpoint_write_all(fd, header, sizeof(header) - 1)) {
        peak_exec_checkpoint_close_fd(fd);
        peak_exec_checkpoint_unlink_path(path);
        return false;
    }

    for (size_t i = 0; i < row_count; i++) {
        if (rows[i].num_calls == 0 || rows[i].name == NULL) {
            continue;
        }
        if (!peak_exec_checkpoint_write_row(
                fd, &rows[i], overhead_per_call)) {
            peak_exec_checkpoint_close_fd(fd);
            peak_exec_checkpoint_unlink_path(path);
            return false;
        }
    }

    if (!peak_exec_checkpoint_close_fd(fd)) {
        peak_exec_checkpoint_unlink_path(path);
        return false;
    }
    return true;
}
