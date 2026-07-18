#ifndef PEAK_DETACH_CONTROLLER_H
#define PEAK_DETACH_CONTROLLER_H

#include "frida-gum.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_DETACH_CONTROLLER_TEST_API __attribute__((visibility("default")))
#else
#define PEAK_DETACH_CONTROLLER_TEST_API
#endif

typedef enum {
    PEAK_DETACH_OPERATION_ATTACH = 0,
    PEAK_DETACH_OPERATION_DETACH,
    PEAK_DETACH_OPERATION_REATTACH,
    PEAK_DETACH_OPERATION_SHUTDOWN,
    PEAK_DETACH_OPERATION_REPLACE,
    PEAK_DETACH_OPERATION_REVERT
} PeakDetachOperation;

typedef enum {
    PEAK_DETACH_STATUS_SAFE = 0,
    PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED,
    PEAK_DETACH_STATUS_DISABLED,
    PEAK_DETACH_STATUS_UNSUPPORTED,
    PEAK_DETACH_STATUS_MISSING_GUM_API,
    PEAK_DETACH_STATUS_PERMISSION_DENIED,
    PEAK_DETACH_STATUS_TIMEOUT,
    PEAK_DETACH_STATUS_CLASSIFY_FAILED,
    PEAK_DETACH_STATUS_ERROR
} PeakDetachStatus;

typedef struct {
    size_t hook_id;
    const char* symbol_name;
    gpointer function_address;
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    PeakDetachOperation operation;
    gpointer blocked_pc_start;
    size_t blocked_pc_size;
} PeakDetachRequest;

typedef struct {
    gboolean prepared;
    gboolean uses_physical_patch;
    PeakDetachStatus status;
} PeakDetachBatchResult;

typedef struct {
    uint64_t completed_stop_window_count;
    uint64_t failed_stop_window_count;
    uint64_t stop_window_wall_ns;
} PeakDetachAccountingSnapshot;

typedef struct {
    const char* reason;
    long tid;
    uintptr_t pc;
    uintptr_t aux;
} PeakDetachFailureDetail;

gboolean peak_detach_controller_prepare_hook_mutation(
    const PeakDetachRequest* request,
    PeakDetachStatus* status_out);

gboolean peak_detach_controller_strict_batch_supported(void);

/* Larger mutation sets must be split into chunks no larger than this value. */
size_t peak_detach_controller_max_batch_requests(void);

void peak_detach_controller_warmup_backend(void);

gboolean peak_detach_controller_prepare_hook_mutation_batch(
    const PeakDetachRequest* requests,
    size_t request_count,
    PeakDetachBatchResult* results,
    size_t* prepared_count_out,
    PeakDetachStatus* status_out);

gboolean peak_detach_controller_threads_are_held(void);

gboolean peak_detach_controller_current_mutation_uses_physical_patch(void);

double peak_detach_controller_last_stop_window_us(void);

gboolean peak_detach_controller_accounting_snapshot(
    PeakDetachAccountingSnapshot* out);

const PeakDetachFailureDetail*
peak_detach_controller_last_failure_detail(void);

void peak_detach_controller_wait_for_mutation_window(void);

void peak_detach_controller_note_thread_creation_gate_installed(
    gboolean installed);

void peak_detach_controller_configure_trace_diagnostics(gboolean enabled);

#ifdef PEAK_ENABLE_TEST_HOOKS
PEAK_DETACH_CONTROLLER_TEST_API int
peak_detach_controller_test_thread_creation_gate_epoch(void);

PEAK_DETACH_CONTROLLER_TEST_API size_t
peak_detach_controller_test_gate_waiter_count(void);

PEAK_DETACH_CONTROLLER_TEST_API int
peak_detach_controller_test_signal_backend_signum(void);

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
PEAK_DETACH_CONTROLLER_TEST_API void
peak_detach_controller_test_accounting_begin_publish(void);

PEAK_DETACH_CONTROLLER_TEST_API void
peak_detach_controller_test_accounting_end_publish(void);

PEAK_DETACH_CONTROLLER_TEST_API void
peak_detach_controller_test_accounting_update_tuple(
    unsigned long long elapsed_ns,
    gboolean completed);
#endif
#endif

gboolean peak_detach_controller_finish_hook_mutation(
    const PeakDetachRequest* request,
    PeakDetachStatus* status_out);

gboolean peak_detach_controller_finish_hook_mutation_batch(
    PeakDetachStatus* status_out);

PEAK_DETACH_CONTROLLER_TEST_API gboolean
peak_detach_controller_shutdown_helper(PeakDetachStatus* status_out);

void peak_detach_controller_abort_after_failed_finish(
    const char* context,
    PeakDetachStatus status);

const char* peak_detach_controller_status_string(PeakDetachStatus status);
const char* peak_detach_controller_operation_string(PeakDetachOperation operation);

#endif /* PEAK_DETACH_CONTROLLER_H */
