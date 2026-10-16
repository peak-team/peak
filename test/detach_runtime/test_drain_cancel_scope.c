/* Pure scope bookkeeping: no target, Gum, ptrace, MPI or cancellation delivery. */
#include <assert.h>
#include "internal/cancel_scope.h"
int main(void)
{
    PeakCancelScope outer, inner;
    assert(peak_cancel_scope_current == 0);
    peak_cancel_scope_mark();
    peak_cancel_scope_enter(&outer);
    assert(peak_cancel_scope_leave(&outer) == 0);
    assert(peak_cancel_scope_current == 0);
    peak_cancel_scope_enter(&outer);
    peak_cancel_scope_enter(&inner);
    peak_cancel_scope_mark();
    assert(peak_cancel_scope_leave(&inner) == 1);
    assert(peak_cancel_scope_current == &outer);
    assert(peak_cancel_scope_leave(&outer) == 1);
    assert(peak_cancel_scope_current == 0);
    /* Ambiguous allocation/open ENOMEM must not invent CP eligibility. */
    peak_cancel_scope_enter(&outer);
    errno = ENOMEM;
    peak_cancel_scope_note_open_result(0, errno);
    assert(errno == ENOMEM);
    assert(peak_cancel_scope_leave(&outer) == 0);
    peak_cancel_scope_enter(&outer);
    peak_cancel_scope_note_open_result(0, ENOENT);
    assert(peak_cancel_scope_leave(&outer) == 1);
    peak_cancel_scope_enter(&outer);
    peak_cancel_scope_note_open_result(&outer, ENOMEM);
    assert(peak_cancel_scope_leave(&outer) == 1);
    return 0;
}
