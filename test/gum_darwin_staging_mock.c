/* Host-independent semantic test for the pinned Darwin transaction overlay.
 * Build with a real 17.15.3 devkit; Gum mutation APIs below are a fake backend.
 * This checks publication ordering, not native Mach thread suspension. */
#define gum_code_allocator_try_alloc_slice_near mock_alloc_near
#define gum_code_slice_unref mock_slice_unref
#define gum_interceptor_attach mock_attach
#define gum_interceptor_replace mock_replace
#define gum_interceptor_detach mock_detach
#define gum_interceptor_revert mock_revert
#define gum_interceptor_begin_transaction mock_begin
#define gum_interceptor_end_transaction mock_end
#define gum_process_get_code_signing_policy mock_signing_policy
#define gum_ensure_code_readable mock_ensure_readable
#include "frida-gum.h"
#ifndef __APPLE__
#define __APPLE__ 1
#endif
#ifndef __aarch64__
#define __aarch64__ 1
#endif
#include "../src/gum_peak_darwin_patch_api.c"
#include <assert.h>
#include <stdio.h>

static unsigned flushes;
static guint8 patch_bytes[32];
static gpointer fake_redirect;
static gboolean fail_near_allocation;
static guint reserved_releases;
static guint near_allocations;
static gpointer near_marker = (gpointer)0x6789;

GumCodeSlice* mock_alloc_near(GumCodeAllocator* allocator,
    const GumAddressSpec* spec, gsize alignment)
{
    assert(allocator->free_slices == NULL);
    assert(alignment == 0 && spec->near_address != NULL);
    assert(spec->max_distance + allocator->pages_per_batch *
        gum_query_page_size() + 4 == ((gsize)1 << 27));
    if (fail_near_allocation) return NULL;
    near_allocations++;
    GumCodeSlice* reserved = g_new0(GumCodeSlice, 1);
    reserved->pc = (guint8*)spec->near_address + 64;
    allocator->free_slices = g_list_append(NULL, near_marker);
    allocator->free_slices = g_list_append(allocator->free_slices, near_marker);
    return reserved;
}
void mock_slice_unref(GumCodeSlice* reserved)
{
    if (reserved != NULL) {
        /* The reservation must survive until outer transaction finalization. */
        assert(peak_gum_darwin_stage.interceptor == NULL ||
            ((PeakGumInterceptor17*)peak_gum_darwin_stage.interceptor)->current_transaction.level == 0);
        reserved_releases++;
        g_free(reserved);
    }
}

GumCodeSigningPolicy mock_signing_policy(void) { return GUM_CODE_SIGNING_OPTIONAL; }
void mock_ensure_readable(gconstpointer address, gsize size)
{ (void)address; (void)size; }

void mock_begin(GumInterceptor* interceptor)
{ ((PeakGumInterceptor17*)interceptor)->current_transaction.level++; }
void mock_end(GumInterceptor* interceptor)
{
    PeakGumTransaction17* txn = &((PeakGumInterceptor17*)interceptor)->current_transaction;
    assert(--txn->level >= 0);
    if (txn->level == 0) {
        /* An unremoved activation would publish bytes at this point. */
        assert(g_hash_table_size(txn->pending_update_tasks) == 0);
        txn->is_dirty = FALSE;
        /* Darwin allocator commit retires unused free slices. */
        PeakGumCodeAllocator17* allocator = &((PeakGumInterceptor17*)interceptor)->allocator;
        g_list_free(allocator->free_slices);
        allocator->free_slices = NULL;
        flushes++;
    }
}

static PeakGumFunctionContext17* fake_install(GumInterceptor* interceptor,
    gpointer address, GumInvocationListener* listener, gpointer replacement,
    const GumInterceptorOptions* options)
{
    PeakGumInterceptor17* self = (PeakGumInterceptor17*)interceptor;
    /* The ordinary unconstrained Gum allocation consumes the primed head. */
    GList* next_slice = self->allocator.free_slices;
    assert(next_slice != NULL && next_slice->data == near_marker);
    self->allocator.free_slices = g_list_delete_link(next_slice, next_slice);
    PeakGumFunctionContext17* context = g_new0(PeakGumFunctionContext17, 1);
    GPtrArray* entries = g_ptr_array_new_with_free_func(g_free);
    GumArm64Writer writer;
    GumRedirectWriteDetails details;
    context->function_address = fake_redirect != NULL ? fake_redirect : address;
    context->overwritten_prologue_len = 16;
    context->overwritten_prologue = g_memdup2(address, 16);
    context->redirect_code = g_malloc0(32);
    context->listener_entries = entries;
    context->replacement_function = replacement;
    if (listener != NULL) {
        PeakGumListenerEntry17* entry = g_new0(PeakGumListenerEntry17, 1);
        entry->listener_instance = listener;
        g_ptr_array_add(entries, entry);
    }
    gum_arm64_writer_init(&writer, context->redirect_code);
    writer.pc = GUM_ADDRESS(address);
    details = (GumRedirectWriteDetails){ &writer,
        (guint8*)address + 64, ARM64_REG_X16, 32 };
    assert(options->write_redirect(&details, options->write_redirect_data) == GUM_REDIRECT_WRITTEN);
    gum_arm64_writer_flush(&writer);
    gum_arm64_writer_clear(&writer);
    g_hash_table_insert(self->function_by_address, context->function_address, context);
    g_hash_table_insert(self->current_transaction.pending_update_tasks,
                        context->function_address, context);
    self->current_transaction.is_dirty = TRUE;
    return context;
}
GumAttachReturn mock_attach(GumInterceptor* interceptor, gpointer address,
    GumInvocationListener* listener, const GumAttachOptions* options)
{
    fake_install(interceptor, address, listener, NULL, &options->instrumentation);
    return GUM_ATTACH_OK;
}
GumReplaceReturn mock_replace(GumInterceptor* interceptor, gpointer address,
    gpointer replacement, gpointer* original, const GumReplaceOptions* options)
{
    fake_install(interceptor, address, NULL, replacement, &options->instrumentation);
    if (original != NULL) *original = address;
    return GUM_REPLACE_OK;
}
static void fake_remove(PeakGumInterceptor17* self,
                        PeakGumFunctionContext17* context)
{
    assert(!context->activated); /* Rollback must never request an entry write. */
    g_hash_table_remove(self->function_by_address, context->function_address);
    g_ptr_array_unref((GPtrArray*)context->listener_entries);
    g_free(context->overwritten_prologue);
    g_free(context->redirect_code);
    g_free(context);
}
void mock_detach(GumInterceptor* interceptor, GumInvocationListener* listener)
{
    PeakGumInterceptor17* self = (PeakGumInterceptor17*)interceptor;
    GHashTableIter iter;
    gpointer value;
    PeakGumFunctionContext17* found = NULL;
    g_hash_table_iter_init(&iter, self->function_by_address);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        if (peak_gum_darwin_context_has_only_listener(value, listener)) found = value;
    assert(found != NULL);
    fake_remove(self, found);
}
void mock_revert(GumInterceptor* interceptor, gpointer address)
{
    PeakGumInterceptor17* self = (PeakGumInterceptor17*)interceptor;
    PeakGumFunctionContext17* context = g_hash_table_lookup(
        self->function_by_address, fake_redirect != NULL ? fake_redirect : address);
    assert(context != NULL);
    fake_remove(self, context);
}

int main(void)
{
    gum_init_embedded();
    PeakGumInterceptor17 self = { 0 };
    guint8 entry[32] = { 0x1f, 0x20, 0x03, 0xd5 }; /* nop */
    guint8 original[32], active[32];
    gpointer canonical, original_function;
    guint len;
    GumInterceptor* interceptor = (GumInterceptor*)&self;
    GumInvocationListener* listener = (GumInvocationListener*)0x1234;
    memcpy(patch_bytes, entry, sizeof(entry));
    g_rec_mutex_init(&self.mutex);
    self.allocator.pages_per_batch = 7;
    self.allocator.slice_size = 256;
    self.allocator.slices_per_batch = 7 * gum_query_page_size() / 256;
    self.function_by_address = g_hash_table_new(NULL, NULL);
    self.current_transaction.pending_update_tasks = g_hash_table_new(NULL, NULL);
    assert(peak_gum_darwin_staged_attach_supported());
    /* Near allocation restores preexisting free-list nodes on failure; on
     * success it places the fresh near batch before every old far slice. */
    GList* old_free = g_list_append(NULL, (gpointer)0x5555);
    self.allocator.free_slices = old_free;
    GumCodeSlice* reservation;
    fail_near_allocation = TRUE;
    assert(!peak_gum_darwin_reserve_near_batch(interceptor, entry, &reservation));
    assert(reservation == NULL && self.allocator.free_slices == old_free);
    fail_near_allocation = FALSE;
    assert(peak_gum_darwin_reserve_near_batch(interceptor, entry, &reservation));
    assert(self.allocator.free_slices->data == near_marker);
    assert(g_list_last(self.allocator.free_slices) == old_free);
    g_list_free(self.allocator.free_slices);
    self.allocator.free_slices = NULL;
    mock_slice_unref(reservation);
    reserved_releases = near_allocations = 0;

    assert(peak_gum_darwin_prepare_attach(interceptor, entry, listener, NULL) == GUM_ATTACH_OK);
    assert(flushes == 1 && memcmp(entry, patch_bytes, 32) == 0);
    assert(peak_gum_darwin_get_staged_patch(interceptor, &canonical, active, original, &len));
    assert(canonical == entry && len == 16 && memcmp(original, entry, len) == 0);
    assert(memcmp(active, entry, len) != 0);
    guint8 queried[32];
    assert(peak_gum_darwin_get_function_patch(interceptor, entry, listener,
        &canonical, queried, original, &len));
    assert(memcmp(queried, active, len) == 0);
    assert(peak_gum_darwin_prepare_attach(interceptor, entry + 16,
        (GumInvocationListener*)0x2345, NULL) != GUM_ATTACH_OK);
    peak_gum_darwin_finish_staged_mutation(interceptor, FALSE);
    assert(g_hash_table_size(self.function_by_address) == 0);
    assert(memcmp(entry, patch_bytes, 32) == 0);

    assert(peak_gum_darwin_prepare_replace(interceptor, entry, (gpointer)0x9876,
        &original_function, NULL) == GUM_REPLACE_OK);
    assert(original_function == entry && memcmp(entry, patch_bytes, 32) == 0);
    peak_gum_darwin_finish_staged_mutation(interceptor, FALSE);
    assert(g_hash_table_size(self.function_by_address) == 0);

    /* Preserve an installed context and reject listener reuse on another entry. */
    assert(peak_gum_darwin_prepare_attach(interceptor, entry, listener, NULL) == GUM_ATTACH_OK);
    peak_gum_darwin_finish_staged_mutation(interceptor, TRUE);
    assert(g_hash_table_size(self.function_by_address) == 1);
    assert(peak_gum_darwin_prepare_attach(interceptor, entry + 16, listener, NULL) != GUM_ATTACH_OK);
    assert(g_hash_table_size(self.function_by_address) == 1);
    PeakGumFunctionContext17* context = g_hash_table_lookup(self.function_by_address, entry);
    context->activated = FALSE;
    fake_remove(&self, context);

    /* Fail before mutation for an outer Gum transaction or redirect entry. */
    self.current_transaction.level = 1;
    assert(peak_gum_darwin_prepare_attach(interceptor, entry, listener, NULL) != GUM_ATTACH_OK);
    self.current_transaction.level = 0;
    guint32 branch = 0x14000001;
    memcpy(entry, &branch, 4);
    assert(peak_gum_darwin_prepare_attach(interceptor, entry, listener, NULL) != GUM_ATTACH_OK);
    memcpy(entry, patch_bytes, 32);

    /* Defend against unexpected backend redirection; roll back without writes. */
    fake_redirect = entry + 16;
    assert(peak_gum_darwin_prepare_attach(interceptor, entry, listener, NULL) != GUM_ATTACH_OK);
    assert(g_hash_table_size(self.function_by_address) == 0);
    fake_redirect = NULL;
    assert(memcmp(entry, patch_bytes, 32) == 0);
    fail_near_allocation = TRUE;
    assert(peak_gum_darwin_prepare_attach(interceptor, entry, listener, NULL) != GUM_ATTACH_OK);
    assert(self.current_transaction.level == 0 && !self.current_transaction.is_dirty);
    assert(g_hash_table_size(self.function_by_address) == 0);
    fail_near_allocation = FALSE;
    assert(near_allocations == reserved_releases);
    /* The custom redirect accepts short direct branches, declines short
     * unreachable entries, and emits a full absolute redirect when needed. */
    for (guint i = 0; i < sizeof(entry); i += 4) {
        guint32 nop = 0xd503201f;
        memcpy(entry + i, &nop, 4);
    }
    guint32 ret = 0xd65f03c0u;
    memcpy(entry + sizeof(entry) - 4, &ret, 4);
    GumArm64Writer writer;
    guint8 redirect[32] = { 0 };
    guint redirect_size = 0;
    gum_arm64_writer_init(&writer, redirect);
    writer.pc = GUM_ADDRESS(entry);
    GumRedirectWriteDetails details = { &writer,
        GSIZE_TO_POINTER(GUM_ADDRESS(entry) + 0x1000000000ULL), ARM64_REG_X16, 4 };
    assert(peak_gum_darwin_write_staged_redirect(&details, &redirect_size) == GUM_REDIRECT_DECLINED);
    details.capacity = 16;
    assert(peak_gum_darwin_write_staged_redirect(&details, &redirect_size) == GUM_REDIRECT_WRITTEN);
    gum_arm64_writer_flush(&writer);
    assert(redirect_size == 16 && gum_arm64_writer_offset(&writer) == 16);
    gum_arm64_writer_reset(&writer, redirect);
    writer.pc = GUM_ADDRESS(entry);
    guint32 read_x16 = 0xaa1003e0u; /* mov x0, x16 */
    guint32 read_x17 = 0xaa1103e1u; /* mov x1, x17 */
    memcpy(entry, &read_x16, 4);
    memcpy(entry + 4, &read_x17, 4);
    redirect_size = 0;
    assert(peak_gum_darwin_write_staged_redirect(&details, &redirect_size) == GUM_REDIRECT_DECLINED);
    assert(strcmp(peak_gum_darwin_last_stage_failure(),
        "absolute-redirect-no-free-scratch-register") == 0);
    assert(redirect_size == 0 && gum_arm64_writer_offset(&writer) == 0);
    details.scratch_register = ARM64_REG_X17;
    assert(peak_gum_darwin_write_staged_redirect(&details, &redirect_size) == GUM_REDIRECT_DECLINED);
    assert(redirect_size == 0 && gum_arm64_writer_offset(&writer) == 0);
    /* The normal on_enter trampoline clobbers the chosen scratch even if
     * its entry redirect is B. One instruction reads both x16 and x17. */
    guint32 read_both = 0x8b110200u; /* add x0, x16, x17 */
    memcpy(entry, &read_both, 4);
    details.target = entry + 64;
    details.capacity = 4;
    assert(peak_gum_darwin_write_staged_redirect(&details, &redirect_size) == GUM_REDIRECT_DECLINED);
    assert(strcmp(peak_gum_darwin_last_stage_failure(),
        "direct-redirect-no-free-scratch-register") == 0);
    assert(redirect_size == 0 && gum_arm64_writer_offset(&writer) == 0);
    details.scratch_register = ARM64_REG_X16;
    assert(peak_gum_darwin_write_staged_redirect(&details, &redirect_size) == GUM_REDIRECT_DECLINED);
    assert(redirect_size == 0 && gum_arm64_writer_offset(&writer) == 0);
    guint32 nop = 0xd503201fu;
    memcpy(entry, &nop, 4);
    assert(peak_gum_darwin_write_staged_redirect(&details, &redirect_size) == GUM_REDIRECT_WRITTEN);
    assert(redirect_size == 4 && gum_arm64_writer_offset(&writer) == 4);
    gum_arm64_writer_clear(&writer);
    g_hash_table_unref(self.current_transaction.pending_update_tasks);
    g_hash_table_unref(self.function_by_address);
    g_rec_mutex_clear(&self.mutex);
    gum_deinit_embedded();
    puts("Darwin staged Gum semantics passed");
    return 0;
}
