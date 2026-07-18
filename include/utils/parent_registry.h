#ifndef PEAK_PARENT_REGISTRY_H
#define PEAK_PARENT_REGISTRY_H

/**
 * @brief Register this PEAK process and check whether its direct parent is registered.
 *
 * On Linux, registry entries contain both a PID and its /proc starttime. This
 * prevents a later, unrelated process that reused the numeric PID from being
 * treated as a PEAK parent. Dead, malformed, legacy PID-only, and identity
 * mismatched entries are pruned while holding the file lock.
 *
 * @return 1 if the current direct parent is live and registered, 0 otherwise,
 *         or -1 on error
 */
int check_parent_process(void);

#endif /* PEAK_PARENT_REGISTRY_H */
