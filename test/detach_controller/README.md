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
It consumes no signal. Other probe results retain ownership, and a window makes
at most one pressure sweep. This proof assumes faithful Linux syscall results,
the same assumption used by main's existing exited-slot detection; a seccomp
supervisor, tracer or security policy synthesizing ESRCH without the lookup is
not covered.
