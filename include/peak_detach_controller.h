#ifndef __PEAK_DETACH_CONTROLLER_H
#define __PEAK_DETACH_CONTROLLER_H

#include "frida-gum.h"

#include <stddef.h>

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

gboolean peak_detach_controller_prepare_hook_mutation(
    const PeakDetachRequest* request,
    PeakDetachStatus* status_out);

gboolean peak_detach_controller_threads_are_held(void);

gboolean peak_detach_controller_current_mutation_uses_physical_patch(void);

gboolean peak_detach_controller_finish_hook_mutation(
    const PeakDetachRequest* request,
    PeakDetachStatus* status_out);

gboolean peak_detach_controller_shutdown_helper(PeakDetachStatus* status_out);

void peak_detach_controller_abort_after_failed_finish(
    const char* context,
    PeakDetachStatus status);

const char* peak_detach_controller_status_string(PeakDetachStatus status);
const char* peak_detach_controller_operation_string(PeakDetachOperation operation);

#endif /* __PEAK_DETACH_CONTROLLER_H */
