# Adaptive global budget validation

Latest candidate `5e100d0` is running full hardware validation in jobs 3654962/3654963. Its results are pending; the previous measured candidate failed combined timing/utility acceptance. No final performance pass is asserted.

Earlier measured revision `aa055912` uses funded admission before the first physical hook attachment in global mode, retains instrumentation and the safe controller, fixes the stop/wake ordering race, and exposes report-stage wall/CPU and final-flush/join timing. It remains experimental and opt-in. Targeted hardware job **3654296** completed all 26 launches with valid data, but failed combined performance/utility acceptance; no 5% performance pass is asserted. Its source is exact `git archive` of `aa05591223146834d6ae3ea5695c76317e855331`, archive SHA256 `42881e3d1a5627bd4c5b62004d41b457714ba86a451adf1cf19415bcca9c9bd1`. Historical evidence below is preserved.

## Earlier interim hardware diagnostic

This is diagnostic job 3653573, not final qualification. SPR (actual Slurm partition), two nodes, 32 ranks × six threads; completed in 5:33 with exit 0:0. The frozen source archive SHA256 is `ce97e882f507150fea1d8a0d1be5048fd5895e2fbe37ce9f8a83a15352bc1444`. Candidate library SHA256: `d61ae41ad4a1a5ab13fba4298f0ac58a1f61d6ce9d4976184e6757b487e5e1fd`. Stage: `/scratch/05450/wangyinz/codex-tests/peak-global-budget-final-small-20261028/20261028-110302-small-3d7a7154`. Subsequent fixes are absent from this run. Historical first-prototype records remain unchanged.

Strict fixed-work, oracle, receipt, binding, local accounting and final-cohort checks passed. This establishes data completeness, not a performance pass. Main-through-finalize starts at main entry and excludes loader/constructors and launcher.

| Case | G cumulative work / N | G main-through-finalize / N | L main-through-finalize / N | G selected calls (two runs) |
|---|---:|---:|---:|---|
| allreduce-rotating | 1.0093 | 1.0731 | 1.1361 | 56,802 / 61,013 |
| allreduce-slack | 1.0084 | 1.0525 | 1.0536 | 231,168 / 214,000 |
| bcast-slack | 1.0012 | 1.0476 | 1.0630 | 239,040 / 221,260 |
| iallreduce-critical | 1.0131 | 1.0573 | 1.0763 | 24,744 / 28,187 |
| funded-allreduce-rotating | 1.0285 | 1.0477 | 1.0330 | 1,373,811 / 1,376,619 |

Three global whole-interval means exceed 5%. All five cumulative-work means are within 5%; funded rotating is +2.851%. Its short main phase is 373.691/374.121 ms versus native 38.910/33.999 ms, using earned lifetime credit; this phase ratio alone is not a cumulative-budget violation. Native rotating main varies from 34.787 to 55.700 ms, so small-stage ratios have substantial baseline variation.

The funded whole-interval mean increases by 664.748 ms: cumulative work +350.673 ms, MPI-init boundary +31.517 ms, MPI-finalize boundary +282.067 ms, remaining gaps +0.491 ms. Global rank0 PEAK-fini samples are 262.259/294.751 ms: report 97.367/123.366 ms, release 59.229/67.623 ms, and final settlement 103.109/101.695 ms. Original MPI_Finalize is 1010.694/1012.664 ms. Bootstrap is only 11.404/11.543 ms and activation 2.35/2.45 microseconds. These measurements identify teardown/report/settlement as material costs, without identifying a deeper cause inside those stages.

All primary global runs report zero emergency fallback. Allreduce-slack G1 reports one rank with runtime degradation; other primary runs report none. Funded global grants reach all 32 ranks, but neither run reattaches. Rank0 final conservative spent/budget for funded G0 is 0.759122/0.674261 seconds, so this run does not establish a hard conservative ledger ceiling.

Both independent recovery diagnostics have zero proven participant-DONE → physical restore → new-call chains. The short diagnostic records a real participant DONE on rank0 but no physical restore; funded records participant DONE without restore. Recovery acceptance remains outstanding. Service trace rings may overwrite earlier events, and monotonic clocks differ across nodes; no cross-node timestamp subtraction is valid.

Across all five cases, each local arm contains 32 `./pressure` reports and four `/sbin/modinfo` reports (the latter have zero targets). Each global arm contains only 32 `./pressure` reports. Neither mode reports a shell. The `PEAK done with:` headers directly demonstrate suppression of those modinfo reports in global mode; they do not isolate how much of the MPI-init timing difference is due to suppression.

Reproduce analysis with:

```sh
python3 benchmarks/mpi_rank_budget/global_analyze.py /tmp/peak-global-small-results/results --set small --require-final-members --require-diagnostics > /tmp/peak-global-small-summary.json
```

The compact companion `global_validation/interim-adaptive-summary.json` retains both samples, per-rank coordination summaries and stage decomposition. Final qualification requires the separately frozen corrected source, full synchronization matrix, useful profiling coverage, and actual recovery evidence.

## Earlier ac5 local gate and hardware plan

Candidate commit `aa6f36d` uses global-budget source SHA256
`ac5ba6bf536f108aea45d1f78974ade9b2591c72efd403bb58969ceb8668d32c`
and header `6f4707dff6ec1bac2f52a52530b1e2ea4a5f7d825195653b878ab09d883d507a`.
The exact local build library SHA256 is
`bcedcf9aa355ea1b7265c3d25f5a16bf3074db885800c3552fc49d957f9b902b`;
its per-file provenance is `/tmp/peak-global-validated-bcedcf9aa355-9nmobr9q/PROVENANCE.json`.
The implementation owner completed 56/56 CTests (61.86 seconds), static checks,
the five real-MPI functional cases, skewed finalization and cold/slack lifecycle
checks. Cold/slack observed rank0 detach/recovery with 16 restored and 12 revisited
targets, while seven cold ranks performed no detach; all ranks confirmed final
cohort settlement. These local functional results do not establish hardware
slowdown or universal absence of emergency fallback.

The earlier hardware plan submitted matrix job **3654125**, covering five MPI modes
× four layouts × six matched arms, plus two independent recovery diagnostics,
and funded job **3654127**, covering five single-thread and five six-thread concurrent
funded cases × six arms. Selected threads use arithmetic-identical twins, with
independent per-thread work/oracle checks. Workload source SHA256:
`726dc38ff93d40255304ce7d72a88549d660376e2a1334daa7743d2300c9b0ae`.
Matrix/funded source archive SHA256 values are respectively
`3a539f50bf1b3dc3425b30929196f2bc6c3e43d98da94f781aab8559a56a91f3`
and `2ba0d970491aebe68d5f40d4a15dfbb59c6f6d7bd33372c2690299edd244a76f`;
production file hashes match across archives. All workload builds explicitly use
`-fcf-protection=none` and pass the sixteen-NOP target-entry check.

Those jobs subsequently failed before completing the plan, as documented in the next section. Replay their partial result directories separately with the existing CLI choices `--set matrix --require-final-members --require-diagnostics` (20 cases) and `--set funded --require-final-members` (10 cases, including concurrent cases). Missing and failed receipts remain errors. Available samples, MPI stages, calls/coverage and counters are preserved below. Timing comparisons use cumulative work and main-through-finalize alongside the short main phase; six-thread and single-thread cases remain separate.

## Incomplete ac5 hardware candidate: preserved failures

Both candidate jobs exited 124: matrix 3654125 ran on PVC and funded 3654127 on
SPR. Matrix first failed at launch 59 (`bcast-slack/L1`); funded first failed at
launch 28 (`funded-iallreduce-rotating/G1`). Both failed rank-log directories are
empty. The preceding arms each have all 32 original MPI_Finalize calls returned.
This locates the first visible failure before wrapper exec/redirection; it does
not demonstrate a PEAK callback, credit or final-settlement deadlock. Launcher
failure attribution remains under investigation. No missing arms are inferred
or counted as passing. No concurrent six-thread funded case was reached.

Strict analysis retains nine complete matrix configurations and four complete
funded configurations, while the full requested datasets remain **invalid due
to failed/missing receipts**. Their compact measured data and exact errors are in
`global_validation/partial-ac5-adaptive-summary.json`. All 26 complete global
samples have final cohort acknowledgment from all 32 ranks, no invalid accounting,
no degraded runtime, no emergency fallback and no rejected-spend count. Some
main arms have actual reattach/revisit activity, but trace-disabled observations
do not prove the strict participant-DONE → restoration → new-call chain; neither
independent diagnostic was reached.

| Complete matrix case | G cumulative work / N | G main-through-finalize / N |
|---|---:|---:|
| allreduce-rotating | 0.62674 | 0.81667 |
| allreduce-slack | 0.60997 | 0.70219 |
| allreduce-critical | 1.06423 | 1.10012 |
| allreduce-aligned | 1.00471 | 1.04145 |
| barrier-rotating | 1.03083 | 1.07978 |
| barrier-slack | 0.99454 | 1.07588 |
| barrier-critical | 1.07786 | 1.08444 |
| barrier-aligned | 1.04378 | 1.07111 |
| bcast-rotating | 0.98552 | 1.14824 |

The first two rows have large native variability (rotating whole 3.849/6.610 s,
slack 13.505/6.672 s) and are not evidence of a speedup. Six complete matrix
whole-interval means exceed 5%, and two cumulative-work means exceed 5%.
Bcast-rotating provides a clear whole-interval failure: native 3.961929/3.998074 s,
global 4.624736/4.515226 s. Global release takes 591.324/152.261 ms, report
72.594/105.916 ms and final settlement 102.578/103.564 ms. Across all completed
global arms, rank0 final settlement remains approximately 101–106 ms; the
hardware data do not reproduce the roughly 1 ms local settlement result.

The four complete single-thread funded cases have cumulative-work ratios
1.01834/1.01667/1.01799/1.01844 and whole-interval ratios
1.03920/1.03868/1.04471/1.03875 (allreduce/barrier/bcast/ring). They profile
roughly 0.69–0.87 million calls per run versus 4096 in local mode. These useful
coverage observations are separate from the unexecuted six-thread validation.
Some short cases' final conservative spent exceeds budget, for example
bcast-rotating G0 0.246476/0.175041 s; all eight complete funded samples retain
spent below budget. The evidence does not establish a universal hard ledger or
slowdown ceiling.

Verified result archive SHA256 values are
`597c726eb97a1e850dcd2245d25e2219891f9b6e84aedb86e1ec1bec33970a71`
(matrix) and `454c1e2b12a113326260dfd1935f643311b2aafa2bb505461aded7b8631403da`
(funded). Actual candidate library SHA256 values are respectively
`a59387742f7a93bd22b2844740f563c23141b9584a6298f9aa965be4cf66c398`
and `c4e2f9e44056606a0a7980c6d84b35e3721ad7cb562dc542c446d8d62c0e9e80`.
All recorded ratios compare matched arms within their own job/partition.

## aa055912 local and controlled causal evidence

The exact local library is `2d4047c90dd5d112db500af145b784da93ba152b70d6a5a7d9eab3902d01aca6`. Per-file source provenance and four real-MPI receipts are retained in `global_validation/aa055912-local-summary.json`. Four two-rank × two-thread fixtures returned zero with work oracles intact: paid initial admission, strict recovery, deliberately unfunded operation, and deliberate transport fallback. Recovery proves participant-DONE → physical restore → new calls on both ranks; normal recovery and initial-admission runs confirm both final settlements. The intentionally unfunded run has spent above its effectively zero budget and is a functional admission test, not a performance-budget pass. Deliberate fallback records degradation and unconfirmed settlement as expected, rather than fabricating success.

The completed local gate covers **57 unique tests across two fresh batches**, not one 57/57 invocation: 27 PEAK integration/report tests plus 30 freshly built standalone protocol tests. The initial broad invocation could not find the newly added stop-wake executable; the fresh standalone batch subsequently executed it and all 30 protocol tests passed. These gates establish functional coverage, not cross-node performance.

Controlled old/new transport experiments reproduce the ac5 stop/wake race without changing FINAL requirements or budget rules. With deterministic scheduling, old join is 100.247 ms versus new 0.083 ms. With 32 clients, each main/helper pinned to the same CPU, an injected 1 ms post-wake scheduling delay and final rank delayed 150 ms, old root flush/join is 151.993/100.220 ms; new is 152.291/1.097 ms. With all 32 ready, new flush/join is 2.167/1.100 ms. The retained manifest includes exact argv, source hashes and bounded timeout commands. The old source hash exactly matches ac5. These controlled results isolate the race and preserve the genuine late-FINAL wait; actual HPC timing still requires job 3654296.

The targeted hardware fixture holds original work parameters and bindings fixed: bcast-rotating, barrier-critical, barrier-aligned and funded-concurrent-allreduce-rotating, each N/L/G/G/L/N, plus two independent recovery diagnostics. Every launch now has separate launcher stdout/stderr, before/after receipts and a wrapper entry marker before PEAK preload selection. This distinguishes pre-wrapper launcher failures from loader, application or finalization failures without blindly replaying an uncertain launch.

A later local check found a known performance defect in this aa055912 candidate:
its first paid set of 16 hooks can split into physical batches of 15 and 1.
At an exact 6,000,000 ns boundary, floating-point eligibility loses approximately
3.79e−10 ns of headroom and rejects the final hook. The actual cash ledger remains
integer; a correction to admission eligibility is being tested separately.
The existing strict restore/revisit proofs remain real, but they do not prove
single-batch initial attachment. Job 3654296 continues to measure this exact
candidate unchanged and must retain this qualification.

## aa055912 targeted hardware result (3654296)

All 26 launches completed on PVC in 4:34 with exit 0:0. Strict workload/oracle, receipt, 32×6 binding, local accounting and final-cohort checks pass (`valid=true`). Verified result archive SHA256 is `aac08a10e0fec96c40ef478f76e84f8c09d066bfd5e8a77487865d2adc1f972d`; actual library SHA256 is `665b4ea073277b43a0ac1b5e7d580bc1c7e133bf8893b170ee53149cbc053922`. The compact companion `global_validation/aa055912-targeted-summary.json` retains both samples, exact errors, local totals, global coordination, new stage timings and strict recovery proofs. This is the known 15+1 candidate, not a measurement of its subsequent integer-admission correction.

| Case | G main / N | G cumulative work / N | G main-through-finalize / N | G calls (two samples) |
|---|---:|---:|---:|---|
| bcast-rotating | 1.04277 | 1.08959 | 1.08490 | 486 / 0 |
| barrier-critical | 1.00854 | 0.99175 | 1.01526 | 39,711 / 81,215 |
| barrier-aligned | 0.98363 | 1.02810 | 1.04068 | 0 / 0 |
| funded-concurrent-allreduce-rotating | 3.48470 | 1.02864 | 1.03047 | 334,164 / 333,961 |

Bcast-rotating fails the timing target and has almost no useful profiling. Both aligned runs record zero selected calls despite 46/15 reattachments and 34/37 control windows. Their acceptable wall ratios therefore do not establish acceptable profiling utility. Funded concurrent calls are only about 3% above local's 324,185/324,132, while global measured control sums rise to 0.367260/0.363442 seconds from local's 0.090349/0.090379. Both concurrent runs report 27 emergency ranks, zero degraded ranks and 58/54 root waves. Emergency counts alone are not classified as a universal correctness failure, but the additional reconfiguration and ledger evidence require diagnosis.

All main global runs confirm final settlement. Pre-report and final conservative spent can exceed budget: concurrent G0 pre-report 0.725658/0.657474 seconds and final 0.750794/0.665195; G1 pre-report 0.732097/0.638819 and final 0.758145/0.647681. Bcast G1 final is 0.155833/0.152247. Other main samples retain spent below budget. No universal hard budget ceiling is proven.

The stop/wake fix is confirmed on hardware: rank0 stop/join takes 0.0717–0.1710 ms, final flush 1.10–5.31 ms and total settlement 1.31–5.50 ms; the former 100 ms floor is absent. CSV publication is now the dominant report stage: global rank0 wall 50.5–180.4 ms but CPU only 0.237–0.288 ms, while capture is roughly 30 microseconds, prepare 5 microseconds and text 60 microseconds. This isolates waiting/file-publication cost rather than formatter CPU, without assigning the wait to a particular filesystem operation.

The independent funded diagnostic proves participant-DONE → physical restoration → fresh same-hook calls on 15 ranks. The short diagnostic proves none: physical restoration events exist, but the required later same-hook calls do not. This distinction is preserved. All local arms emit 32 modinfo and 32 pressure reports; global arms emit only 32 pressure reports, confirming runtime-utility report suppression. These findings improve the causal evidence but do not constitute final full-matrix acceptance.

The zero-call cases have different admission causes. Aligned's hot rank is always rank0. In G0 it received exactly 6,000,000 ns, attached no hooks, consumed no control credit and returned all 6,000,000 ns: this supports the independently reproduced integer-boundary admission defect. In G1 hot rank0 received no grant; meanwhile 19 cold ranks first attached 15 hooks and one attached 16. Bcast G1 gave no grants and no initial attachments to any rank. Its pre-report spent/budget of 129.369/145.901 ms, after the fixed 16 ms world-wide grace reserve (not a registered closing-cost reservation), leaves only 0.532 ms, below a 6 ms initial lease; root startup alone is 101.244 ms. Correcting exact-ns eligibility therefore cannot by itself ensure useful coverage in every budget-starved short case. Main-arm event tracing is disabled, so the archive does not establish first-grant/installation timing relative to warmup versus main; diagnostic arms are not substituted for that missing observation.

## Current 5e100d0 full rerun (results pending)

The corrected-source fixture is frozen at 5e100d0 and submitted in jobs 3654962/3654963. Matrix
contains the original 20 workload configurations, two additional complete
bcast-rotating repeats, and two diagnostics (134 launches). Funded contains five
single-thread and five six-thread concurrent configurations (60 launches).
There are 30 unique workload configurations, with 32 independently matched
six-arm groups after repeats. Parameters, fixed integer work and output
locations remain unchanged. Each launch retains entry and launcher receipts.

`global_analyze.py --set matrix22` preserves the original 20-case and targeted
replay mappings while including both repeat groups. Its bcast pooled result
contains all six native, six local and six global samples, mean and range, with
each original group retained; repeats cannot selectively replace earlier samples.
`--set final32` accepts a combined results tree with both matrix and funded groups.
No result or performance claim is recorded for this pending fixture.

The corrected-source full rerun is now submitted against exact commit
`5e100d0453ac99cf1a0d54184ed4cf337d30c3a8`: matrix job **3654962** (134 launches)
and funded job **3654963** (60 launches). Both source archives have SHA256
`9c1e3b18fe65eb882ae9546b3ebb96f0dc6180210551e2e571bb7bfa0d23ff03`;
input-manifest SHA256 is
`084aa4c58fb9cac8816d6e65398f657f332ae34c40180766c39afa90fa047c0a`.
All 152 inventoried source files match `git show` of that commit; test-only
working-tree changes were not overlaid. Results and actual partitions remain
pending verified raw archives. Submission does not imply performance acceptance.
