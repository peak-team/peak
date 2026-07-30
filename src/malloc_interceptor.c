#define _GNU_SOURCE
#include "malloc_interceptor.h"
#include "malloc_otf2.h"
#include "logging.h"
#include <sched.h>

/*=========================
  Types
=========================*/

typedef struct {
    void*   ptr;
    size_t  size;
} AllocationEntry;

/*=========================
  mmapped event buffer (binary, fixed capacity)
=========================*/
#ifndef PEAK_MEMLOG_CHUNK_EVENTS
#define PEAK_MEMLOG_CHUNK_EVENTS (1u * 500u * 1000u) /* fixed event capacity */
#endif

/*=========================
  Globals
=========================*/
extern gboolean            peak_memory_track_all;
extern size_t              peak_hook_address_count;
extern char**              peak_hook_strings;
static GumInterceptor*     malloc_interceptor;
static pthread_mutex_t     track_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t     caller_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t     memlog_finalize_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t     memlog_admission_mutex = PTHREAD_MUTEX_INITIALIZER;
static GumMetalHashTable*  track_table = NULL;
static GumMetalHashTable*  memory_caller_target_table = NULL;
static _Atomic int         cleanup_in_progress = 0;
static _Atomic int         active_alloc_hooks = 0;
static gulong              max_memory = 0;
static gulong              current_memory = 0;
static gpointer            malloc_addr = NULL;
static gpointer            free_addr = NULL;
static gpointer            calloc_addr = NULL;
static gpointer            realloc_addr = NULL;
static gpointer            aligned_alloc_addr = NULL;
static gpointer            posix_memalign_addr = NULL;
static PeakMemLog          g_memlog = {
    .fd = -1,
    .state = ATOMIC_VAR_INIT(PEAK_MEMLOG_UNINITIALIZED),
};
static __thread int        in_peak_alloc_hook = 0;
static __thread int        in_backtrace = 0;

#ifdef PEAK_ENABLE_TEST_HOOKS
static _Atomic PeakMemLogTestFailure g_memlog_test_failure = PEAK_MEMLOG_TEST_FAIL_NONE;
static _Atomic int g_memlog_test_pause_before_commit = 0;
static _Atomic int g_memlog_test_writer_paused = 0;
static _Atomic size_t g_memlog_test_capacity_override = 0;
static _Atomic size_t g_memlog_test_exported = 0;

static void* peak_memlog_test_realloc_failure(void* ptr, size_t size) {
    (void) ptr;
    (void) size;
    errno = ENOMEM;
    return NULL;
}
#endif

/*=========================
  Original function pointers
=========================*/

static void* (*original_malloc)(size_t size);
static void  (*original_free)(void* ptr);
static void* (*original_calloc)(size_t nmemb, size_t size);
static void* (*original_realloc)(void* ptr, size_t size);
static void* (*original_aligned_alloc)(size_t alignment, size_t size);
static int   (*original_posix_memalign)(void** memptr, size_t alignment, size_t size);

/*=========================
  Internal alloc helpers (use originals)
=========================*/

static void* internal_malloc(size_t size) {
#ifdef PEAK_ENABLE_TEST_HOOKS
    PeakMemLogTestFailure expected = PEAK_MEMLOG_TEST_FAIL_ALLOCATION;
    if (atomic_compare_exchange_strong_explicit(&g_memlog_test_failure,
                                                &expected,
                                                PEAK_MEMLOG_TEST_FAIL_NONE,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        errno = ENOMEM;
        return NULL;
    }
#endif
    return original_malloc(size);
}
static void  internal_free(void* ptr)     { original_free(ptr); }

/*=========================
  Fast time & TID helpers
=========================*/

static inline uint32_t peak_gettid(void) {
#ifdef SYS_gettid
    return (uint32_t) syscall(SYS_gettid);
#else
    return (uint32_t) getpid();
#endif
}

static inline uint64_t nsec_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

static int checked_add_size(size_t a, size_t b, size_t* out) {
    if (a > SIZE_MAX - b) return 0;
    *out = a + b;
    return 1;
}

static int checked_mul_size(size_t a, size_t b, size_t* out) {
    if (a != 0 && b > SIZE_MAX / a) return 0;
    *out = a * b;
    return 1;
}

static int page_align_up(size_t v, size_t* out) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return 0;
    size_t p = (size_t) page_size;
    size_t remainder = v % p;
    if (remainder == 0) {
        *out = v;
        return 1;
    }
    return checked_add_size(v, p - remainder, out);
}

static int size_fits_off_t(size_t size) {
    const unsigned int bits = (unsigned int) (sizeof(off_t) * CHAR_BIT);
    uintmax_t max_off;

    if (bits >= sizeof(uintmax_t) * CHAR_BIT) {
        max_off = UINTMAX_MAX >> 1;
    } else {
        max_off = (UINTMAX_C(1) << (bits - 1)) - 1;
    }
    return (uintmax_t) size <= max_off;
}

static int parse_memlog_capacity(const char* value, size_t* out) {
    char* end = NULL;
    unsigned long long parsed;

    if (!value || !*value || value[0] == '-') return 0;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' ||
        parsed == 0 || parsed > SIZE_MAX) {
        return 0;
    }
    *out = (size_t) parsed;
    return 1;
}

static int size_to_event_delta(size_t size, int64_t* out) {
    if ((uintmax_t) size > INT64_MAX) return 0;
    *out = (int64_t) size;
    return 1;
}

/*=========================
  MPI helper (as-is)
=========================*/

static void get_mpi_rank(int *rank) {
    if (check_MPI()) {
        *rank = get_MPI_local_rank();
    }
}

/*=========================
  Backtrace filter
=========================*/

gboolean str_equal_function(gconstpointer a, gconstpointer b) {
    return g_strcmp0((const gchar *)a, (const gchar *)b) == 0;
}

static int peak_log_backtrace_malloc() {
    if (peak_memory_track_all) return 1; // Track all memory allocation events

    if (in_backtrace) return 0; // prevent recursion
    int flag = 0;
    in_backtrace++;

    g_autoptr(GumBacktracer) backtracer = gum_backtracer_make_accurate();
    GumCpuContext *cpu_context = NULL; // walk from here
    GumReturnAddressArray retaddrs;
    gum_backtracer_generate(backtracer, cpu_context, &retaddrs);
    for (guint i = 0; i != retaddrs.len; i++) {
        const gchar *sym = gum_symbol_name_from_address(retaddrs.items[i]);
        if (!sym || !*sym) continue;

        gchar *symbol_name = strdup(sym);
        removeTrailingOffset(symbol_name);
        char *demangledName = cxa_demangle(symbol_name);
        char *func          = extract_function_name(demangledName);

        // lookup peak CPU target string map
        // if found in map, add this memory profile to entry
        pthread_mutex_lock(&caller_mutex);
        if (memory_caller_target_table) {
            char* pm = gum_metal_hash_table_lookup(memory_caller_target_table, func);
            if (pm != NULL) {
                flag = 1;
            }
        }
        pthread_mutex_unlock(&caller_mutex);

        free(func);
        free(symbol_name);
        free(demangledName);

        if (flag) {
            break;
        }
    }

    in_backtrace--;
    return flag;
}

/*=========================
  Path builder (no malloc)
=========================*/

static void build_paths(char out_tmp[512], char out_csv[512], char out_otf2[512]) {
    char base[256] = {0};
    const char *env_path = getenv("PEAK_MEMLOG_PATH");
    if (env_path && *env_path) {
        size_t n = strlen(env_path);
        if (n >= sizeof(base)) n = sizeof(base) - 1;
        memcpy(base, env_path, n);
        base[n] = '\0';
    } else {
        snprintf(base, sizeof(base), "./peak_memlog");
    }

    int rank = -1;
    get_mpi_rank(&rank);

    int pid = (int) getpid();
    if (rank == -1) {
        snprintf(out_tmp, 512, "%s-p%d.tmp", base, pid);
        snprintf(out_csv, 512, "%s-p%d.csv", base, pid);
        snprintf(out_otf2, 512, "p%d", pid);
    } else {
        snprintf(out_tmp, 512, "%s-r%d-p%d.tmp", base, rank, pid);
        snprintf(out_csv, 512, "%s-r%d-p%d.csv", base, rank, pid);
        snprintf(out_otf2, 512, "r%d-p%d", rank, pid);
    }
}

/*=========================
  Memlog: fixed mapping / publish / finalize
=========================*/

static int peak_memlog_writer_enter(void) {
    int admitted = 0;
    pthread_mutex_lock(&memlog_admission_mutex);
    if (atomic_load_explicit(&g_memlog.state, memory_order_acquire) ==
        PEAK_MEMLOG_ACTIVE) {
        admitted = 1;
    }
    if (admitted) {
        atomic_fetch_add_explicit(&g_memlog.active_writers, 1, memory_order_acq_rel);
    }
    pthread_mutex_unlock(&memlog_admission_mutex);
    return admitted;
}

static void peak_memlog_writer_leave(void) {
    atomic_fetch_sub_explicit(&g_memlog.active_writers, 1, memory_order_release);
}

static _Atomic uint8_t* peak_memlog_ready_flags(void) {
    return (_Atomic uint8_t*) ((uint8_t*) g_memlog.map + g_memlog.ready_offset);
}

static int peak_memlog_reserve_slot(size_t* index_out) {
    size_t reserved = atomic_load_explicit(&g_memlog.reserved, memory_order_relaxed);

    for (;;) {
        if (reserved >= g_memlog.capacity_events) {
            atomic_fetch_add_explicit(&g_memlog.dropped, 1, memory_order_relaxed);
            return 0;
        }
        if (atomic_compare_exchange_weak_explicit(&g_memlog.reserved,
                                                  &reserved,
                                                  reserved + 1,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            *index_out = reserved;
            return 1;
        }
    }
}

static int peak_memlog_ftruncate(int fd, size_t bytes) {
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (atomic_load_explicit(&g_memlog_test_failure, memory_order_acquire) ==
        PEAK_MEMLOG_TEST_FAIL_SIZING) {
        errno = ENOSPC;
        return -1;
    }
#endif
    return ftruncate(fd, (off_t) bytes);
}

static inline void peak_log_event(uint64_t ts, int64_t delta, uint64_t current, uint8_t op) {
    size_t index;
    PeakMemEvent* event;
    _Atomic uint8_t* ready;
    uint8_t* base;

    if (!peak_memlog_writer_enter()) return;
    if (!peak_memlog_reserve_slot(&index)) {
        peak_memlog_writer_leave();
        return;
    }

    base = (uint8_t*) g_memlog.map + g_memlog.header_bytes;
    event = (PeakMemEvent*) (base + index * sizeof(*event));
    ready = peak_memlog_ready_flags();
    event->ts_ns = ts;
    event->delta = delta;
    event->current = current;
    event->tid = peak_gettid();
    event->op = op;

#ifdef PEAK_ENABLE_TEST_HOOKS
    if (atomic_load_explicit(&g_memlog_test_pause_before_commit,
                             memory_order_acquire)) {
        atomic_store_explicit(&g_memlog_test_writer_paused, 1, memory_order_release);
        while (atomic_load_explicit(&g_memlog_test_pause_before_commit,
                                    memory_order_acquire)) {
            sched_yield();
        }
        atomic_store_explicit(&g_memlog_test_writer_paused, 0, memory_order_release);
    }
#endif

    /* Release publishes every payload field to an acquire scanner/exporter. */
    atomic_store_explicit(&ready[index], 1, memory_order_release);
    atomic_fetch_add_explicit(&g_memlog.committed, 1, memory_order_release);
    peak_memlog_writer_leave();
}

static void peak_memlog_disable(void) {
    PeakMemLogState state;

    /* The mutex closes admission before a finalizer observes writer count. */
    pthread_mutex_lock(&memlog_admission_mutex);
    state = atomic_load_explicit(&g_memlog.state, memory_order_acquire);
    if (state == PEAK_MEMLOG_ACTIVE) {
        atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_DISABLED,
                              memory_order_release);
    }
    pthread_mutex_unlock(&memlog_admission_mutex);

    if (state == PEAK_MEMLOG_ACTIVE || state == PEAK_MEMLOG_DISABLED) {
        while (atomic_load_explicit(&g_memlog.active_writers,
                                    memory_order_acquire) != 0) {
            sched_yield();
        }
    }
}

static size_t peak_memlog_compact_committed(void) {
    size_t reserved = atomic_load_explicit(&g_memlog.reserved, memory_order_acquire);
    size_t complete = 0;
    uint8_t* base;
    _Atomic uint8_t* ready;

    if (reserved > g_memlog.capacity_events) reserved = g_memlog.capacity_events;
    base = (uint8_t*) g_memlog.map + g_memlog.header_bytes;
    ready = peak_memlog_ready_flags();
    for (size_t i = 0; i < reserved; ++i) {
        PeakMemEvent* source = (PeakMemEvent*) (base + i * sizeof(*source));
        if (!atomic_load_explicit(&ready[i], memory_order_acquire)) continue;

        if (complete != i) {
            PeakMemEvent* destination =
                (PeakMemEvent*) (base + complete * sizeof(*destination));
            destination->ts_ns = source->ts_ns;
            destination->delta = source->delta;
            destination->current = source->current;
            destination->tid = source->tid;
            destination->op = source->op;
        }
        ++complete;
    }
    return complete;
}

static void peak_memlog_open(void) {
    size_t capacity_events = PEAK_MEMLOG_CHUNK_EVENTS;
    size_t header_bytes;
    size_t event_bytes;
    size_t ready_offset;
    size_t ready_bytes;
    size_t map_bytes;
    int fd = -1;
    void* base = MAP_FAILED;
    const char* env_capacity;

    if (atomic_load_explicit(&g_memlog.state, memory_order_acquire) !=
        PEAK_MEMLOG_UNINITIALIZED) return;

    env_capacity = getenv("PEAK_MEMLOG_CHUNK_EVENTS");
    if (env_capacity && !parse_memlog_capacity(env_capacity, &capacity_events)) {
        peak_log_warn("[peak] memlog: invalid PEAK_MEMLOG_CHUNK_EVENTS; disabling log\n");
        atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_DISABLED, memory_order_release);
        return;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    {
        size_t override = atomic_load_explicit(&g_memlog_test_capacity_override,
                                               memory_order_acquire);
        if (override != 0) capacity_events = override;
    }
#endif

    if (!page_align_up(sizeof(PeakMemHeader), &header_bytes) ||
        header_bytes > UINT32_MAX ||
        !checked_mul_size(capacity_events, sizeof(PeakMemEvent), &event_bytes) ||
        !checked_add_size(header_bytes, event_bytes, &ready_offset) ||
        !checked_mul_size(capacity_events, sizeof(_Atomic uint8_t), &ready_bytes) ||
        !checked_add_size(ready_offset, ready_bytes, &map_bytes) ||
        !size_fits_off_t(map_bytes)) {
        peak_log_warn("[peak] memlog: fixed storage size is invalid; disabling log\n");
        atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_DISABLED, memory_order_release);
        return;
    }

    build_paths(g_memlog.tmp_path, g_memlog.csv_path, g_memlog.otf2_prefix);
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (atomic_load_explicit(&g_memlog_test_failure, memory_order_acquire) ==
        PEAK_MEMLOG_TEST_FAIL_CREATE) {
        errno = EACCES;
    } else
#endif
    {
        fd = open(g_memlog.tmp_path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
    }
    if (fd < 0) {
        peak_log_warn("[peak] memlog: open(%s) failed: %s\n", g_memlog.tmp_path, strerror(errno));
        atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_DISABLED, memory_order_release);
        return;
    }

    if (peak_memlog_ftruncate(fd, map_bytes) != 0) {
        peak_log_warn("[peak] memlog: ftruncate init failed: %s\n", strerror(errno));
        close(fd);
        unlink(g_memlog.tmp_path);
        atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_DISABLED, memory_order_release);
        return;
    }

#ifdef PEAK_ENABLE_TEST_HOOKS
    if (atomic_load_explicit(&g_memlog_test_failure, memory_order_acquire) ==
        PEAK_MEMLOG_TEST_FAIL_MAPPING) {
        errno = ENOMEM;
    } else
#endif
    {
        base = mmap(NULL, map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    if (base == MAP_FAILED) {
        peak_log_warn("[peak] memlog: mmap init failed: %s\n", strerror(errno));
        close(fd);
        unlink(g_memlog.tmp_path);
        atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_DISABLED, memory_order_release);
        return;
    }

    PeakMemHeader* hdr = (PeakMemHeader*) base;
    memset(hdr, 0, sizeof(*hdr));
    memcpy(hdr->magic, "PEAKMEM\0", 8);
    hdr->header_bytes = (uint32_t) header_bytes;
    g_memlog.t0_ns = nsec_now();
    hdr->t0_ns = g_memlog.t0_ns;
    hdr->clock_id = (uint64_t) CLOCK_MONOTONIC_RAW;
    {
        int rank = -1;
        get_mpi_rank(&rank);
        hdr->mpi_rank = rank;
    }
    hdr->pid = (int32_t) getpid();
    hdr->ppid = (int32_t) getppid();

    g_memlog.fd = fd;
    g_memlog.map = base;
    g_memlog.map_bytes = map_bytes;
    g_memlog.header_bytes = header_bytes;
    g_memlog.capacity_events = capacity_events;
    g_memlog.ready_offset = ready_offset;
    atomic_store_explicit(&g_memlog.reserved, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memlog.committed, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memlog.dropped, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memlog.active_writers, 0, memory_order_relaxed);
#ifdef PEAK_ENABLE_TEST_HOOKS
    atomic_store_explicit(&g_memlog_test_exported, 0, memory_order_relaxed);
#endif
    atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_ACTIVE, memory_order_release);

    peak_log_event(nsec_now(), 0, 0, 1);
}

/* small helper to keep CSV emit identical but clearer */
static inline void peak_csv_emit_line(int fd_csv, const PeakMemEvent *e) {
    dprintf(fd_csv, "%llu,%lld,%llu,%u,%u\n",
            (unsigned long long) e->ts_ns,
            (long long)          e->delta,
            (unsigned long long) e->current,
            (unsigned)           e->tid,
            (unsigned)           e->op);
}

/* Convert the mmapped binary buffer to a CSV file (and remove the temp backing file). */
static void peak_memlog_finalize(void) {
    size_t events;
    size_t event_bytes;
    size_t used_bytes;
    PeakMemLogState state;

    /* Serialize finalizers; a DISABLED state can still have admitted writers. */
    pthread_mutex_lock(&memlog_finalize_mutex);
    state = atomic_load_explicit(&g_memlog.state, memory_order_acquire);
    if (state == PEAK_MEMLOG_FINALIZED) {
        pthread_mutex_unlock(&memlog_finalize_mutex);
        return;
    }
    if (state == PEAK_MEMLOG_UNINITIALIZED) {
        atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_FINALIZED, memory_order_release);
        pthread_mutex_unlock(&memlog_finalize_mutex);
        return;
    }

    peak_memlog_disable();
    if (!g_memlog.map) {
        atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_FINALIZED, memory_order_release);
        pthread_mutex_unlock(&memlog_finalize_mutex);
        return;
    }

    /* Never use committed as a prefix length: reservations can complete out of order. */
    events = peak_memlog_compact_committed();
    if (!checked_mul_size(events, sizeof(PeakMemEvent), &event_bytes) ||
        !checked_add_size(g_memlog.header_bytes, event_bytes, &used_bytes)) {
        peak_log_warn("[peak] memlog: final size overflow; exporting no events\n");
        events = 0;
        used_bytes = g_memlog.header_bytes;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    atomic_store_explicit(&g_memlog_test_exported, events, memory_order_release);
#endif

    msync(g_memlog.map, used_bytes, MS_SYNC);

    /* base pointer of the events region (after header) */
    uint8_t *base_bytes = (uint8_t *) g_memlog.map + g_memlog.header_bytes;
    PeakMemEvent *base_chunk  = (PeakMemEvent *) base_bytes;

    /* 1) OTF2 export */
    peak_memlog_export_otf2(g_memlog.otf2_prefix, base_chunk, events);

    /* 2) CSV export (exactly as before) */
    int fd_csv = open(g_memlog.csv_path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd_csv < 0) {
        peak_log_warn("[peak] memlog: open CSV %s failed: %s\n", g_memlog.csv_path, strerror(errno));
    } else {
        dprintf(fd_csv, "ts_ns,delta,current,tid,op\n");

        uint8_t *base = (uint8_t *) g_memlog.map + g_memlog.header_bytes;
        for (size_t i = 0; i < events; i++) {
            PeakMemEvent *e = (PeakMemEvent *) (base + i * sizeof(PeakMemEvent));
            peak_csv_emit_line(fd_csv, e);
        }
        close(fd_csv);
    }
    peak_log_report("[peak] memlog CSV written: %s (events=%zu dropped=%zu)\n",
                    g_memlog.csv_path,
                    events,
                    atomic_load_explicit(&g_memlog.dropped, memory_order_acquire));

    munmap(g_memlog.map, g_memlog.map_bytes);
    close(g_memlog.fd);
    unlink(g_memlog.tmp_path);

    g_memlog.map = NULL;
    g_memlog.map_bytes = 0;
    g_memlog.capacity_events = 0;
    g_memlog.fd = -1;
    atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_FINALIZED, memory_order_release);
    pthread_mutex_unlock(&memlog_finalize_mutex);
}

/*=========================
  Tracking table helpers
=========================*/

static void add_tracking_entry(void* ptr, size_t size, int log) {
    if (!track_table || atomic_load_explicit(&cleanup_in_progress, memory_order_acquire)) return;
    if (!log) return;

    AllocationEntry* entry = internal_malloc(sizeof(AllocationEntry));
    if (!entry) return;

    entry->ptr      = ptr;
    entry->size     = size;

    pthread_mutex_lock(&track_mutex);
    if (size > G_MAXULONG - current_memory) {
        pthread_mutex_unlock(&track_mutex);
        internal_free(entry);
        peak_log_warn("[peak] memory profiler accounting overflow; allocation not tracked\n");
        return;
    }
    gum_metal_hash_table_insert(track_table, ptr, entry);
    current_memory += size;
    max_memory = current_memory > max_memory ? current_memory : max_memory;
    gulong current_snapshot = current_memory;
    pthread_mutex_unlock(&track_mutex);

    int64_t delta;
    if (log && size_to_event_delta(size, &delta)) {
        peak_log_event(nsec_now(), delta, (uint64_t) current_snapshot, 1);
    }
}

static AllocationEntry* find_tracking_entry(void* ptr) {
    if (!track_table || atomic_load_explicit(&cleanup_in_progress, memory_order_acquire)) return NULL;

    pthread_mutex_lock(&track_mutex);
    AllocationEntry* entry = gum_metal_hash_table_lookup(track_table, ptr);
    if (entry && entry->ptr == ptr) {
        pthread_mutex_unlock(&track_mutex);
        return entry;
    }
    pthread_mutex_unlock(&track_mutex);
    return NULL;
}

static void remove_tracking_entry(void* ptr) {
    if (!track_table || atomic_load_explicit(&cleanup_in_progress, memory_order_acquire)) return;

    pthread_mutex_lock(&track_mutex);
    AllocationEntry* entry = gum_metal_hash_table_lookup(track_table, ptr);
    if (entry) {
        gum_metal_hash_table_remove(track_table, ptr);
        current_memory -= entry->size;
    }

    if (entry) {
        int64_t delta;
        if (size_to_event_delta(entry->size, &delta)) {
            peak_log_event(nsec_now(), -delta, (uint64_t) current_memory, 2);
        }
        internal_free(entry);
    }
    pthread_mutex_unlock(&track_mutex);
}

static int update_tracking_entry_after_realloc(void* old_ptr,
                                               void* new_ptr,
                                               size_t new_size,
                                               size_t* old_size_out,
                                               gulong* current_after_remove_out,
                                               gulong* current_after_add_out) {
    AllocationEntry* entry;

    if (!track_table || atomic_load_explicit(&cleanup_in_progress, memory_order_acquire)) {
        return 0;
    }

    pthread_mutex_lock(&track_mutex);
    entry = gum_metal_hash_table_lookup(track_table, old_ptr);
    if (!entry || entry->ptr != old_ptr) {
        pthread_mutex_unlock(&track_mutex);
        return 0;
    }

    *old_size_out = entry->size;
    current_memory -= entry->size;
    *current_after_remove_out = current_memory;
    if (new_size > G_MAXULONG - current_memory) {
        gum_metal_hash_table_remove(track_table, old_ptr);
        pthread_mutex_unlock(&track_mutex);
        internal_free(entry);
        peak_log_warn("[peak] memory profiler accounting overflow after realloc\n");
        return 0;
    }
    if (new_ptr != old_ptr) {
        gum_metal_hash_table_remove(track_table, old_ptr);
        gum_metal_hash_table_insert(track_table, new_ptr, entry);
    }
    entry->ptr = new_ptr;
    entry->size = new_size;
    current_memory += new_size;
    max_memory = current_memory > max_memory ? current_memory : max_memory;
    *current_after_add_out = current_memory;
    pthread_mutex_unlock(&track_mutex);
    return 1;
}

static void init_table(void) {
    track_table = gum_metal_hash_table_new(g_direct_hash, g_direct_equal);
    memory_caller_target_table = gum_metal_hash_table_new(g_str_hash, str_equal_function);
    if (!track_table) {
        peak_log_warn("[peak] Failed to initialize tracking table\n");
        exit(1);
    }
    if (!memory_caller_target_table) {
        peak_log_warn("[peak] Failed to initialize memory caller target table\n");
        exit(1);
    }

    pthread_mutex_lock(&caller_mutex);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        gum_metal_hash_table_insert(memory_caller_target_table, peak_hook_strings[i], peak_hook_strings[i]);
    }
    pthread_mutex_unlock(&caller_mutex);
}

/*=========================
  Custom alloc family (no logic changes)
=========================*/

static int malloc_hook_enter(void)
{
    if (in_peak_alloc_hook ||
        atomic_load_explicit(&cleanup_in_progress, memory_order_acquire)) {
        return 0;
    }

    atomic_fetch_add_explicit(&active_alloc_hooks, 1, memory_order_acquire);
    if (atomic_load_explicit(&cleanup_in_progress, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&active_alloc_hooks, 1, memory_order_release);
        return 0;
    }
    return 1;
}

static void malloc_hook_leave(void)
{
    atomic_fetch_sub_explicit(&active_alloc_hooks, 1, memory_order_release);
}

static int malloc_interceptor_wait_for_quiescence(void)
{
    for (unsigned int attempt = 0; attempt < 1000; attempt++) {
        if (atomic_load_explicit(&active_alloc_hooks, memory_order_acquire) == 0) {
            return 1;
        }
        usleep(1000);
    }
    return atomic_load_explicit(&active_alloc_hooks, memory_order_acquire) == 0;
}

static void* custom_malloc(size_t size) {
    if (!malloc_hook_enter()) {
        return original_malloc(size);
    }

    int hook_val = in_peak_alloc_hook;
    in_peak_alloc_hook = 1;
    void* ptr = original_malloc(size);
    if (ptr) {
        int flag = peak_log_backtrace_malloc();
        add_tracking_entry(ptr, size, flag);
    }
    in_peak_alloc_hook = hook_val;
    malloc_hook_leave();

    return ptr;
}

static void custom_free(void* ptr) {
    if (!ptr) return;

    if (!malloc_hook_enter()) {
        original_free(ptr);
        return;
    }

    int hook_val = in_peak_alloc_hook;
    in_peak_alloc_hook = 1;
    original_free(ptr);
    AllocationEntry* entry = find_tracking_entry(ptr);
    if (entry) remove_tracking_entry(ptr);
    in_peak_alloc_hook = hook_val;
    malloc_hook_leave();
}

static void* custom_calloc(size_t nmemb, size_t size) {
    if (!malloc_hook_enter()) {
        return original_calloc(nmemb, size);
    }

    if (size && nmemb > SIZE_MAX / size) {
        errno = ENOMEM;
        malloc_hook_leave();
        return NULL;
    }

    int hook_val = in_peak_alloc_hook;
    in_peak_alloc_hook = 1;
    size_t total_size = nmemb * size;
    void* ptr = original_calloc(nmemb, size);
    if (ptr) {
        int flag = peak_log_backtrace_malloc();
        add_tracking_entry(ptr, total_size, flag);
    }
    in_peak_alloc_hook = hook_val;
    malloc_hook_leave();

    return ptr;
}

static void* custom_realloc(void* ptr, size_t size) {
    if (!malloc_hook_enter()) {
        return original_realloc(ptr, size);
    }

    if (!ptr) {
        int hook_val = in_peak_alloc_hook;
        in_peak_alloc_hook = 1;
        void* new_ptr = original_malloc(size);
        if (new_ptr) {
            int flag = peak_log_backtrace_malloc();
            add_tracking_entry(new_ptr, size, flag);
        }
        in_peak_alloc_hook = hook_val;
        malloc_hook_leave();
        return new_ptr;
    }
    if (!size) {
        int hook_val = in_peak_alloc_hook;
        in_peak_alloc_hook = 1;
        original_free(ptr);
        AllocationEntry* entry = find_tracking_entry(ptr);
        if (entry) remove_tracking_entry(ptr);
        in_peak_alloc_hook = hook_val;
        malloc_hook_leave();
        return NULL;
    }

    int hook_val = in_peak_alloc_hook;
    in_peak_alloc_hook = 1;
    void* new_ptr = original_realloc(ptr, size);
    if (!new_ptr) {
        /* ISO C preserves ptr on realloc failure; do not touch its entry. */
        in_peak_alloc_hook = hook_val;
        malloc_hook_leave();
        return NULL;
    }

    size_t old_size;
    gulong current_after_remove;
    gulong current_after_add;
    if (update_tracking_entry_after_realloc(ptr,
                                            new_ptr,
                                            size,
                                            &old_size,
                                            &current_after_remove,
                                            &current_after_add)) {
        int64_t old_delta;
        int64_t new_delta;
        if (size_to_event_delta(old_size, &old_delta)) {
            peak_log_event(nsec_now(), -old_delta,
                           (uint64_t) current_after_remove, 2);
        }
        if (size_to_event_delta(size, &new_delta)) {
            peak_log_event(nsec_now(), new_delta,
                           (uint64_t) current_after_add, 1);
        }
    } else {
        int flag = peak_log_backtrace_malloc();
        add_tracking_entry(new_ptr, size, flag);
    }
    in_peak_alloc_hook = hook_val;
    malloc_hook_leave();

    return new_ptr;
}

static void* custom_aligned_alloc(size_t alignment, size_t size) {
    if (!malloc_hook_enter()) {
        return original_aligned_alloc(alignment, size);
    }

    int hook_val = in_peak_alloc_hook;
    in_peak_alloc_hook = 1;
    void* ptr = original_aligned_alloc(alignment, size);
    if (ptr) {
        int flag = peak_log_backtrace_malloc();
        add_tracking_entry(ptr, size, flag);
    }
    in_peak_alloc_hook = hook_val;
    malloc_hook_leave();

    return ptr;
}

static int custom_posix_memalign(void** memptr, size_t alignment, size_t size) {
    if (!malloc_hook_enter()) {
        return original_posix_memalign(memptr, alignment, size);
    }

    int hook_val = in_peak_alloc_hook;
    in_peak_alloc_hook = 1;
    int ret = original_posix_memalign(memptr, alignment, size);
    if (ret == 0 && memptr != NULL && *memptr != NULL) {
        int flag = peak_log_backtrace_malloc();
        add_tracking_entry(*memptr, size, flag);
    }
    in_peak_alloc_hook = hook_val;
    malloc_hook_leave();
    return ret;
}

/*=========================
  Diagnostics
=========================*/

static void memory_usage_log_print(void) {
    peak_log_info("[peak] Memory allocation interceptors detached and resources cleaned up\n");
    peak_log_report("Max usage (bytes): %lu\n", max_memory);
}

/*=========================
  Attach / Detach
=========================*/

#define DO_REPLACE_FAST(_addr, _hook, _orig, _name)                                       \
    do {                                                                                  \
        if (_addr) {                                                                      \
            GumReplaceReturn r = gum_interceptor_replace_fast(malloc_interceptor,         \
                                                              _addr, _hook,               \
                                                              (gpointer*)(&_orig),        \
                                                              NULL);                      \
            if (r != GUM_REPLACE_OK)                                                      \
                peak_log_warn("[peak] Failed to replace " _name ": %d\n", r);             \
        }                                                                                 \
    } while (0)

int malloc_interceptor_attach(void) {
    malloc_interceptor = gum_interceptor_obtain();
    gum_interceptor_begin_transaction(malloc_interceptor);

    malloc_addr        = (void*) malloc;
    free_addr          = (void*) free;
    calloc_addr        = (void*) calloc;
    realloc_addr       = (void*) realloc;
    aligned_alloc_addr = (void*) aligned_alloc;
    posix_memalign_addr= (void*) posix_memalign;

    DO_REPLACE_FAST(malloc_addr,        custom_malloc,        original_malloc,        "malloc");
    DO_REPLACE_FAST(free_addr,          custom_free,          original_free,          "free");
    DO_REPLACE_FAST(calloc_addr,        custom_calloc,        original_calloc,        "calloc");
    DO_REPLACE_FAST(realloc_addr,       custom_realloc,       original_realloc,       "realloc");
    DO_REPLACE_FAST(aligned_alloc_addr, custom_aligned_alloc, original_aligned_alloc, "aligned_alloc");
    DO_REPLACE_FAST(posix_memalign_addr,custom_posix_memalign,original_posix_memalign,"posix_memalign");

    gum_interceptor_end_transaction(malloc_interceptor);

    init_table();
    peak_memlog_open();

    peak_log_info("[peak] Memory allocation functions intercepted successfully\n");
    return 0;
}

void malloc_interceptor_detach(void) {
    atomic_store_explicit(&cleanup_in_progress, 1, memory_order_release);

    gum_interceptor_begin_transaction(malloc_interceptor);
    if (malloc_addr)        gum_interceptor_revert(malloc_interceptor, malloc_addr);
    if (free_addr)          gum_interceptor_revert(malloc_interceptor, free_addr);
    if (calloc_addr)        gum_interceptor_revert(malloc_interceptor, calloc_addr);
    if (realloc_addr)       gum_interceptor_revert(malloc_interceptor, realloc_addr);
    if (aligned_alloc_addr) gum_interceptor_revert(malloc_interceptor, aligned_alloc_addr);
    if (posix_memalign_addr)gum_interceptor_revert(malloc_interceptor, posix_memalign_addr);
    gum_interceptor_end_transaction(malloc_interceptor);

    if (!gum_interceptor_flush(malloc_interceptor) ||
        !malloc_interceptor_wait_for_quiescence()) {
        peak_log_warn(
                "Memory allocation interceptor teardown did not quiesce; leaving profiler state alive\n");
        return;
    }

    pthread_mutex_lock(&track_mutex);
    gum_metal_hash_table_unref(track_table);
    track_table = NULL;
    pthread_mutex_unlock(&track_mutex);

    pthread_mutex_lock(&caller_mutex);
    gum_metal_hash_table_unref(memory_caller_target_table);
    memory_caller_target_table = NULL;
    pthread_mutex_unlock(&caller_mutex);

    g_object_unref(malloc_interceptor);

    memory_usage_log_print();
    peak_memlog_finalize();
}

#ifdef PEAK_ENABLE_TEST_HOOKS
void
peak_memlog_test_set_failure(PeakMemLogTestFailure failure)
{
    atomic_store_explicit(&g_memlog_test_failure, failure, memory_order_release);
}

int
peak_memlog_test_open(size_t capacity_events)
{
    atomic_store_explicit(&g_memlog_test_capacity_override,
                          capacity_events,
                          memory_order_release);
    peak_memlog_open();
    return atomic_load_explicit(&g_memlog.state, memory_order_acquire) ==
        PEAK_MEMLOG_ACTIVE;
}

void
peak_memlog_test_log_event(uint64_t ts, int64_t delta, uint64_t current, uint8_t op)
{
    peak_log_event(ts, delta, current, op);
}

size_t
peak_memlog_test_ready_records(void)
{
    size_t reserved;
    size_t ready_count = 0;
    _Atomic uint8_t* ready_flags;

    if (!g_memlog.map) return 0;
    reserved = atomic_load_explicit(&g_memlog.reserved, memory_order_acquire);
    if (reserved > g_memlog.capacity_events) reserved = g_memlog.capacity_events;
    ready_flags = peak_memlog_ready_flags();
    for (size_t i = 0; i < reserved; ++i) {
        if (atomic_load_explicit(&ready_flags[i], memory_order_acquire)) ++ready_count;
    }
    return ready_count;
}

int
peak_memlog_test_read_event(size_t index, PeakMemEvent* out)
{
    PeakMemEvent* event;
    _Atomic uint8_t* ready;

    if (!out || !g_memlog.map || index >= g_memlog.capacity_events) return 0;
    ready = peak_memlog_ready_flags();
    if (!atomic_load_explicit(&ready[index], memory_order_acquire)) return 0;
    event = (PeakMemEvent*) ((uint8_t*) g_memlog.map + g_memlog.header_bytes +
                             index * sizeof(*event));
    out->ts_ns = event->ts_ns;
    out->delta = event->delta;
    out->current = event->current;
    out->tid = event->tid;
    out->op = event->op;
    return 1;
}

void
peak_memlog_test_snapshot(PeakMemLogTestSnapshot* out)
{
    if (!out) return;
    out->state = atomic_load_explicit(&g_memlog.state, memory_order_acquire);
    out->capacity = g_memlog.capacity_events;
    out->reserved = atomic_load_explicit(&g_memlog.reserved, memory_order_acquire);
    out->committed = atomic_load_explicit(&g_memlog.committed, memory_order_acquire);
    out->dropped = atomic_load_explicit(&g_memlog.dropped, memory_order_acquire);
    out->active_writers = atomic_load_explicit(&g_memlog.active_writers, memory_order_acquire);
    out->exported = atomic_load_explicit(&g_memlog_test_exported, memory_order_acquire);
    out->mapping_live = g_memlog.map != NULL;
}

void
peak_memlog_test_pause_before_commit(int enabled)
{
    atomic_store_explicit(&g_memlog_test_pause_before_commit, enabled != 0,
                          memory_order_release);
    if (!enabled) {
        atomic_store_explicit(&g_memlog_test_writer_paused, 0, memory_order_release);
    }
}

int
peak_memlog_test_writer_is_paused(void)
{
    return atomic_load_explicit(&g_memlog_test_writer_paused, memory_order_acquire);
}

void
peak_memlog_test_finalize(void)
{
    peak_memlog_finalize();
}

int
peak_malloc_test_failed_realloc_preserves_accounting(void)
{
    AllocationEntry* entry;
    AllocationEntry* observed;
    void* ptr;
    void* result;
    int preserved;
    void* (*saved_malloc)(size_t) = original_malloc;
    void (*saved_free)(void*) = original_free;
    void* (*saved_realloc)(void*, size_t) = original_realloc;

    if (track_table != NULL) return 0;
    original_malloc = malloc;
    original_free = free;
    original_realloc = peak_memlog_test_realloc_failure;
    ptr = original_malloc(64);
    entry = original_malloc(sizeof(*entry));
    if (!ptr || !entry) {
        original_free(ptr);
        original_free(entry);
        original_malloc = saved_malloc;
        original_free = saved_free;
        original_realloc = saved_realloc;
        return 0;
    }
    track_table = gum_metal_hash_table_new(g_direct_hash, g_direct_equal);
    if (!track_table) {
        original_free(entry);
        original_free(ptr);
        original_malloc = saved_malloc;
        original_free = saved_free;
        original_realloc = saved_realloc;
        return 0;
    }
    entry->ptr = ptr;
    entry->size = 64;
    current_memory = 64;
    gum_metal_hash_table_insert(track_table, ptr, entry);

    result = custom_realloc(ptr, SIZE_MAX);
    pthread_mutex_lock(&track_mutex);
    observed = gum_metal_hash_table_lookup(track_table, ptr);
    preserved = result == NULL && observed == entry && observed->size == 64 &&
        current_memory == 64;
    gum_metal_hash_table_remove(track_table, ptr);
    pthread_mutex_unlock(&track_mutex);
    gum_metal_hash_table_unref(track_table);
    track_table = NULL;
    current_memory = 0;
    original_free(entry);
    original_free(ptr);
    original_malloc = saved_malloc;
    original_free = saved_free;
    original_realloc = saved_realloc;
    return preserved;
}

int
peak_malloc_test_tracking_allocation_failure(void)
{
    void* ptr;
    void* (*saved_malloc)(size_t) = original_malloc;
    void (*saved_free)(void*) = original_free;
    GumMetalHashTable* table;
    int rejected;

    if (track_table != NULL) return 0;
    original_malloc = malloc;
    original_free = free;
    ptr = original_malloc(64);
    table = gum_metal_hash_table_new(g_direct_hash, g_direct_equal);
    if (!ptr || !table) {
        original_free(ptr);
        if (table) gum_metal_hash_table_unref(table);
        original_malloc = saved_malloc;
        original_free = saved_free;
        return 0;
    }
    track_table = table;
    current_memory = 0;
    peak_memlog_test_set_failure(PEAK_MEMLOG_TEST_FAIL_ALLOCATION);
    add_tracking_entry(ptr, 64, 1);
    pthread_mutex_lock(&track_mutex);
    rejected = gum_metal_hash_table_lookup(track_table, ptr) == NULL && current_memory == 0;
    pthread_mutex_unlock(&track_mutex);
    gum_metal_hash_table_unref(track_table);
    track_table = NULL;
    current_memory = 0;
    original_free(ptr);
    original_malloc = saved_malloc;
    original_free = saved_free;
    return rejected;
}
#endif
