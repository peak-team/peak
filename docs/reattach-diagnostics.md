# Reattach admission diagnostics

Reattach diagnostics record the controller facts needed to distinguish an
admission decision from an accepted request that has not progressed. They do not
change admission, force reattach, extend the observation period, or qualify a
performance result.

## Enable and collect

Set `PEAK_REATTACH_DIAGNOSTICS=1` and a nonempty `PEAK_DETACH_TRACE_PATH` before
PEAK initializes. The existing 28-column detach trace retains its format. A
separate `<PEAK_DETACH_TRACE_PATH>.reattach-diagnostics` file is written at the
first normal `controller_stop`. Choose a fresh path in a directory you own;
existing files and symlinks are not overwritten. The initial implementation
supports Linux x86-64 and AArch64. Other platforms leave the feature disabled.

Diagnostics are off by default. Target enter/leave callbacks have no diagnostic
bookkeeping. When disabled, the added control-path guards bypass diagnostic
argument evaluation, counters, clocks, formatting, allocation and output.

Enabled recording uses bounded process-local storage. It adds no heartbeat-round
file writes and no target-call records. A writer that cannot acquire diagnostic
record ownership drops its record instead of waiting. The first stop seals the
capture. A concurrent producer makes the capture incomplete without reading its
payload. Fork children do not publish inherited parent records. The normal
controller state machine, retry policy, helper protocol and shutdown order are
unchanged.

Output uses raw Linux file syscalls and preserves `errno`. The emitter adds no
explicit cancellation test, temporarily uses deferred cancellation while it owns
the output descriptor, and restores the caller's cancellation type after closing
it. An owned-descriptor cleanup handler also covers cancellation during
formatting. The caller's cancellation state is unchanged. Enabled output can
delay asynchronous cancellation. File output occurs after the controller thread
is joined and outside controller/listener locks. Regular-file I/O can still block
on the file system. This opt-in output has no bounded-latency or
performance-equivalence claim.

## Read the evidence outside the workload

The sidecar is a versioned text stream. `RD_BEGIN` declares capture scope and
capacity, `RD_EVENT` records ordered state/request/cache events, `RD_HOOK` records
heartbeat summaries, `RD_REASON` names rejection counters, and `RD_END` supplies
loss and completeness information. Hook, detach-generation and request IDs link
observations. Request IDs advance only for a newly accepted reattach request, not an
idempotent repeat. `controller_attempt` precedes preparation; `prepare_success`
marks the later transition into a mutation state. Operation values follow
`PeakDetachOperation` (`detach=1`, `reattach=2`, `shutdown=3`); state values follow
`PeakHookState` in the physical-controller design. Other state events use
`operation=-1`.

Heartbeat summaries are cumulative per hook. `sample_round/sample_gen` identify
the last target sample; `budget_round/budget_gen` and
`budget_sample_round/budget_sample_gen` identify the last global admission
calculation and its sample. `budget_profile` is the global profile total, whereas
`profile` is that hook's last sample. The ten named reason counters retain a
generation bitmask and first/last heartbeat round. Bit zero denotes activity
before the first detach seed; reason observations at generation 64 or above mark
overflow. These are
not per-generation counts or a complete log of every budget value. Values from
different rounds or generations must not be combined into one budget
calculation. A numeric last-global-budget association requires the reason
`last_round` to equal `budget_round` and `generation_mask` to equal exactly
`1 << budget_gen`; a mask containing several generations is fact-only. Local
headroom/lease, per-target and cooldown reasons do not record every operand
needed for a full recomputation. `after_cut` counts attempts excluded after sealing, rather than
claiming those events were captured.

Event values are kind-specific; an unavailable elapsed value is `0`, not a
measured zero duration. No new clock is taken for these events.

| Event | Additional fields |
| --- | --- |
| `seed` | `calls`, `profile` describe the detach cache refresh when `a=1`; `saved` is the saved detach profile; `a` indicates a valid refresh. |
| `request_accept` | `elapsed` is request-context time; `a` is request ratio, `b` global overhead and `reason` the existing request-source enum. |
| `controller_attempt` | `a` is retry count; `b` is batch pending age when available. |
| `retry_scheduled` | `a` is the existing next-retry time, `b` retry count and `reason` the existing status enum. |
| Existing trace results | `a` is retry count, `b` STOP-window microseconds and `reason` the existing status enum. |

The capture ends at the first `controller_stop`, rather than process exit.
Subsequent shutdown mutations and a restarted controller are outside its scope.
Counts summarize only observed events. Missing records do not establish that an
operation never happened when the capture is incomplete.

Before interpreting a run, require a supported schema, complete header and
trailer, contiguous event sequence, and zero producer overlap, busy drops,
overflow and out-of-range hooks. Missing output, an existing output destination,
a partial write, malformed values, loss, or concurrent capture makes the run
inconclusive. Do not turn a missing diagnostic into a successful lifecycle result.

| Complete observation | Supported conclusion |
| --- | --- |
| Detach seed, heartbeat decisions for that generation, rejection reasons, no newly accepted request | The recorded admission gate rejected the observed opportunities. State/cooldown filters are distinct from budget refusal. |
| Detach seed, no heartbeat decision opportunity in the capture | No decision opportunity was observed. This alone proves neither budget refusal nor request loss. |
| Newly accepted request and controller attempt or prepare failure for the same request | The controller reached that request. Inspect failure/retry/state events; this is not an unconsumed request. |
| Newly accepted request without a corresponding controller attempt | Accepted-but-not-observed-as-attempted within this capture. Retry readiness, capture cutoff, finalization and scheduling must be excluded before alleging a lost wake. |
| Successful reattach state and later detach | The recorded lifecycle worked in this run. It does not establish a historical root cause or performance repair. |

Keep the original strict lifecycle checker as the qualification authority. Run
all parsers, identity checks and result validation outside the job and its called
scripts. This feature supplies observations; it adds no in-job checker or probe.
