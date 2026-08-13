#include "internal/signal_policy_internal.h"

#include <errno.h>
#include <stddef.h>

int
peak_signal_policy_choose_reserved_signal(void)
{
    return -1;
}

void
peak_signal_policy_configure(void)
{
}

void
peak_signal_policy_set_reserved_signal(int signum)
{
    (void)signum;
}

void
peak_signal_policy_clear_reserved_signal(void)
{
}

int
peak_signal_policy_reserved_signal(void)
{
    return 0;
}

void
peak_signal_policy_enter_internal(void)
{
}

void
peak_signal_policy_leave_internal(void)
{
}

int
peak_signal_policy_send_thread_signal(pid_t tid,
                                      int signum,
                                      unsigned long cookie)
{
    (void)tid;
    (void)signum;
    (void)cookie;
    errno = ENOTSUP;
    return -1;
}

int
peak_signal_policy_atomics_lock_free(void)
{
    return 1;
}

unsigned long
peak_signal_policy_cookie_for(int epoch, pid_t tid)
{
    (void)epoch;
    (void)tid;
    return 0;
}

int
peak_signal_policy_cookie_matches_async(const siginfo_t* info,
                                        int epoch,
                                        pid_t tid)
{
    (void)info;
    (void)epoch;
    (void)tid;
    return 0;
}

void
peak_signal_policy_note_unexpected_delivery(void)
{
}

int
peak_signal_policy_unblock_reserved_for_current_thread(void)
{
    return 0;
}

void
peak_signal_policy_push_migration_disabled(void)
{
}

void
peak_signal_policy_pop_migration_disabled(void)
{
}

int
peak_signal_policy_unexpected_delivery_count(void)
{
    return 0;
}

int
peak_signal_policy_conflict_count(void)
{
    return 0;
}

int
peak_signal_policy_migration_count(void)
{
    return 0;
}

const char*
peak_signal_policy_last_conflict_api(void)
{
    return NULL;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
int
peak_signal_policy_test_block_reserved_for_current_thread(void)
{
    return -1;
}

int
peak_signal_policy_test_send_bad_cookie_to_current_thread(void)
{
    return -1;
}
#endif
