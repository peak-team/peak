#ifndef __PEAK_SIGNAL_POLICY_H
#define __PEAK_SIGNAL_POLICY_H

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_SIGNAL_POLICY_API __attribute__((visibility("default")))
#else
#define PEAK_SIGNAL_POLICY_API
#endif

PEAK_SIGNAL_POLICY_API int peak_signal_policy_unexpected_delivery_count(void);
PEAK_SIGNAL_POLICY_API int peak_signal_policy_conflict_count(void);
PEAK_SIGNAL_POLICY_API const char* peak_signal_policy_last_conflict_api(void);

#ifdef PEAK_ENABLE_TEST_HOOKS
PEAK_SIGNAL_POLICY_API int
peak_signal_policy_test_block_reserved_for_current_thread(void);

PEAK_SIGNAL_POLICY_API int
peak_signal_policy_test_send_bad_cookie_to_current_thread(void);
#endif

#endif /* __PEAK_SIGNAL_POLICY_H */
