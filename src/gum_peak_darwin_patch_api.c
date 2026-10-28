/*
 * Minimal Frida Gum 17.15.3 Darwin Arm64 private-layout overlay.
 *
 * Keep this separate from PEAK's Linux Gum overlay: Darwin physical detach
 * only needs the canonical entry address and the exact active/original entry
 * bytes.  No Linux module-sync, fast-listener, or PC-classification code is
 * shared with this implementation.
 */

#include "internal/gum_peak_darwin_patch_api.h"

#include <stdint.h>
#include <string.h>

#if !defined(__APPLE__) || \
    !(defined(__aarch64__) || defined(__arm64__))
#error "The PEAK Darwin Gum patch overlay only supports macOS Arm64"
#endif

/* Internal exported symbol in the pinned static devkit. The custom-redirect
 * API substitutes x16 when no scratch register is free, so absolute redirects
 * must independently retain the checked relocator's liveness guarantee. */
extern gboolean gum_arm64_relocator_can_relocate(
    gpointer address, guint min_bytes, GumRelocationScenario scenario,
    GumRelocationPolicy policy, guint* maximum, arm64_reg* available_scratch_reg);

typedef guint8 PeakGumInterceptorType17;

typedef struct {
    gsize slice_size, pages_per_batch, slices_per_batch, pages_metadata_size;
    GSList* uncommitted_pages;
    GHashTable* dirty_pages;
    GList* free_slices;
    GSList* dispatchers;
} PeakGumCodeAllocator17;

typedef struct {
    gboolean is_dirty;
    gint level;
    GQueue* pending_destroy_tasks;
    GHashTable* pending_update_tasks;
    GumInterceptor* interceptor;
} PeakGumTransaction17;

typedef struct {
    GObject parent;
    GRecMutex mutex;
    GHashTable* function_by_address;
    gpointer backend;
    PeakGumCodeAllocator17 allocator;
    GumInterceptorOptions options;
    volatile guint selected_thread_id;
    PeakGumTransaction17 current_transaction;
} PeakGumInterceptor17;

typedef struct {
    gpointer function_address;
    gpointer grafted_hook;
    gpointer import_target;
    PeakGumInterceptorType17 type;
    guint8 destroyed;
    guint8 activated;
    guint8 has_on_leave_listener;
    guint8 has_unignorable_listener;
    GumCodeSlice* trampoline_slice;
    GumCodeDeflector* trampoline_deflector;
    volatile gint trampoline_usage_counter;
    gpointer on_enter_trampoline;
    guint8* overwritten_prologue;
    guint overwritten_prologue_len;
    guint8* redirect_code;
    gpointer on_invoke_trampoline;
    gpointer on_leave_trampoline;
    volatile GPtrArray* listener_entries;
    gpointer replacement_function;
    gpointer replacement_data;
} PeakGumFunctionContext17;

typedef struct {
#ifndef GUM_DIET
    GumInvocationListenerInterface* listener_interface;
    GumInvocationListener* listener_instance;
#else
    union {
        GumInvocationListener* listener_interface;
        GumInvocationListener* listener_instance;
    };
#endif
    gpointer function_data;
    gboolean unignorable;
} PeakGumListenerEntry17;

typedef struct {
    PeakGumFunctionContext17* context;
    gpointer activate;
} PeakGumUpdateTask17;

typedef struct {
    GumInterceptor* interceptor;
    PeakGumFunctionContext17* context;
    GumInvocationListener* listener;
    guint8 active[PEAK_GUM_DARWIN_MAX_PROLOGUE_SIZE];
} PeakGumDarwinStage;

static _Thread_local PeakGumDarwinStage peak_gum_darwin_stage;
static _Thread_local guint peak_gum_darwin_redirect_size;
static _Thread_local const gchar* peak_gum_darwin_stage_failure = "none";

const gchar*
peak_gum_darwin_last_stage_failure(void)
{
    return peak_gum_darwin_stage_failure;
}

G_STATIC_ASSERT(sizeof(gpointer) == 8);
G_STATIC_ASSERT(sizeof(guint) == 4);

static gboolean
peak_gum_darwin_context_has_only_listener(PeakGumFunctionContext17* context,
                                          GumInvocationListener* listener)
{
    GPtrArray* entries;
    PeakGumListenerEntry17* entry;

    if (listener == NULL) {
        return FALSE;
    }

    entries = (GPtrArray*)g_atomic_pointer_get(&context->listener_entries);
    if (entries == NULL || entries->len != 1) {
        return FALSE;
    }

    entry = g_ptr_array_index(entries, 0);
    return entry != NULL && entry->listener_instance == listener;
}

static gboolean
peak_gum_darwin_context_is_entry_patch(PeakGumFunctionContext17* context,
                                       GumInvocationListener* listener)
{
    return context != NULL &&
           !context->destroyed &&
           context->activated &&
           context->function_address != NULL &&
           context->grafted_hook == NULL &&
           context->import_target == NULL &&
           context->replacement_function == NULL &&
           context->overwritten_prologue != NULL &&
           context->overwritten_prologue_len > 0 &&
           context->overwritten_prologue_len <=
               PEAK_GUM_DARWIN_MAX_PROLOGUE_SIZE &&
           peak_gum_darwin_context_has_only_listener(context, listener);
}

static PeakGumFunctionContext17*
peak_gum_darwin_find_context_exact(PeakGumInterceptor17* interceptor,
                                   gpointer function_address,
                                   GumInvocationListener* listener)
{
    PeakGumFunctionContext17* context;
    gpointer stripped_address = gum_strip_code_pointer(function_address);

    if (interceptor->function_by_address == NULL || stripped_address == NULL) {
        return NULL;
    }

    context = g_hash_table_lookup(interceptor->function_by_address,
                                  stripped_address);
    return peak_gum_darwin_context_is_entry_patch(context, listener)
               ? context
               : NULL;
}

static PeakGumFunctionContext17*
peak_gum_darwin_find_context(PeakGumInterceptor17* interceptor,
                             gpointer function_address,
                             GumInvocationListener* listener)
{
    PeakGumFunctionContext17* context = peak_gum_darwin_find_context_exact(
        interceptor, function_address, listener);

    if (context != NULL) {
        return context;
    }
    if (listener != NULL) {
        GHashTableIter iter;
        gpointer value;

        g_hash_table_iter_init(&iter, interceptor->function_by_address);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            PeakGumFunctionContext17* candidate = value;

            if (!peak_gum_darwin_context_is_entry_patch(candidate,
                                                        listener)) {
                continue;
            }
            if (context != NULL) {
                return NULL;
            }
            context = candidate;
        }
    }

    return context;
}

gboolean
peak_gum_darwin_get_function_patch(GumInterceptor* interceptor,
                                   gpointer function_address,
                                   GumInvocationListener* listener,
                                   gpointer* canonical_address_out,
                                   guint8* active_patch,
                                   guint8* original_prologue,
                                   guint* prologue_len)
{
    PeakGumInterceptor17* private_interceptor;
    PeakGumFunctionContext17* context;
    guint len;
    gboolean found = FALSE;

    if (interceptor == NULL || function_address == NULL || listener == NULL ||
        canonical_address_out == NULL || active_patch == NULL ||
        original_prologue == NULL || prologue_len == NULL) {
        return FALSE;
    }

    *canonical_address_out = NULL;
    *prologue_len = 0;
    private_interceptor = (PeakGumInterceptor17*)interceptor;

    g_rec_mutex_lock(&private_interceptor->mutex);
    context = peak_gum_darwin_find_context(private_interceptor,
                                           function_address,
                                           listener);
    if (context != NULL) {
        len = context->overwritten_prologue_len;
        memcpy(original_prologue, context->overwritten_prologue, len);
        if (peak_gum_darwin_stage.context == context) {
            memcpy(active_patch, peak_gum_darwin_stage.active, len);
        } else {
            memcpy(active_patch, context->function_address, len);
        }
        *canonical_address_out = context->function_address;
        *prologue_len = len;
        found = TRUE;
    }
    g_rec_mutex_unlock(&private_interceptor->mutex);

    return found;
}

gboolean
peak_gum_darwin_get_canonical_address_exact(
    GumInterceptor* interceptor,
    gpointer function_address,
    GumInvocationListener* listener,
    gpointer* canonical_address_out)
{
    PeakGumInterceptor17* private_interceptor;
    PeakGumFunctionContext17* context;
    gboolean found = FALSE;

    if (interceptor == NULL || function_address == NULL || listener == NULL ||
        canonical_address_out == NULL) {
        return FALSE;
    }

    *canonical_address_out = NULL;
    private_interceptor = (PeakGumInterceptor17*)interceptor;

    g_rec_mutex_lock(&private_interceptor->mutex);
    context = peak_gum_darwin_find_context_exact(private_interceptor,
                                                 function_address,
                                                 listener);
    if (context != NULL) {
        *canonical_address_out = context->function_address;
        found = TRUE;
    }
    g_rec_mutex_unlock(&private_interceptor->mutex);

    return found;
}

/* A custom redirect forces Gum's ordinary allocation path: unlike its near
 * trampoline fallback this never installs a deflector in an existing image's
 * code cave while peers are running. Short entries are accepted only when a
 * single direct branch reaches the prepared trampoline. */
static GumRedirectWriteResult
peak_gum_darwin_write_staged_redirect(const GumRedirectWriteDetails* details,
                                      gpointer user_data)
{
    GumArm64Writer* writer = details->writer;
    guint* redirect_size = user_data;

    if (details->capacity >= 4 &&
        gum_arm64_writer_can_branch_directly_between(
            writer, writer->pc, GUM_ADDRESS(details->target))) {
        if (!gum_arm64_writer_put_b_imm(writer, GUM_ADDRESS(details->target))) {
            return GUM_REDIRECT_DECLINED;
        }
        *redirect_size = 4;
        return GUM_REDIRECT_WRITTEN;
    }
    if (details->capacity < 16) {
        peak_gum_darwin_stage_failure = "short-entry-unreachable-trampoline";
        return GUM_REDIRECT_DECLINED;
    }
    arm64_reg scratch = details->scratch_register;
    guint relocatable = 0;
    /* Prove the exact 16-byte prefix this absolute redirect overwrites. A
     * four-byte direct branch above requires no scratch register proof. */
    if (!gum_arm64_relocator_can_relocate(GSIZE_TO_POINTER(writer->pc),
            16, GUM_SCENARIO_ONLINE, GUM_RELOCATION_CHECKED,
            &relocatable, &scratch) || relocatable < 16) {
        peak_gum_darwin_stage_failure = "absolute-redirect-entry-too-short";
        return GUM_REDIRECT_DECLINED;
    }
    if (scratch == ARM64_REG_INVALID ||
        scratch != (arm64_reg)details->scratch_register) {
        peak_gum_darwin_stage_failure = "absolute-redirect-no-free-scratch-register";
        return GUM_REDIRECT_DECLINED;
    }
    if (!gum_arm64_writer_put_ldr_reg_address(writer,
            details->scratch_register, GUM_ADDRESS(details->target)) ||
        !gum_arm64_writer_put_br_reg(writer, details->scratch_register)) {
        return GUM_REDIRECT_DECLINED;
    }
    *redirect_size = 16;
    return GUM_REDIRECT_WRITTEN;
}

gboolean
peak_gum_darwin_staged_attach_supported(void)
{
    /* This unit is selected only for the hash-pinned Arm64 Darwin devkit. */
    return TRUE;
}

static gboolean
peak_gum_darwin_begin_stage(GumInterceptor* interceptor, gpointer address,
                            GumInvocationListener* listener)
{
    PeakGumInterceptor17* self = (PeakGumInterceptor17*)interceptor;

    peak_gum_darwin_stage_failure = "none";
    if (self == NULL || address == NULL ||
        peak_gum_darwin_stage.interceptor != NULL) {
        peak_gum_darwin_stage_failure = "invalid-or-pending-stage";
        return FALSE;
    }
    g_rec_mutex_lock(&self->mutex);
    /* Never consume another caller's transaction or pending code writes. */
    if (self->current_transaction.level != 0) {
        peak_gum_darwin_stage_failure = "gum-transaction-open";
    } else if (self->current_transaction.is_dirty) {
        peak_gum_darwin_stage_failure = "gum-transaction-dirty";
    } else if (g_hash_table_size(
            self->current_transaction.pending_update_tasks) != 0) {
        peak_gum_darwin_stage_failure = "gum-pending-entry-updates";
    } else if (g_hash_table_lookup(self->function_by_address,
            gum_strip_code_pointer(address)) != NULL) {
        peak_gum_darwin_stage_failure = "entry-already-instrumented";
    } else if (gum_process_get_code_signing_policy() == GUM_CODE_SIGNING_REQUIRED) {
        peak_gum_darwin_stage_failure = "code-signing-policy-required";
    }
    if (strcmp(peak_gum_darwin_stage_failure, "none") != 0) {
        g_rec_mutex_unlock(&self->mutex);
        return FALSE;
    }
    gum_ensure_code_readable(address, 16);
    guint32 first_instruction;
    memcpy(&first_instruction, gum_strip_code_pointer(address),
           sizeof(first_instruction));
    /* Gum 17.15.3's Darwin resolver follows a leading B or the
     * ADRP x17 / ADD / LDR / BRAA import stub. Conservatively reject
     * either prefix before Gum can touch a different context. */
    if ((first_instruction & 0xfc000000u) == 0x14000000u ||
        (first_instruction & 0x9f00001fu) == 0x90000011u) {
        peak_gum_darwin_stage_failure = "entry-redirect-prefix";
        g_rec_mutex_unlock(&self->mutex);
        return FALSE;
    }
    if (listener != NULL) {
        GHashTableIter iter;
        gpointer value;
        g_hash_table_iter_init(&iter, self->function_by_address);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            PeakGumFunctionContext17* context = value;
            GPtrArray* entries = (GPtrArray*)g_atomic_pointer_get(
                &context->listener_entries);
            for (guint i = 0; entries != NULL && i < entries->len; i++) {
                PeakGumListenerEntry17* entry = g_ptr_array_index(entries, i);
                if (entry != NULL && entry->listener_instance == listener) {
                    /* Rollback uses Gum's listener-wide detach, so it must
                     * own a listener that no published context references. */
                    peak_gum_darwin_stage_failure = "listener-already-published";
                    g_rec_mutex_unlock(&self->mutex);
                    return FALSE;
                }
            }
        }
    }
    gum_interceptor_begin_transaction(interceptor);
    return TRUE;
}

/* Called with our sole outer transaction open. Remove activation before
 * ending it, so Gum finalizes executable trampolines and listener metadata
 * without touching the target's entry. */
static gboolean
peak_gum_darwin_end_stage(GumInterceptor* interceptor, gpointer address,
                           GumInvocationListener* listener,
                           gboolean replacing, guint redirect_size,
                           gboolean operation_succeeded)
{
    PeakGumInterceptor17* self = (PeakGumInterceptor17*)interceptor;
    PeakGumFunctionContext17* context = g_hash_table_lookup(
        self->function_by_address, gum_strip_code_pointer(address));
    gboolean valid = operation_succeeded && context != NULL &&
        context->function_address == gum_strip_code_pointer(address) &&
        !context->activated && !context->destroyed &&
        context->grafted_hook == NULL && context->import_target == NULL &&
        context->trampoline_deflector == NULL &&
        context->redirect_code != NULL &&
        context->overwritten_prologue != NULL && redirect_size > 0 &&
        redirect_size <= context->overwritten_prologue_len &&
        context->overwritten_prologue_len <= PEAK_GUM_DARWIN_MAX_PROLOGUE_SIZE &&
        (replacing ? context->replacement_function != NULL :
            peak_gum_darwin_context_has_only_listener(context, listener));

    if (operation_succeeded && !valid) {
        if (context == NULL) {
            peak_gum_darwin_stage_failure = "prepared-context-not-exact";
        } else if (context->trampoline_deflector != NULL) {
            peak_gum_darwin_stage_failure = "prepared-deflector-unsupported";
        } else if (redirect_size == 0 ||
                   redirect_size > context->overwritten_prologue_len ||
                   context->overwritten_prologue_len > PEAK_GUM_DARWIN_MAX_PROLOGUE_SIZE) {
            peak_gum_darwin_stage_failure = "prepared-prologue-size-mismatch";
        } else {
            peak_gum_darwin_stage_failure = "prepared-context-shape-mismatch";
        }
    }
    /* The lock excludes other Gum writers, and begin_stage required an empty
     * transaction, so every queued update belongs to this operation. */
    g_hash_table_remove_all(self->current_transaction.pending_update_tasks);
    if (valid) {
        guint len = context->overwritten_prologue_len;
        peak_gum_darwin_stage = (PeakGumDarwinStage){
            .interceptor = interceptor, .context = context, .listener = listener
        };
        memcpy(peak_gum_darwin_stage.active, context->overwritten_prologue, len);
        memcpy(peak_gum_darwin_stage.active, context->redirect_code, redirect_size);
        /* Gum's invocation path expects an activated context. The entry is
         * still original; only the controller can publish the prepared bytes. */
        context->activated = TRUE;
    } else if (operation_succeeded) {
        /* Resolution may have followed a redirect to another entry. Since its
         * activation was removed above, detach/revert only frees metadata. */
        if (replacing) {
            gum_interceptor_revert(interceptor, address);
        } else {
            gum_interceptor_detach(interceptor, listener);
        }
    }
    gum_interceptor_end_transaction(interceptor);
    if (!valid) {
        g_rec_mutex_unlock(&self->mutex);
    }
    return valid;
}

GumAttachReturn
peak_gum_darwin_prepare_attach(GumInterceptor* interceptor,
                               gpointer function_address,
                               GumInvocationListener* listener,
                               const GumAttachOptions* options)
{
    GumAttachOptions staged_options = { 0 };
    GumAttachReturn result;
    guint redirect_size;

    if (listener == NULL ||
        (options != NULL && options->instrumentation.write_redirect != NULL)) {
        peak_gum_darwin_stage_failure = "invalid-listener-or-custom-redirect";
        return GUM_ATTACH_WRONG_TYPE;
    }
    if (!peak_gum_darwin_begin_stage(interceptor, function_address, listener)) {
        return GUM_ATTACH_WRONG_TYPE;
    }
    if (options != NULL) {
        staged_options = *options;
    }
    peak_gum_darwin_redirect_size = 0;
    staged_options.instrumentation.write_redirect =
        peak_gum_darwin_write_staged_redirect;
    staged_options.instrumentation.write_redirect_data =
        &peak_gum_darwin_redirect_size;
    staged_options.instrumentation.redirect_space_hint =
        PEAK_GUM_DARWIN_MAX_PROLOGUE_SIZE;
    result = gum_interceptor_attach(interceptor, function_address, listener,
                                    &staged_options);
    if (result != GUM_ATTACH_OK &&
        strcmp(peak_gum_darwin_stage_failure, "none") == 0) {
        peak_gum_darwin_stage_failure = "gum-attach-rejected";
    }
    redirect_size = peak_gum_darwin_redirect_size;
    if (!peak_gum_darwin_end_stage(interceptor, function_address, listener,
                                   FALSE, redirect_size,
                                   result == GUM_ATTACH_OK) &&
        result == GUM_ATTACH_OK) {
        result = GUM_ATTACH_WRONG_SIGNATURE;
    }
    return result;
}

GumReplaceReturn
peak_gum_darwin_prepare_replace(GumInterceptor* interceptor,
                                gpointer function_address,
                                gpointer replacement_function,
                                gpointer* original_function,
                                const GumReplaceOptions* options)
{
    GumReplaceOptions staged_options = { 0 };
    GumReplaceReturn result;
    guint redirect_size;

    if (original_function != NULL) {
        *original_function = NULL;
    }
    if (replacement_function == NULL ||
        (options != NULL && options->instrumentation.write_redirect != NULL) ||
        !peak_gum_darwin_begin_stage(interceptor, function_address, NULL)) {
        return GUM_REPLACE_WRONG_TYPE;
    }
    if (options != NULL) {
        staged_options = *options;
    }
    peak_gum_darwin_redirect_size = 0;
    staged_options.instrumentation.write_redirect =
        peak_gum_darwin_write_staged_redirect;
    staged_options.instrumentation.write_redirect_data =
        &peak_gum_darwin_redirect_size;
    staged_options.instrumentation.redirect_space_hint =
        PEAK_GUM_DARWIN_MAX_PROLOGUE_SIZE;
    result = gum_interceptor_replace(interceptor, function_address,
        replacement_function, original_function, &staged_options);
    if (result != GUM_REPLACE_OK &&
        strcmp(peak_gum_darwin_stage_failure, "none") == 0) {
        peak_gum_darwin_stage_failure = "gum-replace-rejected";
    }
    redirect_size = peak_gum_darwin_redirect_size;
    if (!peak_gum_darwin_end_stage(interceptor, function_address, NULL,
                                   TRUE, redirect_size,
                                   result == GUM_REPLACE_OK) &&
        result == GUM_REPLACE_OK) {
        result = GUM_REPLACE_WRONG_SIGNATURE;
    }
    if (result != GUM_REPLACE_OK && original_function != NULL) {
        *original_function = NULL;
    }
    return result;
}

gboolean
peak_gum_darwin_get_staged_patch(GumInterceptor* interceptor,
                                gpointer* canonical_address_out,
                                guint8* active_patch,
                                guint8* original_prologue,
                                guint* prologue_len)
{
    PeakGumDarwinStage* stage = &peak_gum_darwin_stage;
    PeakGumFunctionContext17* context = stage->context;
    if (stage->interceptor != interceptor || context == NULL ||
        canonical_address_out == NULL || active_patch == NULL ||
        original_prologue == NULL || prologue_len == NULL) {
        return FALSE;
    }
    *canonical_address_out = context->function_address;
    *prologue_len = context->overwritten_prologue_len;
    memcpy(active_patch, stage->active, *prologue_len);
    memcpy(original_prologue, context->overwritten_prologue, *prologue_len);
    return TRUE;
}

void
peak_gum_darwin_finish_staged_mutation(GumInterceptor* interceptor,
                                      gboolean committed)
{
    PeakGumDarwinStage stage = peak_gum_darwin_stage;
    if (stage.interceptor != interceptor || stage.context == NULL) {
        return;
    }
    peak_gum_darwin_stage = (PeakGumDarwinStage){ 0 };
    if (!committed) {
        /* Nothing can have entered this trampoline: no entry was published. */
        stage.context->activated = FALSE;
        if (stage.listener != NULL) {
            gum_interceptor_detach(interceptor, stage.listener);
        } else {
            gum_interceptor_revert(interceptor, stage.context->function_address);
        }
    }
    g_rec_mutex_unlock(&((PeakGumInterceptor17*)interceptor)->mutex);
}

void
peak_gum_darwin_finish_attach(GumInterceptor* interceptor,
                             GumInvocationListener* listener,
                             gboolean committed)
{
    if (peak_gum_darwin_stage.listener == listener) {
        peak_gum_darwin_finish_staged_mutation(interceptor, committed);
    }
}
