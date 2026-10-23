# Signal cancellation recovery regressions

These regressions exercise the existing signal backend and real Gum entry
patches. They cover recovery from a canceled owned notification and safe reuse
of handler slots. They do not replace the physical detach/reattach mechanism or
turn off reattachment.

## Build and run

On a supported Linux host with the normal PEAK build dependencies:

```sh
cmake -S . -B build-signal \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON \
  -DPEAK_ENABLE_MPI=OFF \
  -DBUILD_CUDA_PROFILE=OFF \
  -DPEAK_BUILD_DYNAMIC_LIB_TESTS=OFF \
  -DPEAK_REQUIRE_GUM_PEAK_API=ON
cmake --build build-signal --target peak_detach_helper test_detach_controller --parallel
ctest --test-dir build-signal --output-on-failure \
  -R '^test_detach_controller_(signal_preflight_|supported_owned_recovery_|signal_receipt_|active_slot_reuse_)'
```

The helper executable is an existing build dependency; these cases explicitly
select the signal backend and set the helper path to a nonexistent file. The
test configuration still requires BLAS/LAPACK even though this target does not
exercise numerical libraries. A site-provided patched Gum devkit can be selected
with the normal provider options instead of downloading it.

The recovery and slot-overlap cases intentionally wait for the controller's
real stop timeout (approximately ten seconds). CTest supplies an outer watchdog;
the tests do not shorten the production timeout.

## What each group proves

| Group | Required observation |
| --- | --- |
| `signal_preflight_scalar/batch` | A delivered unexpected packet recorded during preflight prevents the first owned send and physical mutation. |
| `signal_preflight_clean_scalar/clean_batch` | Clean control still performs physical detach and reattach with entry bytes and callbacks checked. |
| `supported_owned_recovery_scalar/batch` | An ordinary handler masks the reserved signal after preflight; the controller's genuine successful send times out during DETACH; late delivery is recognized; subsequent physical detach and reattach succeed. |
| `supported_owned_recovery_consumed_replay` | After genuine late consumption and successful recovery, replaying that consumed identity earns no second ownership exception. Only this negative follow-up is synthetic. |
| `supported_owned_recovery_no_debt_handoff` | After all old owned notifications have been consumed, native application handler installation and an ordinary queued payload succeed on the original worker; physical detach and reattach still work through the migrated reservation. |
| `supported_owned_recovery_reap_live` | A pressure sweep retains a genuine pending notification for a live worker; after unmasking, its exact late receipt is consumed and real detach/reattach still succeeds. |
| `supported_owned_recovery_exited_target` | A worker exits with a genuine owned notification still pending; a one-record ledger recovers that capacity under pressure and completes physical detach/reattach with a new worker. The fixture deliberately uses Linux thread exit from the ordinary handler, not a claim of portable `pthread_exit` semantics inside a signal handler. |
| `signal_receipt_control_capacity/failed_send` | Insufficient initial-cohort capacity sends nothing; a failed send earns no later ownership credit. |
| `signal_receipt_identity_pending_published/sent` | A simulated unconsumed identity at the last record is found even when an earlier record is free or the allocation limit is smaller. This is a protocol-state test, not real TID reuse. |
| `active_slot_reuse_before_pin/after_pin` | A real old handler delayed around its reader pin cannot modify reused generation payload; physical detach/reattach succeeds after it returns. |

The recovery oracle compares saved original bytes, saved active patch bytes,
Gum diagnostics, callback deltas, held-window state and the thread-start gate.
An attached function produces a callback; physically detached entry bytes equal
the saved original and a new call produces none; reattachment restores the
active patch and callback. It never treats a logical flag alone as evidence of
physical detachment.

## Scope and remaining boundaries

The initial delayed notification is not a replayed cookie. A normal `sigaction`
installs an application handler whose `sa_mask` blocks the reserved signal; a
test-only scheduling seam enters it after the real mask preflight. The real
sender is observed with a linker wrapper that forwards the notification to the
production send function. Successful send, the pending bit and exact late
receipt consumption are checked separately.

The no-debt handoff test is a positive compatibility test. It does **not** cover
native handoff while an owned packet is still pending. If application handler H
replaces the PEAK action before that old packet is delivered, the packet can
bypass the ledger. Likewise, a user-space before-pin pause does not reproduce
the kernel-selected-handler/pre-user-entry boundary, and reader pins do not
establish an unload grace period for handler text.

The changes keep main's signal policy, patch/release failure handling, retry
budgets and supported platforms. Live or unprovable unconsumed receipts and
unreturned reader pins remain bounded resource debt; exhaustion can still
decline profiler windows for as long as that debt remains. Capacity recovery
does not clear a genuine live packet just to make room.
These focused tests do not qualify every application signal interleaving,
thread-identity reuse, full handler unload, end-to-end performance or a new
multi-node deployment.

### Why an exited target's receipt can be reclaimed

Linux's [thread-directed sender](https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c)
uses `PIDTYPE_PID` and the selected task's private pending queue. A real
[`tgkill(tgid, tid, 0)` ESRCH result](https://man7.org/linux/man-pages/man2/tkill.2.html)
is a negative existence observation for that thread in this process. The
[task exit path](https://github.com/torvalds/linux/blob/v6.12/kernel/exit.c)
unhashes the task and flushes its private queue; the old task cannot subsequently
return to a user handler. Future numeric TID reuse does not inherit that queue.
Reuse before the probe in the same process yields success, so the implementation
conservatively retains the record in that case.

The pressure sweep considers only successfully sent, retired, unconsumed records,
uses the internal direct syscall primitive, preserves `errno`, and requires a
successful compare-and-exchange on the same allocation state before reclaiming.
It consumes no signal. Other probe results retain ownership, and each allocation attempt makes
at most one pressure sweep. This proof assumes faithful Linux syscall results,
the same assumption used by main's existing exited-slot detection; a seccomp
supervisor, tracer or security policy synthesizing ESRCH without the lookup is
not covered.

## Retained a149 integration and remaining boundary

This checkout starts at a1490e9dae9613e5c105edd91581f497b4cad989 and
selectively ports the bounded-receipt recovery increment from PR127 commit
f7145fd2dbcb6c9257cb9c1952f47a7cce258864 relative to ad781. The controller
is not replaced wholesale. The a149 ordinary-event/scalar and NULL-mask
signal-policy reentry fixes, tagged handler readers, forward-effect latch,
contamination checks, release/planning restoration, and all corresponding
regression and fatal-effect fixtures remain in place.

The existing twenty scalar/batch forward-effect cases retain their genuine
owned-send and mode-specific effect witnesses, expected fatal exit/detail,
and recoverable-refusal oracles. The original recovery groups are extended by
three cases: no-debt native handoff, live pending receipt retention, and
exited-target capacity recovery. The first canceled physical request now uses
DETACH, not an already-attached REATTACH no-op. Timeout assertions retain the
production lower bound; CTest provides the outer watchdog.

The new high-water mark bounds controller identity/retirement scans only.
Handler lookup still covers the full fixed pool. Child-fork cleanup clears
receipts without reducing the inherited conservative high-water bound or
resetting nonwrapping tickets. Under pressure, only native ESRCH for a retired,
successfully sent, unconsumed receipt permits full-tag CAS reclamation. A
concurrent SEEN update defeats that CAS; the normal SENT+SEEN allocation path
can reclaim it later. Same-group numeric TID reuse before the probe gives
success and conservatively retains debt. Duplicate identity checks still cover
all published records, including those beyond a test allocation limit.

This increment repairs exited-target capacity loss; it does not solve the
pending-owned-packet native handoff boundary. Exact81's observed accepted O can
reach application H (owned_H=1, app_H=1) after a blocked-handler timeout and
successful native H installation. The unchanged external negative evidence is
peak-pr123-signal-handoff-20261016/NATIVE-HANDOFF-BASELINE-v1.json, baseline002.
Commit 07abc3630c0f3a54a47886788838990864733c83 additionally records a focused
pending-O native-handoff diagnostic; that missing in-tree diagnostic is not a
new product feature requirement, and it must not be inverted or registered as
a green handoff qualification. It is not ported by this increment. A no-debt
positive test is not evidence that pending O was withdrawn or that selected
old frames can safely outlive unloaded handler code.

No monotone disable, new application-API rejection, helper fallback, additional
fatal category, or replacement transport is introduced. Performance, native
platform qualification, CI, and deployment require separate actual receipts;
this source patch itself claims none of them.
