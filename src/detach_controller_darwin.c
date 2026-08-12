#include "detach_controller.h"
#include "internal/gum_peak_darwin_patch_api.h"

#include <libkern/OSCacheControl.h>
#include <mach/arm/thread_status.h>
#include <mach/mach.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if !defined(__APPLE__) || \
    !(defined(__aarch64__) || defined(__arm64__))
#error "The PEAK Darwin detach controller only supports macOS Arm64"
#endif

#define PEAK_DARWIN_MAX_THREADS 4096u
#define PEAK_DARWIN_MAX_PATCH_RECORDS 8192u
#define PEAK_DARWIN_GATE_WAIT_US 100u
#define PEAK_DARWIN_ACCOUNTING_ATTEMPTS 64u

#if !defined(__DARWIN_OPAQUE_ARM_THREAD_STATE64)
#define __darwin_arm_thread_state64_get_pc_fptr(state) \
    ((void*)(uintptr_t)((state).__pc))
#endif

typedef struct {
    gboolean used;
    size_t hook_id;
    gpointer function_address;
    guint8 active_patch[PEAK_GUM_DARWIN_MAX_PROLOGUE_SIZE];
    guint8 original_prologue[PEAK_GUM_DARWIN_MAX_PROLOGUE_SIZE];
    guint patch_size;
} PeakDarwinPatchRecord;

typedef struct {
    gboolean active;
    gboolean uses_physical_patch;
    size_t hook_id;
    PeakDetachOperation operation;
    thread_act_t threads[PEAK_DARWIN_MAX_THREADS];
    mach_msg_type_number_t thread_count;
    gpointer writable_pages;
    guint writable_page_count;
    gsize writable_patch_offset;
    struct timespec started_at;
} PeakDarwinHeldMutation;

typedef struct {
    gpointer function_address;
    guint8 active_patch[PEAK_GUM_DARWIN_MAX_PROLOGUE_SIZE];
    guint8 original_prologue[PEAK_GUM_DARWIN_MAX_PROLOGUE_SIZE];
    guint patch_size;
    const guint8* bytes_to_write;
    const guint8* expected_bytes;
    PeakDarwinPatchRecord* record;
} PeakDarwinPatchPlan;

static pthread_mutex_t peak_darwin_mutation_mutex = PTHREAD_MUTEX_INITIALIZER;
static PeakDarwinHeldMutation peak_darwin_held;
static PeakDarwinPatchRecord
    peak_darwin_patch_records[PEAK_DARWIN_MAX_PATCH_RECORDS];
static _Atomic int peak_darwin_thread_gate = 0;
static _Atomic int peak_darwin_thread_gate_epoch = 1;
static _Atomic int peak_darwin_thread_gate_installed = 0;
static _Atomic size_t peak_darwin_gate_waiters = 0;
static _Atomic int peak_darwin_trace_diagnostics = 0;
static _Atomic unsigned long long peak_darwin_completed_windows = 0;
static _Atomic unsigned long long peak_darwin_failed_windows = 0;
static _Atomic unsigned long long peak_darwin_window_wall_ns = 0;
static _Atomic unsigned long long peak_darwin_accounting_sequence = 0;
static double peak_darwin_last_stop_window_us;
static __thread PeakDetachFailureDetail peak_darwin_failure = {
    "none", 0, 0, 0
};

static void
peak_darwin_set_status(PeakDetachStatus* status_out,
                       PeakDetachStatus status)
{
    if (status_out != NULL) {
        *status_out = status;
    }
}

static void
peak_darwin_clear_failure(void)
{
    peak_darwin_failure = (PeakDetachFailureDetail){ "none", 0, 0, 0 };
}

static void
peak_darwin_note_failure(const char* reason,
                         long tid,
                         uintptr_t pc,
                         uintptr_t aux)
{
    peak_darwin_failure.reason = reason != NULL ? reason : "unknown";
    peak_darwin_failure.tid = tid;
    peak_darwin_failure.pc = pc;
    peak_darwin_failure.aux = aux;
}

static gboolean
peak_darwin_operation_is_valid(PeakDetachOperation operation)
{
    return operation >= PEAK_DETACH_OPERATION_ATTACH &&
           operation <= PEAK_DETACH_OPERATION_REVERT;
}

static gboolean
peak_darwin_validate_request(const PeakDetachRequest* request,
                             PeakDetachStatus* status_out)
{
    if (request == NULL || request->interceptor == NULL ||
        request->function_address == NULL ||
        !peak_darwin_operation_is_valid(request->operation) ||
        (request->listener == NULL &&
         request->operation != PEAK_DETACH_OPERATION_REPLACE &&
         request->operation != PEAK_DETACH_OPERATION_REVERT)) {
        peak_darwin_set_status(status_out, PEAK_DETACH_STATUS_ERROR);
        return FALSE;
    }

    return TRUE;
}

static PeakDarwinPatchRecord*
peak_darwin_find_patch_record(size_t hook_id, gboolean create)
{
    PeakDarwinPatchRecord* free_record = NULL;

    for (size_t i = 0; i < PEAK_DARWIN_MAX_PATCH_RECORDS; i++) {
        PeakDarwinPatchRecord* record = &peak_darwin_patch_records[i];

        if (record->used && record->hook_id == hook_id) {
            return record;
        }
        if (!record->used && free_record == NULL) {
            free_record = record;
        }
    }

    if (!create || free_record == NULL) {
        return NULL;
    }

    *free_record = (PeakDarwinPatchRecord){ 0 };
    free_record->used = TRUE;
    free_record->hook_id = hook_id;
    return free_record;
}

static unsigned long long
peak_darwin_timespec_delta_ns(const struct timespec* start,
                              const struct timespec* end)
{
    time_t seconds;
    long nanoseconds;

    if (start == NULL || end == NULL ||
        (end->tv_sec < start->tv_sec) ||
        (end->tv_sec == start->tv_sec && end->tv_nsec < start->tv_nsec)) {
        return 0;
    }

    seconds = end->tv_sec - start->tv_sec;
    nanoseconds = end->tv_nsec - start->tv_nsec;
    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1000000000L;
    }
    if ((unsigned long long)seconds >
        (ULLONG_MAX - (unsigned long long)nanoseconds) / 1000000000ULL) {
        return ULLONG_MAX - 1;
    }
    return (unsigned long long)seconds * 1000000000ULL +
           (unsigned long long)nanoseconds;
}

static void
peak_darwin_accounting_add_saturated(_Atomic unsigned long long* counter,
                                     unsigned long long delta)
{
    const unsigned long long maximum = ULLONG_MAX - 1;
    unsigned long long current = atomic_load_explicit(counter,
                                                      memory_order_seq_cst);

    for (;;) {
        unsigned long long next =
            delta > maximum - current ? maximum : current + delta;

        if (atomic_compare_exchange_weak_explicit(counter,
                                                  &current,
                                                  next,
                                                  memory_order_seq_cst,
                                                  memory_order_seq_cst)) {
            return;
        }
    }
}

static void
peak_darwin_publish_window(gboolean completed,
                           const struct timespec* started_at)
{
    struct timespec finished_at = { 0 };
    unsigned long long elapsed_ns = 0;

    if (started_at != NULL && started_at->tv_sec != 0 &&
        clock_gettime(CLOCK_MONOTONIC, &finished_at) == 0) {
        elapsed_ns = peak_darwin_timespec_delta_ns(started_at, &finished_at);
    }

    atomic_fetch_add_explicit(&peak_darwin_accounting_sequence,
                              1,
                              memory_order_seq_cst);
    peak_darwin_accounting_add_saturated(&peak_darwin_window_wall_ns,
                                         elapsed_ns);
    peak_darwin_accounting_add_saturated(
        completed ? &peak_darwin_completed_windows :
                    &peak_darwin_failed_windows,
        1);
    atomic_fetch_add_explicit(&peak_darwin_accounting_sequence,
                              1,
                              memory_order_seq_cst);

    if (completed) {
        peak_darwin_last_stop_window_us = (double)elapsed_ns / 1000.0;
    }
}

static int
peak_darwin_begin_thread_gate(void)
{
    int epoch = atomic_fetch_add_explicit(&peak_darwin_thread_gate_epoch,
                                          1,
                                          memory_order_acq_rel) + 1;

    if (epoch <= 0) {
        atomic_store_explicit(&peak_darwin_thread_gate_epoch,
                              1,
                              memory_order_release);
        epoch = 1;
    }
    atomic_store_explicit(&peak_darwin_thread_gate,
                          epoch,
                          memory_order_release);
    return epoch;
}

static void
peak_darwin_end_thread_gate(void)
{
    atomic_store_explicit(&peak_darwin_thread_gate, 0, memory_order_release);
}

static gboolean
peak_darwin_thread_is_held(thread_act_t thread)
{
    for (mach_msg_type_number_t i = 0;
         i < peak_darwin_held.thread_count;
         i++) {
        if (peak_darwin_held.threads[i] == thread) {
            return TRUE;
        }
    }
    return FALSE;
}

static void
peak_darwin_release_snapshot(thread_act_array_t threads,
                             mach_msg_type_number_t count)
{
    if (threads == NULL) {
        return;
    }

    for (mach_msg_type_number_t i = 0; i < count; i++) {
        if (MACH_PORT_VALID(threads[i])) {
            (void)mach_port_deallocate(mach_task_self(), threads[i]);
        }
    }
    (void)vm_deallocate(mach_task_self(),
                        (vm_address_t)threads,
                        (vm_size_t)count * sizeof(thread_act_t));
}

static gboolean
peak_darwin_suspend_snapshot(thread_act_t controller_thread,
                             PeakDetachStatus* status_out)
{
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t count = 0;
    kern_return_t kr = task_threads(mach_task_self(), &threads, &count);

    if (kr != KERN_SUCCESS) {
        peak_darwin_note_failure("task-threads-failed", 0, 0, (uintptr_t)kr);
        peak_darwin_set_status(status_out, PEAK_DETACH_STATUS_ERROR);
        return FALSE;
    }
    if (count > PEAK_DARWIN_MAX_THREADS) {
        peak_darwin_note_failure("thread-capacity-exceeded",
                                 0,
                                 0,
                                 (uintptr_t)count);
        peak_darwin_release_snapshot(threads, count);
        peak_darwin_set_status(status_out,
                               PEAK_DETACH_STATUS_CLASSIFY_FAILED);
        return FALSE;
    }

    for (mach_msg_type_number_t i = 0; i < count; i++) {
        thread_act_t thread = threads[i];

        if (thread == controller_thread || peak_darwin_thread_is_held(thread)) {
            (void)mach_port_deallocate(mach_task_self(), thread);
            threads[i] = MACH_PORT_NULL;
            continue;
        }
        if (peak_darwin_held.thread_count >= PEAK_DARWIN_MAX_THREADS) {
            peak_darwin_note_failure("thread-capacity-race",
                                     0,
                                     0,
                                     (uintptr_t)peak_darwin_held.thread_count);
            peak_darwin_release_snapshot(threads, count);
            peak_darwin_set_status(status_out,
                                   PEAK_DETACH_STATUS_CLASSIFY_FAILED);
            return FALSE;
        }

        kr = thread_suspend(thread);
        if (kr == KERN_INVALID_ARGUMENT) {
            (void)mach_port_deallocate(mach_task_self(), thread);
            threads[i] = MACH_PORT_NULL;
            continue;
        }
        if (kr != KERN_SUCCESS) {
            peak_darwin_note_failure("thread-suspend-failed",
                                     (long)thread,
                                     0,
                                     (uintptr_t)kr);
            peak_darwin_release_snapshot(threads, count);
            peak_darwin_set_status(status_out, PEAK_DETACH_STATUS_ERROR);
            return FALSE;
        }

        peak_darwin_held.threads[peak_darwin_held.thread_count++] = thread;
        threads[i] = MACH_PORT_NULL;
    }

    (void)vm_deallocate(mach_task_self(),
                        (vm_address_t)threads,
                        (vm_size_t)count * sizeof(thread_act_t));
    return TRUE;
}

static gboolean
peak_darwin_threads_avoid_patch_interior(const PeakDarwinPatchPlan* plan,
                                         PeakDetachStatus* status_out)
{
    uintptr_t begin = (uintptr_t)plan->function_address;
    uintptr_t end = begin + (uintptr_t)plan->patch_size;

    if (end < begin) {
        peak_darwin_note_failure("patch-range-overflow", 0, begin, end);
        peak_darwin_set_status(status_out,
                               PEAK_DETACH_STATUS_CLASSIFY_FAILED);
        return FALSE;
    }

    for (mach_msg_type_number_t i = 0;
         i < peak_darwin_held.thread_count;
         i++) {
        arm_thread_state64_t state;
        mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
        thread_act_t thread = peak_darwin_held.threads[i];
        kern_return_t kr = thread_get_state(thread,
                                            ARM_THREAD_STATE64,
                                            (thread_state_t)&state,
                                            &state_count);
        uintptr_t pc;

        if (kr != KERN_SUCCESS) {
            peak_darwin_note_failure("thread-state-failed",
                                     (long)thread,
                                     0,
                                     (uintptr_t)kr);
            peak_darwin_set_status(status_out,
                                   PEAK_DETACH_STATUS_CLASSIFY_FAILED);
            return FALSE;
        }

        pc = (uintptr_t)__darwin_arm_thread_state64_get_pc_fptr(state);
        if (pc > begin && pc < end) {
            peak_darwin_note_failure("patch-interior-pc",
                                     (long)thread,
                                     pc,
                                     (uintptr_t)plan->patch_size);
            peak_darwin_set_status(status_out,
                                   PEAK_DETACH_STATUS_CLASSIFY_FAILED);
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean
peak_darwin_resume_threads(PeakDetachStatus* status_out)
{
    gboolean resumed = TRUE;

    for (mach_msg_type_number_t i = peak_darwin_held.thread_count;
         i > 0;
         i--) {
        thread_act_t thread = peak_darwin_held.threads[i - 1];
        kern_return_t kr = thread_resume(thread);

        if (kr != KERN_SUCCESS && kr != KERN_INVALID_ARGUMENT) {
            peak_darwin_note_failure("thread-resume-failed",
                                     (long)thread,
                                     0,
                                     (uintptr_t)kr);
            resumed = FALSE;
        }
        (void)mach_port_deallocate(mach_task_self(), thread);
        peak_darwin_held.threads[i - 1] = MACH_PORT_NULL;
    }
    peak_darwin_held.thread_count = 0;

    if (!resumed) {
        peak_darwin_set_status(status_out, PEAK_DETACH_STATUS_ERROR);
    }
    return resumed;
}

static void
peak_darwin_dispose_writable_pages(void)
{
    if (peak_darwin_held.writable_pages != NULL) {
        gum_memory_dispose_writable_pages(
            peak_darwin_held.writable_pages,
            peak_darwin_held.writable_page_count);
        peak_darwin_held.writable_pages = NULL;
        peak_darwin_held.writable_page_count = 0;
    }
}

static void
peak_darwin_cancel_stop_window(void)
{
    PeakDetachStatus resume_status = PEAK_DETACH_STATUS_ERROR;

    if (!peak_darwin_resume_threads(&resume_status)) {
        peak_detach_controller_abort_after_failed_finish(
            "Darwin STOP cancellation",
            resume_status);
    }

    peak_darwin_dispose_writable_pages();
    peak_darwin_end_thread_gate();
    peak_darwin_publish_window(FALSE, &peak_darwin_held.started_at);
    peak_darwin_held = (PeakDarwinHeldMutation){ 0 };
}

static gboolean
peak_darwin_prepare_writable_alias(const PeakDarwinPatchPlan* plan,
                                   PeakDetachStatus* status_out)
{
    gsize page_size = gum_query_page_size();
    uintptr_t address = (uintptr_t)plan->function_address;
    uintptr_t first_page;
    uintptr_t end;
    guint page_count;

    if (page_size == 0 || (page_size & (page_size - 1)) != 0 ||
        address > UINTPTR_MAX - plan->patch_size) {
        peak_darwin_note_failure("writable-range-invalid",
                                 0,
                                 address,
                                 plan->patch_size);
        peak_darwin_set_status(status_out, PEAK_DETACH_STATUS_ERROR);
        return FALSE;
    }

    first_page = address & ~((uintptr_t)page_size - 1);
    end = address + plan->patch_size;
    page_count = (guint)((end - first_page + page_size - 1) / page_size);
    peak_darwin_held.writable_pages =
        gum_memory_try_remap_writable_pages((gpointer)first_page, page_count);
    if (peak_darwin_held.writable_pages == NULL) {
        peak_darwin_note_failure("writable-remap-failed",
                                 0,
                                 address,
                                 page_count);
        peak_darwin_set_status(status_out, PEAK_DETACH_STATUS_ERROR);
        return FALSE;
    }
    peak_darwin_held.writable_page_count = page_count;
    peak_darwin_held.writable_patch_offset = address - first_page;
    return TRUE;
}

static gboolean
peak_darwin_bytes_equal(const guint8* left,
                        const guint8* right,
                        guint size)
{
    for (guint i = 0; i < size; i++) {
        if (left[i] != right[i]) {
            return FALSE;
        }
    }
    return TRUE;
}

static void
peak_darwin_write_patch(const PeakDarwinPatchPlan* plan)
{
    guint8* writable =
        (guint8*)peak_darwin_held.writable_pages +
        peak_darwin_held.writable_patch_offset;

    for (guint i = 0; i < plan->patch_size; i++) {
        writable[i] = plan->bytes_to_write[i];
    }
    atomic_thread_fence(memory_order_seq_cst);
    sys_dcache_flush(plan->function_address, plan->patch_size);
    sys_icache_invalidate(plan->function_address, plan->patch_size);
}

static gboolean
peak_darwin_build_patch_plan(const PeakDetachRequest* request,
                             PeakDarwinPatchPlan* plan,
                             PeakDetachStatus* status_out)
{
    *plan = (PeakDarwinPatchPlan){ 0 };

    if (request->operation == PEAK_DETACH_OPERATION_DETACH) {
        if (!peak_gum_darwin_get_function_patch(
                request->interceptor,
                request->function_address,
                request->listener,
                &plan->function_address,
                plan->active_patch,
                plan->original_prologue,
                &plan->patch_size)) {
            peak_darwin_note_failure("gum-patch-missing",
                                     0,
                                     (uintptr_t)request->function_address,
                                     request->operation);
            peak_darwin_set_status(status_out,
                                   PEAK_DETACH_STATUS_CLASSIFY_FAILED);
            return FALSE;
        }
        plan->bytes_to_write = plan->original_prologue;
        plan->expected_bytes = plan->active_patch;
        plan->record = peak_darwin_find_patch_record(request->hook_id, TRUE);
        if (plan->record == NULL) {
            peak_darwin_note_failure("patch-record-capacity", 0, 0, 0);
            peak_darwin_set_status(status_out, PEAK_DETACH_STATUS_ERROR);
            return FALSE;
        }
        plan->record->function_address = plan->function_address;
        plan->record->patch_size = plan->patch_size;
        memcpy(plan->record->active_patch,
               plan->active_patch,
               plan->patch_size);
        memcpy(plan->record->original_prologue,
               plan->original_prologue,
               plan->patch_size);
        return TRUE;
    }

    PeakDarwinPatchRecord* record =
        peak_darwin_find_patch_record(request->hook_id, FALSE);
    if (record == NULL || record->patch_size == 0 ||
        record->patch_size > PEAK_GUM_DARWIN_MAX_PROLOGUE_SIZE) {
        peak_darwin_note_failure("reattach-patch-record-missing",
                                 0,
                                 (uintptr_t)request->function_address,
                                 record != NULL ? record->patch_size : 0);
        peak_darwin_set_status(status_out,
                               PEAK_DETACH_STATUS_CLASSIFY_FAILED);
        return FALSE;
    }

    gpointer current_function_address = NULL;
    if (!peak_gum_darwin_get_canonical_address_exact(
            request->interceptor,
            request->function_address,
            request->listener,
            &current_function_address)) {
        peak_darwin_note_failure("reattach-patch-context-missing",
                                 0,
                                 (uintptr_t)record->function_address,
                                 (uintptr_t)request->function_address);
        peak_darwin_set_status(status_out,
                               PEAK_DETACH_STATUS_CLASSIFY_FAILED);
        return FALSE;
    }
    if (current_function_address != record->function_address) {
        peak_darwin_note_failure("reattach-patch-address-mismatch",
                                 0,
                                 (uintptr_t)record->function_address,
                                 (uintptr_t)current_function_address);
        peak_darwin_set_status(status_out,
                               PEAK_DETACH_STATUS_CLASSIFY_FAILED);
        return FALSE;
    }

    plan->function_address = record->function_address;
    plan->patch_size = record->patch_size;
    memcpy(plan->active_patch, record->active_patch, record->patch_size);
    memcpy(plan->original_prologue,
           record->original_prologue,
           record->patch_size);
    plan->bytes_to_write = plan->active_patch;
    plan->expected_bytes = plan->original_prologue;
    plan->record = record;
    return TRUE;
}

gboolean
peak_detach_controller_prepare_hook_mutation(const PeakDetachRequest* request,
                                             PeakDetachStatus* status_out)
{
    PeakDarwinPatchPlan plan;
    thread_act_t controller_thread = MACH_PORT_NULL;
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    gboolean prepared = FALSE;

    peak_darwin_clear_failure();
    if (!peak_darwin_validate_request(request, status_out)) {
        return FALSE;
    }

    pthread_mutex_lock(&peak_darwin_mutation_mutex);
    if (peak_darwin_held.active) {
        peak_darwin_note_failure("mutation-already-active", 0, 0, 0);
        peak_darwin_set_status(status_out, PEAK_DETACH_STATUS_ERROR);
        pthread_mutex_unlock(&peak_darwin_mutation_mutex);
        return FALSE;
    }

    if (request->operation == PEAK_DETACH_OPERATION_SHUTDOWN) {
        peak_darwin_note_failure("shutdown-gum-teardown-disabled",
                                 0,
                                 (uintptr_t)request->function_address,
                                 request->operation);
        peak_darwin_set_status(status_out, PEAK_DETACH_STATUS_UNSUPPORTED);
        pthread_mutex_unlock(&peak_darwin_mutation_mutex);
        return FALSE;
    }

    if (request->operation != PEAK_DETACH_OPERATION_DETACH &&
        request->operation != PEAK_DETACH_OPERATION_REATTACH) {
        peak_darwin_note_failure("runtime-gum-mutation-disabled",
                                 0,
                                 (uintptr_t)request->function_address,
                                 request->operation);
        peak_darwin_set_status(status_out, PEAK_DETACH_STATUS_UNSUPPORTED);
        pthread_mutex_unlock(&peak_darwin_mutation_mutex);
        return FALSE;
    }

    if (atomic_load_explicit(&peak_darwin_thread_gate_installed,
                             memory_order_acquire) == 0) {
        peak_darwin_note_failure("thread-creation-gate-unavailable",
                                 0,
                                 (uintptr_t)request->function_address,
                                 request->operation);
        peak_darwin_set_status(status_out, PEAK_DETACH_STATUS_UNSUPPORTED);
        pthread_mutex_unlock(&peak_darwin_mutation_mutex);
        return FALSE;
    }

    if (!peak_darwin_build_patch_plan(request, &plan, &status)) {
        peak_darwin_set_status(status_out, status);
        pthread_mutex_unlock(&peak_darwin_mutation_mutex);
        return FALSE;
    }

    peak_darwin_held = (PeakDarwinHeldMutation){
        .uses_physical_patch = TRUE,
        .hook_id = request->hook_id,
        .operation = request->operation
    };
    if (!peak_darwin_prepare_writable_alias(&plan, &status)) {
        peak_darwin_held = (PeakDarwinHeldMutation){ 0 };
        peak_darwin_set_status(status_out, status);
        pthread_mutex_unlock(&peak_darwin_mutation_mutex);
        return FALSE;
    }

    (void)peak_darwin_begin_thread_gate();
    (void)clock_gettime(CLOCK_MONOTONIC, &peak_darwin_held.started_at);
    controller_thread = mach_thread_self();
    if (!MACH_PORT_VALID(controller_thread)) {
        peak_darwin_note_failure("controller-thread-port-failed", 0, 0, 0);
        status = PEAK_DETACH_STATUS_ERROR;
        goto cancel;
    }

    /*
     * The creation gate stops new pthread_create calls.  Enumerating again
     * after the first suspension pass closes the race with a creator that had
     * already crossed the gate before it was published.
     */
    if (!peak_darwin_suspend_snapshot(controller_thread, &status) ||
        !peak_darwin_suspend_snapshot(controller_thread, &status) ||
        !peak_darwin_threads_avoid_patch_interior(&plan, &status)) {
        goto cancel;
    }

    if (!peak_darwin_bytes_equal(plan.function_address,
                                 plan.expected_bytes,
                                 plan.patch_size)) {
        peak_darwin_note_failure("entry-bytes-changed",
                                 0,
                                 (uintptr_t)plan.function_address,
                                 plan.patch_size);
        status = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
        goto cancel;
    }

    peak_darwin_write_patch(&plan);

    peak_darwin_held.active = TRUE;
    prepared = TRUE;
    status = PEAK_DETACH_STATUS_SAFE;
    goto beach;

cancel:
    peak_darwin_cancel_stop_window();

beach:
    if (MACH_PORT_VALID(controller_thread)) {
        (void)mach_port_deallocate(mach_task_self(), controller_thread);
    }
    peak_darwin_set_status(status_out, status);
    pthread_mutex_unlock(&peak_darwin_mutation_mutex);
    return prepared;
}

gboolean
peak_detach_controller_strict_batch_supported(void)
{
    return FALSE;
}

size_t
peak_detach_controller_max_batch_requests(void)
{
    return 1;
}

void
peak_detach_controller_warmup_backend(void)
{
}

void
peak_detach_controller_configure_mpi_process(gboolean is_mpi_process)
{
    (void)is_mpi_process;
}

gboolean
peak_detach_controller_prepare_hook_mutation_batch(
    const PeakDetachRequest* requests,
    size_t request_count,
    PeakDetachBatchResult* results,
    size_t* prepared_count_out,
    PeakDetachStatus* status_out)
{
    (void)requests;

    if (results != NULL) {
        for (size_t i = 0; i < request_count; i++) {
            results[i] = (PeakDetachBatchResult){
                .prepared = FALSE,
                .uses_physical_patch = FALSE,
                .status = PEAK_DETACH_STATUS_UNSUPPORTED
            };
        }
    }
    if (prepared_count_out != NULL) {
        *prepared_count_out = 0;
    }
    peak_darwin_set_status(status_out, PEAK_DETACH_STATUS_UNSUPPORTED);
    return FALSE;
}

gboolean
peak_detach_controller_threads_are_held(void)
{
    gboolean active;

    pthread_mutex_lock(&peak_darwin_mutation_mutex);
    active = peak_darwin_held.active;
    pthread_mutex_unlock(&peak_darwin_mutation_mutex);
    return active;
}

gboolean
peak_detach_controller_current_mutation_uses_physical_patch(void)
{
    gboolean physical;

    pthread_mutex_lock(&peak_darwin_mutation_mutex);
    physical = peak_darwin_held.active &&
               peak_darwin_held.uses_physical_patch;
    pthread_mutex_unlock(&peak_darwin_mutation_mutex);
    return physical;
}

double
peak_detach_controller_last_stop_window_us(void)
{
    double value;

    pthread_mutex_lock(&peak_darwin_mutation_mutex);
    value = peak_darwin_last_stop_window_us;
    pthread_mutex_unlock(&peak_darwin_mutation_mutex);
    return value;
}

gboolean
peak_detach_controller_accounting_snapshot(PeakDetachAccountingSnapshot* out)
{
    if (out == NULL) {
        return FALSE;
    }

    for (unsigned int attempt = 0;
         attempt < PEAK_DARWIN_ACCOUNTING_ATTEMPTS;
         attempt++) {
        unsigned long long before =
            atomic_load_explicit(&peak_darwin_accounting_sequence,
                                 memory_order_seq_cst);
        PeakDetachAccountingSnapshot snapshot;
        unsigned long long after;

        if ((before & 1u) != 0) {
            continue;
        }
        snapshot.completed_stop_window_count =
            atomic_load_explicit(&peak_darwin_completed_windows,
                                 memory_order_seq_cst);
        snapshot.failed_stop_window_count =
            atomic_load_explicit(&peak_darwin_failed_windows,
                                 memory_order_seq_cst);
        snapshot.stop_window_wall_ns =
            atomic_load_explicit(&peak_darwin_window_wall_ns,
                                 memory_order_seq_cst);
        after = atomic_load_explicit(&peak_darwin_accounting_sequence,
                                     memory_order_seq_cst);
        if (before == after && (after & 1u) == 0) {
            *out = snapshot;
            return TRUE;
        }
    }

    return FALSE;
}

const PeakDetachFailureDetail*
peak_detach_controller_last_failure_detail(void)
{
    return &peak_darwin_failure;
}

void
peak_detach_controller_wait_for_mutation_window(void)
{
    gboolean published = FALSE;

    while (atomic_load_explicit(&peak_darwin_thread_gate,
                                memory_order_acquire) != 0) {
        if (!published) {
            atomic_fetch_add_explicit(&peak_darwin_gate_waiters,
                                      1,
                                      memory_order_acq_rel);
            published = TRUE;
        }
        (void)usleep(PEAK_DARWIN_GATE_WAIT_US);
    }
    if (published) {
        atomic_fetch_sub_explicit(&peak_darwin_gate_waiters,
                                  1,
                                  memory_order_acq_rel);
    }
}

void
peak_detach_controller_note_thread_creation_gate_installed(gboolean installed)
{
    atomic_store_explicit(&peak_darwin_thread_gate_installed,
                          installed ? 1 : 0,
                          memory_order_release);
}

void
peak_detach_controller_configure_trace_diagnostics(gboolean enabled)
{
    atomic_store_explicit(&peak_darwin_trace_diagnostics,
                          enabled ? 1 : 0,
                          memory_order_release);
}

gboolean
peak_detach_controller_finish_hook_mutation(const PeakDetachRequest* request,
                                            PeakDetachStatus* status_out)
{
    PeakDetachStatus status = PEAK_DETACH_STATUS_SAFE;
    gboolean finished = TRUE;

    if (request != NULL && !peak_darwin_operation_is_valid(request->operation)) {
        peak_darwin_set_status(status_out, PEAK_DETACH_STATUS_ERROR);
        return FALSE;
    }

    pthread_mutex_lock(&peak_darwin_mutation_mutex);
    if (peak_darwin_held.active) {
        struct timespec started_at = peak_darwin_held.started_at;

        if (request != NULL &&
            (request->hook_id != peak_darwin_held.hook_id ||
             request->operation != peak_darwin_held.operation)) {
            peak_darwin_note_failure("finish-request-mismatch",
                                     0,
                                     request->hook_id,
                                     request->operation);
            status = PEAK_DETACH_STATUS_ERROR;
            finished = FALSE;
        }
        if (!peak_darwin_resume_threads(&status)) {
            finished = FALSE;
        }
        peak_darwin_dispose_writable_pages();
        peak_darwin_end_thread_gate();
        peak_darwin_held = (PeakDarwinHeldMutation){ 0 };
        peak_darwin_publish_window(finished, &started_at);
    } else if (request != NULL &&
               request->operation != PEAK_DETACH_OPERATION_DETACH &&
               request->operation != PEAK_DETACH_OPERATION_REATTACH) {
        peak_darwin_note_failure(
            request->operation == PEAK_DETACH_OPERATION_SHUTDOWN
                ? "shutdown-gum-teardown-disabled"
                : "runtime-gum-mutation-disabled",
            0,
            (uintptr_t)request->function_address,
            request->operation);
        status = PEAK_DETACH_STATUS_UNSUPPORTED;
        finished = FALSE;
    }
    peak_darwin_set_status(status_out, status);
    pthread_mutex_unlock(&peak_darwin_mutation_mutex);
    return finished;
}

gboolean
peak_detach_controller_finish_hook_mutation_batch(PeakDetachStatus* status_out)
{
    return peak_detach_controller_finish_hook_mutation(NULL, status_out);
}

gboolean
peak_detach_controller_shutdown_helper(PeakDetachStatus* status_out)
{
    pthread_mutex_lock(&peak_darwin_mutation_mutex);
    gboolean idle = !peak_darwin_held.active;
    pthread_mutex_unlock(&peak_darwin_mutation_mutex);

    peak_darwin_set_status(status_out,
                           idle ? PEAK_DETACH_STATUS_SAFE :
                                  PEAK_DETACH_STATUS_ERROR);
    return idle;
}

void
peak_detach_controller_abort_after_failed_finish(const char* context,
                                                 PeakDetachStatus status)
{
    static const char message[] =
        "[peak] fatal Darwin detach finish failure; terminating to avoid running with suspended threads\n";

    (void)context;
    (void)status;
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(128);
}

const char*
peak_detach_controller_status_string(PeakDetachStatus status)
{
    switch (status) {
        case PEAK_DETACH_STATUS_SAFE: return "safe";
        case PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED:
            return "compatibility-allowed";
        case PEAK_DETACH_STATUS_DISABLED: return "disabled";
        case PEAK_DETACH_STATUS_UNSUPPORTED: return "unsupported";
        case PEAK_DETACH_STATUS_MISSING_GUM_API: return "missing-gum-api";
        case PEAK_DETACH_STATUS_PERMISSION_DENIED: return "permission-denied";
        case PEAK_DETACH_STATUS_TIMEOUT: return "timeout";
        case PEAK_DETACH_STATUS_CLASSIFY_FAILED: return "classify-failed";
        case PEAK_DETACH_STATUS_ERROR: return "error";
        default: return "unknown";
    }
}

const char*
peak_detach_controller_operation_string(PeakDetachOperation operation)
{
    switch (operation) {
        case PEAK_DETACH_OPERATION_ATTACH: return "attach";
        case PEAK_DETACH_OPERATION_DETACH: return "detach";
        case PEAK_DETACH_OPERATION_REATTACH: return "reattach";
        case PEAK_DETACH_OPERATION_SHUTDOWN: return "shutdown";
        case PEAK_DETACH_OPERATION_REPLACE: return "replace";
        case PEAK_DETACH_OPERATION_REVERT: return "revert";
        default: return "unknown";
    }
}

#ifdef PEAK_ENABLE_TEST_HOOKS
int
peak_detach_controller_test_thread_creation_gate_epoch(void)
{
    return atomic_load_explicit(&peak_darwin_thread_gate,
                                memory_order_acquire);
}

size_t
peak_detach_controller_test_gate_waiter_count(void)
{
    return atomic_load_explicit(&peak_darwin_gate_waiters,
                                memory_order_acquire);
}

int
peak_detach_controller_test_signal_backend_signum(void)
{
    return 0;
}

gboolean
peak_detach_controller_test_replace_helper_env(const char* name,
                                               const char* value)
{
    (void)name;
    (void)value;
    return FALSE;
}
#endif
