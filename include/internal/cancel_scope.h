#ifndef PEAK_INTERNAL_CANCEL_SCOPE_H
#define PEAK_INTERNAL_CANCEL_SCOPE_H
/* Owner-thread only: no handler, allocation, syscall or cancellation operation. */
typedef struct PeakCancelScope {
    struct PeakCancelScope* previous;
    volatile int seen;
} PeakCancelScope;
extern __thread PeakCancelScope* peak_cancel_scope_current
    __attribute__((visibility("hidden")));
static inline void peak_cancel_scope_enter(PeakCancelScope* scope)
{
    scope->previous = peak_cancel_scope_current;
    scope->seen = 0;
    peak_cancel_scope_current = scope;
}
static inline void peak_cancel_scope_mark(void)
{
    if (peak_cancel_scope_current != 0) peak_cancel_scope_current->seen = 1;
}
static inline int peak_cancel_scope_leave(PeakCancelScope* scope)
{
    int seen = scope->seen;
    peak_cancel_scope_current = scope->previous;
    if (seen && scope->previous != 0) scope->previous->seen = 1;
    return seen;
}
#endif
