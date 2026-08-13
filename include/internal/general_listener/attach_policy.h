#ifndef PEAK_ATTACH_POLICY_H
#define PEAK_ATTACH_POLICY_H

/**
 * @file attach_policy.h
 * @brief Decide whether PEAK may attach Gum hooks to resolved targets.
 */

#include "frida-gum.h"

/** Initializes the process-wide attach policy from the environment once. */
void peak_general_listener_init_attach_policy(void);

/**
 * Returns whether a profiling target satisfies PEAK's Gum attach policy.
 *
 * The policy is initialized once from the process environment. Darwin Arm64
 * rejects targets that stock Gum would redirect to another entry because its
 * v1 backend has no exact-entry attach. That attribution check cannot be
 * overridden. Otherwise, an unsafe override bypasses the minimum-prologue and
 * relocation-safety checks.
 */
gboolean peak_general_listener_attach_target_is_supported(
    const char* symbol_name,
    gpointer address);

/** Returns whether an internal support replacement may defer to Gum. */
gboolean peak_general_listener_support_attach_target_is_supported(
    const char* symbol_name,
    gpointer address);

/**
 * Returns whether startup attach can avoid stopping peer threads.
 *
 * Failure to inspect `/proc/self/task` is conservative and returns FALSE.
 */
gboolean peak_general_listener_startup_attach_can_skip_stop(void);

#endif /* PEAK_ATTACH_POLICY_H */
