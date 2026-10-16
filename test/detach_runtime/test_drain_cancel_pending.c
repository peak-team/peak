/* Prepared focused pthread-only test; no Gum/ptrace/MPI or target workload. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include "internal/cancel_scope.h"
static atomic_int before_original_cp, cleanup_seen;
static void cleanup(void* unused)
{
    (void)unused;
    assert(peak_cancel_scope_current == 0);
    atomic_store(&cleanup_seen, 1);
}
static void* run(void* ambiguous)
{
    int state;
    PeakCancelScope scope;
    assert(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &state) == 0);
    assert(pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, 0) == 0);
    assert(pthread_cancel(pthread_self()) == 0);
    pthread_cleanup_push(cleanup, 0);
    peak_cancel_scope_enter(&scope);
    if (ambiguous) peak_cancel_scope_note_open_result(0, ENOMEM);
    assert(peak_cancel_scope_leave(&scope) == 0);
    /* Empty/no-work and ambiguous ENOMEM do not cause explicit delivery. */
    assert(pthread_setcancelstate(state, 0) == 0);
    atomic_store(&before_original_cp, 1);
    pthread_testcancel(); /* The next original application CP must deliver it. */
    assert(0);
    pthread_cleanup_pop(0);
    return 0;
}
int main(void)
{
    for (int ambiguous = 0; ambiguous != 2; ++ambiguous) {
        pthread_t thread;
        void* result = 0;
        atomic_store(&before_original_cp, 0);
        atomic_store(&cleanup_seen, 0);
        assert(pthread_create(&thread, 0, run, ambiguous ? &thread : 0) == 0);
        assert(pthread_join(thread, &result) == 0);
        assert(result == PTHREAD_CANCELED);
        assert(atomic_load(&before_original_cp) == 1);
        assert(atomic_load(&cleanup_seen) == 1);
    }
    return 0;
}
