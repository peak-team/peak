# Linux x86-64 unwind frame ownership

The frame pool in `cmake/peak-gum/peak_gum_unwind_frames.inc` repairs return
PCs for ordinary returns, exceptions and nonlocal exits. Its owner outlives
all application pthread-key destructors. The owner is not freed merely
because a destructor ran, a frame count reached zero, or a detach started.

## Readers and writers

| State | Writers | Readers and lifetime protection |
| --- | --- | --- |
| Owner `active`, `free`, frame context/slot/caller | Owning thread's prepare/enter/leave/reap/reconcile | Owning thread and its nested callbacks; dead-thread settlement only after death proof |
| Frame `diagnostic_active` | Owning thread's enter/release, relaxed atomic stores | Diagnostics uses relaxed atomic loads while holding the owners lock |
| Owner/block list | Creation, growth, join/reaper under owners lock | Diagnostics and owner lookup under the same lock |
| Context trampoline usage lease | Enter/release, detach handoff, dead-context settlement | Gum's existing atomic lease protocol; unchanged |
| `retired` | Pthread-key destructor | Notification of possible cleanup work, never proof of death |
| CFI lookup cache | Owning thread; dead-owner free | Cleared when the owning active chain is empty, or after death proof |

The remote collector must establish the existing successful join/identity
match or missing kernel TID condition, unlink under the owners lock, and
only then deregister FDEs/unmap blocks/free the owner. Diagnostics cannot
race with unmapping because its entire block scan holds that lock. It does
not dereference a live thread's active/free chain or frame context.

A throw can skip leave and leave metadata pending until stack-based reap or
explicit unwind reconciliation. Recursion has distinct reserved frames and
return slots. Forced unwind and pthread exit retain frames until settlement
or actual thread completion. Later key destructors can still enter targets,
including first access in the fourth POSIX destructor pass. Neither the
retired notification nor an empty active chain authorizes freeing the pool.

## Why an ordinary counter update is insufficient

The former relaxed atomic RMW live count was used for diagnostics, a local
canonical-caller traversal bound and local last-frame cache cleanup. It was
not the remote lifetime barrier. Splitting an RMW into a load and store is
nevertheless unsafe: an interrupted update may resume after a nested
callback's nonlocal exit has left another frame pending. The stale store
loses that change and can clear a cache while a frame remains active.
A balanced nested enter/leave alone does not demonstrate this failure.

The release boundary regression compiles the actual enter/release functions
with one deterministic interrupt hook. A handler uses its own sigsetjmp and
siglongjmp to leave a nested frame pending. An additional OMEN
comparison retains the old RMW control, which preserves one live frame; the
split-counter diagnostic loses it. This fault injection
isolates the metadata protocol; it does not claim all C++/Gum operations are
async-signal-safe. The integration regression separately exercises a warmed
ordinary target from a real signal handler, recursion/throw, forced exit,
cancel/RAII and a blocked fourth key destructor while another thread reads
diagnostics and attempts reclamation.

`peak_uw_internal` protects allocation/FDE registration and explicit unwind
walking; it does not exclude signals throughout enter/release. The listener
callback guard also is not such a proof: skipped observation may still need
a return frame.

## Per-frame diagnostic stores

Each reserved frame has an atomic diagnostic flag. Enter sets its flag;
release clears it **before** publishing that frame on the free list, with a
signal compiler fence before publication. A second compiler fence keeps the
local empty-chain read after that publication. A nested callback can reuse the
frame immediately after publication, so clearing at the former counter
update location would erase the new instance. The release-boundary test
requires this intentionally broken ordering to fail.

The active chain already provides the local cache-empty condition and a
bounded canonical-caller search. No diagnostic total participates in memory
reclamation, lease release, or cache lifetime. The new flag occupies existing
padding; the x86-64 frame size and the offsets used by CFI remain unchanged.
The context lease atomics and thread-death protocol remain intact.

Diagnostics now scans registered frame slots rather than reading one count
per owner. This trades cold diagnostic scan work for removing two locked
RMWs per ordinary call. While threads execute, the result is advisory: a
scan can count two slots that were active at different moments. It is exact
at a quiescent checkpoint, not a linearizable concurrent snapshot. The tests
therefore assert exact live counts only while the worker is held at a
handshake, and never use advisory totals to trigger reclamation.

## Validation limits

The new integration oracle explicitly resolves unwind imports with shared
`libgcc_s` before the project's global static GCC runtime libraries. An OMEN
control with the embedded static unwinder aborted in both the previous
05e30d7 and flag builds; the same O3 fixture using the shared unwinder passed
in both. That existing static-unwinder failure is retained as a separate
unresolved limitation, not repaired or qualified by this counter change.
The ordinary application semantics and all forced-exit/cancel operations in
the oracle remain enabled.
