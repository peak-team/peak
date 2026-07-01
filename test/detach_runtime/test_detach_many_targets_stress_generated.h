#ifndef TEST_DETACH_MANY_TARGETS_STRESS_GENERATED_H
#define TEST_DETACH_MANY_TARGETS_STRESS_GENERATED_H

#include <stdint.h>

#ifndef PEAK_STRESS_TARGET_COUNT
#define PEAK_STRESS_TARGET_COUNT 1024
#endif

typedef void (*StressTargetFn)(uint64_t value);

extern StressTargetFn stress_targets[PEAK_STRESS_TARGET_COUNT];

#endif
