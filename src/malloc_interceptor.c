#include "malloc_interceptor.h"

/*=========================
  Types
=========================*/

typedef struct {
    void*   ptr;
    size_t  size;
    int     is_mmap;
    char*   filename;  // Store filename for mmap allocations
} AllocationEntry;

/*=========================
  mmapped event buffer (binary)
=========================*/
#ifndef PEAK_MEMLOG_CHUNK_EVENTS
#define PEAK_MEMLOG_CHUNK_EVENTS (1u * 500u * 1000u) // grow step (0.5M events)
#endif

typedef struct {
    uint64_t ts_ns;     // relative to t0_ns
    int64_t  delta;     // +alloc / -free / +/-realloc
    uint64_t current;   // current memory after applying delta
    uint32_t tid;       // Linux thread id
    uint8_t  op;        // 1=alloc,2=free,3=realloc_old,4=realloc_new
} __attribute__((packed)) PeakMemEvent;

typedef struct {
    char     magic[8];       // "PEAKMEM\0", indicating that the byte is PeakMemHeader
    uint32_t header_bytes;   // page-aligned size of header
    uint64_t t0_ns;          // base time
    uint64_t clock_id;       // CLOCK_MONOTONIC_RAW
    int32_t  mpi_rank;       // -1 if unknown
    int32_t  pid;            // getpid()
    int32_t  ppid;           // getppid()
} __attribute__((packed)) PeakMemHeader;

typedef struct {
    int      fd;               // fd of temp binary file
    void    *map;              // base mapping (header + events)
    size_t   map_bytes;        // mapped size
    size_t   header_bytes;     // aligned header length
    size_t   capacity_events;  // how many events can fit now
    size_t   chunk_events;     // growth step
    _Atomic size_t index;      // next event slot
    _Atomic int    growing;    // CAS gate for growth
    uint64_t t0_ns;
    int      initialized;
    char     tmp_path[512];    // mmapped temp file path (unlinked at end)
    char     csv_path[512];    // final CSV output path
} PeakMemLog;

/*=========================
  Globals
=========================*/
extern size_t              peak_hook_address_count;
extern char**              peak_hook_strings;
static GumInterceptor*     malloc_interceptor;
static pthread_mutex_t     track_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t     caller_mutex = PTHREAD_MUTEX_INITIALIZER;
static GumMetalHashTable*  track_table = NULL;
static GumMetalHashTable*  memory_caller_target_table = NULL;
static int                 cleanup_in_progress = 0;
static gulong              max_memory = 0;
static gulong              current_memory = 0;
static gpointer            malloc_addr = NULL;
static gpointer            free_addr = NULL;
static gpointer            calloc_addr = NULL;
static gpointer            realloc_addr = NULL;
static gpointer            aligned_alloc_addr = NULL;
static gpointer            posix_memalign_addr = NULL;
static PeakMemLog          g_memlog = {0};
static __thread int        in_backtrace = 0;

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

static void* internal_malloc(size_t size) { return original_malloc(size); }
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

static size_t page_align_up(size_t v) {
    size_t p = (size_t) sysconf(_SC_PAGESIZE);
    return (v + p - 1) & ~(p - 1);
}

/*=========================
  MPI helper (as-is)
=========================*/

static int get_mpi_rank(int *rank) {
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

static int peak_log_backtrace_malloc(void* ret_ptr, size_t sz) {
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
        
        // for (guint idx = 0; idx < peak_hook_address_count; idx++) {
        //     if (strcmp(func, peak_hook_strings[idx]) == 0) {
        //         flag = 1;
        //         break;
        //     }
        // }

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

static void build_paths(char out_tmp[512], char out_csv[512]) {
    char base[256] = {0};
    const char *env_path = getenv("PEAK_MEMLOG_PATH");
    if (env_path && *env_path) {
        size_t n = strlen(env_path);
        if (n >= sizeof(base)) n = sizeof(base) - 1;
        memcpy(base, env_path, n);
        base[n] = '\0';
    } else {
        snprintf(base, sizeof(base), "/tmp/peak_memlog");
    }

    int rank = -1;
    (void) get_mpi_rank(&rank);

    int pid = (int) getpid();
    if (rank == -1) {
        snprintf(out_tmp, 512, "%s-p%d.tmp", base, pid);
        snprintf(out_csv, 512, "%s-p%d.csv", base, pid);
    } else {
        snprintf(out_tmp, 512, "%s-r%d-p%d.tmp", base, rank, pid);
        snprintf(out_csv, 512, "%s-r%d-p%d.csv", base, rank, pid);
    }
}

/*=========================
  Memlog: open / grow / event / finalize
=========================*/

static void peak_memlog_open(void) {
    if (g_memlog.initialized) return;

    size_t chunk_ev = PEAK_MEMLOG_CHUNK_EVENTS;
    const char *env_chunk = getenv("PEAK_MEMLOG_CHUNK_EVENTS");
    if (env_chunk) {
        long long v = atoll(env_chunk);
        if (v > 1000) chunk_ev = (size_t) v;
    }

    build_paths(g_memlog.tmp_path, g_memlog.csv_path);

    int fd = open(g_memlog.tmp_path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "[peak] memlog: open(%s) failed: %s\n", g_memlog.tmp_path, strerror(errno));
        g_memlog.initialized = 1;
        return;
    }

    size_t header_bytes = page_align_up(sizeof(PeakMemHeader));
    size_t init_events  = chunk_ev;
    size_t init_bytes   = header_bytes + init_events * sizeof(PeakMemEvent);

    if (ftruncate(fd, (off_t) init_bytes) != 0) {
        fprintf(stderr, "[peak] memlog: ftruncate init failed: %s\n", strerror(errno));
        close(fd);
        g_memlog.initialized = 1;
        return;
    }

    void *base = mmap(NULL, init_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        fprintf(stderr, "[peak] memlog: mmap init failed: %s\n", strerror(errno));
        close(fd);
        g_memlog.initialized = 1;
        return;
    }

    PeakMemHeader *hdr = (PeakMemHeader *) base;
    memset(hdr, 0, sizeof(*hdr));
    memcpy(hdr->magic, "PEAKMEM\0", 8);
    hdr->header_bytes = (uint32_t) header_bytes;
    g_memlog.t0_ns    = nsec_now();
    hdr->t0_ns        = g_memlog.t0_ns;
    hdr->clock_id     = (uint64_t) CLOCK_MONOTONIC_RAW;

    int rank = -1;
    (void) get_mpi_rank(&rank);
    hdr->mpi_rank = rank;
    hdr->pid      = (int32_t) getpid();
    hdr->ppid     = (int32_t) getppid();

    g_memlog.fd              = fd;
    g_memlog.map             = base;
    g_memlog.map_bytes       = init_bytes;
    g_memlog.header_bytes    = header_bytes;
    g_memlog.capacity_events = init_events;
    g_memlog.chunk_events    = chunk_ev;
    atomic_store(&g_memlog.index, 0);
    atomic_store(&g_memlog.growing, 0);
    g_memlog.initialized = 1;
}

static void peak_memlog_grow_if_needed(size_t want_index) {
    if (want_index < g_memlog.capacity_events) return;

    int exp = 0;
    if (!atomic_compare_exchange_strong(&g_memlog.growing, &exp, 1)) {
        while (atomic_load(&g_memlog.growing) != 0) { /* brief spin */ }
        return;
    }

    size_t need_events = want_index + 1;
    size_t cap         = g_memlog.capacity_events;
    size_t chunk       = g_memlog.chunk_events;

    size_t add_chunks = (need_events > cap) ? ((need_events - cap + chunk - 1) / chunk) : 0;
    if (add_chunks == 0) { atomic_store(&g_memlog.growing, 0); return; }

    size_t old_bytes = g_memlog.map_bytes;
    size_t new_events= cap + add_chunks * chunk;
    size_t new_bytes = g_memlog.header_bytes + new_events * sizeof(PeakMemEvent);

    if (ftruncate(g_memlog.fd, (off_t) new_bytes) != 0) {
        fprintf(stderr, "[peak] memlog: ftruncate grow failed: %s\n", strerror(errno));
        atomic_store(&g_memlog.growing, 0);
        return;
    }

    void *new_map = mremap(g_memlog.map, old_bytes, new_bytes, MREMAP_MAYMOVE);
    if (new_map == MAP_FAILED) {
        fprintf(stderr, "[peak] memlog: mremap failed: %s (keeping old cap)\n", strerror(errno));
        atomic_store(&g_memlog.growing, 0);
        return;
    }

    g_memlog.map             = new_map;
    g_memlog.map_bytes       = new_bytes;
    g_memlog.capacity_events = new_events;

    atomic_store(&g_memlog.growing, 0);
}

static inline void peak_log_event(int64_t delta, uint64_t current, uint8_t op) {
    if (!g_memlog.initialized || !g_memlog.map) return;

    size_t i = atomic_fetch_add_explicit(&g_memlog.index, 1, memory_order_relaxed);
    if (i >= g_memlog.capacity_events) {
        peak_memlog_grow_if_needed(i);
        if (i >= g_memlog.capacity_events) return; // growth failed (rare)
    }

    uint8_t *base = (uint8_t *) g_memlog.map + g_memlog.header_bytes;
    PeakMemEvent *e = (PeakMemEvent *) (base + i * sizeof(PeakMemEvent));
    e->ts_ns   = nsec_now() - g_memlog.t0_ns;
    e->delta   = delta;
    e->current = current;
    e->tid     = peak_gettid();
    e->op      = op;
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
static void peak_memlog_finalize_to_csv(void) {
    if (!g_memlog.initialized || !g_memlog.map) return;

    size_t events = atomic_load_explicit(&g_memlog.index, memory_order_relaxed);
    size_t used_bytes = g_memlog.header_bytes + events * sizeof(PeakMemEvent);
    if (used_bytes > g_memlog.map_bytes) used_bytes = g_memlog.map_bytes;
    
    msync(g_memlog.map, used_bytes, MS_SYNC);

    int fd_csv = open(g_memlog.csv_path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd_csv < 0) {
        fprintf(stderr, "[peak] memlog: open CSV %s failed: %s\n", g_memlog.csv_path, strerror(errno));
    } else {
        dprintf(fd_csv, "ts_ns,delta,current,tid,op\n");

        uint8_t *base = (uint8_t *) g_memlog.map + g_memlog.header_bytes;
        for (size_t i = 0; i < events; i++) {
            PeakMemEvent *e = (PeakMemEvent *) (base + i * sizeof(PeakMemEvent));
            peak_csv_emit_line(fd_csv, e);
        }
        close(fd_csv);
    }
    fprintf(stderr, "[peak] memlog CSV written: %s (events=%zu)\n", g_memlog.csv_path, events);

    munmap(g_memlog.map, g_memlog.map_bytes);
    (void) ftruncate(g_memlog.fd, (off_t) used_bytes);
    close(g_memlog.fd);
    unlink(g_memlog.tmp_path);

    g_memlog.map = NULL;
    g_memlog.map_bytes = 0;
    g_memlog.capacity_events = 0;
    g_memlog.initialized = 0;
}

/*=========================
  Tracking table helpers
=========================*/

static void add_tracking_entry(void* ptr, size_t size, int is_mmap, char* filename, int log) {
    if (!track_table || cleanup_in_progress) return;
    if (!log) return;

    AllocationEntry* entry = internal_malloc(sizeof(AllocationEntry));
    if (!entry) return;

    entry->ptr      = ptr;
    entry->size     = size;
    entry->is_mmap  = is_mmap;
    entry->filename = filename;  // May be NULL for non-mmap allocations

    pthread_mutex_lock(&track_mutex);
    gum_metal_hash_table_insert(track_table, ptr, entry);
    current_memory += size;
    max_memory = current_memory > max_memory ? current_memory : max_memory;
    pthread_mutex_unlock(&track_mutex);

    if (log) peak_log_event((int64_t) size, (uint64_t) current_memory, 1);
}

static AllocationEntry* find_tracking_entry(void* ptr) {
    if (!track_table || cleanup_in_progress) return NULL;

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
    if (!track_table || cleanup_in_progress) return;

    pthread_mutex_lock(&track_mutex);
    AllocationEntry* entry = gum_metal_hash_table_lookup(track_table, ptr);
    if (entry) {
        gum_metal_hash_table_remove(track_table, ptr);
        current_memory -= entry->size;
    }
    pthread_mutex_unlock(&track_mutex);

    if (entry->filename) {
        remove(entry->filename);
        internal_free(entry->filename);
    }

    if (entry) {
        peak_log_event(-((int64_t) entry->size), (uint64_t) current_memory, 2);
        internal_free(entry);
    }
}

static void init_table(void) {
    track_table = gum_metal_hash_table_new(g_direct_hash, g_direct_equal);
    memory_caller_target_table = gum_metal_hash_table_new(g_str_hash, str_equal_function);
    if (!track_table) {
        fprintf(stderr, "Failed to initialize tracking table\n");
        exit(1);
    }
    if (!memory_caller_target_table) {
        fprintf(stderr, "Failed to initialize memory caller target table\n");
        exit(1);
    }
    
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        pthread_mutex_lock(&caller_mutex);
        gum_metal_hash_table_insert(memory_caller_target_table, peak_hook_strings[i], peak_hook_strings[i]);
        // char * f = gum_metal_hash_table_lookup(memory_caller_target_table, peak_hook_strings[i]);
        pthread_mutex_unlock(&caller_mutex);
    }
}

/*=========================
  Custom alloc family (no logic changes)
=========================*/

static void* custom_malloc(size_t size) {
    void* ptr = original_malloc(size);
    if (ptr) {
        int flag = peak_log_backtrace_malloc(ptr, size);
        add_tracking_entry(ptr, size, 0, NULL, flag);
    }
    return ptr;
}

static void custom_free(void* ptr) {
    if (!ptr) return;
    original_free(ptr);
    AllocationEntry* entry = find_tracking_entry(ptr);
    if (!entry) return;
    remove_tracking_entry(ptr);
}

static void* custom_calloc(size_t nmemb, size_t size) {
    if (size && nmemb > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }

    size_t total_size = nmemb * size;
    void* ptr = original_calloc(nmemb, size);
    if (ptr) {
        int flag = peak_log_backtrace_malloc(ptr, size);
        add_tracking_entry(ptr, total_size, 0, NULL, flag);
    } 
    return ptr;
}

static void* custom_realloc(void* ptr, size_t size) {
    if (!ptr) return custom_malloc(size);
    if (!size) { custom_free(ptr); return NULL; }

    AllocationEntry* entry = find_tracking_entry(ptr);
    if (!entry) {
        return original_realloc(ptr, size);
    }

    void* new_ptr = original_realloc(ptr, size);
    if (new_ptr) {
        remove_tracking_entry(ptr);
        int flag = peak_log_backtrace_malloc(new_ptr, size);
        add_tracking_entry(new_ptr, size, 0, NULL, flag);
    }
    return new_ptr;
}

static void* custom_aligned_alloc(size_t alignment, size_t size) {
    void* ptr = original_aligned_alloc(alignment, size);
    if (ptr) {
        int flag = peak_log_backtrace_malloc(ptr, size);
        add_tracking_entry(ptr, size, 0, NULL, flag);
    }
    return ptr;
}

static int custom_posix_memalign(void** memptr, size_t alignment, size_t size) {
    int ret = original_posix_memalign(memptr, alignment, size);
    if (ret == 0) {
        int flag = peak_log_backtrace_malloc(*memptr, size);
        add_tracking_entry(*memptr, size, 0, NULL, flag);
    }
    return 0;
}

/*=========================
  Diagnostics
=========================*/

static void memory_usage_log_print(void) {
    fprintf(stderr, "Memory allocation interceptors detached and resources cleaned up\n");
    fprintf(stderr, "Max usage (bytes): %lu\n", max_memory);
}

/*=========================
  Attach / Detach
=========================*/

#define DO_REPLACE_FAST(_addr, _hook, _orig, _name)                                       \
    do {                                                                                  \
        if (_addr) {                                                                      \
            GumReplaceReturn r = gum_interceptor_replace_fast(malloc_interceptor,         \
                                                              _addr, _hook,               \
                                                              (gpointer*)(&_orig));       \
            if (r != GUM_REPLACE_OK)                                                      \
                fprintf(stderr, "Failed to replace " _name ": %d\n", r);                  \
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

    fprintf(stderr, "Memory allocation functions intercepted successfully\n");
    return 0;
}

void malloc_interceptor_dettach(void) {
    cleanup_in_progress = 1;

    if (malloc_addr)        gum_interceptor_revert(malloc_interceptor, malloc_addr);
    if (free_addr)          gum_interceptor_revert(malloc_interceptor, free_addr);
    if (calloc_addr)        gum_interceptor_revert(malloc_interceptor, calloc_addr);
    if (realloc_addr)       gum_interceptor_revert(malloc_interceptor, realloc_addr);
    if (aligned_alloc_addr) gum_interceptor_revert(malloc_interceptor, aligned_alloc_addr);
    if (posix_memalign_addr)gum_interceptor_revert(malloc_interceptor, posix_memalign_addr);

    gum_metal_hash_table_unref(track_table);
    gum_metal_hash_table_unref(memory_caller_target_table);
    g_object_unref(malloc_interceptor);

    memory_usage_log_print();
    peak_memlog_finalize_to_csv();
}