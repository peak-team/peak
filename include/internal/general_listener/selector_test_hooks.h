#ifndef PEAK_SELECTOR_TEST_HOOKS_H
#define PEAK_SELECTOR_TEST_HOOKS_H

#include "internal/gum_peak_compat.h"

#include <stddef.h>

#ifdef PEAK_ENABLE_TEST_HOOKS
#if defined(__GNUC__) || defined(__clang__)
#define PEAK_SELECTOR_TEST_API __attribute__((visibility("default")))
#else
#define PEAK_SELECTOR_TEST_API
#endif

PEAK_SELECTOR_TEST_API gulong
peak_general_listener_test_startup_selector_batches(void);

/** Returns the borrowed report name for a settled test hook. */
PEAK_SELECTOR_TEST_API const char*
peak_general_listener_test_demangled_name(size_t hook_id);
#endif

#endif /* PEAK_SELECTOR_TEST_HOOKS_H */
