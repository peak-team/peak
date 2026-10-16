# Exact81 plus forward-effect guards: partial candidate

This source-only candidate starts at exact81a
`81a6e2f10cd77e274183e5e5b97dc9e6d706c8ef`. It ports only the reviewed
orthogonal forward-effect guards and corresponding tests from qualified local
C `7883974b197bbd65f524a49b8bcca77797b6164b` (a88 safety semantics adapted
to the existing authentic receipt ledger and tagged reader leases).
It is not C, a full PR123 fix, or qualified merely because C passed tests.

The product delta is confined to `src/detach_controller.c`: coordinator
effect latch, contamination checks before effects and release, fatal handling
after effects/partial evacuation/unproved temporary trap installation, and
scalar/batch planning-record restoration before peer resume on recoverable
refusal. Fork-child abandonment clears only the coordinator effect latch;
existing receipt/reader cleanup is preserved.

There is no monotone issuer gate, typed action adapter, private pause exemption,
or policy/header/listener change. Exact81 receipt transport, whole-ledger
identity rejection, tagged readers, ordinary event/scalar and NULL-mask fixes
are retained. The original controller fixture remains byte-identical to81,
including its public initial P setter and all13 recovery/regression oracles.

The20 new cases cover recoverable refusal, real PC and temporary-breakpoint
positive effects, before/after/permission contamination fatal paths, PC/trap
fatal paths, actual post-trap-write error, and partial multi-evacuation failure,
each scalar and batch. Fatal tests require real owned-send and mode-specific
effect witnesses, expected exit128 and diagnostic; they do not fabricate
ownership. The static checker adds these boundaries, named wake/publication
ordering, and receipt-owned sender invariants; its admitted-thread extraction
uses exact81's unwrapped function name, not C's issuer-wrapper implementation.

Inherited measured defect: exact81's actual accepted owned O can reach native
application H (`owned_H=1, app_H=1`) after a blocked-handler timeout and
successful native H installation. Original evidence:
`peak-pr123-signal-handoff-20261016/NATIVE-HANDOFF-BASELINE-v1.json`,
baseline002, with actual pending O and unchanged instrumented bytes/physical0.
Forward-effect guards do not fix native H transparency or kernel-selected old
handler/frame retirement. No oracle is inverted and no failing handoff is
registered as a green test.

A separately authorized partial7995452 qualification may evaluate the same
MILC workload, heartbeat/active-slot behavior and real physical transitions
without claiming full PR readiness. Absence of the global admission gate
retains81's opportunity for physical mutation but also its inherited safety
gap. No build/runtime/commit/deployment has been performed for this candidate.

## PR-head invariant reconciliation (not merge ancestry)

- 7c canceled-window owned-delivery credit: retained exact81 whole-ledger
  receipts bind epoch/TID/signum before publication; only native rc0 grants
  SENT, failed sends discard the tagged reference, and retirement retains SENT
  debt before peer release. Supported-owned-recovery scalar/batch/replay and
  receipt capacity/failed-send/identity-pending controls remain registered.
- 7c reusable-slot/handler-reader reclamation: exact81's tagged slot readers
  and separate receipt identity supersede slot-bound owned_state/retired
  credit. Existing active-slot-reuse pre-pinned/selected/arrived tests remain;
  fork-child tagged-reader/receipt preservation remains unchanged. This is not
  proof against kernel-selected old frames not yet entered in userspace.
- a88 forward-effect boundary: the qualified C product guard delta is applied
  verbatim to81; the20 genuine-send tests replace obsolete slot-seeded effect
  setup, while preserving fatal-exit/detail and real-effect witness oracles.
- The old PR head a88 is NOT an ancestor of this checkout. Before publication,
  a separately reviewed genuine non-force merge must include its ancestry and
  reconcile these invariants; no ours strategy, force push, or claim that this
  source snapshot is already a merge is authorized here.
