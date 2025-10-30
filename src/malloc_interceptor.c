#include "malloc_interceptor.h"

typedef struct {
    void* ptr;
    size_t size;
    int is_mmap;
    char* filename;  // Store filename for mmap allocations
} AllocationEntry;

// Global variables
static GumInterceptor* malloc_interceptor;
static pthread_mutex_t track_mutex = PTHREAD_MUTEX_INITIALIZER;
static GumMetalHashTable* track_table = NULL;
static int cleanup_in_progress = 0;
static gulong max_memory = 0;
static gulong current_memory = 0;
static gpointer malloc_addr = NULL; 
static gpointer free_addr = NULL;
static gpointer calloc_addr = NULL;
static gpointer realloc_addr = NULL;
static gpointer aligned_alloc_addr = NULL;
static gpointer posix_memalign_addr = NULL;

// Original function pointers
static void* (*original_malloc)(size_t size);
static void (*original_free)(void* ptr);
static void* (*original_calloc)(size_t nmemb, size_t size);
static void* (*original_realloc)(void* ptr, size_t size);
static void* (*original_aligned_alloc)(size_t alignment, size_t size);
static int (*original_posix_memalign)(void** memptr, size_t alignment, size_t size);

// Internal versions of malloc/free for tracking entry use
static void* internal_malloc(size_t size) {
    return original_malloc(size);
}

static void internal_free(void* ptr) {
    original_free(ptr);
}

static void add_tracking_entry(void* ptr, size_t size, int is_mmap, char* filename) {
    if (!track_table || cleanup_in_progress) return;
    
    AllocationEntry* entry = internal_malloc(sizeof(AllocationEntry));
    if (!entry) return;
    
    entry->ptr = ptr;
    entry->size = size;
    entry->is_mmap = is_mmap;
    entry->filename = filename;  // May be NULL for non-mmap allocations
    
    pthread_mutex_lock(&track_mutex);
        
    gum_metal_hash_table_insert(track_table, ptr, entry);
    current_memory+=size;
    max_memory = current_memory > max_memory ? current_memory : max_memory;
    
    pthread_mutex_unlock(&track_mutex);
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
        current_memory-=entry->size;
    }

    pthread_mutex_unlock(&track_mutex);
    if (entry->filename) {
        remove(entry->filename);
        internal_free(entry->filename);
    }

    if (entry) {
        internal_free(entry);
    }
}

static void init_table() {
    // Create tracking table
    track_table = gum_metal_hash_table_new(g_direct_hash, g_direct_equal);
    if (!track_table) {
        fprintf(stderr, "Failed to initialize tracking table\n");
        exit(1);
    }
}

// Custom implementation of malloc
static void* custom_malloc(size_t size) {
    void* ptr = original_malloc(size);
    if (ptr) {
        add_tracking_entry(ptr, size, 0, NULL);
    }

    return ptr;
}

// Custom implementation of free
static void custom_free(void* ptr) {
    if (!ptr) return;

    original_free(ptr);
    
    AllocationEntry* entry = find_tracking_entry(ptr);
    if (!entry) {
        return;
    }
   
    remove_tracking_entry(ptr);
}

// Custom implementation of calloc
static void* custom_calloc(size_t nmemb, size_t size) {
    // Check for overflow in nmemb * size
    if (size && nmemb > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }

    size_t total_size = nmemb * size;
    
    // Try original calloc first
    void* ptr = original_calloc(nmemb, size);
    if (ptr) {
        add_tracking_entry(ptr, total_size, 0, NULL);
    }
    
    return ptr;
}

// Custom implementation of realloc
static void* custom_realloc(void* ptr, size_t size) {
    if (!ptr) return custom_malloc(size);
    if (!size) { custom_free(ptr); return NULL; }
    
    AllocationEntry* entry = find_tracking_entry(ptr);
    if (!entry) {
        // Not tracked by us, use original realloc
        return original_realloc(ptr, size);
    }
    
    void* new_ptr;
    
    // Try original realloc first
    new_ptr = original_realloc(ptr, size);
    if (new_ptr) {
        remove_tracking_entry(ptr);
        add_tracking_entry(new_ptr, size, 0, NULL);
    }
    
    return new_ptr;
}

// Custom implementation of aligned_alloc
static void* custom_aligned_alloc(size_t alignment, size_t size) {
    void* ptr = original_aligned_alloc(alignment, size);
    if (ptr) {
        add_tracking_entry(ptr, size, 0, NULL);
    }
    
    return ptr;
}

// Custom implementation of posix_memalign
static int custom_posix_memalign(void** memptr, size_t alignment, size_t size) {    
    int ret = original_posix_memalign(memptr, alignment, size);
    if (ret == 0) {
        add_tracking_entry(*memptr, size, 0, NULL);
    }
    
    return 0;
}

static void memory_usage_log_print() {
    fprintf(stderr, "Memory allocation interceptors detached and resources cleaned up\n");
    fprintf(stderr, "Max usage (bytes): %lu\n", max_memory);
}

// Main function to attach interceptors
int malloc_interceptor_attach() {
    GumReplaceReturn replace_check;
    
    // Initialize Frida interceptor
    malloc_interceptor = gum_interceptor_obtain();
    gum_interceptor_begin_transaction(malloc_interceptor);
    
    // Find function addresses
    malloc_addr = (void*)malloc;
    free_addr = (void*)free;
    calloc_addr = (void*)calloc;
    realloc_addr = (void*)realloc;
    aligned_alloc_addr = (void*)aligned_alloc;
    posix_memalign_addr = (void*)posix_memalign;

    // Replace functions with our custom implementations
    if (malloc_addr) {
        replace_check = gum_interceptor_replace_fast(malloc_interceptor, 
                                                  malloc_addr, 
                                                  custom_malloc,
                                                  (gpointer*)(&original_malloc));
        if (replace_check != GUM_REPLACE_OK) {
            fprintf(stderr, "Failed to replace malloc: %d\n", replace_check);
        }
    }
    
    if (free_addr) {
        replace_check = gum_interceptor_replace_fast(malloc_interceptor, 
                                                  free_addr, 
                                                  custom_free,
                                                  (gpointer*)(&original_free));
        if (replace_check != GUM_REPLACE_OK) {
            fprintf(stderr, "Failed to replace free: %d\n", replace_check);
        }
    }
    
    if (calloc_addr) {
        replace_check = gum_interceptor_replace_fast(malloc_interceptor, 
                                                  calloc_addr, 
                                                  custom_calloc,
                                                  (gpointer*)(&original_calloc));
        if (replace_check != GUM_REPLACE_OK) {
            fprintf(stderr, "Failed to replace calloc: %d\n", replace_check);
        }
    }
    
    if (realloc_addr) {
        replace_check = gum_interceptor_replace_fast(malloc_interceptor, 
                                                  realloc_addr, 
                                                  custom_realloc,
                                                  (gpointer*)(&original_realloc));
        if (replace_check != GUM_REPLACE_OK) {
            fprintf(stderr, "Failed to replace realloc: %d\n", replace_check);
        }
    }
    
    if (aligned_alloc_addr) {
        replace_check = gum_interceptor_replace_fast(malloc_interceptor, 
                                                  aligned_alloc_addr, 
                                                  custom_aligned_alloc,
                                                  (gpointer*)(&original_aligned_alloc));
        if (replace_check != GUM_REPLACE_OK) {
            fprintf(stderr, "Failed to replace aligned_alloc: %d\n", replace_check);
        }
    }
    
    if (posix_memalign_addr) {
        replace_check = gum_interceptor_replace(malloc_interceptor, 
                                                  posix_memalign_addr, 
                                                  custom_posix_memalign, NULL,
                                                  (gpointer*)(&original_posix_memalign));
        if (replace_check != GUM_REPLACE_OK) {
            fprintf(stderr, "Failed to replace posix_memalign: %d\n", replace_check);
        }
    }

    gum_interceptor_end_transaction(malloc_interceptor);
    
    // Initialize the tracking table if not already done
    init_table();
    
    fprintf(stderr, "Memory allocation functions intercepted successfully\n");
    
    return 0;
}

// Function to detach interceptors and clean up
void malloc_interceptor_dettach() {
    cleanup_in_progress = 1;
    
    // Revert all interceptors
    if (malloc_addr) gum_interceptor_revert(malloc_interceptor, malloc_addr);
    if (free_addr) gum_interceptor_revert(malloc_interceptor, free_addr);
    if (calloc_addr) gum_interceptor_revert(malloc_interceptor, calloc_addr);
    if (realloc_addr) gum_interceptor_revert(malloc_interceptor, realloc_addr);
    if (aligned_alloc_addr) gum_interceptor_revert(malloc_interceptor, aligned_alloc_addr);
    if (posix_memalign_addr) gum_interceptor_revert(malloc_interceptor, posix_memalign_addr);
    
    // Clean up tracking table if it exists
    gum_metal_hash_table_unref(track_table);
    
    // Release interceptor
    g_object_unref(malloc_interceptor);

    memory_usage_log_print();
}

