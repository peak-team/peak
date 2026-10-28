/* Compile the real Darwin controller against deterministic Mach/Gum doubles.
 * This tests ordering/failure paths; it does not substitute for native tests. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/detach_controller_darwin.c"

static unsigned char entry[4096] __attribute__((aligned(4096)));
static unsigned char active[4] = { 1, 2, 3, 4 };
static unsigned char original[4] = { 0 };
static unsigned int snapshots, suspended, resumed, staged, finished, cancelled;
static gboolean bad_pc, bad_bytes, bad_identity, endless;
static int prepare_result;
static gboolean clone_mode, other_page_change, map_failure, writable_source;
static unsigned int allocations;
static gboolean peer_held[PEAK_DARWIN_MAX_THREADS];
unsigned int mach_task_self(void) { return 1; }
thread_act_t mach_thread_self(void) { return 1; }
kern_return_t task_threads(unsigned int task, thread_act_array_t* out,
                           mach_msg_type_number_t* count) {
    (void)task;
    snapshots++;
    /* Peer 2 was already creating peer 3 when the ungated first snapshot
     * began. The child becomes visible only after its creator is held. */
    if (!endless && snapshots == 2) {
        assert(peer_held[2]);
        assert(!peer_held[3]);
        assert(memcmp(entry, original, 4) == 0 || bad_bytes);
    }
    *count = endless ? snapshots + 1 : (snapshots == 1 ? 2 : 3);
    *out = malloc(*count * sizeof(**out));
    for (unsigned int i = 0; i < *count; i++) (*out)[i] = i + 1;
    return KERN_SUCCESS;
}
kern_return_t thread_suspend(thread_act_t t) {
    if (other_page_change) entry[100] = 7;
    assert(!peer_held[t]); peer_held[t] = TRUE; suspended++; return 0;
}
kern_return_t thread_resume(thread_act_t t) {
    assert(peer_held[t]); peer_held[t] = FALSE; resumed++; return 0;
}
kern_return_t thread_get_state(thread_act_t t, int flavor, thread_state_t out,
                               mach_msg_type_number_t* count) {
    (void)t; (void)flavor; (void)count;
    ((arm_thread_state64_t*)out)->__pc = (uintptr_t)entry + (bad_pc ? 1 : 0);
    return 0;
}
kern_return_t mach_port_deallocate(unsigned int task, unsigned int t) {
    (void)task; (void)t; return 0;
}
kern_return_t vm_deallocate(unsigned int task, vm_address_t a, vm_size_t n) {
    (void)task; (void)n; free((void*)a); return 0;
}
gboolean gum_memory_can_remap_writable(void) { return !clone_mode; }
gboolean gum_memory_query_protection(const void* p, GumPageProtection* protection) {
    (void)p; *protection = writable_source ? GUM_PAGE_RW : GUM_PAGE_RX; return TRUE;
}
gpointer gum_try_alloc_n_pages(guint n, GumPageProtection p) {
    assert(suspended == resumed);
    allocations++; (void)p; return malloc(n * 4096);
}
void gum_free_pages(gpointer p) { assert(suspended == resumed); allocations--; free(p); }
kern_return_t mach_vm_protect(unsigned int t, mach_vm_address_t a, size_t n, int m, int p) {
    (void)t; (void)a; (void)n; (void)m; (void)p; return 0;
}
kern_return_t mach_vm_remap(unsigned int t, mach_vm_address_t* a, size_t n,
    uintptr_t mask, int flags, unsigned int source, mach_vm_address_t from,
    int copy, vm_prot_t* current, vm_prot_t* maximum, int inheritance) {
    (void)t; (void)mask; (void)flags; (void)source; (void)copy;
    (void)current; (void)maximum; (void)inheritance;
    assert(peer_held[2] && peer_held[3]);
    if (map_failure) return KERN_INVALID_ARGUMENT;
    memcpy((void*)*a, (void*)from, n); return 0;
}
gsize gum_query_page_size(void) { return 4096; }
gpointer gum_memory_try_remap_writable_pages(gpointer p, guint n) {
    assert(suspended == resumed); (void)n; return p;
}
void gum_memory_dispose_writable_pages(gpointer p, guint n) {
    assert(suspended == resumed); (void)p; (void)n;
}
gpointer gum_strip_code_pointer(gpointer p) { return p; }
void sys_dcache_flush(void* p, size_t n) {
    (void)p; (void)n;
    if (clone_mode && suspended == resumed) return;
    /* Publication cannot occur after only the first or second snapshot.
     * Creator and child must both remain held through the stable third pass. */
    assert(snapshots == 3 && peer_held[2] && peer_held[3]);
    assert(suspended == 2 && resumed == 0);
}
void sys_icache_invalidate(void* p, size_t n) { (void)p; (void)n; }
GumAttachReturn peak_gum_darwin_prepare_attach(GumInterceptor* i, gpointer p,
    GumInvocationListener* l, const GumAttachOptions* o) {
    (void)i; (void)p; (void)l; (void)o;
    assert(suspended == resumed); staged++;
    if (bad_bytes) entry[0] = 9;
    return prepare_result;
}
const char* peak_gum_darwin_last_stage_failure(void) { return "mock-stage-refused"; }
GumReplaceReturn peak_gum_darwin_prepare_replace(GumInterceptor* i, gpointer p,
    gpointer r, gpointer* o, const GumReplaceOptions* options) {
    (void)r; (void)options;
    if (o != NULL) *o = (void*)4;
    return peak_gum_darwin_prepare_attach(i, p, NULL, NULL);
}
gboolean peak_gum_darwin_get_staged_patch(GumInterceptor* i, gpointer* p,
    guint8* a, guint8* o, guint* n) {
    (void)i; assert(suspended == resumed);
    *p = bad_identity ? entry + 4 : entry; *n = 4;
    memcpy(a, active, 4); memcpy(o, original, 4); return TRUE;
}
void peak_gum_darwin_finish_staged_mutation(GumInterceptor* i, gboolean commit) {
    (void)i; assert(suspended == resumed);
    if (commit) finished++; else cancelled++;
}
gboolean peak_gum_darwin_get_function_patch(GumInterceptor* i, gpointer p,
    GumInvocationListener* l, gpointer* c, guint8* a, guint8* o, guint* n) {
    (void)i; (void)p; (void)l; (void)c; (void)a; (void)o; (void)n; return FALSE;
}
gboolean peak_gum_darwin_get_canonical_address_exact(GumInterceptor* i,
    gpointer p, GumInvocationListener* l, gpointer* c) {
    (void)i; (void)p; (void)l; (void)c; return FALSE;
}
static PeakDetachRequest request = {
    .hook_id = 1, .symbol_name = "pthread_create", .function_address = entry,
    .interceptor = (void*)1, .listener = (void*)2,
    .operation = PEAK_DETACH_OPERATION_ATTACH,
};
static void reset(void) {
    memset(entry, 0, sizeof(entry));
    memset(peer_held, 0, sizeof(peer_held));
    snapshots = suspended = resumed = staged = finished = cancelled = 0;
    bad_pc = bad_bytes = bad_identity = endless = FALSE;
    prepare_result = GUM_ATTACH_OK;
    clone_mode = other_page_change = map_failure = writable_source = FALSE;
    assert(allocations == 0);
    peak_detach_controller_note_thread_creation_gate_installed(FALSE);
}
int main(void) {
    PeakDetachStatus status;
    reset();
    assert(!peak_detach_controller_prepare_hook_mutation(&request, &status));
    assert(status == PEAK_DETACH_STATUS_UNSUPPORTED && staged == 0);
    assert(peak_detach_controller_prepare_pthread_bootstrap(&request, &status));
    assert(snapshots == 3 && suspended == 2 && resumed == 0);
    assert(memcmp(entry, active, 4) == 0 && finished == 0);
    assert(peak_detach_controller_finish_hook_mutation(&request, &status));
    assert(resumed == 2 && finished == 1);
    reset(); clone_mode = TRUE;
    assert(peak_detach_controller_prepare_pthread_bootstrap(&request, &status));
    assert(memcmp(entry, active, 4) == 0);
    assert(peak_detach_controller_finish_hook_mutation(&request, &status));
    assert(allocations == 0);
    reset(); clone_mode = other_page_change = TRUE;
    assert(!peak_detach_controller_prepare_pthread_bootstrap(&request, &status));
    assert(entry[0] == 0 && entry[100] == 7 && allocations == 0);
    assert(strcmp(peak_detach_controller_last_failure_detail()->reason,
                  "clone-page-bytes-changed") == 0);
    reset(); clone_mode = map_failure = TRUE;
    assert(!peak_detach_controller_prepare_pthread_bootstrap(&request, &status));
    assert(entry[0] == 0 && allocations == 0 && suspended == resumed);
    reset(); clone_mode = writable_source = TRUE;
    assert(!peak_detach_controller_prepare_pthread_bootstrap(&request, &status));
    assert(suspended == 0 && allocations == 0);
    reset(); prepare_result = -4;
    assert(!peak_detach_controller_prepare_pthread_bootstrap(&request, &status));
    assert(strcmp(peak_detach_controller_last_failure_detail()->reason,
                  "mock-stage-refused") == 0);
    assert((intptr_t)peak_detach_controller_last_failure_detail()->aux == -4);
    assert(suspended == 0 && cancelled == 0);
    reset(); bad_pc = TRUE;
    assert(!peak_detach_controller_prepare_pthread_bootstrap(&request, &status));
    assert(suspended == resumed && cancelled == 1 && entry[0] == 0);
    reset(); bad_bytes = TRUE;
    assert(!peak_detach_controller_prepare_pthread_bootstrap(&request, &status));
    assert(suspended == resumed && cancelled == 1 && entry[0] == 9);
    reset(); bad_identity = TRUE;
    assert(!peak_detach_controller_prepare_pthread_bootstrap(&request, &status));
    assert(suspended == 0 && cancelled == 1);
    reset(); endless = TRUE;
    assert(!peak_detach_controller_prepare_pthread_bootstrap(&request, &status));
    assert(snapshots == PEAK_DARWIN_SNAPSHOT_ATTEMPTS);
    assert(suspended == resumed && cancelled == 1 && entry[0] == 0);
    reset(); peak_detach_controller_note_thread_creation_gate_installed(TRUE);
    request.operation = PEAK_DETACH_OPERATION_REPLACE;
    request.replacement_function = (void*)3;
    assert(peak_detach_controller_prepare_hook_mutation(&request, &status));
    assert(peak_detach_controller_finish_hook_mutation(&request, &status));
    assert(finished == 1);
    reset(); peak_detach_controller_note_thread_creation_gate_installed(TRUE);
    gpointer replacement_original = NULL;
    request.original_function = &replacement_original;
    bad_identity = TRUE;
    assert(!peak_detach_controller_prepare_hook_mutation(&request, &status));
    assert(cancelled == 1 && replacement_original == NULL);
    request.operation = PEAK_DETACH_OPERATION_REVERT;
    assert(!peak_detach_controller_prepare_hook_mutation(&request, &status));
    assert(status == PEAK_DETACH_STATUS_UNSUPPORTED);
    return 0;
}
