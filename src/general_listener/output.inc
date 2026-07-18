#include <sys/syscall.h>
#include <sys/uio.h>

static char*
peak_stats_csv_path(void)
{
    char base[256] = {0};
    char out_csv[512] = {0};

    const char *env_path = getenv("PEAK_STATSLOG_PATH");
    if (env_path && *env_path) {
        size_t n = strlen(env_path);
        if (n >= sizeof(base)) n = sizeof(base) - 1;
        memcpy(base, env_path, n);
        base[n] = '\0';
    } else {
        snprintf(base, sizeof(base), "./peak_statslog");
    }

    int pid = (int) getpid();
    snprintf(out_csv, 512, "%s-p%d.csv", base, pid);

    return g_strdup(out_csv);
}

#ifdef HAVE_MPI
_Static_assert(sizeof(uint64_t) * CHAR_BIT == 64,
               "PEAK MPI reporting requires an exact 64-bit uint64_t");
#if defined(PEAK_HAVE_MPI_UINT64_T)
#define PEAK_MPI_UINT64_DATATYPE MPI_UINT64_T
#elif UINT64_MAX == ULONG_MAX
#define PEAK_MPI_UINT64_DATATYPE MPI_UNSIGNED_LONG
#elif UINT64_MAX == ULLONG_MAX
#define PEAK_MPI_UINT64_DATATYPE MPI_UNSIGNED_LONG_LONG
#else
#error No exact MPI datatype fallback is available for uint64_t
#endif
#endif

static long
peak_exec_checkpoint_raw_syscall6(long number,
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

static gboolean
peak_exec_checkpoint_append_char(char* buffer,
                                 size_t buffer_size,
                                 size_t* out,
                                 char value)
{
    if (*out + 1 >= buffer_size) {
        errno = ENAMETOOLONG;
        return FALSE;
    }
    buffer[(*out)++] = value;
    buffer[*out] = '\0';
    return TRUE;
}

static gboolean
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
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
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
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
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
            return FALSE;
        }
    }
    return TRUE;
}

static int
peak_stats_exec_checkpoint_path(char* buffer,
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

static void
peak_stats_csv_unlink(void)
{
    char* out_csv = peak_stats_csv_path();
    (void)unlink(out_csv);
    g_free(out_csv);
}

static gboolean
peak_exec_checkpoint_write_all(int fd, const char* data, size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t rc = (ssize_t)peak_exec_checkpoint_raw_syscall6(SYS_write,
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
            return FALSE;
        }
        if (rc == 0) {
            errno = EIO;
            return FALSE;
        }
        written += (size_t)rc;
    }
    return TRUE;
}

static int
peak_exec_checkpoint_open_exclusive(const char* path)
{
    return (int)peak_exec_checkpoint_raw_syscall6(SYS_openat,
                                                  AT_FDCWD,
                                                  (long)path,
                                                  O_WRONLY | O_CREAT |
                                                      O_EXCL | O_CLOEXEC,
                                                  0600,
                                                  0,
                                                  0);
}

static gboolean
peak_exec_checkpoint_close_fd(int fd)
{
    return peak_exec_checkpoint_raw_syscall6(SYS_close,
                                             fd,
                                             0,
                                             0,
                                             0,
                                             0,
                                             0) == 0;
}

static void
peak_exec_checkpoint_unlink_path(const char* path)
{
    (void)peak_exec_checkpoint_raw_syscall6(SYS_unlinkat,
                                            AT_FDCWD,
                                            (long)path,
                                            0,
                                            0,
                                            0,
                                            0);
}

static gboolean
peak_exec_checkpoint_write_csv_name(int fd, const char* name)
{
    const char* start;
    const char* cursor;

    if (!peak_exec_checkpoint_write_all(fd, "\"", 1)) {
        return FALSE;
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
                    fd,
                    start,
                    (size_t)(cursor - start))) {
                return FALSE;
            }
            if (!peak_exec_checkpoint_write_all(fd, "\"\"", 2)) {
                return FALSE;
            }
            start = cursor + 1;
        }
        cursor++;
    }
    if (cursor > start &&
        !peak_exec_checkpoint_write_all(fd,
                                        start,
                                        (size_t)(cursor - start))) {
        return FALSE;
    }
    return peak_exec_checkpoint_write_all(fd, "\"", 1);
}

#ifdef PEAK_ENABLE_TEST_HOOKS
PEAK_API int
peak_general_listener_test_checkpoint_snapshot_lock_hold(void)
{
    return pthread_mutex_lock(&lock);
}

PEAK_API int
peak_general_listener_test_checkpoint_snapshot_lock_release(void)
{
    return pthread_mutex_unlock(&lock);
}
#endif

static gboolean
peak_general_listener_checkpoint_snapshot_lock(void)
{
    return pthread_mutex_trylock(&lock) == 0;
}

typedef struct {
    char* name;
    gulong num_calls;
    gulong threads_seen;
    gdouble total_time;
    gdouble max_total_time;
    gdouble min_total_time;
    gdouble exclusive_time;
    gfloat max_time;
    gfloat min_time;
} PeakExecCheckpointRecord;

typedef struct {
    PeakGeneralListenerCheckpointShadow* shadows;
    gulong* sequences;
    int* invalid;
} PeakExecCheckpointThreadSnapshot;

#ifdef PEAK_ENABLE_TEST_HOOKS
static _Atomic int peak_checkpoint_test_busy_pause = 0;
static _Atomic int peak_checkpoint_test_busy_held = 0;
static _Atomic int peak_checkpoint_test_busy_released = 0;

PEAK_API void
peak_general_listener_test_checkpoint_busy_pause_enable(void)
{
    atomic_store_explicit(&peak_checkpoint_test_busy_released,
                          0,
                          memory_order_release);
    atomic_store_explicit(&peak_checkpoint_test_busy_held,
                          0,
                          memory_order_release);
    atomic_store_explicit(&peak_checkpoint_test_busy_pause,
                          1,
                          memory_order_release);
}

PEAK_API int
peak_general_listener_test_checkpoint_busy_is_held(void)
{
    return atomic_load_explicit(&peak_checkpoint_test_busy_held,
                                memory_order_acquire);
}

PEAK_API void
peak_general_listener_test_checkpoint_busy_release(void)
{
    atomic_store_explicit(&peak_checkpoint_test_busy_released,
                          1,
                          memory_order_release);
}

static void
peak_general_listener_test_checkpoint_busy_pause(void)
{
    if (atomic_load_explicit(&peak_checkpoint_test_busy_pause,
                             memory_order_acquire) == 0) {
        return;
    }
    atomic_store_explicit(&peak_checkpoint_test_busy_held,
                          1,
                          memory_order_release);
    while (atomic_load_explicit(&peak_checkpoint_test_busy_released,
                                memory_order_acquire) == 0) {
        sched_yield();
    }
    atomic_store_explicit(&peak_checkpoint_test_busy_pause,
                          0,
                          memory_order_release);
}
#endif

static gboolean
peak_exec_checkpoint_copy_listener(
    const PeakGeneralListener* listener,
    PeakExecCheckpointThreadSnapshot* snapshot,
    size_t thread_count)
{
#if defined(__linux__) && defined(SYS_process_vm_readv)
    if (listener->checkpoint_shadow == NULL) {
        errno = EOPNOTSUPP;
        return FALSE;
    }
    struct iovec local = { snapshot->shadows,
                           thread_count * sizeof(*snapshot->shadows) };
    struct iovec remote = { listener->checkpoint_shadow,
                            thread_count * sizeof(*listener->checkpoint_shadow) };
    size_t expected = thread_count * sizeof(*snapshot->shadows);
    for (unsigned int attempt = 0; attempt < 8; attempt++) {
        gboolean busy = FALSE;

        for (size_t index = 0; index < thread_count; index++) {
            snapshot->sequences[index] = atomic_load_explicit(
                &listener->checkpoint_shadow[index].sequence, memory_order_acquire);
            snapshot->invalid[index] = atomic_load_explicit(
                &listener->checkpoint_shadow[index].invalid, memory_order_acquire);
            if ((snapshot->sequences[index] & 1UL) != 0 ||
                snapshot->invalid[index] != 0) {
                busy = TRUE;
                break;
            }
        }
        if (busy) {
#ifdef PEAK_ENABLE_TEST_HOOKS
            peak_general_listener_test_checkpoint_busy_pause();
#endif
            continue;
        }

        long copied = peak_exec_checkpoint_raw_syscall6(
            SYS_process_vm_readv,
            (long)getpid(),
            (long)&local,
            1,
            (long)&remote,
            1,
            0);
        if (copied < 0 || (size_t)copied != expected) {
            return FALSE;
        }
        for (size_t index = 0; index < thread_count; index++) {
            gulong current = atomic_load_explicit(
                &listener->checkpoint_shadow[index].sequence, memory_order_acquire);
            int invalid = atomic_load_explicit(
                &listener->checkpoint_shadow[index].invalid, memory_order_acquire);
            if (current != snapshot->sequences[index] ||
                invalid != snapshot->invalid[index] || invalid != 0 ||
                (current & 1UL) != 0) {
                busy = TRUE;
                break;
            }
        }
        if (!busy) {
            return TRUE;
        }
    }
    errno = EAGAIN;
    return FALSE;
#else
    (void)listener;
    (void)snapshot;
    (void)thread_count;
    errno = ENOSYS;
    return FALSE;
#endif
}

static void
peak_exec_checkpoint_free_snapshot(PeakExecCheckpointRecord* records,
                                   size_t* name_lengths,
                                   char* names,
                                   void* thread_storage)
{
    free(thread_storage);
    free(names);
    free(name_lengths);
    free(records);
}

static gboolean
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
        return peak_exec_checkpoint_append_literal(buffer,
                                                   buffer_size,
                                                   out,
                                                   "nan");
    }
    if (value < 0.0) {
        if (!peak_exec_checkpoint_append_char(buffer,
                                              buffer_size,
                                              out,
                                              '-')) {
            return FALSE;
        }
        value = -value;
    }
    if (value > DBL_MAX) {
        return peak_exec_checkpoint_append_literal(buffer,
                                                   buffer_size,
                                                   out,
                                                   "inf");
    }
    if (value == 0.0) {
        return peak_exec_checkpoint_append_literal(buffer,
                                                   buffer_size,
                                                   out,
                                                   "0.000000000e+00");
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
        return FALSE;
    }
    if (exponent < 0) {
        if (!peak_exec_checkpoint_append_char(buffer,
                                              buffer_size,
                                              out,
                                              '-')) {
            return FALSE;
        }
        exponent = -exponent;
    } else if (!peak_exec_checkpoint_append_char(buffer,
                                                  buffer_size,
                                                  out,
                                                  '+')) {
        return FALSE;
    }
    return peak_exec_checkpoint_append_padded_u64(buffer,
                                                 buffer_size,
                                                 out,
                                                 (unsigned long long)exponent,
                                                 2);
}

static gboolean
peak_exec_checkpoint_write_row(int fd,
                               const char* name,
                               gulong num_calls,
                               gulong threads_seen,
                               gdouble total_time,
                               gdouble max_total_time,
                               gdouble min_total_time,
                               gdouble exclusive_time,
                               gfloat max_time,
                               gfloat min_time)
{
    char row[512];
    size_t out = 0;
    gulong safe_thread_count = threads_seen != 0 ? threads_seen : 1;
    gulong per_thread =
        num_calls / safe_thread_count +
        ((num_calls % safe_thread_count != 0) ? 1 : 0);

    if (exclusive_time < 0.0) {
        exclusive_time = 0.0;
    }
    if (total_time >= 0.0 && exclusive_time > total_time) {
        exclusive_time = total_time;
    }

    if (!peak_exec_checkpoint_write_csv_name(fd, name)) {
        return FALSE;
    }

    if (!peak_exec_checkpoint_append_char(row, sizeof(row), &out, ',') ||
        !peak_exec_checkpoint_append_u64(row,
                                         sizeof(row),
                                         &out,
                                         (unsigned long long)num_calls) ||
        !peak_exec_checkpoint_append_char(row, sizeof(row), &out, ',') ||
        !peak_exec_checkpoint_append_u64(row,
                                         sizeof(row),
                                         &out,
                                         (unsigned long long)per_thread) ||
        !peak_exec_checkpoint_append_char(row, sizeof(row), &out, ',') ||
        !peak_exec_checkpoint_append_u64(row,
                                         sizeof(row),
                                         &out,
                                         (unsigned long long)num_calls) ||
        !peak_exec_checkpoint_append_char(row, sizeof(row), &out, ',') ||
        !peak_exec_checkpoint_append_double(row,
                                            sizeof(row),
                                            &out,
                                            (double)max_time) ||
        !peak_exec_checkpoint_append_char(row, sizeof(row), &out, ',') ||
        !peak_exec_checkpoint_append_double(row,
                                            sizeof(row),
                                            &out,
                                            (double)min_time) ||
        !peak_exec_checkpoint_append_char(row, sizeof(row), &out, ',') ||
        !peak_exec_checkpoint_append_double(row,
                                            sizeof(row),
                                            &out,
                                            (double)total_time) ||
        !peak_exec_checkpoint_append_char(row, sizeof(row), &out, ',') ||
        !peak_exec_checkpoint_append_double(row,
                                            sizeof(row),
                                            &out,
                                            (double)exclusive_time) ||
        !peak_exec_checkpoint_append_char(row, sizeof(row), &out, ',') ||
        !peak_exec_checkpoint_append_double(row,
                                            sizeof(row),
                                            &out,
                                            (double)max_total_time) ||
        !peak_exec_checkpoint_append_char(row, sizeof(row), &out, ',') ||
        !peak_exec_checkpoint_append_double(row,
                                            sizeof(row),
                                            &out,
                                            (double)min_total_time) ||
        !peak_exec_checkpoint_append_char(row, sizeof(row), &out, ',') ||
        !peak_exec_checkpoint_append_double(
            row,
            sizeof(row),
            &out,
            (double)num_calls * peak_general_overhead) ||
        !peak_exec_checkpoint_append_char(row, sizeof(row), &out, '\n')) {
        return FALSE;
    }
    return peak_exec_checkpoint_write_all(fd, row, out);
}

gboolean
peak_general_listener_checkpoint_for_exec(unsigned long long checkpoint_index)
{
    char path[PATH_MAX];
    int fd = -1;
    size_t hook_count;
    size_t thread_count;
    size_t name_bytes = 0;
    size_t thread_storage_bytes;
    PeakExecCheckpointRecord* records = NULL;
    size_t* name_lengths = NULL;
    char* names = NULL;
    void* thread_storage = NULL;
    PeakExecCheckpointThreadSnapshot threads = {0};
    static const char header[] =
        "function,"
        "count,per_thread,per_rank,call_max_s,call_min_s,"
        "total_s,exclusive_s,thread_max_s,thread_min_s,overhead_s\n";

    if (!peak_general_listener_checkpoint_snapshot_lock()) {
        return FALSE;
    }
    hook_count = peak_hook_address_count;
    thread_count = peak_max_num_threads;
    pthread_mutex_unlock(&lock);

    if (hook_count > SIZE_MAX / sizeof(*records) ||
        hook_count > SIZE_MAX / sizeof(*name_lengths) ||
        thread_count > SIZE_MAX / sizeof(gdouble)) {
        errno = EOVERFLOW;
        return FALSE;
    }
    records = calloc(hook_count != 0 ? hook_count : 1, sizeof(*records));
    name_lengths = calloc(hook_count != 0 ? hook_count : 1,
                          sizeof(*name_lengths));
    if (records == NULL || name_lengths == NULL) {
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           NULL,
                                           NULL);
        return FALSE;
    }

    if (!peak_general_listener_checkpoint_snapshot_lock()) {
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           NULL,
                                           NULL);
        return FALSE;
    }
    for (size_t i = 0; i < hook_count; i++) {
        const char* name =
            (peak_demangled_strings != NULL &&
             peak_demangled_strings[i] != NULL) ?
                peak_demangled_strings[i] :
            (peak_hook_strings != NULL && peak_hook_strings[i] != NULL) ?
                peak_hook_strings[i] :
                "";
        size_t length = strlen(name) + 1;

        if (length > SIZE_MAX - name_bytes) {
            pthread_mutex_unlock(&lock);
            peak_exec_checkpoint_free_snapshot(records,
                                               name_lengths,
                                               NULL,
                                               NULL);
            errno = EOVERFLOW;
            return FALSE;
        }
        name_lengths[i] = length;
        name_bytes += length;
    }
    pthread_mutex_unlock(&lock);

    if (thread_count != 0 &&
        thread_count > SIZE_MAX /
            (sizeof(PeakGeneralListenerCheckpointShadow) + sizeof(gulong) +
             sizeof(int))) {
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           NULL,
                                           NULL);
        errno = EOVERFLOW;
        return FALSE;
    }
    thread_storage_bytes = thread_count *
        (sizeof(PeakGeneralListenerCheckpointShadow) + sizeof(gulong) +
         sizeof(int));
    names = malloc(name_bytes != 0 ? name_bytes : 1);
    int thread_storage_error = posix_memalign(
        &thread_storage,
        64,
        thread_storage_bytes != 0 ? thread_storage_bytes : 1);
    if (thread_storage_error != 0) {
        thread_storage = NULL;
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           names,
                                           thread_storage);
        errno = thread_storage_error;
        return FALSE;
    }
    if (names == NULL) {
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           names,
                                           thread_storage);
        return FALSE;
    }
    threads.shadows = thread_storage;
    threads.sequences = (gulong*)(threads.shadows + thread_count);
    threads.invalid = (int*)(threads.sequences + thread_count);

    if (!peak_general_listener_checkpoint_snapshot_lock()) {
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           names,
                                           thread_storage);
        return FALSE;
    }
    size_t name_offset = 0;
    for (size_t i = 0; i < hook_count; i++) {
        const char* name;
        size_t current_length;

        if (i >= peak_hook_address_count || hook_address == NULL ||
            array_listener == NULL || hook_address[i] == NULL ||
            array_listener[i] == NULL) {
            continue;
        }
        name =
            (peak_demangled_strings != NULL &&
             peak_demangled_strings[i] != NULL) ?
                peak_demangled_strings[i] :
            (peak_hook_strings != NULL && peak_hook_strings[i] != NULL) ?
                peak_hook_strings[i] :
                "";
        current_length = strlen(name) + 1;
        if (current_length > name_lengths[i] ||
            !peak_exec_checkpoint_copy_listener(
                PEAKGENERAL_LISTENER(array_listener[i]),
                &threads,
                thread_count)) {
            pthread_mutex_unlock(&lock);
            peak_exec_checkpoint_free_snapshot(records,
                                               name_lengths,
                                               names,
                                               thread_storage);
            return FALSE;
        }
        records[i].name = names + name_offset;
        memcpy(records[i].name, name, current_length);
        name_offset += name_lengths[i];

        for (size_t j = 0; j < thread_count; j++) {
            const PeakGeneralListenerCheckpointShadow* shadow =
                &threads.shadows[j];
            gulong calls = shadow->completed_calls;

            records[i].num_calls += calls;
            records[i].total_time += shadow->total_time;
            records[i].exclusive_time += shadow->exclusive_time;
            if (calls != 0) {
                records[i].threads_seen++;
                if (shadow->total_time > records[i].max_total_time) {
                    records[i].max_total_time = shadow->total_time;
                }
                if (shadow->total_time < records[i].min_total_time ||
                    records[i].threads_seen == 1) {
                    records[i].min_total_time = shadow->total_time;
                }
                if (shadow->max_time > records[i].max_time) {
                    records[i].max_time = shadow->max_time;
                }
                if (shadow->min_time < records[i].min_time ||
                    records[i].threads_seen == 1) {
                    records[i].min_time = shadow->min_time;
                }
            }
        }
    }
    pthread_mutex_unlock(&lock);

    for (unsigned int attempt = 0; attempt < 1024; attempt++) {
        if (peak_stats_exec_checkpoint_path(path,
                                            sizeof(path),
                                            checkpoint_index + attempt) != 0) {
            peak_exec_checkpoint_free_snapshot(records,
                                               name_lengths,
                                               names,
                                               thread_storage);
            return FALSE;
        }
        fd = peak_exec_checkpoint_open_exclusive(path);
        if (fd >= 0) {
            break;
        }
        if (errno != EEXIST) {
            peak_exec_checkpoint_free_snapshot(records,
                                               name_lengths,
                                               names,
                                               thread_storage);
            return FALSE;
        }
    }
    if (fd < 0) {
        errno = EEXIST;
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           names,
                                           thread_storage);
        return FALSE;
    }

    if (!peak_exec_checkpoint_write_all(fd, header, sizeof(header) - 1)) {
        peak_exec_checkpoint_close_fd(fd);
        peak_exec_checkpoint_unlink_path(path);
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           names,
                                           thread_storage);
        return FALSE;
    }

    for (size_t i = 0; i < hook_count; i++) {
        if (records[i].num_calls == 0 || records[i].name == NULL) {
            continue;
        }
        if (!peak_exec_checkpoint_write_row(fd,
                                            records[i].name,
                                            records[i].num_calls,
                                            records[i].threads_seen,
                                            records[i].total_time,
                                            records[i].max_total_time,
                                            records[i].min_total_time,
                                            records[i].exclusive_time,
                                            records[i].max_time,
                                            records[i].min_time)) {
            peak_exec_checkpoint_close_fd(fd);
            peak_exec_checkpoint_unlink_path(path);
            peak_exec_checkpoint_free_snapshot(records,
                                               name_lengths,
                                               names,
                                               thread_storage);
            return FALSE;
        }
    }

    if (!peak_exec_checkpoint_close_fd(fd)) {
        peak_exec_checkpoint_unlink_path(path);
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           names,
                                           thread_storage);
        return FALSE;
    }
    peak_exec_checkpoint_free_snapshot(records,
                                       name_lengths,
                                       names,
                                       thread_storage);
    return TRUE;
}

static FILE* peak_stats_csv_open(void) {
    char* out_csv = peak_stats_csv_path();
    FILE* fp = fopen(out_csv, "w");
    if (!fp) {
        g_printerr("[peak] failed to open stats csv '%s': %s\n", out_csv, strerror(errno));
        g_free(out_csv);
        return NULL;
    }
    g_free(out_csv);

    fprintf(fp,
            "function,"
            "count,per_thread,per_rank,call_max_s,call_min_s,"
            "total_s,exclusive_s,thread_max_s,thread_min_s,overhead_s\n");
    return fp;
}

#if defined(PEAK_ENABLE_TEST_HOOKS) && defined(HAVE_MPI)
typedef PeakMpiReportTestTuple PeakReportRankTuple;
#else
typedef struct {
    gboolean accounting_valid;
    unsigned int local_ranks;
    uint64_t stop_window_count;
    uint64_t failed_stop_window_count;
    double elapsed_seconds;
    double profile_seconds;
    double control_seconds;
    double management_seconds;
    double control_risk_seconds;
    double profile_control_risk_seconds;
    double profile_ratio;
    double control_ratio;
    double profile_control_risk_ratio;
    double control_risk_ratio;
    double management_ratio;
    double ratio;
} PeakReportRankTuple;
#endif

typedef struct {
    gboolean valid;
    gboolean accounting_valid;
    gboolean per_rank_max;
    unsigned int local_ranks;
    uint64_t stop_window_count;
    uint64_t failed_stop_window_count;
    double elapsed_seconds;
    double elapsed_min_seconds;
    double elapsed_max_seconds;
    double profile_seconds;
    double control_seconds;
    double management_seconds;
    double control_risk_seconds;
    double profile_control_risk_seconds;
    double profile_ratio;
    double control_ratio;
    double profile_control_risk_ratio;
    double control_risk_ratio;
    double management_ratio;
    double ratio;
    PeakReportRankTuple per_rank_max_reports[6];
    int per_rank_max_owner_ranks[6];
} PeakReportOverhead;

static void
peak_general_listener_print_mpi_maximum_reports(
    const PeakReportRankTuple maximum[6],
    const int owner_rank[6])
{
    peak_log_report("[peak] per-rank maximum profile+control overhead: owner_rank=%d profile_seconds=%.9f control_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f\n",
                    owner_rank[0], maximum[0].profile_seconds,
                    maximum[0].control_seconds,
                    maximum[0].elapsed_seconds,
                    maximum[0].ratio);
    peak_log_report("[peak] per-rank maximum profile overhead: owner_rank=%d profile_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f\n",
                    owner_rank[1], maximum[1].profile_seconds,
                    maximum[1].elapsed_seconds,
                    maximum[1].profile_ratio);
    peak_log_report("[peak] per-rank maximum control overhead: owner_rank=%d control_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f\n",
                    owner_rank[2], maximum[2].control_seconds,
                    maximum[2].elapsed_seconds,
                    maximum[2].control_ratio);
    peak_log_report("[peak] per-rank maximum profile+control risk overhead: owner_rank=%d profile_seconds=%.9f raw_control_seconds=%.9f local_ranks=%u control_risk_seconds=%.9f risk_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f\n",
                    owner_rank[4], maximum[4].profile_seconds,
                    maximum[4].control_seconds,
                    maximum[4].local_ranks,
                    maximum[4].control_risk_seconds,
                    maximum[4].profile_control_risk_seconds,
                    maximum[4].elapsed_seconds,
                    maximum[4].profile_control_risk_ratio);
    peak_log_report("[peak] per-rank maximum control risk overhead: owner_rank=%d raw_control_seconds=%.9f local_ranks=%u control_risk_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f\n",
                    owner_rank[5], maximum[5].control_seconds,
                    maximum[5].local_ranks,
                    maximum[5].control_risk_seconds,
                    maximum[5].elapsed_seconds,
                    maximum[5].control_risk_ratio);
    peak_log_report("[peak] per-rank maximum heartbeat management overhead: owner_rank=%d cpu_seconds=%.9f elapsed_seconds=%.9f ratio=%.9f\n",
                    owner_rank[3], maximum[3].management_seconds,
                    maximum[3].elapsed_seconds,
                    maximum[3].management_ratio);
}

static void
peak_general_listener_export_csv_result(gulong* sum_num_calls,
    gdouble* sum_total_time,
    gdouble* max_total_time,
    gdouble* min_total_time,
    gdouble* sum_exclusive_time,
    gfloat* sum_max_time,
    gfloat* sum_min_time,
    gulong* thread_count,
    const int rank_count)
{
    gboolean have_output = FALSE;
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i] && sum_num_calls[i] != 0) {
            have_output = TRUE;
            break;
        }
    }

    if (!have_output) {
        return;
    }

    FILE* csv = peak_stats_csv_open();

    if (csv) {
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (hook_address[i] && sum_num_calls[i] != 0) {
                double hook_profile_overhead =
                    (double)sum_num_calls[i] * peak_general_overhead;
                fprintf(csv, "\"%s\",%lu,%lu,%lu,%.9e,%.9e,%.9e,%.9e,%.9e,%.9e,%.9e\n",
                    peak_demangled_strings[i],
                    (unsigned long)sum_num_calls[i],
                    (unsigned long)sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                    (unsigned long)sum_num_calls[i] / rank_count,
                    (double)sum_max_time[i],
                    (double)sum_min_time[i],
                    (double)sum_total_time[i],
                    (double)sum_exclusive_time[i],
                    (double)max_total_time[i],
                    (double)min_total_time[i],
                    hook_profile_overhead);
            }
        }
        fclose(csv);
    }
}

enum {
    PEAK_TEXT_REPORT_FUNCTION_WIDTH = 32,
    PEAK_TEXT_REPORT_COLUMN_WIDTH = 10,
    PEAK_TEXT_REPORT_ROW_WIDTH =
        PEAK_TEXT_REPORT_FUNCTION_WIDTH +
        PEAK_TEXT_REPORT_COLUMN_WIDTH * 5 + 7,
};

static void
peak_general_listener_print_text_section(const char* title,
                                         const char* separator)
{
    peak_log_report("\n%s\n", title);
    peak_log_report("%s\n", separator);
}

static void
peak_general_listener_print_text_result(gulong* sum_num_calls,
                                        gdouble* sum_total_time,
                                        gdouble* max_total_time,
                                        gdouble* min_total_time,
                                        gdouble* sum_exclusive_time,
                                        gfloat* sum_max_time,
                                        gfloat* sum_min_time,
                                        gulong* thread_count,
                                        const int rank_count,
                                        const PeakReportOverhead* report_overhead)
{
    const guint max_function_width = PEAK_TEXT_REPORT_FUNCTION_WIDTH;
    const guint max_col_width = PEAK_TEXT_REPORT_COLUMN_WIDTH;
    const guint row_width = PEAK_TEXT_REPORT_ROW_WIDTH;
    char row_separator[PEAK_TEXT_REPORT_ROW_WIDTH + 1];
    char report_separator[PEAK_TEXT_REPORT_ROW_WIDTH + 1];
    memset(row_separator, '-', row_width);
    row_separator[row_width] = '\0';
    memset(report_separator, '=', row_width);
    report_separator[row_width] = '\0';

    char* argv_o;
    get_argv0(&argv_o);
    double total_overhead = 0.0;
    PeakDetachAccountingSnapshot detach_accounting;
    uint64_t stop_window_count = 0;
    uint64_t failed_stop_window_count = 0;
    double stop_window_seconds = 0.0;
    double stop_window_ratio = 0.0;
    double elapsed_seconds = peak_main_time;
    gboolean accounting_valid = FALSE;
    int stop_window_owner_rank = -1;
    size_t detached_targets = 0;
    size_t reattached_targets = 0;
    size_t revisited_targets = 0;
    size_t instrumented_targets = 0;
    size_t profiled_targets = 0;
    uint64_t total_calls = 0;
    gboolean total_calls_saturated = FALSE;
    gboolean have_output = FALSE;

    if (report_overhead != NULL && report_overhead->valid) {
        accounting_valid = report_overhead->accounting_valid;
        stop_window_count = report_overhead->stop_window_count;
        failed_stop_window_count = report_overhead->failed_stop_window_count;
        stop_window_seconds = report_overhead->control_seconds;
        elapsed_seconds = report_overhead->elapsed_seconds;
        if (report_overhead->per_rank_max) {
            stop_window_owner_rank =
                report_overhead->per_rank_max_owner_ranks[0];
        }
    } else {
        accounting_valid =
            peak_general_listener_runtime_accounting_snapshot(
                &detach_accounting);
        stop_window_count =
            peak_general_listener_control_window_count_since_heartbeat(
                &detach_accounting);
        failed_stop_window_count =
            peak_general_listener_failed_window_count_since_heartbeat(
                &detach_accounting);
        stop_window_seconds =
            (double)peak_general_listener_control_wall_ns_since_heartbeat(
                &detach_accounting) / 1e9;
    }
    if (elapsed_seconds > 0.0) {
        if (report_overhead != NULL && report_overhead->valid) {
            stop_window_ratio = stop_window_seconds / elapsed_seconds;
        } else {
            stop_window_ratio = stop_window_seconds / peak_main_time;
        }
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (array_listener_detached != NULL &&
            array_listener_detached[i]) {
            detached_targets++;
        }
        if (array_listener_reattached != NULL &&
            array_listener_reattached[i]) {
            reattached_targets++;
        }
        if (array_listener_revisited != NULL &&
            array_listener_revisited[i]) {
            revisited_targets++;
        }
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i]) {
            instrumented_targets++;
        }
        if (hook_address[i] && sum_num_calls[i] != 0) {
            uint64_t calls = (uint64_t)sum_num_calls[i];
            profiled_targets++;
            if (calls > UINT64_MAX - total_calls) {
                total_calls = UINT64_MAX;
                total_calls_saturated = TRUE;
            } else {
                total_calls += calls;
            }
            total_overhead +=
                (double)sum_num_calls[i] * peak_general_overhead;
            have_output = TRUE;
        }
    }
    if (report_overhead != NULL &&
        report_overhead->valid &&
        peak_general_listener_positive_finite(
            report_overhead->profile_seconds)) {
        total_overhead = report_overhead->profile_seconds;
    } else {
        double heartbeat_profile_seconds =
            peak_general_listener_report_heartbeat_overhead_seconds();
        if (peak_general_listener_positive_finite(
                heartbeat_profile_seconds) &&
            total_overhead <= DBL_MAX - heartbeat_profile_seconds) {
            total_overhead += heartbeat_profile_seconds;
        }
    }
    if (!have_output) {
        have_output =
            (report_overhead != NULL && report_overhead->valid) ||
            detached_targets > 0 ||
            reattached_targets > 0 ||
            peak_general_listener_positive_finite(stop_window_seconds);
    }
    if (have_output &&
        peak_general_listener_should_print_text(
            rank_count <= 1 && peak_mpi_env_size() > 1)) {
        const char* report_title = "PEAK Library Performance Report";
        int report_title_field =
            (int)(row_width + strlen(report_title)) / 2;

        peak_log_report("%s\n", report_separator);
        peak_log_report("%*s\n", report_title_field, report_title);
        peak_log_report("%s\n", report_separator);

        peak_general_listener_print_text_section("Application", row_separator);
        peak_log_report("Time: %f\n", elapsed_seconds);
        if (report_overhead != NULL && report_overhead->valid) {
            peak_log_report("[peak] per-rank elapsed range: min_seconds=%.9f max_seconds=%.9f\n",
                            report_overhead->elapsed_min_seconds,
                            report_overhead->elapsed_max_seconds);
        }
        peak_log_report("PEAK done with: %s\n", argv_o);
        if (rank_count > 1) {
            peak_log_report("Report scope: aggregate (%d MPI ranks)\n", rank_count);
        } else {
            peak_log_report("Report scope: local (1 process)\n");
        }
        peak_log_report("Instrumented targets: %zu\n", instrumented_targets);
        peak_log_report("Profiled targets: %zu\n", profiled_targets);
        peak_log_report("Recorded calls: %s%llu\n",
                        total_calls_saturated ? ">=" : "",
                        (unsigned long long)total_calls);

        peak_general_listener_print_text_section("Overhead summary",
                                                 row_separator);
        peak_log_report("Estimated overhead: %.3es per call and %.3es total\n", peak_general_overhead, total_overhead);
        if (report_overhead != NULL && report_overhead->valid) {
            if (report_overhead->per_rank_max) {
                peak_log_report("Aggregated profile estimate: %.9f s\n",
                                report_overhead->profile_seconds);
            } else {
                peak_log_report("Profile estimate: %.9f s (%8.4f%% of runtime)\n",
                                report_overhead->profile_seconds,
                                report_overhead->profile_ratio * 100.0);
                peak_log_report("Control stop windows: %.9f s (%8.4f%% of runtime)\n",
                                report_overhead->control_seconds,
                                report_overhead->control_ratio * 100.0);
                peak_log_report("Profile + control: %8.4f%% of runtime\n",
                                report_overhead->ratio * 100.0);
                peak_log_report("Local-rank risk estimate: %.9f s (%8.4f%% of runtime)\n",
                                report_overhead->profile_control_risk_seconds,
                                report_overhead->profile_control_risk_ratio * 100.0);
                peak_log_report("Heartbeat management CPU: %.9f s (%8.4f%% of runtime)\n",
                                report_overhead->management_seconds,
                                report_overhead->management_ratio * 100.0);
            }
        }

        peak_general_listener_print_text_section("Controller accounting",
                                                 row_separator);
        if (stop_window_owner_rank >= 0) {
            peak_log_report("Control windows: %llu, %.9f s (%8.4f%%), owner rank %d\n",
                            (unsigned long long)stop_window_count,
                            stop_window_seconds,
                            stop_window_ratio * 100.0,
                            stop_window_owner_rank);
        } else {
            peak_log_report("Control windows: %llu, %.9f s (%8.4f%%), %s\n",
                            (unsigned long long)stop_window_count,
                            stop_window_seconds,
                            stop_window_ratio * 100.0,
                            rank_count > 1 ? "rank-0 local" : "local");
        }
        peak_log_report("Failed control windows: %llu (%s scope)\n",
                        (unsigned long long)failed_stop_window_count,
                        rank_count > 1 ? "aggregate" : "local");
        peak_log_report("Accounting snapshot: %s\n",
                        accounting_valid ? "valid" : "unavailable");
        peak_log_report("Transition coverage (%s, ever observed): %zu detached, %zu reattached, %zu revisited\n",
                        rank_count > 1 ? "aggregate" : "local",
                        detached_targets,
                        reattached_targets,
                        revisited_targets);

        peak_general_listener_print_text_section(
            "Detailed metrics (stable key=value)", row_separator);
        if (report_overhead != NULL && report_overhead->valid) {
            if (report_overhead->per_rank_max) {
                peak_general_listener_print_mpi_maximum_reports(
                    report_overhead->per_rank_max_reports,
                    report_overhead->per_rank_max_owner_ranks);
            } else {
                unsigned int report_local_ranks =
                    peak_general_listener_local_mpi_ranks();
                double report_control_risk_seconds =
                    peak_general_listener_control_risk_seconds(
                        report_overhead->control_seconds);
                peak_log_report("[peak] local profile+control overhead: profile_seconds=%.9f control_seconds=%.9f profile_ratio=%.9f control_ratio=%.9f ratio=%.9f\n",
                                report_overhead->profile_seconds,
                                report_overhead->control_seconds,
                                report_overhead->profile_ratio,
                                report_overhead->control_ratio,
                                report_overhead->ratio);
                peak_log_report("[peak] local profile+local-rank-control risk: profile_seconds=%.9f raw_control_seconds=%.9f local_ranks=%u risk_control_seconds=%.9f ratio=%.9f\n",
                                report_overhead->profile_seconds,
                                report_overhead->control_seconds,
                                report_local_ranks,
                                report_control_risk_seconds,
                                report_overhead->profile_control_risk_ratio);
                peak_log_report("[peak] local profile+control risk overhead ratio: %.9f\n",
                                report_overhead->profile_control_risk_ratio);
                peak_log_report("[peak] local control risk overhead ratio: %.9f\n",
                                report_overhead->control_risk_ratio);
                peak_log_report("[peak] heartbeat management overhead: cpu_seconds=%.9f ratio=%.9f\n",
                                report_overhead->management_seconds,
                                report_overhead->management_ratio);
            }
        }
        if (stop_window_owner_rank >= 0) {
            peak_log_report("[peak] owner/local control stop-window overhead: owner_rank=%d windows=%llu wall_seconds=%.9f ratio=%.9f\n",
                            stop_window_owner_rank,
                            (unsigned long long)stop_window_count,
                            stop_window_seconds,
                            stop_window_ratio);
        } else {
            peak_log_report("[peak] %s control stop-window overhead: windows=%llu wall_seconds=%.9f ratio=%.9f\n",
                            rank_count > 1 ? "rank-0/local" : "local",
                            (unsigned long long)stop_window_count,
                            stop_window_seconds,
                            stop_window_ratio);
        }
        peak_log_report("[peak] %s failed control windows: windows=%llu snapshot_valid=%d\n",
                        rank_count > 1 ? "aggregate" : "local",
                        (unsigned long long)failed_stop_window_count,
                        accounting_valid ? 1 : 0);
        peak_log_report("[peak] %s final transition coverage: detached_targets=%zu reattached_targets=%zu revisited_targets=%zu\n",
                        rank_count > 1 ? "aggregate" : "local",
                        detached_targets,
                        reattached_targets,
                        revisited_targets);

        peak_general_listener_print_text_section("Function call statistics",
                                                 row_separator);
        peak_log_report("Detailed function statistics (call): counts and per-call timing in seconds\n");
        peak_log_report("|%*s|%*s|%*s|%*s|%*s|%*s|\n",
                   max_function_width, "function",
                   max_col_width, "calls",
                   max_col_width, "per thread",
                   max_col_width, "per rank",
                   max_col_width, "max (s)",
                   max_col_width, "min (s)");
        peak_log_report("%s\n", row_separator);
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (hook_address[i] && sum_num_calls[i] != 0) {
                const char* marker = "";
                if (array_listener_detached[i]) {
                    marker = array_listener_reattached[i] ? "**" : "*";
                }
                int function_field_width =
                    (int)max_function_width - (int)strlen(marker);
                char* truncated_name = truncate_string(
                    peak_demangled_strings[i], function_field_width);
                peak_log_report("|%*s%s|%*lu|%*lu|%*lu|%*.3e|%*.3e|\n",
                                function_field_width, truncated_name, marker,
                                max_col_width, sum_num_calls[i],
                                max_col_width, sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                                max_col_width, sum_num_calls[i] / rank_count,
                                max_col_width, sum_max_time[i],
                                max_col_width, sum_min_time[i]);
                free(truncated_name);
            }
        }
        peak_log_report("%s\n", row_separator);
        peak_log_report("Markers: * ever detached; ** ever reattached after detachment.\n");
        peak_log_report("Revisited targets are summarized in Controller accounting.\n");

        peak_general_listener_print_text_section("Function timing statistics",
                                                 row_separator);
        peak_log_report("Detailed function statistics (thread): aggregate timing in seconds\n");
        peak_log_report("|%*s|%*s|%*s|%*s|%*s|%*s|\n",
                   max_function_width, "function",
                   max_col_width, "total (s)",
                   max_col_width, "exclusive",
                   max_col_width, "max (s)",
                   max_col_width, "min (s)",
                   max_col_width, "est. cost");
        peak_log_report("%s\n", row_separator);
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (hook_address[i] && sum_num_calls[i] != 0) {
                char* truncated_name = truncate_string(peak_demangled_strings[i], max_function_width);
                peak_log_report("|%*s|%*.3f|%*.3f|%*.3f|%*.3f|%*.3e|\n",
                                max_function_width, truncated_name,
                                max_col_width, sum_total_time[i],
                                max_col_width, sum_exclusive_time[i],
                                max_col_width, max_total_time[i],
                                max_col_width, min_total_time[i],
                                max_col_width, (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                                    * peak_general_overhead);
                free(truncated_name);
            }
        }
        peak_log_report("%s\n", row_separator);
    }
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        g_free(peak_demangled_strings[i]);
    }
    g_free(peak_demangled_strings);
    peak_demangled_strings = NULL;
    free(argv_o);
}

static void
peak_general_listener_sanitize_output_times(gdouble* sum_total_time,
                                            gdouble* sum_exclusive_time)
{
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (sum_exclusive_time[i] < 0.0) {
            sum_exclusive_time[i] = 0.0;
        }
        if (sum_total_time[i] >= 0.0 &&
            sum_exclusive_time[i] > sum_total_time[i]) {
            sum_exclusive_time[i] = sum_total_time[i];
        }
    }
}

static double
peak_general_listener_profile_seconds_from_calls(gulong* sum_num_calls)
{
    double profile_seconds = 0.0;

    if (sum_num_calls == NULL) {
        return 0.0;
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i] && sum_num_calls[i] != 0) {
            profile_seconds +=
                (double)sum_num_calls[i] * peak_general_overhead;
        }
    }
    double heartbeat_profile_seconds =
        peak_general_listener_report_heartbeat_overhead_seconds();
    if (peak_general_listener_positive_finite(heartbeat_profile_seconds) &&
        profile_seconds <= DBL_MAX - heartbeat_profile_seconds) {
        profile_seconds += heartbeat_profile_seconds;
    }

    return profile_seconds;
}

static void
peak_general_listener_populate_report_risk_fields(PeakReportOverhead* report)
{
    double profile_control_risk_seconds = DBL_MAX;

    if (report == NULL) {
        return;
    }
    report->local_ranks = peak_general_listener_local_mpi_ranks();
    report->control_risk_seconds =
        peak_general_listener_control_risk_seconds(report->control_seconds);
    (void)peak_general_listener_add_nonnegative_finite(
        report->profile_seconds,
        report->control_risk_seconds,
        &profile_control_risk_seconds);
    report->profile_control_risk_seconds = profile_control_risk_seconds;
}

static PeakReportOverhead
peak_general_listener_local_report_overhead(gulong* sum_num_calls)
{
    PeakReportOverhead overhead = {0};
    PeakDetachAccountingSnapshot accounting;
    double elapsed_seconds = peak_main_time;
    double control_risk_seconds = 0.0;
    double profile_control_risk_seconds = DBL_MAX;

    if (peak_general_listener_final_report_snapshot.valid) {
        overhead = (PeakReportOverhead) {
            .valid = TRUE,
            .accounting_valid =
                peak_general_listener_final_report_snapshot.accounting_valid,
            .local_ranks =
                peak_general_listener_final_report_snapshot.local_ranks,
            .stop_window_count =
                peak_general_listener_final_report_snapshot.stop_window_count,
            .failed_stop_window_count =
                peak_general_listener_final_report_snapshot.failed_stop_window_count,
            .elapsed_seconds =
                peak_general_listener_final_report_snapshot.elapsed_seconds,
            .elapsed_min_seconds =
                peak_general_listener_final_report_snapshot.elapsed_seconds,
            .elapsed_max_seconds =
                peak_general_listener_final_report_snapshot.elapsed_seconds,
            .profile_seconds =
                peak_general_listener_final_report_snapshot.profile_seconds,
            .control_seconds =
                peak_general_listener_final_report_snapshot.control_seconds,
            .management_seconds =
                peak_general_listener_final_report_snapshot.management_seconds,
            .control_risk_seconds =
                peak_general_listener_final_report_snapshot.control_risk_seconds,
            .profile_control_risk_seconds =
                peak_general_listener_final_report_snapshot.profile_control_risk_seconds,
            .profile_ratio =
                peak_general_listener_final_report_snapshot.profile_ratio,
            .control_ratio =
                peak_general_listener_final_report_snapshot.control_ratio,
            .profile_control_risk_ratio =
                peak_general_listener_final_report_snapshot.profile_control_risk_ratio,
            .control_risk_ratio =
                peak_general_listener_final_report_snapshot.control_risk_ratio,
            .ratio = peak_general_listener_final_report_snapshot.ratio,
            .management_ratio =
                peak_general_listener_final_report_snapshot.management_ratio,
        };
        return overhead;
    }

    overhead.profile_seconds =
        peak_general_listener_profile_seconds_from_calls(sum_num_calls);
    overhead.accounting_valid =
        peak_general_listener_runtime_accounting_snapshot(&accounting);
    overhead.stop_window_count =
        peak_general_listener_control_window_count_since_heartbeat(
            &accounting);
    overhead.failed_stop_window_count =
        peak_general_listener_failed_window_count_since_heartbeat(
            &accounting);
    overhead.elapsed_seconds = elapsed_seconds;
    overhead.elapsed_min_seconds = elapsed_seconds;
    overhead.elapsed_max_seconds = elapsed_seconds;
    overhead.control_seconds =
        (double)peak_general_listener_control_wall_ns_since_heartbeat(
            &accounting) / 1e9;
    overhead.management_seconds =
        peak_general_listener_management_cpu_seconds();
    control_risk_seconds =
        peak_general_listener_control_risk_seconds(overhead.control_seconds);
    (void)peak_general_listener_add_nonnegative_finite(
        overhead.profile_seconds,
        control_risk_seconds,
        &profile_control_risk_seconds);
    if (elapsed_seconds > 0.0) {
        overhead.profile_ratio =
            overhead.profile_seconds / elapsed_seconds;
        overhead.control_ratio =
            overhead.control_seconds / elapsed_seconds;
        overhead.profile_control_risk_ratio =
            profile_control_risk_seconds / elapsed_seconds;
        overhead.control_risk_ratio =
            control_risk_seconds / elapsed_seconds;
        overhead.ratio =
            (overhead.profile_seconds + overhead.control_seconds) /
            elapsed_seconds;
        overhead.management_ratio =
            overhead.management_seconds / elapsed_seconds;
    }
    overhead.valid = TRUE;
    peak_general_listener_populate_report_risk_fields(&overhead);
    return overhead;
}

static void
peak_general_listener_print_result(gulong* sum_num_calls,
                                   gdouble* sum_total_time,
                                   gdouble* max_total_time,
                                   gdouble* min_total_time,
                                   gdouble* sum_exclusive_time,
                                   gfloat* sum_max_time,
                                   gfloat* sum_min_time,
                                   gulong* thread_count,
                                   const int rank_count,
                                   const PeakReportOverhead* report_overhead)
{
    peak_general_listener_sanitize_output_times(sum_total_time,
                                                sum_exclusive_time);
    peak_general_listener_export_csv_result(sum_num_calls,
                                            sum_total_time,
                                            max_total_time,
                                            min_total_time,
                                            sum_exclusive_time,
                                            sum_max_time,
                                            sum_min_time,
                                            thread_count,
                                            rank_count);
    peak_general_listener_print_text_result(sum_num_calls,
                                            sum_total_time,
                                            max_total_time,
                                            min_total_time,
                                            sum_exclusive_time,
                                            sum_max_time,
                                            sum_min_time,
                                            thread_count,
                                            rank_count,
                                            report_overhead);
}

#ifdef HAVE_MPI
static uint64_t
peak_general_listener_slot_identity_hash(size_t hook_id)
{
    const unsigned char* text;
    uint64_t hash = 1469598103934665603ULL;

    if (hook_id >= peak_hook_address_count) {
        return 0;
    }

    text = (const unsigned char*)
        (peak_demangled_strings != NULL && peak_demangled_strings[hook_id] != NULL ?
             peak_demangled_strings[hook_id] :
             peak_hook_strings != NULL && peak_hook_strings[hook_id] != NULL ?
                 peak_hook_strings[hook_id] : "");

    while (*text != '\0') {
        hash ^= (uint64_t)*text++;
        hash *= 1099511628211ULL;
    }

    return hash;
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

    for (text = (const unsigned char*)label; text != NULL && *text != '\0'; text++) {
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
    gboolean saw_shared_value = FALSE;

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
            saw_shared_value = TRUE;
        }
    }

    if (!saw_shared_value) {
        peak_socket_reduce_hash_text(&hash, "fallback", "single-launcher");
    }
    return hash;
}

static gboolean
peak_general_listener_has_duplicate_slot_names(void)
{
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        const char* left =
            peak_demangled_strings != NULL && peak_demangled_strings[i] != NULL ?
                peak_demangled_strings[i] :
                peak_hook_strings != NULL && peak_hook_strings[i] != NULL ?
                    peak_hook_strings[i] : NULL;

        if (left == NULL) {
            continue;
        }

        for (size_t j = i + 1; j < peak_hook_address_count; j++) {
            const char* right =
                peak_demangled_strings != NULL && peak_demangled_strings[j] != NULL ?
                    peak_demangled_strings[j] :
                    peak_hook_strings != NULL && peak_hook_strings[j] != NULL ?
                        peak_hook_strings[j] : NULL;

            if (right != NULL && strcmp(left, right) == 0) {
                return TRUE;
            }
        }
    }

    return FALSE;
}

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t rank;
    uint64_t hook_count;
    uint64_t session_token;
    double elapsed_seconds;
    double profile_seconds;
    double profile_ratio;
    double control_ratio;
    double profile_control_ratio;
    double profile_control_risk_ratio;
    double control_risk_ratio;
    double management_ratio;
    uint64_t failed_stop_window_count;
    uint32_t accounting_valid;
    uint32_t reserved;
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

#define PEAK_SOCKET_REDUCE_MAGIC 0x5045414b52454431ULL
#define PEAK_SOCKET_REDUCE_VERSION 8U
#define PEAK_SOCKET_REDUCE_RELEASE_ACK 0x51U
#define PEAK_SOCKET_REDUCE_RELEASE_FALLBACK 0x52U
#define PEAK_SOCKET_REDUCE_DEFAULT_TIMEOUT_MS 60000
#define PEAK_SOCKET_REDUCE_DEFAULT_PORT_BASE 42000
#define PEAK_SOCKET_REDUCE_DEFAULT_PORT_SPAN 20000

static int
peak_socket_reduce_parse_positive_int_env(const char* name, int default_value)
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
peak_socket_reduce_deadline_us(int timeout_ms)
{
    return g_get_monotonic_time() + (int64_t)timeout_ms * 1000;
}

static int
peak_socket_reduce_remaining_ms(int64_t deadline_us)
{
    int64_t now = g_get_monotonic_time();
    int64_t remaining = deadline_us - now;

    if (remaining <= 0) {
        return 0;
    }

    remaining = (remaining + 999) / 1000;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

static gboolean
peak_socket_reduce_poll_fd(int fd, short events, int64_t deadline_us)
{
    while (peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        struct pollfd pfd;
        int result;

        pfd.fd = fd;
        pfd.events = events;
        pfd.revents = 0;
        result = poll(&pfd, 1, peak_socket_reduce_remaining_ms(deadline_us));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FALSE;
        }
        if (result == 0) {
            return FALSE;
        }
        return (pfd.revents & events) != 0;
    }

    return FALSE;
}

static gboolean
peak_socket_reduce_send_all(int fd,
                            const void* data,
                            size_t size,
                            int64_t deadline_us)
{
    const char* cursor = (const char*)data;

    while (size > 0 && peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        if (!peak_socket_reduce_poll_fd(fd, POLLOUT, deadline_us)) {
            return FALSE;
        }
        ssize_t written = send(fd, cursor, size, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FALSE;
        }
        if (written == 0) {
            return FALSE;
        }
        cursor += written;
        size -= (size_t)written;
    }

    return size == 0;
}

static gboolean
peak_socket_reduce_recv_all(int fd,
                            void* data,
                            size_t size,
                            int64_t deadline_us)
{
    char* cursor = (char*)data;

    while (size > 0 && peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        if (!peak_socket_reduce_poll_fd(fd, POLLIN, deadline_us)) {
            return FALSE;
        }
        ssize_t read_count = recv(fd, cursor, size, MSG_WAITALL);
        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FALSE;
        }
        if (read_count == 0) {
            return FALSE;
        }
        cursor += read_count;
        size -= (size_t)read_count;
    }

    return size == 0;
}

static void
peak_socket_reduce_set_timeout(int fd, int timeout_ms)
{
    struct timeval tv;

    if (timeout_ms <= 0) {
        timeout_ms = 1;
    }

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static gboolean
peak_socket_reduce_hostlist_token_is_ascending_range(const char* token,
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
        return FALSE;
    }
    for (const char* cursor = token; cursor < token + token_len; cursor++) {
        if (cursor == dash) {
            continue;
        }
        if (!g_ascii_isdigit(*cursor)) {
            return FALSE;
        }
    }

    errno = 0;
    first = strtol(token, &end, 10);
    if (errno != 0 || end != dash) {
        return FALSE;
    }
    errno = 0;
    last = strtol(dash + 1, &end, 10);
    if (errno != 0 || end != token + token_len || first > last) {
        return FALSE;
    }

    if (range_prefix_len != NULL) {
        *range_prefix_len = (size_t)(dash - token);
    }
    return TRUE;
}

static gboolean
peak_socket_reduce_first_host_from_slurm_nodelist(const char* nodelist,
                                                 char* out,
                                                 size_t out_size)
{
    const char* bracket;
    const char* comma;
    const char* dash;
    size_t prefix_len;
    size_t token_len;
    size_t host_token_len;

    if (out == NULL || out_size == 0) {
        return FALSE;
    }
    out[0] = '\0';

    if (nodelist == NULL || nodelist[0] == '\0') {
        return FALSE;
    }

    bracket = strchr(nodelist, '[');
    if (bracket == NULL) {
        comma = strchr(nodelist, ',');
        token_len = comma != NULL ? (size_t)(comma - nodelist) : strlen(nodelist);
        if (token_len == 0 || token_len >= out_size) {
            return FALSE;
        }
        memcpy(out, nodelist, token_len);
        out[token_len] = '\0';
        return TRUE;
    }

    prefix_len = (size_t)(bracket - nodelist);
    comma = strchr(bracket + 1, ',');
    dash = strchr(bracket + 1, ']');
    if (dash == NULL) {
        return FALSE;
    }
    if (comma != NULL && comma < dash) {
        dash = comma;
    }
    token_len = (size_t)(dash - (bracket + 1));
    host_token_len = token_len;
    (void)peak_socket_reduce_hostlist_token_is_ascending_range(bracket + 1,
                                                               token_len,
                                                               &host_token_len);
    if (prefix_len + host_token_len == 0 ||
        prefix_len + host_token_len >= out_size) {
        return FALSE;
    }
    memcpy(out, nodelist, prefix_len);
    memcpy(out + prefix_len, bracket + 1, host_token_len);
    out[prefix_len + host_token_len] = '\0';
    return TRUE;
}

static gboolean
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
gboolean
peak_general_listener_test_first_slurm_host(const char* nodelist,
                                            char* out,
                                            size_t out_size)
{
    return peak_socket_reduce_first_host_from_slurm_nodelist(nodelist,
                                                            out,
                                                            out_size);
}
#endif

static gboolean
peak_socket_reduce_root_host(char* out, size_t out_size)
{
    const char* override = getenv(PEAK_OUTPUT_AGGREGATION_HOST_ENV);

    if (override != NULL && override[0] != '\0') {
        g_strlcpy(out, override, out_size);
        return TRUE;
    }

    if (peak_socket_reduce_first_slurm_host(out, out_size)) {
        return TRUE;
    }

    g_strlcpy(out, "127.0.0.1", out_size);
    return TRUE;
}

static PeakSocketReduceRecord*
peak_socket_reduce_build_records(gulong* sum_num_calls,
                                 gdouble* sum_total_time,
                                 gdouble* max_total_time,
                                 gdouble* min_total_time,
                                 gdouble* sum_exclusive_time,
                                 gfloat* sum_max_time,
                                 gfloat* sum_min_time,
                                 gulong* thread_count)
{
    PeakSocketReduceRecord* records =
        g_new0(PeakSocketReduceRecord, peak_hook_address_count);

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        records[i].identity_hash =
            peak_general_listener_slot_identity_hash(i);
        records[i].num_calls = (uint64_t)sum_num_calls[i];
        records[i].total_time = (double)sum_total_time[i];
        records[i].max_total_time = (double)max_total_time[i];
        records[i].min_total_time = (double)min_total_time[i];
        records[i].exclusive_time = (double)sum_exclusive_time[i];
        records[i].max_time = (float)sum_max_time[i];
        records[i].min_time = (float)sum_min_time[i];
        records[i].thread_count = (uint64_t)thread_count[i];
        records[i].detached = array_listener_detached[i] ? 1 : 0;
        records[i].reattached = array_listener_reattached[i] ? 1 : 0;
        records[i].revisited = array_listener_revisited[i] ? 1 : 0;
    }

    return records;
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

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
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

static gboolean
peak_socket_reduce_wait_connected(int fd, int64_t deadline_us)
{
    while (peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        struct pollfd pfd;
        int poll_result;
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);

        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        poll_result = poll(&pfd, 1, peak_socket_reduce_remaining_ms(deadline_us));
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FALSE;
        }
        if (poll_result == 0) {
            return FALSE;
        }
        if (getsockopt(fd,
                       SOL_SOCKET,
                       SO_ERROR,
                       &socket_error,
                       &socket_error_size) != 0 ||
            socket_error != 0) {
            return FALSE;
        }
        return TRUE;
    }

    return FALSE;
}

static int
peak_socket_reduce_connect(const char* host, int port, int64_t deadline_us)
{
    char port_text[16];
    char qualified_host[NI_MAXHOST];
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    struct addrinfo* entry;

    snprintf(port_text, sizeof(port_text), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    qualified_host[0] = '\0';
    if (host != NULL && strchr(host, '.') == NULL) {
        char local_host[NI_MAXHOST];
        if (gethostname(local_host, sizeof(local_host)) == 0) {
            local_host[sizeof(local_host) - 1] = '\0';
            char* domain = strchr(local_host, '.');
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
                for (entry = result; entry != NULL; entry = entry->ai_next) {
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

static gboolean
peak_socket_reduce_wait_for_release(const char* host,
                                    int port,
                                    const PeakSocketReduceHeader* header,
                                    int timeout_ms)
{
    int64_t deadline_us = peak_socket_reduce_deadline_us(timeout_ms);
    int fd = peak_socket_reduce_connect(host, port, deadline_us);
    PeakSocketReduceReleaseFrame frame;
    gboolean released;

    if (fd < 0 || header == NULL) {
        if (fd >= 0) {
            close(fd);
        }
        return FALSE;
    }

    memset(&frame, 0, sizeof(frame));
    frame.magic = header->magic;
    frame.version = header->version;
    frame.rank = header->rank;
    frame.hook_count = header->hook_count;
    frame.session_token = header->session_token;
    released = peak_socket_reduce_send_all(fd,
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
               frame.ack == PEAK_SOCKET_REDUCE_RELEASE_ACK;
    close(fd);
    return released;
}

static gboolean
peak_socket_reduce_release_peers(int port,
                                 gboolean* release_targets,
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
        return TRUE;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (ack == PEAK_SOCKET_REDUCE_RELEASE_ACK &&
        peak_env_value_truthy(
            getenv(PEAK_TEST_OUTPUT_AGGREGATION_RELEASE_FAIL_ENV))) {
        g_printerr("[peak] Socket aggregation release failure requested by test hook; peer ranks may fall back to local output\n");
        return FALSE;
    }
#endif

    for (int i = 1; i < size; i++) {
        if (release_targets[i]) {
            peer_count++;
        }
    }
    if (peer_count == 0) {
        return TRUE;
    }

    listener = peak_socket_reduce_create_listener(port);
    if (listener < 0) {
        g_printerr("[peak] Socket aggregation could not listen on release port %d; peer ranks may exit after timeout\n",
                   port);
        return FALSE;
    }

    deadline_us = peak_socket_reduce_deadline_us(timeout_ms);
    while (released < peer_count &&
           peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        struct pollfd pfd;
        int poll_result;
        int fd;
        PeakSocketReduceReleaseFrame frame;
        gboolean valid;

        pfd.fd = listener;
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll_result = poll(&pfd, 1, peak_socket_reduce_remaining_ms(deadline_us));
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
                frame.rank < (uint32_t)size &&
                frame.rank > 0 &&
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
            release_targets[frame.rank] = FALSE;
            released++;
        }
    }

    close(listener);
    if (released != peer_count) {
        peak_log_info("[peak] Socket aggregation released %u/%u peer ranks before timeout\n",
                   released,
                   peer_count);
        return FALSE;
    }
    return TRUE;
}

static gboolean
peak_socket_reduce_merge_records(PeakSocketReduceRecord* aggregate,
                                 const PeakSocketReduceRecord* incoming)
{
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (incoming[i].identity_hash != aggregate[i].identity_hash) {
            return FALSE;
        }

        aggregate[i].num_calls += incoming[i].num_calls;
        aggregate[i].total_time += incoming[i].total_time;
        if (incoming[i].max_total_time > aggregate[i].max_total_time) {
            aggregate[i].max_total_time = incoming[i].max_total_time;
        }
        if (incoming[i].min_total_time < aggregate[i].min_total_time) {
            aggregate[i].min_total_time = incoming[i].min_total_time;
        }
        aggregate[i].exclusive_time += incoming[i].exclusive_time;
        if (incoming[i].max_time > aggregate[i].max_time) {
            aggregate[i].max_time = incoming[i].max_time;
        }
        if (incoming[i].min_time < aggregate[i].min_time) {
            aggregate[i].min_time = incoming[i].min_time;
        }
        aggregate[i].thread_count += incoming[i].thread_count;
        aggregate[i].detached = aggregate[i].detached || incoming[i].detached;
        aggregate[i].reattached =
            aggregate[i].reattached || incoming[i].reattached;
        aggregate[i].revisited =
            aggregate[i].revisited || incoming[i].revisited;
    }

    return TRUE;
}

static void
peak_socket_reduce_records_to_arrays(PeakSocketReduceRecord* records,
                                     gulong* sum_num_calls,
                                     gdouble* sum_total_time,
                                     gdouble* max_total_time,
                                     gdouble* min_total_time,
                                     gdouble* sum_exclusive_time,
                                     gfloat* sum_max_time,
                                     gfloat* sum_min_time,
                                     gulong* thread_count,
                                     gboolean* detached,
                                     gboolean* reattached,
                                     gboolean* revisited)
{
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        sum_num_calls[i] = (gulong)records[i].num_calls;
        sum_total_time[i] = (gdouble)records[i].total_time;
        max_total_time[i] = (gdouble)records[i].max_total_time;
        min_total_time[i] = (gdouble)records[i].min_total_time;
        sum_exclusive_time[i] = (gdouble)records[i].exclusive_time;
        sum_max_time[i] = (gfloat)records[i].max_time;
        sum_min_time[i] = (gfloat)records[i].min_time;
        thread_count[i] = (gulong)records[i].thread_count;
        detached[i] = records[i].detached ? TRUE : FALSE;
        reattached[i] = records[i].reattached ? TRUE : FALSE;
        revisited[i] = records[i].revisited ? TRUE : FALSE;
    }
}

static gboolean
peak_socket_reduce_rank_size(int* rank_out, int* size_out, gboolean allow_mpi)
{
#ifdef HAVE_MPI
    int initialized = 0;
    int finalized = 0;

    if (allow_mpi) {
        MPI_Initialized(&initialized);
        MPI_Finalized(&finalized);
        if (initialized && !finalized) {
            MPI_Comm_rank(MPI_COMM_WORLD, rank_out);
            MPI_Comm_size(MPI_COMM_WORLD, size_out);
            return TRUE;
        }
    }
#endif

    long env_size = peak_mpi_env_size();
    long env_rank = peak_mpi_env_rank();
    if (env_size > 1) {
        if (env_rank >= 0 && env_rank < env_size) {
            *rank_out = (int)env_rank;
            *size_out = (int)env_size;
            return TRUE;
        }
        return FALSE;
    }

    if (env_rank > 0) {
        return FALSE;
    }

    *rank_out = 0;
    *size_out = 1;
    return TRUE;
}

static gboolean
peak_general_listener_socket_reduce_result_with_rank_source(
    gulong* sum_num_calls,
    gdouble* sum_total_time,
    gdouble* max_total_time,
    gdouble* min_total_time,
    gdouble* sum_exclusive_time,
    gfloat* sum_max_time,
    gfloat* sum_min_time,
    gulong* thread_count,
    gboolean allow_mpi_rank_query)
{
    int rank = 0;
    int size = 1;
    int port = peak_socket_reduce_port();
    int release_port = peak_socket_reduce_release_port(port);
    int timeout_ms = peak_socket_reduce_parse_positive_int_env(
        PEAK_OUTPUT_AGGREGATION_TIMEOUT_MS_ENV,
        PEAK_SOCKET_REDUCE_DEFAULT_TIMEOUT_MS);
    int64_t deadline_us = peak_socket_reduce_deadline_us(timeout_ms);
    uint64_t session_token = peak_socket_reduce_session_token();
    PeakSocketReduceRecord* local_records = NULL;
    PeakSocketReduceHeader header;
    PeakReportOverhead local_report = {0};

    if (!peak_socket_reduce_rank_size(&rank, &size, allow_mpi_rank_query)) {
        g_printerr("[peak] Socket aggregation could not determine rank/size from %s; skipping aggregate output\n",
                   allow_mpi_rank_query ? "MPI or launcher metadata"
                                        : "launcher metadata");
        return FALSE;
    }

    if (peak_general_listener_has_duplicate_slot_names()) {
        if (rank == 0) {
            g_printerr("[peak] Socket aggregation contains duplicate hook names, likely from multiple JIT generations; skipping aggregate output\n");
        }
        return FALSE;
    }

    local_report = peak_general_listener_local_report_overhead(sum_num_calls);
    if (size <= 1) {
        peak_general_listener_print_result(sum_num_calls,
                                           sum_total_time,
                                           max_total_time,
                                           min_total_time,
                                           sum_exclusive_time,
                                           sum_max_time,
                                           sum_min_time,
                                           thread_count,
                                           1,
                                           &local_report);
        return TRUE;
    }
    if (!peak_general_listener_positive_finite(
            local_report.elapsed_seconds)) {
        g_printerr("[peak] Socket aggregation observed an invalid local elapsed time; skipping aggregate output\n");
        return FALSE;
    }

    local_records = peak_socket_reduce_build_records(sum_num_calls,
                                                     sum_total_time,
                                                     max_total_time,
                                                     min_total_time,
                                                     sum_exclusive_time,
                                                     sum_max_time,
                                                     sum_min_time,
                                                     thread_count);
    memset(&header, 0, sizeof(header));
    header.magic = PEAK_SOCKET_REDUCE_MAGIC;
    header.version = PEAK_SOCKET_REDUCE_VERSION;
    header.rank = (uint32_t)rank;
    header.hook_count = (uint64_t)peak_hook_address_count;
    header.session_token = session_token;
    header.elapsed_seconds = local_report.elapsed_seconds;
    header.profile_seconds = local_report.profile_seconds;
    header.profile_ratio = local_report.profile_ratio;
    header.control_ratio = local_report.control_ratio;
    header.profile_control_ratio = local_report.ratio;
    header.profile_control_risk_ratio =
        local_report.profile_control_risk_ratio;
    header.control_risk_ratio = local_report.control_risk_ratio;
    header.management_ratio = local_report.management_ratio;
    header.failed_stop_window_count = local_report.failed_stop_window_count;
    header.accounting_valid = local_report.accounting_valid ? 1U : 0U;

    if (rank != 0) {
        char root_host[256];
        int fd;
        gboolean sent;
        gboolean released;

        if (!peak_socket_reduce_root_host(root_host, sizeof(root_host))) {
            g_printerr("[peak] Socket aggregation could not determine root host; skipping aggregate output\n");
            g_free(local_records);
            return FALSE;
        }

        if (rank > 0) {
            usleep((useconds_t)((rank % 1024) * 1000));
        }
        fd = peak_socket_reduce_connect(root_host, port, deadline_us);
        if (fd < 0) {
            g_printerr("[peak] Socket aggregation could not connect to %s:%d; skipping aggregate output\n",
                       root_host,
                       port);
            g_free(local_records);
            return FALSE;
        }

        sent = peak_socket_reduce_send_all(fd,
                                           &header,
                                           sizeof(header),
                                           deadline_us) &&
               peak_socket_reduce_send_all(fd,
                                           local_records,
                                           sizeof(PeakSocketReduceRecord) *
                                               peak_hook_address_count,
                                           deadline_us);
        close(fd);
        g_free(local_records);
        if (!sent) {
            g_printerr("[peak] Socket aggregation send failed; skipping aggregate output\n");
            return FALSE;
        }
        released = peak_socket_reduce_wait_for_release(root_host,
                                                       release_port,
                                                       &header,
                                                       timeout_ms);
        if (!released) {
            g_printerr("[peak] Socket aggregation release wait failed; skipping aggregate output\n");
            return FALSE;
        }
        return TRUE;
    }

    int listener = peak_socket_reduce_create_listener(port);
    if (listener < 0) {
        g_printerr("[peak] Socket aggregation could not listen on port %d; skipping aggregate output\n",
                   port);
        g_free(local_records);
        return FALSE;
    }

    gboolean* seen = g_new0(gboolean, size);
    gboolean* release_targets = g_new0(gboolean, size);
    PeakSocketReduceRecord* aggregate =
        g_memdup2(local_records,
                  sizeof(PeakSocketReduceRecord) * peak_hook_address_count);
    double socket_profile_seconds = local_report.profile_seconds;
    double socket_min_elapsed_seconds = local_report.elapsed_seconds;
    double socket_max_elapsed_seconds = local_report.elapsed_seconds;
    double socket_max_profile_ratio = local_report.profile_ratio;
    double socket_max_control_ratio = local_report.control_ratio;
    double socket_max_profile_control_ratio = local_report.ratio;
    double socket_max_profile_control_risk_ratio =
        local_report.profile_control_risk_ratio;
    double socket_max_control_risk_ratio = local_report.control_risk_ratio;
    double socket_max_management_ratio = local_report.management_ratio;
    uint64_t socket_failed_stop_window_count =
        local_report.failed_stop_window_count;
    gboolean socket_accounting_valid = local_report.accounting_valid;
    unsigned int received = 0;
    gboolean failed = FALSE;
    seen[0] = TRUE;

    while (received < (unsigned int)(size - 1) &&
           peak_socket_reduce_remaining_ms(deadline_us) > 0) {
        struct pollfd pfd;
        int poll_result;

        pfd.fd = listener;
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll_result = poll(&pfd, 1, peak_socket_reduce_remaining_ms(deadline_us));
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            failed = TRUE;
            break;
        }
        if (poll_result == 0) {
            break;
        }

        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            failed = TRUE;
            break;
        }
        peak_socket_reduce_set_timeout(
            fd,
            peak_socket_reduce_remaining_ms(deadline_us));

        PeakSocketReduceHeader incoming_header;
        PeakSocketReduceRecord* incoming =
            g_new0(PeakSocketReduceRecord, peak_hook_address_count);
        gboolean ok =
            peak_socket_reduce_recv_all(fd,
                                        &incoming_header,
                                        sizeof(incoming_header),
                                        deadline_us) &&
            incoming_header.magic == PEAK_SOCKET_REDUCE_MAGIC &&
            incoming_header.version == PEAK_SOCKET_REDUCE_VERSION &&
            incoming_header.session_token == session_token &&
            incoming_header.hook_count == (uint64_t)peak_hook_address_count &&
            incoming_header.rank < (uint32_t)size &&
            incoming_header.rank > 0 &&
            incoming_header.accounting_valid <= 1U &&
            incoming_header.failed_stop_window_count != UINT64_MAX &&
            peak_general_listener_positive_finite(
                incoming_header.elapsed_seconds) &&
            !seen[incoming_header.rank];
        if (ok) {
            release_targets[incoming_header.rank] = TRUE;
        }
        ok = ok &&
            peak_socket_reduce_recv_all(
                fd,
                incoming,
                sizeof(PeakSocketReduceRecord) * peak_hook_address_count,
                deadline_us) &&
            peak_socket_reduce_merge_records(aggregate, incoming);
        if (ok) {
            if (incoming_header.elapsed_seconds <
                socket_min_elapsed_seconds) {
                socket_min_elapsed_seconds =
                    incoming_header.elapsed_seconds;
            }
            if (incoming_header.elapsed_seconds >
                socket_max_elapsed_seconds) {
                socket_max_elapsed_seconds =
                    incoming_header.elapsed_seconds;
            }
            if (peak_general_listener_positive_finite(
                    incoming_header.profile_seconds) &&
                socket_profile_seconds <=
                    DBL_MAX - incoming_header.profile_seconds) {
                socket_profile_seconds += incoming_header.profile_seconds;
            }
            if (incoming_header.profile_control_ratio >
                socket_max_profile_control_ratio) {
                socket_max_profile_control_ratio =
                    incoming_header.profile_control_ratio;
            }
            if (incoming_header.profile_ratio >
                socket_max_profile_ratio) {
                socket_max_profile_ratio =
                    incoming_header.profile_ratio;
            }
            if (incoming_header.control_ratio >
                socket_max_control_ratio) {
                socket_max_control_ratio =
                    incoming_header.control_ratio;
            }
            if (incoming_header.profile_control_risk_ratio >
                socket_max_profile_control_risk_ratio) {
                socket_max_profile_control_risk_ratio =
                    incoming_header.profile_control_risk_ratio;
            }
            if (incoming_header.control_risk_ratio >
                socket_max_control_risk_ratio) {
                socket_max_control_risk_ratio =
                    incoming_header.control_risk_ratio;
            }
            if (incoming_header.management_ratio >
                socket_max_management_ratio) {
                socket_max_management_ratio =
                    incoming_header.management_ratio;
            }
            socket_failed_stop_window_count =
                peak_general_listener_add_uint64_saturated(
                    socket_failed_stop_window_count,
                    incoming_header.failed_stop_window_count);
            socket_accounting_valid =
                socket_accounting_valid &&
                incoming_header.accounting_valid != 0U;
        }
        close(fd);
        g_free(incoming);

        if (!ok) {
            failed = TRUE;
            break;
        }

        seen[incoming_header.rank] = TRUE;
        received++;
    }

    close(listener);
    g_free(local_records);
    g_free(seen);

    if (failed || received != (unsigned int)(size - 1)) {
        g_printerr("[peak] Socket aggregation received %u/%d peer ranks; skipping aggregate output on root\n",
                   received,
                   size - 1);
        (void)peak_socket_reduce_release_peers(release_port,
                                               release_targets,
                                               size,
                                               (uint64_t)peak_hook_address_count,
                                               session_token,
                                               timeout_ms,
                                               PEAK_SOCKET_REDUCE_RELEASE_FALLBACK);
        g_free(release_targets);
        g_free(aggregate);
        return FALSE;
    }

    gulong* socket_sum_num_calls = g_new0(gulong, peak_hook_address_count);
    gdouble* socket_sum_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* socket_max_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* socket_min_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* socket_sum_exclusive_time = g_new0(gdouble, peak_hook_address_count);
    gfloat* socket_sum_max_time = g_new0(gfloat, peak_hook_address_count);
    gfloat* socket_sum_min_time = g_new0(gfloat, peak_hook_address_count);
    gulong* socket_thread_count = g_new0(gulong, peak_hook_address_count);
    gboolean* socket_array_listener_detached =
        g_new0(gboolean, peak_hook_address_count);
    gboolean* socket_array_listener_reattached =
        g_new0(gboolean, peak_hook_address_count);
    gboolean* socket_array_listener_revisited =
        g_new0(gboolean, peak_hook_address_count);
    peak_socket_reduce_records_to_arrays(aggregate,
                                         socket_sum_num_calls,
                                         socket_sum_total_time,
                                         socket_max_total_time,
                                         socket_min_total_time,
                                         socket_sum_exclusive_time,
                                         socket_sum_max_time,
                                         socket_sum_min_time,
                                         socket_thread_count,
                                         socket_array_listener_detached,
                                         socket_array_listener_reattached,
                                         socket_array_listener_revisited);
    g_free(aggregate);

    gboolean* previous_array_listener_detached = array_listener_detached;
    gboolean* previous_array_listener_reattached = array_listener_reattached;
    gboolean* previous_array_listener_revisited = array_listener_revisited;
    array_listener_detached = socket_array_listener_detached;
    array_listener_reattached = socket_array_listener_reattached;
    array_listener_revisited = socket_array_listener_revisited;

    peak_general_listener_export_csv_result(socket_sum_num_calls,
                                            socket_sum_total_time,
                                            socket_max_total_time,
                                            socket_min_total_time,
                                            socket_sum_exclusive_time,
                                            socket_sum_max_time,
                                            socket_sum_min_time,
                                            socket_thread_count,
                                            size);
    if (!peak_socket_reduce_release_peers(release_port,
                                          release_targets,
                                          size,
                                          (uint64_t)peak_hook_address_count,
                                          session_token,
                                          timeout_ms,
                                          PEAK_SOCKET_REDUCE_RELEASE_ACK)) {
        peak_stats_csv_unlink();
        array_listener_detached = previous_array_listener_detached;
        array_listener_reattached = previous_array_listener_reattached;
        array_listener_revisited = previous_array_listener_revisited;
        g_free(socket_array_listener_detached);
        g_free(socket_array_listener_reattached);
        g_free(socket_array_listener_revisited);
        g_free(socket_sum_num_calls);
        g_free(socket_sum_total_time);
        g_free(socket_max_total_time);
        g_free(socket_min_total_time);
        g_free(socket_sum_exclusive_time);
        g_free(socket_sum_max_time);
        g_free(socket_sum_min_time);
        g_free(socket_thread_count);
        g_free(release_targets);
        return FALSE;
    }
    PeakReportOverhead socket_report = {0};
    socket_report.valid = TRUE;
    socket_report.accounting_valid = socket_accounting_valid;
    socket_report.per_rank_max = TRUE;
    socket_report.stop_window_count = local_report.stop_window_count;
    socket_report.failed_stop_window_count =
        socket_failed_stop_window_count;
    socket_report.elapsed_seconds = local_report.elapsed_seconds;
    socket_report.elapsed_min_seconds = socket_min_elapsed_seconds;
    socket_report.elapsed_max_seconds = socket_max_elapsed_seconds;
    socket_report.profile_seconds = socket_profile_seconds;
    socket_report.control_seconds = local_report.control_seconds;
    socket_report.management_seconds = local_report.management_seconds;
    socket_report.profile_ratio = socket_max_profile_ratio;
    socket_report.control_ratio = socket_max_control_ratio;
    socket_report.ratio = socket_max_profile_control_ratio;
    socket_report.profile_control_risk_ratio =
        socket_max_profile_control_risk_ratio;
    socket_report.control_risk_ratio = socket_max_control_risk_ratio;
    socket_report.management_ratio = socket_max_management_ratio;
    peak_general_listener_print_text_result(socket_sum_num_calls,
                                            socket_sum_total_time,
                                            socket_max_total_time,
                                            socket_min_total_time,
                                            socket_sum_exclusive_time,
                                            socket_sum_max_time,
                                            socket_sum_min_time,
                                            socket_thread_count,
                                            size,
                                            &socket_report);
    g_free(previous_array_listener_detached);
    g_free(previous_array_listener_reattached);
    g_free(previous_array_listener_revisited);
    g_free(socket_sum_num_calls);
    g_free(socket_sum_total_time);
    g_free(socket_max_total_time);
    g_free(socket_min_total_time);
    g_free(socket_sum_exclusive_time);
    g_free(socket_sum_max_time);
    g_free(socket_sum_min_time);
    g_free(socket_thread_count);
    g_free(release_targets);
    return TRUE;
}

static gboolean
peak_general_listener_socket_reduce_result(gulong* sum_num_calls,
                                           gdouble* sum_total_time,
                                           gdouble* max_total_time,
                                           gdouble* min_total_time,
                                           gdouble* sum_exclusive_time,
                                           gfloat* sum_max_time,
                                           gfloat* sum_min_time,
                                           gulong* thread_count)
{
    return peak_general_listener_socket_reduce_result_with_rank_source(
        sum_num_calls,
        sum_total_time,
        max_total_time,
        min_total_time,
        sum_exclusive_time,
        sum_max_time,
        sum_min_time,
        thread_count,
        TRUE);
}

static unsigned int
peak_mpi_output_collective_timeout_ms(void)
{
    unsigned int timeout_ms = peak_general_listener_parse_uint_env_default(
        PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS_ENV,
        PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS_DEFAULT);
    return timeout_ms == 0 ? PEAK_MPI_OUTPUT_AGGREGATION_TIMEOUT_MS_DEFAULT
                           : timeout_ms;
}

static gboolean
peak_mpi_reducer_forced_failure(const char* label)
{
#ifdef PEAK_ENABLE_TEST_HOOKS
    const char* value = getenv(PEAK_TEST_MPI_REDUCER_FAIL_LABEL_ENV);

    if (value == NULL || value[0] == '\0') {
        return FALSE;
    }
    return strcmp(value, "*") == 0 || strcmp(value, label) == 0;
#else
    (void)label;
    return FALSE;
#endif
}

static gboolean
peak_mpi_wait_collective_request(MPI_Request* request, const char* label)
{
    MPI_Status status;
    int done = 0;
    unsigned int timeout_ms = peak_mpi_output_collective_timeout_ms();
    double deadline = peak_second() + (double)timeout_ms / 1000.0;

    while (1) {
        int mpi_result = MPI_Test(request, &done, &status);
        if (mpi_result != MPI_SUCCESS) {
            g_printerr("[peak] MPI_Test for %s failed; abandoning MPI reducer without touching MPI again\n",
                       label);
            peak_general_listener_mark_mpi_reducer_failed_closed();
            return FALSE;
        }
        if (done) {
            return TRUE;
        }
        if (peak_second() >= deadline) {
            /*
             * As with the finalize proof, active nonblocking collectives do
             * not have a portable cancel path. Leave the request owned by MPI,
             * abandon PEAK's MPI reducer, and do not issue later MPI calls from
             * this teardown path.
             */
            g_printerr("[peak] MPI %s timed out after %u ms; abandoning MPI reducer without touching MPI again\n",
                       label,
                       timeout_ms);
            peak_general_listener_mark_mpi_reducer_failed_closed();
            return FALSE;
        }
        sched_yield();
    }
}

static gboolean
peak_mpi_allreduce_checked(const void* sendbuf,
                           void* recvbuf,
                           int count,
                           MPI_Datatype datatype,
                           MPI_Op op,
                           const char* label)
{
    if (peak_mpi_reducer_forced_failure(label)) {
        g_printerr("[peak] MPI reducer test hook forced failure for %s; abandoning MPI reducer without touching MPI again\n",
                   label);
        peak_general_listener_mark_mpi_reducer_failed_closed();
        return FALSE;
    }

    MPI_Request request = MPI_REQUEST_NULL;
    int mpi_result = MPI_Iallreduce(sendbuf,
                                    recvbuf,
                                    count,
                                    datatype,
                                    op,
                                    MPI_COMM_WORLD,
                                    &request);
    if (mpi_result != MPI_SUCCESS) {
        g_printerr("[peak] MPI_Iallreduce for %s failed; abandoning MPI reducer\n",
                   label);
        peak_general_listener_mark_mpi_reducer_failed_closed();
        return FALSE;
    }
    return peak_mpi_wait_collective_request(&request, label);
}

static gboolean
peak_mpi_reduce_checked(const void* sendbuf,
                        void* recvbuf,
                        int count,
                        MPI_Datatype datatype,
                        MPI_Op op,
                        int root,
                        const char* label)
{
    if (peak_mpi_reducer_forced_failure(label)) {
        g_printerr("[peak] MPI reducer test hook forced failure for %s; abandoning MPI reducer without touching MPI again\n",
                   label);
        peak_general_listener_mark_mpi_reducer_failed_closed();
        return FALSE;
    }

    MPI_Request request = MPI_REQUEST_NULL;
    int mpi_result = MPI_Ireduce(sendbuf,
                                 recvbuf,
                                 count,
                                 datatype,
                                 op,
                                 root,
                                 MPI_COMM_WORLD,
                                 &request);
    if (mpi_result != MPI_SUCCESS) {
        g_printerr("[peak] MPI_Ireduce for %s failed; abandoning MPI reducer\n",
                   label);
        peak_general_listener_mark_mpi_reducer_failed_closed();
        return FALSE;
    }
    return peak_mpi_wait_collective_request(&request, label);
}

static gboolean
peak_mpi_bcast_checked(void* buffer,
                       int count,
                       MPI_Datatype datatype,
                       int root,
                       const char* label)
{
    if (peak_mpi_reducer_forced_failure(label)) {
        g_printerr("[peak] MPI reducer test hook forced failure for %s; abandoning MPI reducer without touching MPI again\n",
                   label);
        peak_general_listener_mark_mpi_reducer_failed_closed();
        return FALSE;
    }

    MPI_Request request = MPI_REQUEST_NULL;
    int mpi_result = MPI_Ibcast(buffer,
                                count,
                                datatype,
                                root,
                                MPI_COMM_WORLD,
                                &request);
    if (mpi_result != MPI_SUCCESS) {
        g_printerr("[peak] MPI_Ibcast for %s failed; abandoning MPI reducer\n",
                   label);
        peak_general_listener_mark_mpi_reducer_failed_closed();
        return FALSE;
    }
    return peak_mpi_wait_collective_request(&request, label);
}

static PeakReportRankTuple
peak_general_listener_report_rank_tuple(const PeakReportOverhead* report)
{
    PeakReportRankTuple tuple = {0};

    if (report == NULL) {
        return tuple;
    }
    tuple.accounting_valid = report->accounting_valid;
    tuple.local_ranks = report->local_ranks;
    tuple.stop_window_count = report->stop_window_count;
    tuple.failed_stop_window_count = report->failed_stop_window_count;
    tuple.elapsed_seconds = report->elapsed_seconds;
    tuple.profile_seconds = report->profile_seconds;
    tuple.control_seconds = report->control_seconds;
    tuple.management_seconds = report->management_seconds;
    tuple.control_risk_seconds = report->control_risk_seconds;
    tuple.profile_control_risk_seconds =
        report->profile_control_risk_seconds;
    tuple.profile_ratio = report->profile_ratio;
    tuple.control_ratio = report->control_ratio;
    tuple.profile_control_risk_ratio = report->profile_control_risk_ratio;
    tuple.control_risk_ratio = report->control_risk_ratio;
    tuple.management_ratio = report->management_ratio;
    tuple.ratio = report->ratio;
    return tuple;
}

static gboolean
peak_mpi_bcast_report_rank_tuple(PeakReportRankTuple* tuple, int owner_rank)
{
    int accounting_valid;
    unsigned int local_ranks;
    uint64_t count_fields[2];
    double second_and_ratio_fields[12];

    if (tuple == NULL) {
        return FALSE;
    }
    accounting_valid = tuple->accounting_valid ? 1 : 0;
    local_ranks = tuple->local_ranks;
    count_fields[0] = tuple->stop_window_count;
    count_fields[1] = tuple->failed_stop_window_count;
    second_and_ratio_fields[0] = tuple->elapsed_seconds;
    second_and_ratio_fields[1] = tuple->profile_seconds;
    second_and_ratio_fields[2] = tuple->control_seconds;
    second_and_ratio_fields[3] = tuple->management_seconds;
    second_and_ratio_fields[4] = tuple->control_risk_seconds;
    second_and_ratio_fields[5] = tuple->profile_control_risk_seconds;
    second_and_ratio_fields[6] = tuple->profile_ratio;
    second_and_ratio_fields[7] = tuple->control_ratio;
    second_and_ratio_fields[8] = tuple->profile_control_risk_ratio;
    second_and_ratio_fields[9] = tuple->control_risk_ratio;
    second_and_ratio_fields[10] = tuple->management_ratio;
    second_and_ratio_fields[11] = tuple->ratio;

    if (!peak_mpi_bcast_checked(&accounting_valid,
                                1,
                                MPI_INT,
                                owner_rank,
                                "profile-control-ratio-tuple-valid") ||
        !peak_mpi_bcast_checked(&local_ranks,
                                1,
                                MPI_UNSIGNED,
                                owner_rank,
                                "profile-control-ratio-tuple-local-ranks") ||
        !peak_mpi_bcast_checked(count_fields,
                                2,
                                PEAK_MPI_UINT64_DATATYPE,
                                owner_rank,
                                "profile-control-ratio-tuple-counts") ||
        !peak_mpi_bcast_checked(second_and_ratio_fields,
                                12,
                                MPI_DOUBLE,
                                owner_rank,
                                "profile-control-ratio-tuple-doubles")) {
        return FALSE;
    }

    tuple->accounting_valid = accounting_valid != 0;
    tuple->local_ranks = local_ranks;
    tuple->stop_window_count = count_fields[0];
    tuple->failed_stop_window_count = count_fields[1];
    tuple->elapsed_seconds = second_and_ratio_fields[0];
    tuple->profile_seconds = second_and_ratio_fields[1];
    tuple->control_seconds = second_and_ratio_fields[2];
    tuple->management_seconds = second_and_ratio_fields[3];
    tuple->control_risk_seconds = second_and_ratio_fields[4];
    tuple->profile_control_risk_seconds = second_and_ratio_fields[5];
    tuple->profile_ratio = second_and_ratio_fields[6];
    tuple->control_ratio = second_and_ratio_fields[7];
    tuple->profile_control_risk_ratio = second_and_ratio_fields[8];
    tuple->control_risk_ratio = second_and_ratio_fields[9];
    tuple->management_ratio = second_and_ratio_fields[10];
    tuple->ratio = second_and_ratio_fields[11];
    return TRUE;
}

static gboolean
peak_mpi_reduce_report_rank_tuples(const PeakReportRankTuple* local_tuple,
                                   PeakReportRankTuple maximum_tuples[6],
                                   int owner_ranks[6],
                                   int rank)
{
    struct {
        double value;
        int rank;
    } local_locations[6], maximum_locations[6] = {{0}};
    double ratios[6];

    if (local_tuple == NULL || maximum_tuples == NULL ||
        owner_ranks == NULL) {
        return FALSE;
    }
    ratios[0] = local_tuple->ratio;
    ratios[1] = local_tuple->profile_ratio;
    ratios[2] = local_tuple->control_ratio;
    ratios[3] = local_tuple->management_ratio;
    ratios[4] = local_tuple->profile_control_risk_ratio;
    ratios[5] = local_tuple->control_risk_ratio;
    for (size_t i = 0; i < G_N_ELEMENTS(local_locations); i++) {
        local_locations[i].value = ratios[i];
        local_locations[i].rank = rank;
    }
    if (!peak_mpi_reduce_checked(local_locations,
                                 maximum_locations,
                                 6,
                                 MPI_DOUBLE_INT,
                                 MPI_MAXLOC,
                                 0,
                                 "profile-control-ratio-maxloc")) {
        return FALSE;
    }
    for (size_t i = 0; i < G_N_ELEMENTS(local_locations); i++) {
        int owner_rank = rank == 0 ? maximum_locations[i].rank : 0;

        if (!peak_mpi_bcast_checked(&owner_rank,
                                    1,
                                    MPI_INT,
                                    0,
                                    "profile-control-ratio-owner")) {
            return FALSE;
        }
        if (rank == owner_rank) {
            maximum_tuples[i] = *local_tuple;
        }
        if (!peak_mpi_bcast_report_rank_tuple(&maximum_tuples[i],
                                              owner_rank)) {
            return FALSE;
        }
        owner_ranks[i] = owner_rank;
    }
    return TRUE;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
gboolean
peak_general_listener_test_reduce_report_tuples(
    const PeakMpiReportTestTuple* local_tuple,
    PeakMpiReportTestTuple maximum_tuples[6],
    int owner_ranks[6])
{
    int rank;

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    return peak_mpi_reduce_report_rank_tuples(local_tuple,
                                              maximum_tuples,
                                              owner_ranks,
                                              rank);
}

void
peak_general_listener_test_print_report_tuples(
    const PeakMpiReportTestTuple maximum_tuples[6],
    const int owner_ranks[6])
{
    peak_general_listener_print_mpi_maximum_reports(maximum_tuples,
                                                    owner_ranks);
}

int
peak_general_listener_test_mpi_uint64_type_size(void)
{
    int type_size = 0;

    if (MPI_Type_size(PEAK_MPI_UINT64_DATATYPE, &type_size) != MPI_SUCCESS) {
        return -1;
    }
    return type_size;
}
#endif

static void
peak_general_listener_print_rank_local_result(gulong* sum_num_calls,
                                              gdouble* sum_total_time,
                                              gdouble* max_total_time,
                                              gdouble* min_total_time,
                                              gdouble* sum_exclusive_time,
                                              gfloat* sum_max_time,
                                              gfloat* sum_min_time,
                                              gulong* thread_count)
{
    PeakReportOverhead local_report =
        peak_general_listener_local_report_overhead(sum_num_calls);
    peak_general_listener_print_result(sum_num_calls,
                                       sum_total_time,
                                       max_total_time,
                                       min_total_time,
                                       sum_exclusive_time,
                                       sum_max_time,
                                       sum_min_time,
                                       thread_count,
                                       1,
                                       &local_report);
}

static gboolean
peak_general_listener_reduce_result(gulong* sum_num_calls,
                                    gdouble* sum_total_time,
                                    gdouble* max_total_time,
                                    gdouble* min_total_time,
                                    gdouble* sum_exclusive_time,
                                    gfloat* sum_max_time,
                                    gfloat* sum_min_time,
                                    gulong* thread_count)
{
    /*
     * The MPI reducer only runs after peak.c has proven that every rank reached
     * the finalizer path. Its collectives use bounded nonblocking wrappers so a
     * failed reducer can fall back to local output, but MPI_Test is still an MPI
     * progress call. For failure-prone teardown or jobs where MPI progress may
     * be compromised, use PEAK_OUTPUT_AGGREGATION=socket.
     */
    int rank, size;
    int init_flag;
    MPI_Initialized(&init_flag);
    if (!init_flag)
        MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    PeakReportOverhead local_report =
        peak_general_listener_local_report_overhead(sum_num_calls);
    PeakReportRankTuple local_report_tuple =
        peak_general_listener_report_rank_tuple(&local_report);
    PeakReportRankTuple max_ratio_reports[6] = {{0}};
    int max_ratio_owner_ranks[6] = {0};
    double local_profile_seconds = local_report.profile_seconds;
    double mpi_profile_seconds = 0.0;
    double local_elapsed_seconds = local_report.elapsed_seconds;
    double mpi_min_elapsed_seconds = 0.0;
    double mpi_max_elapsed_seconds = 0.0;
    uint64_t local_failed_stop_window_count =
        local_report.failed_stop_window_count;
    uint64_t mpi_failed_stop_window_count = 0;
    uint64_t mpi_max_failed_stop_window_count = 0;
    int local_elapsed_valid =
        peak_general_listener_positive_finite(local_elapsed_seconds) ? 1 : 0;
    int all_elapsed_valid = 0;
    int local_accounting_valid = local_report.accounting_valid ? 1 : 0;
    int all_accounting_valid = 0;
    gulong local_hook_count = (gulong)peak_hook_address_count;
    gulong min_hook_count = 0;
    gulong max_hook_count = 0;
    if (!peak_mpi_allreduce_checked(&local_elapsed_valid,
                                    &all_elapsed_valid,
                                    1,
                                    MPI_INT,
                                    MPI_MIN,
                                    "elapsed-valid")) {
        goto rank_local;
    }
    if (!all_elapsed_valid) {
        if (rank == 0) {
            g_printerr("[peak] MPI output observed an invalid per-rank elapsed time; writing rank-local output\n");
        }
        goto rank_local;
    }
    if (!peak_mpi_allreduce_checked(&local_accounting_valid,
                                    &all_accounting_valid,
                                    1,
                                    MPI_INT,
                                    MPI_MIN,
                                    "accounting-valid")) {
        goto rank_local;
    }
    if (!peak_mpi_allreduce_checked(&local_hook_count,
                                    &min_hook_count,
                                    1,
                                    MPI_UNSIGNED_LONG,
                                    MPI_MIN,
                                    "hook-count-min")) {
        goto rank_local;
    }
    if (!peak_mpi_allreduce_checked(&local_hook_count,
                                    &max_hook_count,
                                    1,
                                    MPI_UNSIGNED_LONG,
                                    MPI_MAX,
                                    "hook-count-max")) {
        goto rank_local;
    }
    int local_duplicate_names =
        peak_general_listener_has_duplicate_slot_names() ? 1 : 0;
    int any_duplicate_names = 0;
    if (!peak_mpi_allreduce_checked(&local_duplicate_names,
                                    &any_duplicate_names,
                                    1,
                                    MPI_INT,
                                    MPI_MAX,
                                    "duplicate-hook-name-check")) {
        goto rank_local;
    }
    if (any_duplicate_names) {
        if (rank == 0) {
            g_printerr("[peak] MPI output contains duplicate hook names, "
                       "likely from multiple JIT generations; writing "
                       "rank-local PEAK output\n");
        }
        peak_general_listener_print_rank_local_result(sum_num_calls,
                                                      sum_total_time,
                                                      max_total_time,
                                                      min_total_time,
                                                      sum_exclusive_time,
                                                      sum_max_time,
                                                      sum_min_time,
                                                      thread_count);
        return FALSE;
    }
    if (min_hook_count != max_hook_count) {
        if (rank == 0) {
            g_printerr("[peak] MPI ranks observed different JIT hook counts "
                       "(min=%lu max=%lu); writing rank-local PEAK output\n",
                       (unsigned long)min_hook_count,
                       (unsigned long)max_hook_count);
        }
        peak_general_listener_print_rank_local_result(sum_num_calls,
                                                      sum_total_time,
                                                      max_total_time,
                                                      min_total_time,
                                                      sum_exclusive_time,
                                                      sum_max_time,
                                                      sum_min_time,
                                                      thread_count);
        return FALSE;
    }
    uint64_t* slot_hashes = g_new0(uint64_t, peak_hook_address_count);
    uint64_t* min_slot_hashes = g_new0(uint64_t, peak_hook_address_count);
    uint64_t* max_slot_hashes = g_new0(uint64_t, peak_hook_address_count);
    gboolean slot_identity_mismatch = FALSE;
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        slot_hashes[i] = peak_general_listener_slot_identity_hash(i);
    }
    if (!peak_mpi_allreduce_checked(slot_hashes,
                                    min_slot_hashes,
                                    peak_hook_address_count,
                                    PEAK_MPI_UINT64_DATATYPE,
                                    MPI_MIN,
                                    "hook-slot-min-hash")) {
        g_free(max_slot_hashes);
        g_free(min_slot_hashes);
        g_free(slot_hashes);
        goto rank_local;
    }
    if (!peak_mpi_allreduce_checked(slot_hashes,
                                    max_slot_hashes,
                                    peak_hook_address_count,
                                    PEAK_MPI_UINT64_DATATYPE,
                                    MPI_MAX,
                                    "hook-slot-max-hash")) {
        g_free(max_slot_hashes);
        g_free(min_slot_hashes);
        g_free(slot_hashes);
        goto rank_local;
    }
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (min_slot_hashes[i] != max_slot_hashes[i]) {
            slot_identity_mismatch = TRUE;
            break;
        }
    }
    g_free(max_slot_hashes);
    g_free(min_slot_hashes);
    g_free(slot_hashes);
    if (slot_identity_mismatch) {
        if (rank == 0) {
            g_printerr("[peak] MPI ranks observed different JIT hook slot "
                       "identities; writing rank-local PEAK output\n");
        }
        peak_general_listener_print_rank_local_result(sum_num_calls,
                                                      sum_total_time,
                                                      max_total_time,
                                                      min_total_time,
                                                      sum_exclusive_time,
                                                      sum_max_time,
                                                      sum_min_time,
                                                      thread_count);
        return FALSE;
    }
    if (!peak_mpi_reduce_report_rank_tuples(&local_report_tuple,
                                            max_ratio_reports,
                                            max_ratio_owner_ranks,
                                            rank)) {
        goto rank_local;
    }
    if (!peak_mpi_reduce_checked(&local_profile_seconds,
                                 &mpi_profile_seconds,
                                 1,
                                 MPI_DOUBLE,
                                 MPI_SUM,
                                 0,
                                 "profile-seconds")) {
        goto rank_local;
    }
    if (!peak_mpi_allreduce_checked(&local_failed_stop_window_count,
                                    &mpi_max_failed_stop_window_count,
                                    1,
                                    PEAK_MPI_UINT64_DATATYPE,
                                    MPI_MAX,
                                    "failed-stop-window-max")) {
        goto rank_local;
    }
    if (mpi_max_failed_stop_window_count >
        (UINT64_MAX - 1) / (uint64_t)size) {
        mpi_failed_stop_window_count = UINT64_MAX - 1;
    } else if (!peak_mpi_reduce_checked(&local_failed_stop_window_count,
                                        &mpi_failed_stop_window_count,
                                        1,
                                        PEAK_MPI_UINT64_DATATYPE,
                                        MPI_SUM,
                                        0,
                                        "failed-stop-window-count")) {
        goto rank_local;
    }
    if (!peak_mpi_reduce_checked(&local_elapsed_seconds,
                                 &mpi_min_elapsed_seconds,
                                 1,
                                 MPI_DOUBLE,
                                 MPI_MIN,
                                 0,
                                 "elapsed-min")) {
        goto rank_local;
    }
    if (!peak_mpi_reduce_checked(&local_elapsed_seconds,
                                 &mpi_max_elapsed_seconds,
                                 1,
                                 MPI_DOUBLE,
                                 MPI_MAX,
                                 0,
                                 "elapsed-max")) {
        goto rank_local;
    }
    PeakReportOverhead mpi_report = {0};
    mpi_report.valid = TRUE;
    mpi_report.accounting_valid = all_accounting_valid != 0;
    mpi_report.per_rank_max = TRUE;
    mpi_report.stop_window_count = max_ratio_reports[0].stop_window_count;
    mpi_report.failed_stop_window_count = mpi_failed_stop_window_count;
    mpi_report.elapsed_seconds = max_ratio_reports[0].elapsed_seconds;
    mpi_report.elapsed_min_seconds = mpi_min_elapsed_seconds;
    mpi_report.elapsed_max_seconds = mpi_max_elapsed_seconds;
    mpi_report.profile_seconds = mpi_profile_seconds;
    mpi_report.control_seconds = max_ratio_reports[0].control_seconds;
    mpi_report.management_seconds = max_ratio_reports[0].management_seconds;
    mpi_report.ratio = max_ratio_reports[0].ratio;
    mpi_report.profile_ratio = max_ratio_reports[1].profile_ratio;
    mpi_report.control_ratio = max_ratio_reports[2].control_ratio;
    mpi_report.management_ratio = max_ratio_reports[3].management_ratio;
    mpi_report.profile_control_risk_ratio =
        max_ratio_reports[4].profile_control_risk_ratio;
    mpi_report.control_risk_ratio = max_ratio_reports[5].control_risk_ratio;
    memcpy(mpi_report.per_rank_max_reports,
           max_ratio_reports,
           sizeof(max_ratio_reports));
    for (size_t i = 0; i < G_N_ELEMENTS(mpi_report.per_rank_max_owner_ranks);
         i++) {
        mpi_report.per_rank_max_owner_ranks[i] = max_ratio_owner_ranks[i];
    }
    gulong* mpi_sum_num_calls = g_new0(gulong, peak_hook_address_count);
    gdouble* mpi_sum_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* mpi_max_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* mpi_min_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* mpi_sum_exclusive_time = g_new0(gdouble, peak_hook_address_count);
    gfloat* mpi_sum_max_time = g_new0(gfloat, peak_hook_address_count);
    gfloat* mpi_sum_min_time = g_new0(gfloat, peak_hook_address_count);
    gulong* mpi_thread_count = g_new0(gulong, peak_hook_address_count);
    gboolean* mpi_array_listener_detached = g_new0(gboolean, peak_hook_address_count);
    gboolean* mpi_array_listener_reattached = g_new0(gboolean, peak_hook_address_count);
    gboolean* mpi_array_listener_revisited = g_new0(gboolean, peak_hook_address_count);
    if (!peak_mpi_reduce_checked(sum_num_calls, mpi_sum_num_calls, peak_hook_address_count, MPI_UNSIGNED_LONG, MPI_SUM, 0, "sum-num-calls") ||
        !peak_mpi_reduce_checked(sum_total_time, mpi_sum_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_SUM, 0, "sum-total-time") ||
        !peak_mpi_reduce_checked(max_total_time, mpi_max_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_MAX, 0, "max-total-time") ||
        !peak_mpi_reduce_checked(min_total_time, mpi_min_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_MIN, 0, "min-total-time") ||
        !peak_mpi_reduce_checked(sum_exclusive_time, mpi_sum_exclusive_time, peak_hook_address_count, MPI_DOUBLE, MPI_SUM, 0, "sum-exclusive-time") ||
        !peak_mpi_reduce_checked(sum_max_time, mpi_sum_max_time, peak_hook_address_count, MPI_FLOAT, MPI_MAX, 0, "sum-max-time") ||
        !peak_mpi_reduce_checked(sum_min_time, mpi_sum_min_time, peak_hook_address_count, MPI_FLOAT, MPI_MIN, 0, "sum-min-time") ||
        !peak_mpi_reduce_checked(thread_count, mpi_thread_count, peak_hook_address_count, MPI_UNSIGNED_LONG, MPI_SUM, 0, "thread-count") ||
        !peak_mpi_reduce_checked(array_listener_detached, mpi_array_listener_detached, peak_hook_address_count, MPI_INT, MPI_MAX, 0, "detached-marker") ||
        !peak_mpi_reduce_checked(array_listener_reattached, mpi_array_listener_reattached, peak_hook_address_count, MPI_INT, MPI_MAX, 0, "reattached-marker") ||
        !peak_mpi_reduce_checked(array_listener_revisited, mpi_array_listener_revisited, peak_hook_address_count, MPI_INT, MPI_MAX, 0, "revisited-marker")) {
        g_free(mpi_sum_num_calls);
        g_free(mpi_sum_total_time);
        g_free(mpi_max_total_time);
        g_free(mpi_min_total_time);
        g_free(mpi_sum_exclusive_time);
        g_free(mpi_sum_max_time);
        g_free(mpi_sum_min_time);
        g_free(mpi_thread_count);
        g_free(mpi_array_listener_detached);
        g_free(mpi_array_listener_reattached);
        g_free(mpi_array_listener_revisited);
        goto rank_local;
    }
    g_free(array_listener_detached);
    g_free(array_listener_reattached);
    g_free(array_listener_revisited);
    array_listener_detached = mpi_array_listener_detached;
    array_listener_reattached = mpi_array_listener_reattached;
    array_listener_revisited = mpi_array_listener_revisited;
    if (rank == 0) {
        peak_general_listener_print_result(mpi_sum_num_calls,
                                           mpi_sum_total_time,
                                           mpi_max_total_time,
                                           mpi_min_total_time,
                                           mpi_sum_exclusive_time,
                                           mpi_sum_max_time,
                                           mpi_sum_min_time,
                                           mpi_thread_count,
                                           size,
                                           &mpi_report);
    }
    g_free(mpi_sum_num_calls);
    g_free(mpi_sum_total_time);
    g_free(mpi_max_total_time);
    g_free(mpi_min_total_time);
    g_free(mpi_sum_exclusive_time);
    g_free(mpi_sum_max_time);
    g_free(mpi_sum_min_time);
    g_free(mpi_thread_count);
    return TRUE;

rank_local:
    if (peak_general_listener_mpi_reducer_failed_closed() &&
        peak_socket_reduce_fallback_enabled()) {
        g_printerr("[peak] MPI reducer failed; trying PEAK-owned socket aggregation fallback without further MPI calls\n");
        if (peak_general_listener_socket_reduce_result_with_rank_source(
                sum_num_calls,
                sum_total_time,
                max_total_time,
                min_total_time,
                sum_exclusive_time,
                sum_max_time,
                sum_min_time,
                thread_count,
                FALSE)) {
            return FALSE;
        }
        g_printerr("[peak] MPI reducer socket fallback failed; writing rank-local output\n");
    }
    peak_general_listener_print_rank_local_result(sum_num_calls,
                                                  sum_total_time,
                                                  max_total_time,
                                                  min_total_time,
                                                  sum_exclusive_time,
                                                  sum_max_time,
                                                  sum_min_time,
                                                  thread_count);
    return FALSE;
}
#endif
