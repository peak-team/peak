#ifndef PEAK_HOOK_COVERAGE_H
#define PEAK_HOOK_COVERAGE_H
#include <stddef.h>
#include <stdint.h>
/* Measurement only. Existing target/controller ownership serializes calls. */
typedef struct PeakHookCoverage PeakHookCoverage;
typedef struct {
  uint64_t attach_successes, reattach_successes, detach_successes;
  double attached_wall_seconds;
} PeakHookCoverageSample;
PeakHookCoverage *peak_hook_coverage_create(size_t capacity);
void peak_hook_coverage_destroy(PeakHookCoverage *coverage);
int peak_hook_coverage_grow(PeakHookCoverage *coverage, size_t capacity);
int peak_hook_coverage_start(PeakHookCoverage *coverage, double now);
int peak_hook_coverage_attach(PeakHookCoverage *coverage, size_t hook,
                              double now, int reattach);
int peak_hook_coverage_detach(PeakHookCoverage *coverage, size_t hook,
                              double now);
int peak_hook_coverage_snapshot(const PeakHookCoverage *coverage, size_t hook,
                                double now, PeakHookCoverageSample *sample);
#endif
