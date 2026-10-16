#ifndef PEAK_INTERNAL_CANCEL_SCOPE_H
#define PEAK_INTERNAL_CANCEL_SCOPE_H
#include <errno.h>
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
/* Supported SDK ordinary fopen modes reach cancellable open unless ENOMEM
 * can originate before open. In that ambiguous case preserve pending requests. */
static inline void peak_cancel_scope_note_open_result(const void* result, int error)
{
    int saved_errno = errno;
    if (result != 0 || error != ENOMEM) peak_cancel_scope_mark();
    errno = saved_errno;
}
static inline int peak_cancel_scope_leave(PeakCancelScope* scope)
{
    int seen = scope->seen;
    peak_cancel_scope_current = scope->previous;
    if (seen && scope->previous != 0) scope->previous->seen = 1;
    return seen;
}
#endif
