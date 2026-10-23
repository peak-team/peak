# Arm64 entry relocation: design and follow-up

This is a design investigation, not a claim that all Arm64 entry hooks are
safe. It evaluates the pinned Frida Gum 17.15.3 implementation, the PEAK
prefix-CFG guard, and upstream source inspected on 2026-10-23. The upstream
snapshot is commit
[`84eff39801382aa354dcb42bb8cd5d8f236652cf`](https://github.com/frida/frida-gum/commit/84eff39801382aa354dcb42bb8cd5d8f236652cf).
PEAK's fetched main at the time of this investigation is `217e1b2` (PR #126);
the guard is a separate draft change. Validation of the draft and research
experiments must remain distinguished from qualification of that newer base.

## Recommendation

Keep the immediate mitigation narrow: reject a target when an omitted
prefix-reachable path enters its actual overwritten span, or when the
supplementary check cannot finish its stated analysis. Check the selected
8-byte fallback as well as a successful 16-byte candidate. A failed request
for 16 bytes is not an attachment refusal if Gum returns enough space for an
8-byte patch. Preserve ordinary targets' existing behavior and allocator.

For the next implementation, integrate the missing prefix edges into the
relocator's existing analysis instead of maintaining two independent CFGs.
Use the upstream prefix-seeding change as a starting point, with explicit
analysis-completion status, bounded work, and coverage of every candidate
span. Treat choosing a smaller patch as a separate, optional recovery step:
it requires an executable-memory placement policy and complete trampoline
register/continuation tests. Do not turn every Arm64 hook into a near hook.

More complete code rewriting is a longer-term option. It needs a contract
for undiscovered external entrances and indirect transfers; a walk from one
symbol entry cannot establish that contract by itself.

## What failed and what the layers establish

The shared Vista QE executable is an AArch64 `ET_EXEC`, SHA-256
`1f8c1018e5b421ae49808682475b2b9cbf3891961ce9c1eb1ba319c6e2426ed6`.
Its `hpsort_eps_` entry is `0x89eb80`, with ELF size 540 bytes. The entry's
conditional branch at `+8` reaches an initialization block at `+0x17c`.
That block has branches at `+0x1fc` and `+0x218` back to `entry+12`.
The observed SIGILL PC, `0x89eb8c`, is that interior destination. A 16-byte
redirect replaces the original comparison there with pointer data. This is
an executable-control-flow defect, independent of report aggregation.

There are several separate obligations:

| Layer | What it establishes | What it does not establish |
| --- | --- | --- |
| Symbol resolution | A selected symbol/address and possibly module, type, and size | That no other entry, label, or indirect target enters the patch |
| PEAK prologue policy | Audited rejection rules and a chosen Gum attach plan | A whole-program CFG or a proof of every relocated instruction |
| Gum relocation | A candidate overwrite length and generated continuation | That an incomplete reachability search proves absence of incoming edges |
| Strict mutation controller | Held-thread/PC and mutation-lifecycle conditions at the transition | That a later branch from the original body cannot enter patched data |
| Reporting | Counts observed by installed listeners | Calls made through targets rejected for safety or while detached |

In 17.15.3, the checked relocation walk starts at `rl.input_cur`, after the
instructions read to satisfy the requested patch size. It does not seed
targets of those instructions. The backend can therefore accept the first
16 bytes while missing the alternate initialization path. The relevant
sources are the pinned
[relocator](https://github.com/frida/frida-gum/blob/e5b0d7d6a9a83da1750b6fc654006a714b81a890/gum/arch-arm64/gumarm64relocator.c#L340-L556)
and
[backend](https://github.com/frida/frida-gum/blob/e5b0d7d6a9a83da1750b6fc654006a714b81a890/gum/backend-arm64/guminterceptor-arm64.c#L646-L785).
The default interceptor policy is checked; merely changing a zero-valued
`GUM_RELOCATION_DEFAULT` option to `CHECKED` does not repair this omission.

The existing PEAK policies are documented in
[physical-detach-controller.md](physical-detach-controller.md) and
[dlopen-profiling.md](dlopen-profiling.md). In particular, the narrowly forced
Linux `B`-to-canonical-PLT plan and Darwin's rejection of canonicalizing
entries are separate contracts. The Linux change must not silently broaden
either. [Issue #80](https://github.com/peak-team/peak/issues/80) records the
Darwin identity and mutation limitations. [PR #123](https://github.com/peak-team/peak/pull/123)
concerns signal-window ownership, not this relocation proof.

## Upstream evidence and precise backport scope

The earliest identified upstream change for this omission is
[`ddc10c5559cbb41a3dd72866bfba6ff3945ffa5c`](https://github.com/frida/frida-gum/commit/ddc10c5559cbb41a3dd72866bfba6ff3945ffa5c),
titled `arm64: Detect branches into the relocated range`. It seeds direct
branch targets from already-read instructions, shrinks the relocatable boundary for interior targets, and
rewinds the subsequent walk to that boundary. Its regression covers a
leading `CBZ` to `+12`. Frida Gum tag `17.17.0` points exactly to that commit;
`17.16.4` lacks the change. The corresponding
[Frida release](https://github.com/frida/frida/releases/tag/17.17.0) provides
that version.

The exact commit changes only `gumarm64relocator.c` and its test. Its patch
applied in a bounded local dry run to the official 17.15.3 versions of both
files without offsets or fuzz. It does not change the public relocator
structure or its API. This is a substantially smaller compatibility change
than replacing the whole devkit.

A local host experiment compiled the precisely patched source under distinct
symbol names and compared it with the pinned static archive. It classified
the real 540-byte `hpsort_eps_` body without executing Arm64 instructions:

| Classifier | Request | Result | Maximum |
| --- | --- | --- | --- |
| Pinned 17.15.3 | 16 | accepted | 16 |
| 17.15.3 plus exact `ddc10c5` patch | 16 | refused | 12 |
| Same backport | 8 | accepted | 8 |

This demonstrates the missing edge is detected. It does not qualify a native
8-byte hook, near allocation, scratch-register semantics, or physical
detach/reattach. Temporary evidence is under
`/tmp/peak-qe-sigill/upstream-backport-check/`; it is not a durable test-suite
artifact. The exact upstream diff is also preserved as
`/tmp/peak-qe-sigill/upstream-prefix-seed.patch`.

The backport is not a complete safety proof:

- Its checked scan is still gated by `!rl.eoi`. An unconditional transfer
  ending the linear prefix does not eliminate another path taken by an
  earlier conditional branch.
- Its prefix-target seeding accepts forward offsets beyond the boundary;
  an external target before the entry is not seeded by that condition.
- Its 1024-byte remaining scan allowance is shared across queued paths,
  rather than reset for each block. Exhausting that allowance can leave
  transitive paths unexplored without an explicit incomplete-analysis result.
- It does not resolve arbitrary indirect targets or prove the absence of
  entrances from elsewhere in the program.

Later upstream work is relevant, but must be evaluated independently:

| Commit | Relevant change | Integration concern |
| --- | --- | --- |
| [`9573d3ef`](https://github.com/frida/frida-gum/commit/9573d3efc0624aeeb9e67a9ad45bc3eab31bc58e) | Entry/exit scratch-register liveness, code ranges, and continuation extension | Changes relocator/backend state and generation; not a layout-neutral transplant |
| [`74d8a287`](https://github.com/frida/frida-gum/commit/74d8a2875c259260f54b8408d6815ba2305d9412) | Direct relocated branches when reachable | Reduces scratch clobbering; far exits still need proof |
| [`d6980bc9`](https://github.com/frida/frida-gum/commit/d6980bc9bd0fd03f658dfdcc9bb783d824481c8e) | Separate runtime PC and bounded code-range reads | Adds/changes APIs and address-translation assumptions |
| [`6b01e86c`](https://github.com/frida/frida-gum/commit/6b01e86cbf83a0d58b211f1fe7211102a2653d73) | Shared 1024-byte reachability budget and known-target bookkeeping | Bounds cost but does not turn an unfinished search into a negative proof |
| [`2e59c2cf`](https://github.com/frida/frida-gum/commit/2e59c2cf0c98cd5ee4d07814d35dabadb3a086ff) | Prefer a 4-byte redirect for eligible Linux targets when a near slice exists | Changes near-space demand and fallback behavior; does not remove mutation-safety obligations |

The inspected main relocator has added `scratch_reg` and `code_range` state.
PEAK mirrors the pinned relocator inside its private backend layout, so using
a newer archive with the old mirror can shift subsequent fields. A successful
link or an unchanged PEAK API macro does not establish layout compatibility.
An upgrade requires a deliberate audit of
[gum_peak_pc_api.c](../cmake/peak-gum/gum_peak_pc_api.c), devkit hashes,
instruction classifiers, and controller metadata.

## Current guard: useful mitigation and limits

The draft routes only the pinned Arm64 interceptor backend's relocator import
through a supplemental check. The refined version checks the backend's
8- or 16-byte candidate span, including a failed 16-byte request whose
`maximum` still permits an 8-byte fallback. Rejecting a target returns false
with `maximum=0`, so the backend cannot silently select a smaller unexamined
patch. Ordinary results and original scratch selection are retained when the
additional analysis completes without a detected conflict.

A host classifier check of the refined guard observed unsafe `maximum=8`
and `maximum=12` candidates change to `maximum=0`, while a safe 8-byte
candidate retained false/`maximum=8`. These are classifier results, not native
8-byte attachment results. The initial 16-only guard skipped those false
results and was insufficient; that limitation must not be hidden by the
successful hpsort-specific case.

The current analysis is deliberately conditional and conservative:

- The fixed 4096-entry pending array and 4096 unique-instruction limit are
  different limits. Duplicate pending edges can exhaust the former before
  4096 unique instructions are examined. That causes refusal, not a safe
  truncated result. Deduplicating when enqueuing would reduce this false
  refusal without increasing the stated instruction budget.
- The original Gum walk runs first. Its work, allocator behavior, and failure
  handling are not bounded by the supplemental queue limit. The additional
  budget is not a total attach-time or whole-program-analysis guarantee.
- A successful 16-byte candidate with no recognized direct branch in its
  prefix retains Gum's original result through the fast path. This supplement
  does not qualify prefix-only indirect transfers or all possible entrances.
- Indirect tail branches, exception returns, undecodable/unreadable code,
  and an unfinished supplementary walk are refused when visited by that walk. Real targets using
  jump tables, tail calls, or large shared paths can therefore lose coverage.
- Normal calls are not followed into callees; returns terminate paths. This
  assumes conventional call/return control flow. Hand-written LR manipulation,
  coroutine transfers, a callee that jumps into the entry interior, and
  exceptional control flow need separate treatment. In the 8-byte case the
  boundary continuation must be scanned as well as prefix branch targets.
- Explicit instruction classification matters. Capstone
  [issue #2992](https://github.com/capstone-engine/capstone/issues/2992)
  documents missing generic groups for `BC.cond`, `ERET*`, and `DRPS`.
  The guard rejects those explicitly and includes both `B.cond`/`BC.cond`
  encodings in its fast filter. This is not evidence that every future ISA
  instruction is classified correctly.
- Walking outward from the selected entry does not discover all inbound
  transfers from unrelated functions, address-taken interior labels,
  alternate symbol entries, or later JIT/code mutation. Symbols and code
  ranges can reject known overlaps and bound reads; neither establishes
  that undiscovered entries do not exist.
- Shared Gum contexts, replacement hooks, custom emitters, forced relocation,
  and caller-provided devkits need explicit compatibility checks. The default
  backend route does not qualify every invocation of the public relocator API.

An offline estimate on the QE image found 451 names with outward conditional
branches in the first 16 bytes. Simulating the supplementary walk with LLVM
decoding found 16 with a reachable interior destination, 29 ending at an
unresolved indirect/exception transfer, and 406 without a detected conflict.
This suggests substantially less coverage loss than rejecting every such
prefix. It does not measure actual rejections: original Gum eligibility,
scratch selection, canonicalization, Capstone differences, and non-function
aliases were not evaluated by that script.

## Compare the longer-term options

### 1. Repair the integrated CFG and basic-block worklist

This is the preferred foundation. Seed both the selected entry and every
direct transfer from instructions that may be relocated, including backward
targets and targets exposed when a candidate shrinks. Re-evaluate paths when
the candidate boundary changes. Do not use a linear block's `eoi` flag as
proof that all earlier conditional alternatives are finished.

Use separate discovered, queued, and decoded sets, enqueue each PC once, and
charge one explicit global instruction/byte budget. Return a structured result
such as `safe-span`, `interior-target`, `unresolved-transfer`, `unreadable`,
`unsupported-instruction`, or `budget-exhausted`. Only completed analysis can
establish a safe span. Preserve precise source/destination evidence for
diagnostics. Queue ordering must not change a completed result; changing
budget or ordering must not convert incomplete analysis into acceptance.

The exact `ddc10c5` patch can be backported without changing the 17.15.3
layout. Building its source against the pinned amalgamated headers and
replacing the unique `arch-arm64_gumarm64relocator.c.o` archive member is
feasible; this should replace that member's implementation, not merely add
duplicate definitions. Verify the before/after defined and undefined symbol
sets, Frida's renamed GLib/Capstone imports, architecture, and header ABI.
Keep source provenance and license intact. This is an implementation route,
not permission to omit the incomplete-search regressions above.

### 2. Choose a smaller patch for a particular target

Given a proved safe boundary, Gum can select 8 bytes (`ADRP` plus register
branch) or 4 bytes (direct `B`) rather than 16 bytes (load, register branch,
and literal). The pinned backend already has near-slice and deflector paths.
The valid candidate is a joint property of entry bytes, all relocated exits,
scratch liveness, actual executable addresses, and the allocator outcome.
It is not simply the smallest number returned by an entry scan.

The custom-emitter experiment showed that `redirect_space_hint=4` only limits
the emitter's writable budget in 17.15.3: its custom path still obtains a
general slice. The measured emitter target was far outside direct-B reach,
so every target failed attachment. That experiment was withdrawn; successful
application exit with zero instrumented calls was not a successful fix.
An optional near emitter must prove reachable placement or decline without
a wider fallback, and use Gum-owned slice/deflector lifetime management.

Smaller patches also do not establish trampoline dataflow safety. The pinned
[branch rewriters](https://github.com/frida/frida-gum/blob/e5b0d7d6a9a83da1750b6fc654006a714b81a890/gum/arch-arm64/gumarm64relocator.c#L694-L795)
use `X16` for relocated direct/conditional branch exits, whereas the prefix
scratch check can select `X17` for the backend. Selecting `X17` there does not
prove a rewritten branch preserves a live `X16` at its original destination.
Test per-exit liveness and flags, including in-range direct and far rewritten
branches, before recovering those targets. The newer upstream liveness work
is relevant evidence, not an automatic guarantee for the pinned code.

Near allocation is also a deployment concern. [Issue #109](https://github.com/peak-team/peak/issues/109)
records a Gum/UCM break-frontier concern; controlled local experiments
distinguish that reproduced mechanism from an unproved natural QE layout. [PR #107](https://github.com/peak-team/peak/pull/107) intentionally
limited its allocator mitigation. A target-specific 8/4-byte recovery plan
must test that interaction; shrinking a patch must not silently expand the
allocator workaround to every Arm64 process.

### 3. Rewrite destinations or move more code

Another approach copies a larger region and maps old PCs to relocated PCs,
rewriting every internal destination to the appropriate new instruction.
That can support loops whose original destinations lie inside an entry patch,
but only if every relevant edge is redirected. An original-body backedge left
unchanged still reaches patched bytes. Address-taken interior entries, jump
tables, cross-function tails, unwind/exception metadata, PAC/BTI requirements,
and observers retaining old code pointers make this a larger binary-rewriting
problem. Symbol sizes and module executable ranges help bound the problem;
they do not prove a single-entry region.

Static proof should be expressed with explicit assumptions and an unknown
result, not a heuristic renamed as complete. Optional dynamic checks can
record a rejected span, inspect generated bytes, or exercise both paths in a
test. Observing no fault on a training input does not prove an untaken edge
safe. Trapping every interior entrance would modify more code and introduce
signal, stepping, and concurrency obligations of its own; it is not a small
replacement for the current guard.

## Executable next steps and regression matrix

1. Finish the immediate guard's qualification on its actual rebased source
   head: unsafe 16/8 candidates refuse without changing bytes, a safe 8-byte
   fallback really attaches, a safe ordinary target still uses 16 bytes, and
   QE reports positive counts for other targets. Preserve a before/after
   library/source identity. An application-only success is insufficient.
2. Build a separate pinned-source experiment with the exact upstream seed
   patch. Add completion status, backward/eoi coverage, and queue
   deduplication before considering it a replacement for PEAK's second walk.
   Keep the target-skip policy until allocation and trampoline tests justify
   a smaller-span recovery policy.
3. Compare the targeted backport with a full devkit upgrade in a separate
   branch. Audit private layouts and generated exits before changing pins.
   Measure attach time, rejected-target reasons, and report coverage on the
   same target sets; do not infer correctness from a smaller rejection count.

| Regression family | Required observation |
| --- | --- |
| Real QE/hpsort entry and distant alternate return to `+12` | Old classifier demonstrates the omission; mitigation refuses unchanged code; any recovery proves actual patch width and both paths |
| Original `can16=false`, maximum 8 or 12 | Unsafe fallback refuses with maximum zero; safe fallback retains the candidate and actually attaches |
| Interior destination at `+4`, `+8`, `+12`, and exact boundary | Only bytes strictly inside the actual patch are forbidden; boundary continuation is checked |
| Prefix direct B, B.cond, CBZ/CBNZ, TBZ/TBNZ | Forward/backward alternatives, nested alternatives, and prefix ending in unconditional B do not hide a path |
| Queue/CFG limits | Cycles and diamond graphs deduplicate; exact limit and one-over-limit have explicit, reproducible completion/refusal results |
| Unreadable, truncated, unknown, indirect, BC.cond, ERET/DRPS paths | Refusal leaves bytes, contexts, and listener counts unchanged; every reason is observable |
| Calls, returns, LR-reading thunks, exceptional/nonlocal exits | State/return-PC behavior is preserved under the stated ABI assumptions, not only numeric return values |
| Live X16/X17 and per-exit register/flag state | Generated entry, taken branch, fallthrough, and resume paths preserve application state |
| Allocation and custom emitter | Near success/failure, range boundaries, signed pointers, slice cleanup, and no silent wider fallback are verified |
| Aliases, leading B/PLT, overlapping/external entries, JIT lifetime | Attribution and known entry boundaries remain correct; unsupported cases refuse explicitly |
| Controller integration | Real physical detach/reattach restores the exact active/original bytes and metadata for 4/8/16-byte candidates |
| Transport/application scale | Same executable/input on 1 and 140 ranks completes with useful nonzero profiling and correct report scope; neither a fault-free empty report nor classifier-only success qualifies it |

Tests requiring executable Arm64 code must run natively. Host-side Arm64
decoding/classification is useful for deterministic graph and source-backport
comparisons, but it cannot establish instruction-cache behavior, branch reach,
register preservation during execution, or controller safety.
