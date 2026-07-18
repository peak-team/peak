#ifndef PEAK_PARENT_REGISTRY_H
#define PEAK_PARENT_REGISTRY_H

/**
 * @file parent_registry.h
 * @brief Per-user process registry for detecting an active PEAK parent.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register this PEAK process and check whether its direct parent is registered.
 *
 * The registry is updated while holding its file lock. Entries contain both a
 * PID and its Linux @c /proc start time, preventing a later process that reused
 * the PID from matching. Dead, malformed, legacy PID-only, and identity-
 * mismatched entries are pruned. The return value reports whether the direct
 * parent was already a live registered PEAK process. On a successful operation,
 * the current process is registered as part of the same update.
 *
 * @return 1 if the current direct parent is live and registered, 0 otherwise,
 *         or -1 on error with @c errno set.
 */
int check_parent_process(void);

#ifdef __cplusplus
}
#endif

#endif /* PEAK_PARENT_REGISTRY_H */
