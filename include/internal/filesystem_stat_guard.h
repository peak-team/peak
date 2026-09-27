#ifndef PEAK_FILESYSTEM_STAT_GUARD_H
#define PEAK_FILESYSTEM_STAT_GUARD_H

/* Linux libc filesystem-stat call exclusion for physical stop windows.
 * Readers never acquire the controller mutation mutex. Internal controller
 * scopes alone may bypass admission; normal application calls may not. */

/* Close admission and drain existing queries for at most 1 ms. Return success
 * only after the reader count reaches zero; timeout reopens entry and defers. */
int peak_filesystem_stat_guard_try_stop(void);

/* Open before releasing stopped threads and waiting for acknowledgements. */
void peak_filesystem_stat_guard_resume(void);

/* Diagnostics count deferrals, not successful physical stops. */
unsigned long peak_filesystem_stat_guard_deferred_stop_count(void);

/* Balanced, thread-local scopes held only under the mutation guard. */
void peak_filesystem_stat_guard_controller_enter(void);
void peak_filesystem_stat_guard_controller_leave(void);

/* Reset the controller bypass scope when its child mutex is reinitialized,
 * including a fork before this guard has lazily initialized. No symbol lookup. */
void peak_filesystem_stat_guard_controller_after_fork_child(void);

/* Registered by initialization before any query is admitted. */
void peak_filesystem_stat_guard_after_fork_child(void);

#endif /* PEAK_FILESYSTEM_STAT_GUARD_H */
