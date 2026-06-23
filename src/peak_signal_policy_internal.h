#ifndef __PEAK_SIGNAL_POLICY_INTERNAL_H
#define __PEAK_SIGNAL_POLICY_INTERNAL_H

#include "peak_signal_policy.h"

#include <signal.h>
#include <sys/types.h>

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_SIGNAL_POLICY_INTERNAL __attribute__((visibility("hidden")))
#else
#define PEAK_SIGNAL_POLICY_INTERNAL
#endif

PEAK_SIGNAL_POLICY_INTERNAL int peak_signal_policy_choose_reserved_signal(void);
PEAK_SIGNAL_POLICY_INTERNAL void peak_signal_policy_set_reserved_signal(int signum);
PEAK_SIGNAL_POLICY_INTERNAL void peak_signal_policy_clear_reserved_signal(void);
PEAK_SIGNAL_POLICY_INTERNAL int peak_signal_policy_reserved_signal(void);

PEAK_SIGNAL_POLICY_INTERNAL void peak_signal_policy_enter_internal(void);
PEAK_SIGNAL_POLICY_INTERNAL void peak_signal_policy_leave_internal(void);

PEAK_SIGNAL_POLICY_INTERNAL int peak_signal_policy_send_thread_signal(pid_t tid,
                                                                      int signum,
                                                                      unsigned long cookie);
PEAK_SIGNAL_POLICY_INTERNAL int peak_signal_policy_atomics_lock_free(void);
PEAK_SIGNAL_POLICY_INTERNAL unsigned long peak_signal_policy_cookie_for(int epoch,
                                                                        pid_t tid);
PEAK_SIGNAL_POLICY_INTERNAL int peak_signal_policy_cookie_matches_async(const siginfo_t* info,
                                                                        int epoch,
                                                                        pid_t tid);
PEAK_SIGNAL_POLICY_INTERNAL void peak_signal_policy_note_unexpected_delivery(void);
PEAK_SIGNAL_POLICY_INTERNAL int peak_signal_policy_unblock_reserved_for_current_thread(void);
PEAK_SIGNAL_POLICY_INTERNAL void peak_signal_policy_push_migration_disabled(void);
PEAK_SIGNAL_POLICY_INTERNAL void peak_signal_policy_pop_migration_disabled(void);

#endif /* __PEAK_SIGNAL_POLICY_INTERNAL_H */
