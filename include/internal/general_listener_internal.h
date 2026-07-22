#ifndef PEAK_GENERAL_LISTENER_INTERNAL_H
#define PEAK_GENERAL_LISTENER_INTERNAL_H

/**
 * @file general_listener_internal.h
 * @brief Internal entry points for listener startup and dynamic attachment.
 */

#include "general_listener.h"

typedef enum {
    PEAK_DYNAMIC_ATTACH_ATTACHED = 0,
    PEAK_DYNAMIC_ATTACH_NO_MATCH,
    PEAK_DYNAMIC_ATTACH_RETRY,
    PEAK_DYNAMIC_ATTACH_FAILED
} PeakDynamicAttachResult;

PeakDynamicAttachResult peak_general_listener_dynamic_attach_symbol(
    const char* symbol_name,
    gpointer symbol_address,
    gsize symbol_size,
    const char* provider_name);

gboolean peak_general_listener_dynamic_symbol_matches_any_target(
    const char* symbol_name,
    const char* provider_name);

gboolean peak_general_listener_checkpoint_for_exec(
    unsigned long long checkpoint_index);

/**
 * Emits one frozen report under an explicit active-MPI-job output policy.
 *
 * An active MPI job requires launcher metadata for any no-more-MPI socket
 * path and host-disambiguates rank-local fallback when that metadata is
 * unavailable. Non-MPI callers use the public wrapper and preserve legacy
 * single-process naming.
 */
gboolean peak_general_listener_print_with_mpi_job_policy(
    PeakOutputAggregationMode aggregation_mode,
    gboolean active_mpi_job,
    gboolean* report_write_succeeded);

/* Starts target lifecycle processing after startup-only Gum hooks are ready. */
void peak_general_listener_controller_start(void);

#endif /* PEAK_GENERAL_LISTENER_INTERNAL_H */
