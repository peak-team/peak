#define _GNU_SOURCE
#include "malloc_interceptor.h"
#include "malloc_otf2.h"
#include "internal/general_listener/output_identity.h"
#include "logging.h"
#include "utils/env_parser.h"
#include <stddef.h>
#include <sched.h>

/*=========================
  Types
=========================*/

#define PEAK_ACCOUNTING_SHARD_COUNT 4096u
#define PEAK_HOOK_ACTIVITY_SHARD_COUNT 4096u
#define PEAK_TRACKING_RADIX_BITS 10u
#define PEAK_TRACKING_RADIX_SIZE (1u << PEAK_TRACKING_RADIX_BITS)
#define PEAK_TRACKING_RADIX_MASK (PEAK_TRACKING_RADIX_SIZE - 1u)
#define PEAK_TRACKING_RADIX_LEVELS \
    ((sizeof(uintptr_t) * CHAR_BIT + PEAK_TRACKING_RADIX_BITS - 1u) / \
     PEAK_TRACKING_RADIX_BITS)

_Static_assert((PEAK_ACCOUNTING_SHARD_COUNT &
                (PEAK_ACCOUNTING_SHARD_COUNT - 1u)) == 0,
               "accounting shard count must be a power of two");
_Static_assert((PEAK_HOOK_ACTIVITY_SHARD_COUNT &
                (PEAK_HOOK_ACTIVITY_SHARD_COUNT - 1u)) == 0,
               "hook activity shard count must be a power of two");
_Static_assert(offsetof(PeakMemLog, reserved) % 64u == 0,
               "memlog reservation counter must start a cache line");
_Static_assert(offsetof(PeakMemLog, state) % 64u == 0,
               "memlog state gate must start a cache line");

typedef struct __attribute__((aligned(64))) {
    _Atomic gulong current_bytes;
    _Atomic uint64_t next_sequence;
} PeakAccountingShard;

typedef struct {
    _Atomic uintptr_t slots[PEAK_TRACKING_RADIX_SIZE];
} PeakTrackingRadixNode;

typedef struct __attribute__((aligned(64))) {
    _Atomic unsigned int active;
} PeakHookActivityShard;

typedef struct {
    uint64_t timestamp_ns;
    uint64_t sequence;
    uint16_t shard;
} PeakTrackingTransition;

/*=========================
  mmapped event buffer (binary, fixed capacity)
=========================*/
#ifndef PEAK_MEMLOG_CHUNK_EVENTS
#define PEAK_MEMLOG_CHUNK_EVENTS (1u * 500u * 1000u) /* fixed event capacity */
#endif
#define PEAK_MEMLOG_RESERVATION_BLOCK 64u
#define PEAK_MEMLOG_BLOCK_THRESHOLD 65536u
/* Matches PEAK_MAX_NUM_THREADS_LIMIT; each producer can strand at most one
 * ready-flag cache line of physical slots without reducing the configured
 * export capacity. */
#define PEAK_MEMLOG_MAX_TRACKED_THREADS 4096u

/*=========================
  Globals
=========================*/
extern gboolean            peak_memory_track_all;
extern size_t              peak_hook_address_count;
extern char**              peak_hook_strings;
static GumInterceptor*     malloc_interceptor;
static pthread_mutex_t     caller_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t     memlog_finalize_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t      malloc_tid_cache_once = PTHREAD_ONCE_INIT;
static PeakAccountingShard accounting_shards[PEAK_ACCOUNTING_SHARD_COUNT];
static PeakTrackingRadixNode tracking_root;
static _Atomic int         tracking_tables_active = 0;
static GumMetalHashTable*  memory_caller_target_table = NULL;
static _Atomic int         cleanup_in_progress = 0;
static PeakHookActivityShard hook_activity_shards[PEAK_HOOK_ACTIVITY_SHARD_COUNT];
static _Atomic gulong      max_memory = 0;
static _Atomic gulong      fallback_current_memory = 0;
static gpointer            malloc_addr = NULL;
static gpointer            free_addr = NULL;
static gpointer            calloc_addr = NULL;
static gpointer            realloc_addr = NULL;
static gpointer            aligned_alloc_addr = NULL;
static gpointer            posix_memalign_addr = NULL;
static gboolean            malloc_replaced = FALSE;
static gboolean            free_replaced = FALSE;
static gboolean            calloc_replaced = FALSE;
static gboolean            realloc_replaced = FALSE;
static gboolean            aligned_alloc_replaced = FALSE;
static gboolean            posix_memalign_replaced = FALSE;
static gboolean            malloc_interceptor_attached = FALSE;
static PeakMemLog          g_memlog = {
    .fd = -1,
    .state = ATOMIC_VAR_INIT(PEAK_MEMLOG_UNINITIALIZED),
};
static __thread int        in_peak_alloc_hook = 0;
static __thread int        in_backtrace = 0;
static __thread uint32_t   cached_thread_id = 0;
static _Atomic int         thread_id_cache_ready = 0;
#ifndef PEAK_ENABLE_TEST_HOOKS
static __thread size_t     memlog_reservation_next = 0;
static __thread size_t     memlog_reservation_end = 0;
#endif
static PeakEnvWarningState peak_memlog_capacity_warning_emitted;
static _Atomic int         peak_memory_tracking_warning_emitted = 0;
static _Atomic int         peak_memlog_full = 0;

#ifdef PEAK_ENABLE_TEST_HOOKS
static _Atomic PeakMemLogTestFailure g_memlog_test_failure = PEAK_MEMLOG_TEST_FAIL_NONE;
static _Atomic int g_memlog_test_pause_before_commit = 0;
static _Atomic int g_memlog_test_writer_paused = 0;
static _Atomic size_t g_memlog_test_capacity_override = 0;
static _Atomic size_t g_memlog_test_exported = 0;
static void* (*g_malloc_test_saved_malloc)(size_t size);
static void (*g_malloc_test_saved_free)(void* ptr);
static void* (*g_malloc_test_saved_realloc)(void* ptr, size_t size);
static gboolean g_malloc_test_saved_track_all;
static int g_malloc_test_active;

static void* peak_memlog_test_realloc_failure(void* ptr, size_t size) {
    (void) ptr;
    (void) size;
    errno = ENOMEM;
    return NULL;
}

static gboolean
peak_malloc_test_should_fail_replace(const char* name)
{
    const char* configured = getenv("PEAK_TEST_FAIL_MALLOC_REPLACE");

    return configured != NULL && name != NULL &&
           strcmp(configured, name) == 0;
}
static gboolean
peak_malloc_test_should_force_standard_replace(const char* name)
{
    const char* configured = getenv("PEAK_TEST_FORCE_MALLOC_STANDARD_REPLACE");

    return configured != NULL && name != NULL &&
           strcmp(configured, name) == 0;
}
#define PEAK_MALLOC_REPLACE_FAST(_addr, _hook, _orig, _name) \
    (peak_malloc_test_should_fail_replace(_name) ? GUM_REPLACE_WRONG_TYPE : \
     peak_malloc_test_should_force_standard_replace(_name) ? \
         GUM_REPLACE_WRONG_SIGNATURE : \
     gum_interceptor_replace_fast(malloc_interceptor, _addr, _hook, \
                                  (gpointer*)(&_orig), NULL))
#else
#define PEAK_MALLOC_REPLACE_FAST(_addr, _hook, _orig, _name) \
    gum_interceptor_replace_fast(malloc_interceptor, _addr, _hook, \
                                 (gpointer*)(&_orig), NULL)
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

static int peak_tracking_entry_prepare(void) {
#ifdef PEAK_ENABLE_TEST_HOOKS
    PeakMemLogTestFailure expected = PEAK_MEMLOG_TEST_FAIL_ALLOCATION;
    if (atomic_compare_exchange_strong_explicit(&g_memlog_test_failure,
                                                &expected,
                                                PEAK_MEMLOG_TEST_FAIL_NONE,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        errno = ENOMEM;
        return 0;
    }
#endif
    return 1;
}

/*=========================
  Fast time & TID helpers
=========================*/

static inline uint32_t peak_gettid(void) {
    uint32_t thread_id;

    if (cached_thread_id != 0) return cached_thread_id;
#ifdef SYS_gettid
    thread_id = (uint32_t) syscall(SYS_gettid);
#else
    thread_id = (uint32_t) getpid();
#endif
    if (atomic_load_explicit(&thread_id_cache_ready, memory_order_acquire)) {
        cached_thread_id = thread_id;
    }
    return thread_id;
}

static void
malloc_tid_cache_atfork_child(void)
{
    cached_thread_id = 0;
#ifndef PEAK_ENABLE_TEST_HOOKS
    memlog_reservation_next = 0;
    memlog_reservation_end = 0;
#endif
}

static void
malloc_tid_cache_initialize(void)
{
    if (pthread_atfork(NULL, NULL, malloc_tid_cache_atfork_child) == 0) {
        atomic_store_explicit(&thread_id_cache_ready, 1, memory_order_release);
    }
}

static inline uint64_t nsec_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
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

static int parse_memlog_capacity(size_t* out) {
    static const PeakEnvUnsignedSchema schema = {
        "PEAK_MEMLOG_CHUNK_EVENTS", "events", PEAK_MEMLOG_CHUNK_EVENTS,
        1, SIZE_MAX, false,
        &peak_memlog_capacity_warning_emitted, false,
    };
    unsigned long long parsed;

    if (!peak_parse_env_unsigned_checked(&schema, &parsed)) {
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
    const char *env_path = getenv("PEAK_MEMLOG_PATH");
    int rank = -1;
    const char* base = env_path != NULL && env_path[0] != '\0' ?
                           env_path : "./peak_memlog";
    const char* template_value = getenv("PEAK_MEMLOG_TEMPLATE");

    get_mpi_rank(&rank);
    if (!peak_output_identity_path(out_csv, 512, base, template_value,
                                   ".csv", rank) ||
        snprintf(out_tmp, 512, "%s.tmp", out_csv) >= 512 ||
        !peak_output_identity_path(out_otf2, 512, "peak_memlog",
                                   NULL, "", rank)) {
        out_tmp[0] = '\0';
        out_csv[0] = '\0';
        out_otf2[0] = '\0';
    }
}

/*=========================
  Memlog: fixed mapping / publish / finalize
=========================*/

#ifdef PEAK_ENABLE_TEST_HOOKS
static int peak_memlog_writer_enter(void) {
    if (atomic_load_explicit(&g_memlog.state, memory_order_acquire) !=
        PEAK_MEMLOG_ACTIVE) {
        return 0;
    }
    atomic_fetch_add_explicit(&g_memlog.active_writers, 1,
                              memory_order_acq_rel);
    if (atomic_load_explicit(&g_memlog.state, memory_order_acquire) ==
        PEAK_MEMLOG_ACTIVE) {
        return 1;
    }
    atomic_fetch_sub_explicit(&g_memlog.active_writers, 1,
                              memory_order_release);
    return 0;
}

static void peak_memlog_writer_leave(void) {
    atomic_fetch_sub_explicit(&g_memlog.active_writers, 1, memory_order_release);
}
#endif

#ifdef PEAK_ENABLE_TEST_HOOKS
static _Atomic uint8_t* peak_memlog_ready_flags(void) {
    return (_Atomic uint8_t*) ((uint8_t*) g_memlog.map + g_memlog.ready_offset);
}
#endif

static int peak_memlog_reserve_slot(size_t* index_out) {
#ifndef PEAK_ENABLE_TEST_HOOKS
    size_t reserved;
    size_t reservation_count = 1;

    if (atomic_load_explicit(&peak_memlog_full, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&g_memlog.dropped, 1, memory_order_relaxed);
        return 0;
    }
    if (g_memlog.storage_capacity_events != g_memlog.capacity_events) {
        if (memlog_reservation_next != memlog_reservation_end) {
            *index_out = memlog_reservation_next++;
            return 1;
        }
        reservation_count = PEAK_MEMLOG_RESERVATION_BLOCK;
    }
    reserved = atomic_fetch_add_explicit(&g_memlog.reserved,
                                         reservation_count,
                                         memory_order_relaxed);
    if (reserved >= g_memlog.storage_capacity_events) {
        atomic_store_explicit(&peak_memlog_full, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_memlog.dropped, 1, memory_order_relaxed);
        return 0;
    }
    *index_out = reserved;
    memlog_reservation_next = reserved + 1;
    memlog_reservation_end = reserved + reservation_count;
    if (memlog_reservation_end > g_memlog.storage_capacity_events) {
        memlog_reservation_end = g_memlog.storage_capacity_events;
    }
    return 1;
#else
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
#endif
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

static inline int
peak_log_event_write(uint64_t ts,
                     int64_t delta,
                     uint64_t current,
                     uint8_t op,
                     uint16_t accounting_shard,
                     uint64_t accounting_sequence)
{
    size_t index;
    PeakMemEvent* event;
#ifdef PEAK_ENABLE_TEST_HOOKS
    _Atomic uint8_t* ready;
#endif
    uint8_t* base;

    if (!peak_memlog_reserve_slot(&index)) {
        return 0;
    }

    base = (uint8_t*) g_memlog.map + g_memlog.header_bytes;
    event = (PeakMemEvent*) (base + index * sizeof(*event));
#ifdef PEAK_ENABLE_TEST_HOOKS
    ready = peak_memlog_ready_flags();
#endif
    event->ts_ns = ts;
    event->delta = delta;
    event->current = current;
    event->accounting_sequence = accounting_sequence;
    event->tid = peak_gettid();
    event->accounting_shard = accounting_shard;

#ifdef PEAK_ENABLE_TEST_HOOKS
    event->op = op;
    if (atomic_load_explicit(&g_memlog_test_pause_before_commit,
                             memory_order_acquire)) {
        atomic_store_explicit(&g_memlog_test_writer_paused, 1, memory_order_release);
        while (atomic_load_explicit(&g_memlog_test_pause_before_commit,
                                    memory_order_acquire)) {
            sched_yield();
        }
        atomic_store_explicit(&g_memlog_test_writer_paused, 0, memory_order_release);
    }
    atomic_store_explicit(&ready[index], 1, memory_order_release);
    atomic_fetch_add_explicit(&g_memlog.committed, 1,
                              memory_order_release);
#else
    /* Production finalization starts only after allocation hooks quiesce.
     * Publishing op last distinguishes completed records from unused slots
     * without a second per-event atomic write on the allocation hot path. */
    event->op = op;
#endif
    return 1;
}

static inline void
peak_log_event(uint64_t ts,
               int64_t delta,
               uint64_t current,
               uint8_t op,
               uint16_t accounting_shard,
               uint64_t accounting_sequence)
{
    /* Allocation hooks are quiesced before production finalization, so their
     * hot path needs only the state gate.  The test API below retains explicit
     * writer admission for concurrent-finalizer coverage. */
    if (atomic_load_explicit(&g_memlog.state, memory_order_acquire) !=
        PEAK_MEMLOG_ACTIVE) {
        return;
    }
    (void)peak_log_event_write(ts, delta, current, op,
                               accounting_shard, accounting_sequence);
}

static void peak_memlog_disable(void) {
    PeakMemLogState state;

    state = atomic_exchange_explicit(&g_memlog.state, PEAK_MEMLOG_DISABLED,
                                     memory_order_acq_rel);

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
#ifdef PEAK_ENABLE_TEST_HOOKS
    _Atomic uint8_t* ready;
#endif

    if (reserved > g_memlog.storage_capacity_events) {
        reserved = g_memlog.storage_capacity_events;
    }
    base = (uint8_t*) g_memlog.map + g_memlog.header_bytes;
#ifdef PEAK_ENABLE_TEST_HOOKS
    ready = peak_memlog_ready_flags();
#endif
    for (size_t i = 0; i < reserved; ++i) {
        PeakMemEvent* source = (PeakMemEvent*) (base + i * sizeof(*source));
#ifdef PEAK_ENABLE_TEST_HOOKS
        if (!atomic_load_explicit(&ready[i], memory_order_acquire)) continue;
#else
        if (source->op == 0) continue;
#endif

        if (complete != i) {
            PeakMemEvent* destination =
                (PeakMemEvent*) (base + complete * sizeof(*destination));
            destination->ts_ns = source->ts_ns;
            destination->delta = source->delta;
            destination->current = source->current;
            destination->accounting_sequence = source->accounting_sequence;
            destination->tid = source->tid;
            destination->accounting_shard = source->accounting_shard;
            destination->op = source->op;
        }
        ++complete;
    }
    return complete;
}

static int
peak_memlog_event_order_compare(const void* left_opaque,
                                const void* right_opaque)
{
    const PeakMemEvent* left = left_opaque;
    const PeakMemEvent* right = right_opaque;

    if (left->ts_ns != right->ts_ns) {
        return left->ts_ns < right->ts_ns ? -1 : 1;
    }
    if (left->accounting_shard == right->accounting_shard &&
        left->accounting_shard != UINT16_MAX &&
        left->accounting_sequence != right->accounting_sequence) {
        return left->accounting_sequence < right->accounting_sequence ? -1 : 1;
    }
    if (left->accounting_shard != right->accounting_shard) {
        return left->accounting_shard < right->accounting_shard ? -1 : 1;
    }
    if (left->accounting_sequence != right->accounting_sequence) {
        return left->accounting_sequence < right->accounting_sequence ? -1 : 1;
    }
    if (left->tid != right->tid) return left->tid < right->tid ? -1 : 1;
    return 0;
}

static void
peak_memlog_replay_accounting(PeakMemEvent* events, size_t count)
{
    uint64_t current = 0;
    uint64_t maximum = 0;
    int valid = 1;

    qsort(events, count, sizeof(*events), peak_memlog_event_order_compare);
    for (size_t i = 0; i < count; ++i) {
        int64_t delta = events[i].delta;

        if (delta >= 0) {
            uint64_t increase = (uint64_t)delta;
            if (increase > UINT64_MAX - current) {
                current = UINT64_MAX;
                valid = 0;
            } else {
                current += increase;
            }
        } else {
            uint64_t decrease = (uint64_t)(-(delta + 1)) + 1u;
            if (decrease > current) {
                current = 0;
                valid = 0;
            } else {
                current -= decrease;
            }
        }
        events[i].current = current;
        if (current > maximum) maximum = current;
    }
    atomic_store_explicit(&max_memory,
                          maximum > G_MAXULONG ? G_MAXULONG : (gulong)maximum,
                          memory_order_relaxed);
    if (!valid) {
        peak_log_warn("[peak] memlog: allocation accounting replay overflowed or underflowed\n");
    }
}

static void peak_memlog_open(void) {
    size_t capacity_events = PEAK_MEMLOG_CHUNK_EVENTS;
    size_t storage_capacity_events;
    size_t header_bytes;
    size_t event_bytes;
    size_t ready_offset;
    size_t ready_bytes;
    size_t map_bytes;
    int fd = -1;
    void* base = MAP_FAILED;
    const char* env_capacity;
    const char* output_template;

    if (atomic_load_explicit(&g_memlog.state, memory_order_acquire) !=
        PEAK_MEMLOG_UNINITIALIZED) return;

    env_capacity = getenv("PEAK_MEMLOG_CHUNK_EVENTS");
    if (env_capacity && !parse_memlog_capacity(&capacity_events)) {
        if (__atomic_exchange_n(&peak_memlog_capacity_warning_emitted.emitted,
                                1,
                                __ATOMIC_RELAXED) == 0) {
            peak_log_warn("[peak] memlog: invalid PEAK_MEMLOG_CHUNK_EVENTS; disabling log\n");
        }
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

    storage_capacity_events = capacity_events;
#ifndef PEAK_ENABLE_TEST_HOOKS
    if (capacity_events >= PEAK_MEMLOG_BLOCK_THRESHOLD &&
        !checked_add_size(capacity_events,
                          PEAK_MEMLOG_MAX_TRACKED_THREADS *
                              (PEAK_MEMLOG_RESERVATION_BLOCK - 1u),
                          &storage_capacity_events)) {
        peak_log_warn("[peak] memlog: reservation slack overflow; disabling log\n");
        atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_DISABLED,
                              memory_order_release);
        return;
    }
#endif

    if (!page_align_up(sizeof(PeakMemHeader), &header_bytes) ||
        header_bytes > UINT32_MAX ||
        !checked_mul_size(storage_capacity_events, sizeof(PeakMemEvent),
                          &event_bytes) ||
        !checked_add_size(header_bytes, event_bytes, &ready_offset) ||
        !checked_mul_size(storage_capacity_events, sizeof(_Atomic uint8_t),
                          &ready_bytes) ||
        !checked_add_size(ready_offset, ready_bytes, &map_bytes) ||
        !size_fits_off_t(map_bytes)) {
        peak_log_warn("[peak] memlog: fixed storage size is invalid; disabling log\n");
        atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_DISABLED, memory_order_release);
        return;
    }

    build_paths(g_memlog.tmp_path, g_memlog.csv_path, g_memlog.otf2_prefix);
    if (g_memlog.tmp_path[0] == '\0') {
        peak_log_warn("[peak] memlog: output path is invalid; disabling log\n");
        atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_DISABLED, memory_order_release);
        return;
    }
    output_template = getenv("PEAK_MEMLOG_TEMPLATE");
    if (output_template != NULL && output_template[0] != '\0' &&
        !peak_output_identity_make_parent(g_memlog.csv_path)) {
        peak_log_warn("[peak] memlog: output directory is invalid; disabling log\n");
        atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_DISABLED, memory_order_release);
        return;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (atomic_load_explicit(&g_memlog_test_failure, memory_order_acquire) ==
        PEAK_MEMLOG_TEST_FAIL_CREATE) {
        errno = EACCES;
    } else
#endif
    {
        fd = open(g_memlog.tmp_path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
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
        base = mmap(NULL, map_bytes, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    }
    if (base == MAP_FAILED) {
        peak_log_warn("[peak] memlog: mmap init failed: %s\n", strerror(errno));
        close(fd);
        unlink(g_memlog.tmp_path);
        atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_DISABLED, memory_order_release);
        return;
    }

    /* Keep anonymous first-write faults out of allocation hooks. */
    memset(base, 0, map_bytes);

    PeakMemHeader* hdr = (PeakMemHeader*) base;
    memcpy(hdr->magic, "PEAKMEM\0", 8);
    hdr->header_bytes = (uint32_t) header_bytes;
    g_memlog.t0_ns = nsec_now();
    hdr->t0_ns = g_memlog.t0_ns;
    hdr->clock_id = (uint64_t) CLOCK_MONOTONIC;
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
    g_memlog.storage_capacity_events = storage_capacity_events;
    g_memlog.ready_offset = ready_offset;
    atomic_store_explicit(&g_memlog.reserved, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memlog.committed, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memlog.dropped, 0, memory_order_relaxed);
    atomic_store_explicit(&g_memlog.active_writers, 0, memory_order_relaxed);
    atomic_store_explicit(&peak_memlog_full, 0, memory_order_relaxed);
#ifdef PEAK_ENABLE_TEST_HOOKS
    atomic_store_explicit(&g_memlog_test_exported, 0, memory_order_relaxed);
#endif
    atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_ACTIVE, memory_order_release);

    peak_log_event(nsec_now(), 0, 0, 1, UINT16_MAX, 0);
}

/* small helper to keep CSV emit identical but clearer */
static inline int peak_csv_emit_line(int fd_csv, const PeakMemEvent *e) {
    return dprintf(fd_csv, "%llu,%lld,%llu,%u,%u\n",
                   (unsigned long long) e->ts_ns,
                   (long long)          e->delta,
                   (unsigned long long) e->current,
                   (unsigned)           e->tid,
                   (unsigned)           e->op) >= 0;
}

/* Convert the fixed event buffer to CSV and remove its temporary reservation. */
static void peak_memlog_finalize(void) {
    size_t events;
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
    if (events > g_memlog.capacity_events) {
        atomic_fetch_add_explicit(&g_memlog.dropped,
                                  events - g_memlog.capacity_events,
                                  memory_order_relaxed);
        events = g_memlog.capacity_events;
    }
    atomic_store_explicit(&g_memlog.committed, events, memory_order_release);
#ifdef PEAK_ENABLE_TEST_HOOKS
    atomic_store_explicit(&g_memlog_test_exported, events, memory_order_release);
#endif

    /* base pointer of the events region (after header) */
    uint8_t *base_bytes = (uint8_t *) g_memlog.map + g_memlog.header_bytes;
    PeakMemEvent *base_chunk  = (PeakMemEvent *) base_bytes;

    peak_memlog_replay_accounting(base_chunk, events);

    /* 1) OTF2 export */
    peak_memlog_export_otf2(g_memlog.otf2_prefix, base_chunk, events);

    /* 2) CSV export: publish the complete file without replacing a prior run. */
    char csv_temp[sizeof(g_memlog.csv_path) + 16];
    int fd_csv;

    if (snprintf(csv_temp, sizeof(csv_temp), "%s.export", g_memlog.csv_path) >=
        (int)sizeof(csv_temp)) {
        csv_temp[0] = '\0';
    }
    fd_csv = csv_temp[0] == '\0' ? -1 :
        open(csv_temp, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
    bool csv_written = false;
    if (fd_csv < 0) {
        peak_log_warn("[peak] memlog: open CSV %s failed: %s\n", g_memlog.csv_path, strerror(errno));
    } else {
        int write_failed = dprintf(fd_csv, "ts_ns,delta,current,tid,op\n") < 0;
        int failure_errno = write_failed ? errno : 0;

        uint8_t *base = (uint8_t *) g_memlog.map + g_memlog.header_bytes;
        for (size_t i = 0; i < events; i++) {
            PeakMemEvent *e = (PeakMemEvent *) (base + i * sizeof(PeakMemEvent));
            if (!write_failed && !peak_csv_emit_line(fd_csv, e)) {
                write_failed = 1;
                failure_errno = errno;
            }
        }
        if (close(fd_csv) != 0) {
            write_failed = 1;
            failure_errno = errno;
        }
        if (!write_failed && link(csv_temp, g_memlog.csv_path) != 0) {
            write_failed = 1;
            failure_errno = errno;
        }
        if (write_failed) {
            peak_log_warn("[peak] memlog: publish CSV %s failed: %s\n",
                          g_memlog.csv_path,
                          strerror(failure_errno != 0 ? failure_errno : EIO));
        }
        else {
            csv_written = true;
        }
        (void)unlink(csv_temp);
    }
    if (csv_written) {
        peak_log_report("[peak] memlog CSV written: %s (events=%zu dropped=%zu)\n",
                        g_memlog.csv_path,
                        events,
                        atomic_load_explicit(&g_memlog.dropped, memory_order_acquire));
    }

    munmap(g_memlog.map, g_memlog.map_bytes);
    close(g_memlog.fd);
    unlink(g_memlog.tmp_path);

    g_memlog.map = NULL;
    g_memlog.map_bytes = 0;
    g_memlog.capacity_events = 0;
    g_memlog.storage_capacity_events = 0;
    g_memlog.fd = -1;
    atomic_store_explicit(&g_memlog.state, PEAK_MEMLOG_FINALIZED, memory_order_release);
    pthread_mutex_unlock(&memlog_finalize_mutex);
}

/*=========================
  Tracking table helpers
=========================*/

static void
note_memory_tracking_degraded(const char* reason)
{
    if (atomic_exchange_explicit(&peak_memory_tracking_warning_emitted, 1,
                                 memory_order_relaxed) == 0) {
        peak_log_message_always(
            "[peak] memory allocation tracking degraded (%s)\n", reason);
    }
}

static int
tracking_tables_create(void)
{
    if (atomic_load_explicit(&tracking_tables_active, memory_order_acquire)) {
        return 0;
    }
    if (!atomic_is_lock_free(&tracking_root.slots[0]) ||
        !atomic_is_lock_free(&accounting_shards[0].current_bytes) ||
        !atomic_is_lock_free(&accounting_shards[0].next_sequence)) {
        return 0;
    }
    for (size_t i = 0; i < PEAK_ACCOUNTING_SHARD_COUNT; ++i) {
        atomic_store_explicit(&accounting_shards[i].current_bytes, 0,
                              memory_order_relaxed);
        atomic_store_explicit(&accounting_shards[i].next_sequence, 0,
                              memory_order_relaxed);
    }
    atomic_store_explicit(&max_memory, 0, memory_order_relaxed);
    atomic_store_explicit(&fallback_current_memory, 0, memory_order_relaxed);
    atomic_store_explicit(&tracking_tables_active, 1, memory_order_release);
    return 1;
}

static void
tracking_radix_release_node(PeakTrackingRadixNode* node,
                            unsigned int remaining_levels)
{
    for (size_t i = 0; i < PEAK_TRACKING_RADIX_SIZE; ++i) {
        uintptr_t value = atomic_load_explicit(&node->slots[i],
                                               memory_order_relaxed);
        if (value == 0) continue;
        if (remaining_levels > 1) {
            tracking_radix_release_node((PeakTrackingRadixNode*)value,
                                        remaining_levels - 1);
        }
        original_free((void*)value);
        atomic_store_explicit(&node->slots[i], 0, memory_order_relaxed);
    }
}

static void
tracking_tables_release(void)
{
    atomic_store_explicit(&tracking_tables_active, 0, memory_order_release);
    tracking_radix_release_node(&tracking_root,
                                PEAK_TRACKING_RADIX_LEVELS - 1u);
    for (size_t i = 0; i < PEAK_ACCOUNTING_SHARD_COUNT; ++i) {
        atomic_store_explicit(&accounting_shards[i].current_bytes, 0,
                              memory_order_relaxed);
        atomic_store_explicit(&accounting_shards[i].next_sequence, 0,
                              memory_order_relaxed);
    }
}

static PeakTrackingRadixNode*
tracking_radix_allocate_node(void)
{
    PeakTrackingRadixNode* node = original_malloc(sizeof(*node));

    if (node != NULL) memset(node, 0, sizeof(*node));
    return node;
}

static _Atomic uintptr_t*
tracking_radix_slot(const void* ptr, int create)
{
    PeakTrackingRadixNode* node = &tracking_root;
    uintptr_t key = (uintptr_t)ptr;

    for (unsigned int level = PEAK_TRACKING_RADIX_LEVELS - 1u;
         level != 0; --level) {
        unsigned int shift = level * PEAK_TRACKING_RADIX_BITS;
        size_t index = (size_t)((key >> shift) & PEAK_TRACKING_RADIX_MASK);
        uintptr_t child = atomic_load_explicit(&node->slots[index],
                                               memory_order_acquire);

        if (child == 0) {
            PeakTrackingRadixNode* allocated;
            uintptr_t expected = 0;

            if (!create) return NULL;
            allocated = tracking_radix_allocate_node();
            if (allocated == NULL) return NULL;
            if (!atomic_compare_exchange_strong_explicit(
                    &node->slots[index], &expected, (uintptr_t)allocated,
                    memory_order_release, memory_order_acquire)) {
                original_free(allocated);
                child = expected;
            } else {
                child = (uintptr_t)allocated;
            }
        }
        node = (PeakTrackingRadixNode*)child;
    }
    return &node->slots[key & PEAK_TRACKING_RADIX_MASK];
}

static int
tracking_radix_insert(const void* ptr, size_t size)
{
    _Atomic uintptr_t* slot;
    uintptr_t expected = 0;

    if (size == SIZE_MAX) return 0;
    slot = tracking_radix_slot(ptr, 1);
    return slot != NULL && atomic_compare_exchange_strong_explicit(
        slot, &expected, (uintptr_t)size + 1u,
        memory_order_release, memory_order_relaxed);
}

static int
tracking_radix_remove(const void* ptr, size_t* size_out)
{
    _Atomic uintptr_t* slot = tracking_radix_slot(ptr, 0);
    uintptr_t encoded;

    if (slot == NULL) return 0;
    encoded = atomic_exchange_explicit(slot, 0, memory_order_acq_rel);
    if (encoded == 0) return 0;
    *size_out = (size_t)(encoded - 1u);
    return 1;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
static int
tracking_radix_lookup(const void* ptr, size_t* size_out)
{
    _Atomic uintptr_t* slot = tracking_radix_slot(ptr, 0);
    uintptr_t encoded;

    if (slot == NULL) return 0;
    encoded = atomic_load_explicit(slot, memory_order_acquire);
    if (encoded == 0) return 0;
    *size_out = (size_t)(encoded - 1u);
    return 1;
}
#endif

static PeakAccountingShard*
accounting_shard_for(const void* ptr)
{
    uintptr_t value = (uintptr_t)ptr >> 4;

    value ^= value >> 17;
    value ^= value >> 9;
    return &accounting_shards[value & (PEAK_ACCOUNTING_SHARD_COUNT - 1u)];
}

static int
tracking_fallback_account_add(size_t size)
{
    gulong observed = atomic_load_explicit(&fallback_current_memory,
                                           memory_order_relaxed);
    gulong updated;

    if ((uintmax_t)size > G_MAXULONG) return 0;
    for (;;) {
        if ((gulong)size > G_MAXULONG - observed) return 0;
        updated = observed + (gulong)size;
        if (atomic_compare_exchange_weak_explicit(&fallback_current_memory,
                                                  &observed, updated,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            gulong maximum = atomic_load_explicit(&max_memory,
                                                  memory_order_relaxed);
            while (maximum < updated &&
                   !atomic_compare_exchange_weak_explicit(
                       &max_memory, &maximum, updated,
                       memory_order_relaxed, memory_order_relaxed)) {
            }
            return 1;
        }
    }
}

static int
tracking_account_add(PeakAccountingShard* shard,
                     size_t size,
                     PeakTrackingTransition* transition_out)
{
    gulong observed;
    gulong updated;

    if ((uintmax_t)size > G_MAXULONG) return 0;
    if (transition_out != NULL) {
        transition_out->timestamp_ns = nsec_now();
        transition_out->sequence = atomic_fetch_add_explicit(
            &shard->next_sequence, 1, memory_order_relaxed);
        transition_out->shard = (uint16_t)(shard - accounting_shards);
    }
    if (atomic_load_explicit(&g_memlog.state, memory_order_relaxed) ==
        PEAK_MEMLOG_ACTIVE) {
        return 1;
    }
    observed = atomic_load_explicit(&shard->current_bytes,
                                    memory_order_relaxed);
    for (;;) {
        if ((gulong)size > G_MAXULONG - observed) return 0;
        updated = observed + (gulong)size;
        if (atomic_compare_exchange_weak_explicit(
                &shard->current_bytes, &observed, updated,
                memory_order_relaxed, memory_order_relaxed)) {
            break;
        }
    }
    if (!tracking_fallback_account_add(size)) {
        atomic_fetch_sub_explicit(&shard->current_bytes, (gulong)size,
                                  memory_order_relaxed);
        return 0;
    }
    return 1;
}

static void
tracking_account_remove(PeakAccountingShard* shard,
                        size_t size,
                        PeakTrackingTransition* transition_out)
{
    transition_out->timestamp_ns = nsec_now();
    transition_out->sequence = atomic_fetch_add_explicit(
        &shard->next_sequence, 1, memory_order_relaxed);
    transition_out->shard = (uint16_t)(shard - accounting_shards);
    if (atomic_load_explicit(&g_memlog.state, memory_order_relaxed) ==
        PEAK_MEMLOG_ACTIVE) {
        return;
    }
    atomic_fetch_sub_explicit(&shard->current_bytes, (gulong)size,
                              memory_order_relaxed);
    atomic_fetch_sub_explicit(&fallback_current_memory, (gulong)size,
                              memory_order_relaxed);
}

static int
tracking_account_restore(PeakAccountingShard* shard, size_t size)
{
    return tracking_account_add(shard, size, NULL);
}

static void add_tracking_entry(void* ptr, size_t size, int log) {
    PeakAccountingShard* shard;
    PeakTrackingTransition transition;
    size_t ignored_size;

    if (!atomic_load_explicit(&tracking_tables_active, memory_order_acquire) ||
        atomic_load_explicit(&cleanup_in_progress, memory_order_acquire)) return;
    if (!log) return;
    if (!peak_tracking_entry_prepare()) {
        note_memory_tracking_degraded("tracking entry allocation failed");
        return;
    }

    if (!tracking_radix_insert(ptr, size)) {
        note_memory_tracking_degraded("tracking radix insertion failed");
        return;
    }
    shard = accounting_shard_for(ptr);
    if (!tracking_account_add(shard, size, &transition)) {
        (void)tracking_radix_remove(ptr, &ignored_size);
        note_memory_tracking_degraded("allocation accounting overflow");
        return;
    }

    int64_t delta;
    if (log && size_to_event_delta(size, &delta)) {
        peak_log_event(transition.timestamp_ns, delta, 0, 1,
                       transition.shard, transition.sequence);
    }
}

static int
detach_tracking_entry(void* ptr,
                      size_t* size_out,
                      PeakTrackingTransition* transition_out)
{
    PeakAccountingShard* shard;

    if (!atomic_load_explicit(&tracking_tables_active, memory_order_acquire) ||
        atomic_load_explicit(&cleanup_in_progress, memory_order_acquire)) {
        return 0;
    }

    /* The atomic slot removal is the lifetime transition.  It precedes the
     * allocator call, which may make the address available for immediate
     * reuse on another thread. */
    if (!tracking_radix_remove(ptr, size_out)) return 0;
    shard = accounting_shard_for(ptr);
    tracking_account_remove(shard, *size_out, transition_out);
    return 1;
}

static void
restore_tracking_entry(void* ptr, size_t size)
{
    PeakAccountingShard* shard = accounting_shard_for(ptr);

    if (!tracking_radix_insert(ptr, size)) {
        note_memory_tracking_degraded("realloc rollback tracking failed");
        return;
    }
    if (!tracking_account_restore(shard, size)) {
        atomic_store_explicit(&shard->current_bytes, G_MAXULONG,
                              memory_order_relaxed);
        note_memory_tracking_degraded("realloc rollback accounting overflow");
    }
}

static int
commit_reallocated_entry(void* new_ptr,
                         size_t new_size,
                         PeakTrackingTransition* transition_out)
{
    PeakAccountingShard* shard = accounting_shard_for(new_ptr);
    size_t ignored_size;

    if (!tracking_radix_insert(new_ptr, new_size)) {
        note_memory_tracking_degraded("realloc tracking insertion failed");
        return 0;
    }
    if (!tracking_account_add(shard, new_size, transition_out)) {
        (void)tracking_radix_remove(new_ptr, &ignored_size);
        note_memory_tracking_degraded("realloc accounting overflow");
        return 0;
    }
    return 1;
}

static void
publish_removed_entry(size_t size,
                      const PeakTrackingTransition* transition)
{
    int64_t delta;

    if (size_to_event_delta(size, &delta)) {
        peak_log_event(transition->timestamp_ns, -delta, 0, 2,
                       transition->shard, transition->sequence);
    }
}

static void
malloc_interceptor_release_tracking_tables(void)
{
    tracking_tables_release();
    if (memory_caller_target_table != NULL) {
        gum_metal_hash_table_unref(memory_caller_target_table);
        memory_caller_target_table = NULL;
    }
}

static int
init_table(void)
{
    GumMetalHashTable* new_caller_table;

#ifdef PEAK_ENABLE_TEST_HOOKS
    if (atomic_load_explicit(&g_memlog_test_failure, memory_order_acquire) ==
            PEAK_MEMLOG_TEST_FAIL_TRACKING_SETUP ||
        getenv("PEAK_TEST_FAIL_MEMORY_TRACKING_SETUP") != NULL) {
        return 0;
    }
#endif
    new_caller_table = gum_metal_hash_table_new(g_str_hash, str_equal_function);
    if (!new_caller_table || !tracking_tables_create()) {
        if (new_caller_table) gum_metal_hash_table_unref(new_caller_table);
        return 0;
    }

    pthread_mutex_lock(&caller_mutex);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        gum_metal_hash_table_insert(new_caller_table,
                                    peak_hook_strings[i],
                                    peak_hook_strings[i]);
    }
    pthread_mutex_unlock(&caller_mutex);
    memory_caller_target_table = new_caller_table;
    return 1;
}

/*=========================
  Custom alloc family (no logic changes)
=========================*/

typedef enum {
    PEAK_MALLOC_HOOK_RECURSIVE = 0,
    PEAK_MALLOC_HOOK_TRACKING,
    PEAK_MALLOC_HOOK_FORWARDING,
} PeakMallocHookEntry;

static inline PeakHookActivityShard*
malloc_hook_activity_shard(void)
{
    uintptr_t shard_key = (uintptr_t)&in_peak_alloc_hook >> 6;

    shard_key ^= shard_key >> 17;
    shard_key ^= shard_key >> 9;
    return &hook_activity_shards[
        shard_key & (PEAK_HOOK_ACTIVITY_SHARD_COUNT - 1u)];
}

static PeakMallocHookEntry
malloc_hook_enter(void)
{
    PeakHookActivityShard* activity_shard;

    if (in_peak_alloc_hook) {
        return PEAK_MALLOC_HOOK_RECURSIVE;
    }

    activity_shard = malloc_hook_activity_shard();
    atomic_fetch_add_explicit(&activity_shard->active, 1,
                              memory_order_acquire);
    if (atomic_load_explicit(&cleanup_in_progress, memory_order_acquire)) {
        return PEAK_MALLOC_HOOK_FORWARDING;
    }
    return PEAK_MALLOC_HOOK_TRACKING;
}

static void malloc_hook_leave(void)
{
    atomic_fetch_sub_explicit(&malloc_hook_activity_shard()->active, 1,
                              memory_order_release);
}

static int
malloc_interceptor_hooks_are_quiescent(void)
{
    for (size_t i = 0; i < PEAK_HOOK_ACTIVITY_SHARD_COUNT; ++i) {
        if (atomic_load_explicit(&hook_activity_shards[i].active,
                                 memory_order_acquire) != 0) {
            return 0;
        }
    }
    return 1;
}

static int malloc_interceptor_wait_for_quiescence(void)
{
    for (unsigned int attempt = 0; attempt < 1000; attempt++) {
        if (malloc_interceptor_hooks_are_quiescent()) {
            return 1;
        }
        usleep(1000);
    }
    return malloc_interceptor_hooks_are_quiescent();
}

static void* custom_malloc(size_t size) {
    PeakMallocHookEntry hook_entry = malloc_hook_enter();
    if (hook_entry != PEAK_MALLOC_HOOK_TRACKING) {
        void* result = original_malloc(size);
        if (hook_entry == PEAK_MALLOC_HOOK_FORWARDING) {
            malloc_hook_leave();
        }
        return result;
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
    size_t tracked_size;
    PeakTrackingTransition transition;
    int tracked;

    if (!ptr) return;

    PeakMallocHookEntry hook_entry = malloc_hook_enter();
    if (hook_entry != PEAK_MALLOC_HOOK_TRACKING) {
        original_free(ptr);
        if (hook_entry == PEAK_MALLOC_HOOK_FORWARDING) {
            malloc_hook_leave();
        }
        return;
    }

    int hook_val = in_peak_alloc_hook;
    in_peak_alloc_hook = 1;
    /* Keep the old entry local while the allocator exposes its address. */
    tracked = detach_tracking_entry(ptr, &tracked_size, &transition);
    original_free(ptr);
    if (tracked) {
        publish_removed_entry(tracked_size, &transition);
    }
    in_peak_alloc_hook = hook_val;
    malloc_hook_leave();
}

static void* custom_calloc(size_t nmemb, size_t size) {
    PeakMallocHookEntry hook_entry = malloc_hook_enter();
    if (hook_entry != PEAK_MALLOC_HOOK_TRACKING) {
        void* result = original_calloc(nmemb, size);
        if (hook_entry == PEAK_MALLOC_HOOK_FORWARDING) {
            malloc_hook_leave();
        }
        return result;
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
    size_t tracked_size = 0;
    int tracked = 0;
    PeakTrackingTransition remove_transition;
    PeakTrackingTransition add_transition;
    int saved_errno = 0;
    int realloc_errno;
    int result_errno;

    PeakMallocHookEntry hook_entry = malloc_hook_enter();
    if (hook_entry != PEAK_MALLOC_HOOK_TRACKING) {
        void* result = original_realloc(ptr, size);
        if (hook_entry == PEAK_MALLOC_HOOK_FORWARDING) {
            malloc_hook_leave();
        }
        return result;
    }

    if (!ptr) {
        int hook_val = in_peak_alloc_hook;
        in_peak_alloc_hook = 1;
        void* new_ptr = original_realloc(NULL, size);
        result_errno = errno;
        if (new_ptr) {
            int flag = peak_log_backtrace_malloc();
            add_tracking_entry(new_ptr, size, flag);
        }
        errno = result_errno;
        in_peak_alloc_hook = hook_val;
        malloc_hook_leave();
        return new_ptr;
    }

    int hook_val = in_peak_alloc_hook;
    in_peak_alloc_hook = 1;
    /*
     * Keep the entry operation-local while the allocator runs.  Failure
     * restores the same lifetime without events; success publishes the old
     * removal and commits the returned pointer as the new lifetime.
     */
    tracked = detach_tracking_entry(ptr, &tracked_size, &remove_transition);

    if (size == 0) {
        saved_errno = errno;
        errno = 0;
    }
    void* new_ptr = original_realloc(ptr, size);
    realloc_errno = errno;
    result_errno = size == 0 && realloc_errno == 0
        ? saved_errno
        : realloc_errno;
    if (!new_ptr && (size != 0 || realloc_errno == ENOMEM)) {
        if (tracked) restore_tracking_entry(ptr, tracked_size);
        errno = result_errno;
        in_peak_alloc_hook = hook_val;
        malloc_hook_leave();
        return NULL;
    }

    if (tracked) {
        publish_removed_entry(tracked_size, &remove_transition);
    }
    if (tracked && new_ptr != NULL &&
        commit_reallocated_entry(new_ptr, size, &add_transition)) {
        int64_t new_delta;
        if (size_to_event_delta(size, &new_delta)) {
            peak_log_event(add_transition.timestamp_ns, new_delta, 0, 1,
                           add_transition.shard, add_transition.sequence);
        }
    } else if (!tracked && new_ptr != NULL) {
        int flag = peak_log_backtrace_malloc();
        add_tracking_entry(new_ptr, size, flag);
    }
    errno = result_errno;
    in_peak_alloc_hook = hook_val;
    malloc_hook_leave();

    return new_ptr;
}

static void* custom_aligned_alloc(size_t alignment, size_t size) {
    PeakMallocHookEntry hook_entry = malloc_hook_enter();
    if (hook_entry != PEAK_MALLOC_HOOK_TRACKING) {
        void* result = original_aligned_alloc(alignment, size);
        if (hook_entry == PEAK_MALLOC_HOOK_FORWARDING) {
            malloc_hook_leave();
        }
        return result;
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
    PeakMallocHookEntry hook_entry = malloc_hook_enter();
    if (hook_entry != PEAK_MALLOC_HOOK_TRACKING) {
        int result = original_posix_memalign(memptr, alignment, size);
        if (hook_entry == PEAK_MALLOC_HOOK_FORWARDING) {
            malloc_hook_leave();
        }
        return result;
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
    peak_log_report("Max usage (bytes): %lu\n",
                    atomic_load_explicit(&max_memory, memory_order_relaxed));
}

/*=========================
  Attach / Detach
=========================*/

#define DO_REPLACE_FAST(_addr, _hook, _orig, _name, _replaced)                            \
    do {                                                                                  \
        if (_addr) {                                                                      \
            GumReplaceReturn r = PEAK_MALLOC_REPLACE_FAST(_addr, _hook, _orig, _name);  \
            /* Legacy libc entry stubs may not satisfy fast-replace's direct            \
             * signature contract.  Standard replacement preserves the same             \
             * transaction and original-function semantics for those entries. */         \
            if (r == GUM_REPLACE_WRONG_SIGNATURE) {                                       \
                r = gum_interceptor_replace(malloc_interceptor, _addr, _hook,             \
                                            (gpointer*)(&_orig), NULL);                    \
            }                                                                             \
            _replaced = r == GUM_REPLACE_OK;                                              \
            if (!_replaced) {                                                             \
                peak_log_warn("[peak] failed to replace " _name ": %d\n", r);            \
            }                                                                             \
        }                                                                                 \
    } while (0)

/* The caller owns an open Gum transaction.  Reverting a partially prepared
 * replacement set before committing that transaction means no wrapper or
 * trampoline can become visible to another thread. */
static void
malloc_interceptor_revert_all_in_transaction(void)
{
    if (malloc_replaced) gum_interceptor_revert(malloc_interceptor, malloc_addr);
    if (free_replaced) gum_interceptor_revert(malloc_interceptor, free_addr);
    if (calloc_replaced) gum_interceptor_revert(malloc_interceptor, calloc_addr);
    if (realloc_replaced) gum_interceptor_revert(malloc_interceptor, realloc_addr);
    if (aligned_alloc_replaced) gum_interceptor_revert(malloc_interceptor, aligned_alloc_addr);
    if (posix_memalign_replaced) gum_interceptor_revert(malloc_interceptor, posix_memalign_addr);
}

static gboolean
malloc_interceptor_flush_rollback(void)
{
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (getenv("PEAK_TEST_FAIL_MALLOC_ROLLBACK_FLUSH") != NULL) {
        return FALSE;
    }
#endif
    return gum_interceptor_flush(malloc_interceptor);
}

PeakMallocInterceptorAttachResult
malloc_interceptor_attach(void)
{
    pthread_once(&malloc_tid_cache_once, malloc_tid_cache_initialize);
    atomic_store_explicit(&cleanup_in_progress, 0, memory_order_release);
    if (!init_table()) {
        return PEAK_MALLOC_ATTACH_PREPARE_FAILED;
    }
    peak_memlog_open();
    malloc_interceptor = gum_interceptor_obtain();
    if (malloc_interceptor == NULL) {
        malloc_interceptor_release_tracking_tables();
        peak_memlog_finalize();
        return PEAK_MALLOC_ATTACH_PREPARE_FAILED;
    }
    gum_interceptor_begin_transaction(malloc_interceptor);

    malloc_addr        = (void*) malloc;
    free_addr          = (void*) free;
    calloc_addr        = (void*) calloc;
    realloc_addr       = (void*) realloc;
    aligned_alloc_addr = (void*) aligned_alloc;
    posix_memalign_addr= (void*) posix_memalign;

    DO_REPLACE_FAST(malloc_addr, custom_malloc, original_malloc, "malloc", malloc_replaced);
    DO_REPLACE_FAST(free_addr, custom_free, original_free, "free", free_replaced);
    DO_REPLACE_FAST(calloc_addr, custom_calloc, original_calloc, "calloc", calloc_replaced);
    DO_REPLACE_FAST(realloc_addr, custom_realloc, original_realloc, "realloc", realloc_replaced);
    DO_REPLACE_FAST(aligned_alloc_addr, custom_aligned_alloc, original_aligned_alloc, "aligned_alloc", aligned_alloc_replaced);
    DO_REPLACE_FAST(posix_memalign_addr, custom_posix_memalign, original_posix_memalign, "posix_memalign", posix_memalign_replaced);

    if (!malloc_replaced || !free_replaced || !calloc_replaced ||
        !realloc_replaced || !aligned_alloc_replaced ||
        !posix_memalign_replaced) {
        /* Do not make a partial replacement visible.  In particular, if the
         * failed entry is free(), a visible malloc wrapper would otherwise
         * have no recorded original_free to use on recursive paths. */
        atomic_store_explicit(&cleanup_in_progress, 1, memory_order_release);
#ifdef PEAK_ENABLE_TEST_HOOKS
        const char* rollback_delay = getenv("PEAK_TEST_MALLOC_ROLLBACK_DELAY_US");
        if (rollback_delay != NULL && rollback_delay[0] != '\0') {
            char* end = NULL;
            unsigned long delay_us = strtoul(rollback_delay, &end, 10);
            if (end != rollback_delay && *end == '\0' && delay_us <= UINT_MAX) {
                usleep((useconds_t)delay_us);
            }
        }
#endif
        malloc_interceptor_revert_all_in_transaction();
        gum_interceptor_end_transaction(malloc_interceptor);
        if (!malloc_interceptor_flush_rollback() ||
            !malloc_interceptor_wait_for_quiescence()) {
            peak_log_warn("[peak] fatal memory interceptor rollback was not proven safe after mutation\n");
            _exit(128);
        }
        g_object_unref(malloc_interceptor);
        malloc_interceptor = NULL;
        malloc_interceptor_release_tracking_tables();
        peak_memlog_finalize();
        return PEAK_MALLOC_ATTACH_ROLLED_BACK;
    }

    gum_interceptor_end_transaction(malloc_interceptor);

    malloc_interceptor_attached = TRUE;
    peak_log_info("[peak] Memory allocation functions intercepted successfully\n");
    return 0;
}

void
malloc_interceptor_detach(void)
{
    if (!malloc_interceptor_attached || malloc_interceptor == NULL) {
        return;
    }
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

    tracking_tables_release();
    pthread_mutex_lock(&caller_mutex);
    if (memory_caller_target_table != NULL) {
        gum_metal_hash_table_unref(memory_caller_target_table);
        memory_caller_target_table = NULL;
    }
    pthread_mutex_unlock(&caller_mutex);

    g_object_unref(malloc_interceptor);
    malloc_interceptor = NULL;
    malloc_interceptor_attached = FALSE;

    peak_memlog_finalize();
    memory_usage_log_print();
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
    if (!peak_memlog_writer_enter()) return;
    (void)peak_log_event_write(ts, delta, current, op, UINT16_MAX, 0);
    peak_memlog_writer_leave();
}

size_t
peak_memlog_test_ready_records(void)
{
    size_t reserved;
    size_t ready_count = 0;
    _Atomic uint8_t* ready_flags;

    if (!g_memlog.map) return 0;
    reserved = atomic_load_explicit(&g_memlog.reserved, memory_order_acquire);
    if (reserved > g_memlog.storage_capacity_events) {
        reserved = g_memlog.storage_capacity_events;
    }
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

    if (!out || !g_memlog.map ||
        index >= g_memlog.storage_capacity_events) return 0;
    ready = peak_memlog_ready_flags();
    if (!atomic_load_explicit(&ready[index], memory_order_acquire)) return 0;
    event = (PeakMemEvent*) ((uint8_t*) g_memlog.map + g_memlog.header_bytes +
                             index * sizeof(*event));
    out->ts_ns = event->ts_ns;
    out->delta = event->delta;
    out->current = event->current;
    out->accounting_sequence = event->accounting_sequence;
    out->tid = event->tid;
    out->accounting_shard = event->accounting_shard;
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

uint32_t
peak_malloc_test_thread_id(void)
{
    pthread_once(&malloc_tid_cache_once, malloc_tid_cache_initialize);
    return peak_gettid();
}

int
peak_malloc_test_failed_realloc_preserves_accounting(void)
{
    size_t observed_size = 0;
    PeakAccountingShard* shard;
    PeakTrackingTransition transition;
    void* ptr;
    void* result;
    int preserved;
    void* (*saved_malloc)(size_t) = original_malloc;
    void (*saved_free)(void*) = original_free;
    void* (*saved_realloc)(void*, size_t) = original_realloc;

    if (atomic_load_explicit(&tracking_tables_active, memory_order_acquire)) return 0;
    original_malloc = malloc;
    original_free = free;
    original_realloc = peak_memlog_test_realloc_failure;
    ptr = original_malloc(64);
    if (!ptr) {
        original_free(ptr);
        original_malloc = saved_malloc;
        original_free = saved_free;
        original_realloc = saved_realloc;
        return 0;
    }
    if (!tracking_tables_create()) {
        original_free(ptr);
        original_malloc = saved_malloc;
        original_free = saved_free;
        original_realloc = saved_realloc;
        return 0;
    }
    shard = accounting_shard_for(ptr);
    if (!tracking_radix_insert(ptr, 64) ||
        !tracking_account_add(shard, 64, &transition)) {
        tracking_tables_release();
        original_free(ptr);
        original_malloc = saved_malloc;
        original_free = saved_free;
        original_realloc = saved_realloc;
        return 0;
    }
    result = custom_realloc(ptr, SIZE_MAX);
    preserved = result == NULL &&
        tracking_radix_lookup(ptr, &observed_size) &&
        observed_size == 64 &&
        atomic_load_explicit(&shard->current_bytes, memory_order_relaxed) == 64;
    (void)tracking_radix_remove(ptr, &observed_size);
    atomic_store_explicit(&shard->current_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&fallback_current_memory, 0, memory_order_relaxed);
    tracking_tables_release();
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
    size_t observed_size;
    int rejected;

    if (atomic_load_explicit(&tracking_tables_active, memory_order_acquire)) return 0;
    original_malloc = malloc;
    original_free = free;
    ptr = original_malloc(64);
    if (!ptr || !tracking_tables_create()) {
        original_free(ptr);
        original_malloc = saved_malloc;
        original_free = saved_free;
        return 0;
    }
    peak_memlog_test_set_failure(PEAK_MEMLOG_TEST_FAIL_ALLOCATION);
    add_tracking_entry(ptr, 64, 1);
    rejected = !tracking_radix_lookup(ptr, &observed_size) &&
        atomic_load_explicit(&accounting_shard_for(ptr)->current_bytes,
                             memory_order_relaxed) == 0;
    tracking_tables_release();
    original_free(ptr);
    original_malloc = saved_malloc;
    original_free = saved_free;
    return rejected;
}

int
peak_malloc_test_begin(const PeakMallocTestAllocator* allocator)
{
    if (allocator == NULL || allocator->malloc_fn == NULL ||
        allocator->free_fn == NULL || allocator->realloc_fn == NULL ||
        g_malloc_test_active ||
        atomic_load_explicit(&tracking_tables_active, memory_order_acquire)) {
        return 0;
    }
    pthread_once(&malloc_tid_cache_once, malloc_tid_cache_initialize);
    if (!tracking_tables_create()) return 0;

    g_malloc_test_saved_malloc = original_malloc;
    g_malloc_test_saved_free = original_free;
    g_malloc_test_saved_realloc = original_realloc;
    g_malloc_test_saved_track_all = peak_memory_track_all;
    original_malloc = allocator->malloc_fn;
    original_free = allocator->free_fn;
    original_realloc = allocator->realloc_fn;
    peak_memory_track_all = TRUE;
    atomic_store_explicit(&cleanup_in_progress, 0, memory_order_release);
    for (size_t i = 0; i < PEAK_HOOK_ACTIVITY_SHARD_COUNT; ++i) {
        atomic_store_explicit(&hook_activity_shards[i].active, 0,
                              memory_order_release);
    }
    in_peak_alloc_hook = 0;
    g_malloc_test_active = 1;
    return 1;
}

int
peak_malloc_test_seed(void* ptr, size_t size)
{
    PeakAccountingShard* shard;
    size_t ignored_size = 0;

    if (!g_malloc_test_active || ptr == NULL) return 0;

    shard = accounting_shard_for(ptr);
    if (!tracking_radix_insert(ptr, size)) return 0;
    if (!tracking_account_add(shard, size, NULL)) {
        (void)tracking_radix_remove(ptr, &ignored_size);
        return 0;
    }
    return 1;
}

void*
peak_malloc_test_malloc(size_t size)
{
    return g_malloc_test_active ? custom_malloc(size) : NULL;
}

void
peak_malloc_test_free(void* ptr)
{
    if (g_malloc_test_active) custom_free(ptr);
}

void*
peak_malloc_test_realloc(void* ptr, size_t size)
{
    return g_malloc_test_active ? custom_realloc(ptr, size) : NULL;
}

static void
peak_malloc_test_snapshot_node(PeakTrackingRadixNode* node,
                               unsigned int level,
                               PeakMallocTestTrackingSnapshot* out)
{
    for (size_t i = 0; i < PEAK_TRACKING_RADIX_SIZE; ++i) {
        uintptr_t value = atomic_load_explicit(&node->slots[i],
                                               memory_order_acquire);
        if (value == 0) continue;
        if (level == 0) {
            ++out->entry_count;
            out->table_bytes += (size_t)(value - 1u);
        } else {
            peak_malloc_test_snapshot_node((PeakTrackingRadixNode*)value,
                                           level - 1u, out);
        }
    }
}

void
peak_malloc_test_tracking_snapshot(void* ptr,
                                   PeakMallocTestTrackingSnapshot* out)
{
    size_t observed_size;

    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (!g_malloc_test_active) return;

    peak_malloc_test_snapshot_node(&tracking_root,
                                   PEAK_TRACKING_RADIX_LEVELS - 1u, out);
    for (size_t i = 0; i < PEAK_ACCOUNTING_SHARD_COUNT; ++i) {
        gulong shard_current = atomic_load_explicit(
            &accounting_shards[i].current_bytes, memory_order_relaxed);
        if (shard_current > G_MAXULONG - out->current_bytes) {
            out->current_bytes = G_MAXULONG;
        } else {
            out->current_bytes += shard_current;
        }
    }
    if (ptr != NULL && tracking_radix_lookup(ptr, &observed_size)) {
        out->pointer_tracked = 1;
        out->pointer_size = observed_size;
    }
}

void
peak_malloc_test_end(void)
{
    if (!g_malloc_test_active) return;
    tracking_tables_release();
    atomic_store_explicit(&max_memory, 0, memory_order_relaxed);
    atomic_store_explicit(&fallback_current_memory, 0, memory_order_relaxed);
    original_malloc = g_malloc_test_saved_malloc;
    original_free = g_malloc_test_saved_free;
    original_realloc = g_malloc_test_saved_realloc;
    peak_memory_track_all = g_malloc_test_saved_track_all;
    g_malloc_test_active = 0;
}
#endif
