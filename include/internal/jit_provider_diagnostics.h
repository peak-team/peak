#ifndef PEAK_JIT_PROVIDER_DIAGNOSTICS_H
#define PEAK_JIT_PROVIDER_DIAGNOSTICS_H

#include <stdint.h>

typedef struct {
    uint64_t pending_queue_full;
    uint64_t non_executable_timeout;
    uint64_t attach_retry_timeout;
    uint64_t allocation_failure;
    uint64_t provider_generation;
    uint64_t pending_count;
    uint64_t pending_high_water;
} PeakJitProviderDiagnostics;

#endif /* PEAK_JIT_PROVIDER_DIAGNOSTICS_H */
