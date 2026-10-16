#ifndef PEAK_REATTACH_DIAGNOSTICS_H
#define PEAK_REATTACH_DIAGNOSTICS_H
#include <stddef.h>
#include <stdint.h>
/* Opt-in diagnostics only. No callback-side caller is permitted. */
#define PEAK_RD_HOOKS 64u
#define PEAK_RD_EVENTS 512u
#define PEAK_RD_HIDDEN __attribute__((visibility("hidden")))
PEAK_RD_HIDDEN int peak_rd_init(const char* enabled, const char* trace_path);
PEAK_RD_HIDDEN void peak_rd_event(size_t hook, const char* kind, int state, int operation,
    uint64_t calls, double profile, double saved, double elapsed,
    double a, double b, unsigned reason);
PEAK_RD_HIDDEN void peak_rd_round(size_t hooks, double elapsed, int ready,
    int reattach_enabled, unsigned interval, unsigned counter,
    double target_ratio, double global_ratio, double factor);
PEAK_RD_HIDDEN void peak_rd_sample(size_t hook, int reuse, uint64_t calls,
    double profile);
PEAK_RD_HIDDEN void peak_rd_reason(size_t hook, const char* reason);
PEAK_RD_HIDDEN void peak_rd_global(size_t hooks, int coherent, int admitted,
    double elapsed, double profile, double control, double spent_ratio,
    double gate_ratio, double headroom, double predicted_stop);
/* First stop seals capture permanently. IO is outside all controller locks. */
PEAK_RD_HIDDEN void peak_rd_pending_budget(size_t hooks);
PEAK_RD_HIDDEN void peak_rd_emit(void);
#endif
