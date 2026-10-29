# Adaptive global budget validation

Current candidate `5afdccb7` has passed 78/78 selected local tests and five affected actual-MPI functional cases with raw emergency counts zero; this is a functional gate, **not a performance pass**. Full hardware jobs 3655586/3655587 are submitted and pending results. All earlier failed measurements and intermediate diagnostics below remain preserved.

Interpretation throughout this record uses three distinct scopes. The budget is
the coordinator's reported lifetime bank: credited original MPI_Init time plus
service lifetime can earn budget, including native intervals outside measured
work stages. S/R/D and any startup or settled-control debt are examined against
that bank and subsequent grant decisions. Observing S above B records debt;
without issuance/settlement evidence it does not by itself identify an incorrect
grant or an unbounded-growth bug. Raw per-rank control costs remain summed.

Main-phase and warmup+main ratios describe fixed-work subintervals. Both can
spend credit earned during earlier MPI initialization or other native gaps, so
a subinterval ratio above 5% is not itself a lifetime-budget violation. All
predefined ratios and threshold flags remain unchanged as observations.
Exec-inclusive elapsed measures the real reported user interval and retains
startup/preparation and CSV/report wall costs; a real whole-interval increase
above 5% is not hidden by the ledger model or by moving cost before main.

Useful profiling coverage and independent long-case recovery are assessed
separately. No universal restoration or coverage minimum is invented for every
short case, and short-stage behavior alone does not refute instrumentation.


Candidate `5e100d0` completed all 194 hardware launches with valid data, but **did not achieve the combined overhead, useful-coverage and recovery target**. The subsequent escrow correction `dd1d7e9` also completed its separate 32-launch diagnostic with valid data and did not establish acceptance. Several MPI configurations exceed 5% and the independent hardware recovery diagnostics show no restoration. The complete results below retain all repeats and earlier failures.

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

## 5e100d0 full rerun submission plan

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
This paragraph records the submission plan; completed measurements follow below.

The corrected-source full rerun is now submitted against exact commit
`5e100d0453ac99cf1a0d54184ed4cf337d30c3a8`: matrix job **3654962** (134 launches)
and funded job **3654963** (60 launches). Both source archives have SHA256
`9c1e3b18fe65eb882ae9546b3ebb96f0dc6180210551e2e571bb7bfa0d23ff03`;
input-manifest SHA256 is
`084aa4c58fb9cac8816d6e65398f657f332ae34c40180766c39afa90fa047c0a`.
All 152 inventoried source files match `git show` of that commit; test-only
working-tree changes were not overlaid. At submission, results and actual partitions were pending. Verified completed measurements and actual partitions are recorded below.

The latest completed local gate matches production 5e100d0: **59/59** freshly
built selected CTests pass (61.96 seconds), with static and diff checks passing.
The local library SHA256 is
`808cbd067af6023c3db3c4fb25c5492128100ee7804320d73868a39f6d779e93`.
Six actual MPI cases pass their mode-specific checks using test-only runner
revision 8db8908; all latest raw emergency counters are zero, with confirmed
settlement except deliberately injected transport fallback. Strict participant
DONE/restore/new-call checks remain in force for recovery. Multiwindow coverage
proves all 80 first attachments with physical batches `[64,16]` and real calls.
The physical API clears the recovery target before its kind7 event, so that trace
is not represented as proof of an exact numeric target. Original failures and
subsequent classification files remain separately preserved. Detailed source
hashes and receipts are in `global_validation/5e100d0-local-summary.json`.
These are functional results, separate from the hardware performance measurements below.

## 5e100d0 funded hardware result (3654963)

All 60 launches completed on SPR in 16:01 with exit 0:0. Strict data validation passes for all ten configurations, including the five six-thread concurrent cases. The detailed compact record is `global_validation/5e100d0-funded-summary.json`; the complete matrix result follows below and the combined assessment is not a performance pass.

| Funded case | G cumulative work / N | G main-through-finalize / N | G calls (two runs) | G target-rank coverage (two runs) |
|---|---:|---:|---|---|
| allreduce-rotating | 1.002003 | 1.040522 | 10,557 / 512 | 6 / 1 |
| barrier-rotating | 1.003020 | 1.021005 | 6,987 / 12,573 | 4 / 7 |
| bcast-rotating | 1.002205 | 1.023426 | 7,011 / 10,634 | 4 / 6 |
| ring-rotating | 1.002951 | 1.027372 | 10,546 / 8,640 | 6 / 5 |
| iallreduce-rotating | 1.002298 | 1.024895 | 10,163 / 10,552 | 6 / 6 |
| concurrent-allreduce-rotating | 1.002040 | 1.020723 | 22,768 / 10,256 | 2 / 1 |
| concurrent-barrier-rotating | 1.003989 | 1.023637 | 22,818 / 10,064 | 2 / 1 |
| concurrent-bcast-rotating | 1.001850 | 1.027982 | 22,828 / 22,767 | 2 / 2 |
| concurrent-ring-rotating | 1.002867 | 1.034544 | 10,064 / 10,285 | 1 / 1 |
| concurrent-iallreduce-rotating | 1.001923 | 1.034841 | 10,256 / 34,418 | 1 / 3 |

All ten cumulative-work and whole-interval means are within 5%, with maximum whole ratio 1.040522. This timing result comes with limited utility: single-thread target-rank coverage is 1–7 versus local 32, concurrent coverage 1–3 versus local 32. Concurrent global records about 10–34 thousand calls versus local about 324 thousand. Every global sample has zero reattachment/revisit and one root wave; most pay for all 32 initial sets and then close them, with roughly 64 windows versus local 32. These observations prevent treating the wall ratios alone as satisfactory profiling coverage.

There are six emergency events in five of 640 global rank-samples: concurrent allreduce G0 rank1 has two; concurrent barrier G1 rank0, bcast G0 rank1 and ring G0/G1 rank0 each have one. Runtime degradation is zero throughout. The events remain diagnostics requiring funding/deadline context rather than a universal per-case rejection rule. Concurrent-ring G0 has pre-report spent/budget 0.690856/0.645635 seconds and final 0.724837/0.656580; all other 19 global final samples retain spent below budget. Raw control remains charged by sum, with wave span diagnostic only.

Verified archive SHA256: `03e80e5cab168dd43b7b6bfad24af77dfcbdaa82ce610cee6f86bafd8424c962`; actual library SHA256: `6c745149d2b71a81c1f93e7c6414e93a55108f1a4393f3f6780b7074ccda0da6`. Source is exact 5e100d0 archive recorded above. All comparisons use matched native arms within SPR; they are not pooled with earlier partitions or sources.

## 5e100d0 complete matrix result (3654962)

All 134 launches completed on PVC in 17:49 with exit 0:0. Matrix22 and funded10 strict validation both pass: all 194 real launcher receipts, fixed-work/oracle checks, bindings and final cohort settlements are complete. Valid data is distinct from satisfying the overhead and useful-profiling target. The compact records are `global_validation/5e100d0-matrix-summary.json`, `5e100d0-funded-summary.json` and `5e100d0-final-overview.json`.

| Matrix group | G main / N | G cumulative work / N | G main-through-finalize / N | G calls (two runs) |
|---|---:|---:|---:|---|
| allreduce-rotating | 0.07931 | 0.67099 | 0.81883 | 451 / 899 |
| allreduce-slack | 0.74934 | 0.73109 | 0.80141 | 2,496 / 3,304 |
| allreduce-critical | 0.98986 | 0.99776 | 1.00945 | 72,338 / 32,303 |
| allreduce-aligned | 1.00840 | 0.97842 | 1.02695 | 6,666 / 7,599 |
| barrier-rotating | 0.87598 | 0.92014 | 0.98761 | 327 / 0 |
| barrier-slack | 1.05769 | 1.08423 | 1.09368 | 10,000 / 0 |
| barrier-critical | 1.02724 | 1.06267 | 1.05311 | 10,854 / 15,759 |
| barrier-aligned | 1.03681 | 1.01600 | 1.04556 | 0 / 810 |
| bcast-rotating | 1.22308 | 1.03175 | 1.05680 | 0 / 0 |
| bcast-slack | 0.99947 | 0.99342 | 1.03492 | 0 / 3,237 |
| bcast-critical | 1.01109 | 1.01022 | 1.04620 | 120,706 / 33,205 |
| bcast-aligned | 1.01564 | 1.02347 | 1.04642 | 18,470 / 7,678 |
| ring-rotating | 2.99677 | 1.03033 | 1.07149 | 6,000 / 0 |
| ring-slack | 1.00204 | 1.00067 | 1.04368 | 0 / 0 |
| ring-critical | 1.00210 | 1.00923 | 1.05492 | 16,596 / 35,824 |
| ring-aligned | 1.00979 | 0.98830 | 1.03405 | 0 / 18,832 |
| iallreduce-rotating | 1.42168 | 0.99704 | 1.04254 | 0 / 1,244 |
| iallreduce-slack | 1.02716 | 0.99883 | 1.02357 | 23,936 / 0 |
| iallreduce-critical | 1.18245 | 1.11552 | 1.12574 | 42,212 / 89,611 |
| iallreduce-aligned | 1.01304 | 0.98529 | 1.02334 | 640 / 0 |
| bcast-rotating-repeat1 | 1.83297 | 1.06210 | 1.07717 | 839 / 2,535 |
| bcast-rotating-repeat2 | 1.10178 | 0.93093 | 1.00730 | 3,011 / 0 |

Six of the original 20 matrix configurations exceed 5% in main-through-finalize mean; barrier-slack, barrier-critical and iallreduce-critical also exceed 5% in cumulative-work mean. Native outliers in the first allreduce groups make ratios below one unsuitable as speedup evidence. No samples are dropped.

The prespecified bcast-rotating aggregation uses all six native and six global samples, with each matched group retained. Native whole mean/range is 3.909891 s / 3.777640–4.033835 s; global is 4.093578 s / 3.900782–4.311411 s, ratio **1.046980**. Cumulative ratio is **1.007759**. The base group alone is 1.05680 and repeat1 is 1.07717; selecting only repeat2 (1.00730) would be misleading. This pooled timing result does not erase its low sampling coverage.

There are 14 zero-call global samples among 44 matrix samples, including both ring-slack samples. Every matrix global emergency and degradation counter is zero. Both independent recovery diagnostics fail to demonstrate recovery: short has zero calls, zero restoration and wave0; funded has 5064 calls, zero restoration and wave1. Neither has kind6 restoration events or controller overflow, so missing event capture does not explain the absence of proof. Local functional recovery success is preserved separately and does not substitute for these hardware observations.

The worst whole-interval mean is iallreduce-critical, 1.12574. Native samples are 6.877873/7.065434 s and global 8.517381/7.179202 s. Global G0 main is 4.183878 s versus native 3.029311/3.076828 s; global G1 main is 3.036323 s. G0 profile/control sums are 0.007976/0.143065 s, while rank0 finalization includes CSV 183.919 ms, release 140.407 ms and settlement 2.324 ms. These observations expose phase variability and reporting costs but do not identify the exact cause of the roughly one-second phase outlier without event evidence.

Final coordinator spent exceeds reported budget in 20 of 44 matrix global samples and one of 20 funded global samples. Ring-rotating G1 is 0.321826/0.158367 s despite zero selected calls. Coordinator cost continues to include summed actual per-rank control costs; wave span remains diagnostic and is not substituted or discounted. The records therefore establish neither a universal ledger ceiling nor a universal measured slowdown ceiling.

Verified matrix archive SHA256 is `b0cdeb486a131a2fb4445dfa4d1636d1b04ea1fa18bbcff3540ad8ae2a40f938`; actual matrix library SHA256 is `2b70c2f8eddf560f9a7b4ccba1d540a8c3e266f91d0f8dbfee71141ea2df3664`. Source remains exact 5e100d0 archive above. Compare paired arms within each partition; do not pool PVC matrix and SPR funded timing samples.

Reproduce the complete strict analyses with:

```sh
python3 benchmarks/mpi_rank_budget/global_analyze.py /tmp/peak-global-final-matrix-results/results --set matrix22 --require-final-members --require-diagnostics > /tmp/peak-global-final-matrix-summary.json
python3 benchmarks/mpi_rank_budget/global_analyze.py /tmp/peak-global-final-funded-results/results --set funded --require-final-members > /tmp/peak-global-final-funded-summary.json
```

## Subsequent control-escrow correction (hardware validation pending)

After preserving the complete 5e100d0 results, a separate ledger defect was
identified: old spent included prepaid control escrow and retained that value
through a high-water maximum. A 3 ms actual control cost with an unsettled 3 ms
prepayment could therefore report 6 ms; after the 3 ms settlement watermark
arrived, the retained spent could remain 6 ms. This permanently counted the same
control work twice and could suppress later grants and recovery.

The correction under separate validation keeps spent based on actual cost and
holds unsettled control prepayment in the global reserved balance, where it can
be released when actual-cost settlement is proven. Local reservation and whole
grant rules remain unchanged; there is no wave-span discount. This is a ledger
correction, not a retroactive alteration of any 5e100d0 measurements or proof of
meeting the 5% target. Fresh local gates and a new exact-source hardware fixture
are required before assessing its effect.

A separate targeted fixture is prepared in
`/tmp/peak-global-budget-escrow-targeted-20261028`: bcast-rotating,
barrier-critical, barrier-aligned, funded-concurrent-allreduce-rotating and
iallreduce-critical, each six matched arms, plus the two original recovery
diagnostics (32 launches). Work, 16 targets, warmup, epoch/call counts, output
locations and launcher/entry receipts remain unchanged. At preparation, execution was guarded until the approved commit was frozen; the submitted exact source is recorded below. Analyze it with
`--set escrow_targeted --require-final-members --require-diagnostics`; no new
performance pass is asserted here.

The separate escrow fixture is submitted as job **3655082**, frozen at exact commit
`dd1d7e961f39efbb2051a0d49337ce2e3da5b246`. Its source archive SHA256 is
`b1312e8210925a694ddc2c7e37a22a42b600f2e326efbfabc5b65751c6425ebe`,
input-manifest SHA256 is
`23e5bf68c864e8bd29f9490fb40789048658b81669a9fa3230a290f52ec87c37`.
All 153 inventoried files match the commit, including `test_control_escrow.c`.
This submission tests the reserved-balance correction separately; the 194-run
5e100d0 outcome above remains unchanged.

## dd1d7e9 escrow hardware diagnostic result (3655082)

All 32 launches completed on PVC in 5:39, exit 0:0. Strict data validation passes; the recorded dataset is `global_validation/dd1d7e9-escrow-summary.json`. This tests the escrow correction independently and does not replace any 5e100d0 result.

| Case | G main / N | G cumulative work / N | G main-through-finalize / N | G calls (two runs) |
|---|---:|---:|---:|---|
| bcast-rotating | 1.195471 | 1.056125 | 1.140711 | 0 / 0 |
| barrier-critical | 1.008792 | 1.002465 | 1.008926 | 15,922 / 15,918 |
| barrier-aligned | 0.865650 | 0.885030 | 0.858213 | 0 / 0 |
| funded-concurrent-allreduce-rotating | 1.403887 | 1.042662 | 1.070654 | 10,616 / 22,612 |
| iallreduce-critical | 1.093106 | 1.063835 | 1.130603 | 27,999 / 21,015 |

Three of five whole-interval means exceed 5%; bcast-rotating and iallreduce-critical also exceed 5% in cumulative work. Aligned native samples are unusually slow (9.082290/8.038202 s versus global 7.297735/7.395292 s), so its ratio below one is not a demonstrated speedup. Both global aligned runs record zero calls, as do both bcast runs; bcast has zero first attachments across all 32 ranks in both runs. The ledger correction alone has not restored useful profiling coverage in these cases.

All ten main global samples have zero emergency, degradation and invalid-accounting counters. Actual reattach/revisit occurs in barrier-critical (16/2 and 48/7) and iallreduce-critical (0/0 and 32/10), but the two independent trace-enabled diagnostics still prove no recovery: short has zero calls/restoration; funded has 2924 calls and zero restoration. Per-case restoration is not universally required, but these diagnostics do not supply the intended hardware recovery proof.

Four of ten final global samples retain spent above budget: bcast G0 0.208814/0.183017 s, bcast G1 0.169683/0.161515 s, barrier-critical G1 0.321960/0.314248 s and iallreduce-critical G1 0.324958/0.321483 s. All pre-report and final reserved balances are zero after retirement; this does not establish that no escrow existed while the application was active. In this revision unsettled control prepayment belongs in global reserved, while spent reflects actual costs; control costs remain summed across ranks without a wave-span discount.

Verified result archive SHA256: `e2dbd4d67705c9ebbed75b0e932dd3e13167a3eb0278508bdc121d63a5dbab31`; actual library SHA256: `806f9cbaf78644045d27047436d4283ca67456c127d369c55864256566aff178`. The exact dd1d7e9 source archive is recorded above. All original parameters and independent oracles remain unchanged; valid receipts and lower accounting estimates are not treated as proof of a performance pass.

## Activation-placement control preparation

The next diagnostic explicitly gives both local and global arms the same activation policy within each group: immediate or post-init. Five unchanged workloads per policy, each N/L/G/G/L/N, plus both recovery diagnostics per policy yield 64 launches. An independent native `exec_meter` starts without LD_PRELOAD or LD_AUDIT, records monotonic time, sets the child's preload/policy environment and execs the workload. The pressure program records exec-start elapsed alongside its existing main/phase times, using a shared final endpoint after the original MPI_Finalize and its timestamp. This captures loader/constructor placement without profiling the meter itself; it excludes launcher/wrapper and the meter's own loader/entry print.

Six bounded local two-rank × two-thread smoke runs (native/local/global under both policies) pass work oracles, empty-preload proof, target-byte checks and exec duration ≥ main duration. Missing meter records are rejected. The receipt is `global_validation/activation-meter-local-smoke.json`. These functional smokes are not performance estimates. The hardware package at `/tmp/peak-global-activation-probe-20261028` is submitted as job3655370 against exact commit `5c4e90dbf0eb4ae5603a79df04ee2f00e3921a41`; runtime production files remain unchanged. Source archive SHA256 is `fdd9ccaaef09791cbd34ed0df2b0605e8214fd57b662c91a757b535d5ad1124b`, with input-manifest SHA256 `4a70249589d44c2c4ddef64d316c8caa4e5310021c15a9e4dd48ab61bffcbf57`. Completed measurements follow below; submission alone did not establish acceptance.

## Activation-placement result (3655370, unchanged c43 runtime)

All 64 launches completed on PVC in 10:38 with exit 0:0. Strict validation passes, including every rank's meter interval, empty-preload entry proof and explicit policy. The compact record `global_validation/5c4e90d-activation-summary.json` retains all paired samples, stage decompositions, rank costs/calls, and actual recovery proofs. Local and global arms use the same explicit policy within each group; native pairs are measured independently.

| Policy / case | G main / N | G cumulative / N | G main→Finalize / N | G exec-inclusive / N | L exec-inclusive / N | G calls |
|---|---:|---:|---:|---:|---:|---|
| immediate-bcast-rotating | 2.736730 | 1.050200 | 1.151395 | 1.161476 | 1.133446 | 52,574 / 51,645 |
| immediate-barrier-critical | 1.025530 | 1.063847 | 1.075761 | 1.080924 | 1.094937 | 5,479 / 4,601 |
| immediate-barrier-aligned | 0.956798 | 0.963145 | 0.993580 | 0.999145 | 1.047076 | 64 / 64 |
| immediate-funded-concurrent-allreduce-rotating | 3.163812 | 0.995080 | 1.009038 | 1.012116 | 1.028794 | 381,124 / 366,488 |
| immediate-iallreduce-critical | 1.032881 | 1.036056 | 1.048008 | 1.053754 | 1.066974 | 4,908 / 4,387 |
| post-init-bcast-rotating | 0.729111 | 1.046733 | 1.092343 | 1.092643 | 1.071699 | 768 / 919 |
| post-init-barrier-critical | 0.997409 | 1.005595 | 1.040107 | 1.040753 | 1.019709 | 36,557 / 15,123 |
| post-init-barrier-aligned | 1.000743 | 0.971482 | 1.027871 | 1.028028 | 1.080891 | 0 / 738 |
| post-init-funded-concurrent-allreduce-rotating | 1.203483 | 0.966622 | 0.985532 | 0.985882 | 1.015583 | 10,576 / 22,904 |
| post-init-iallreduce-critical | 1.029512 | 0.992027 | 1.026118 | 1.026700 | 1.045418 | 38,586 / 17,508 |

Immediate global group-mean pre-main elapsed is 41.9–47.0 ms versus native 2.4–4.9 ms; its MPI-init line reports roughly 3 microseconds of activation because physical preparation has already occurred in constructors. Post-init global group-mean pre-main elapsed is 4.35–7.40 ms, while activation of 74.3–80.0 ms is inside main. The cost has not disappeared: this is placement plus a measured change in initialization duration, not proof of an equal-cost transfer. Main-only comparisons miss pre-main cost. For example immediate iallreduce-critical is +4.80% by main-through-finalize but +5.38% by exec-inclusive time. Four global exec-inclusive group means exceed 5%.

The main phase, cumulative work and whole interval answer different questions. Immediate funded concurrent has main ratio 3.16 but cumulative ratio 0.995 and exec ratio 1.012: its short main slowdown alone does not refute the existing instrumentation approach or violate a cumulative-runtime limit. It records 381,124/366,488 calls versus local 324,171/327,875, with target-rank coverage 33/32 versus local 32/33. This useful profiling coverage comes with 15/9 emergency events across 9/8 ranks, which are retained without treating all emergency counts as universal failures. Post-init funded records 10,576/22,904 calls and coverage 1/2 with no emergency events; its exec ratio below one is affected by native/global warmup means 12.266/11.851 s and is not claimed as a speedup.

Reported startup and teardown remain material fixed costs. Across group means, bootstrap ranges about 10.8–25.3 ms. Global report CSV stage wall is 106–218 ms while its CPU is 0.246–0.305 ms; PEAK finalization is 174–281 ms. Stop/join stays 0.094–0.188 ms, confirming the previous 100 ms race remains fixed. These fixed initialization/publication costs are distinguished from controllable profiling and mutation increments; no filesystem-operation cause is asserted from aggregate wall/CPU alone.

The immediate short diagnostic proves one rank's participant-DONE → physical restoration → new calls. Post-init short has restoration activity but no qualifying later calls. Both independent funded diagnostics prove zero restoration; this remains a hardware-evidence gap. No requirement is invented that every short main phase must restore, and these observations do not by themselves argue for replacing instrumentation. They show that changing activation placement alone does not establish the full overhead/utility/recovery target.

Verified result archive SHA256: `26351f8f6dc11fff148c6e19868d23ffe980be02e2c113133582ed8fb0639c35`; candidate library SHA256: `d799bfab9e419af710b064b6183c3449d53d5453b0c83b31b00fe5be87eecdb7`. Native meter binary SHA256: `230f3c2305735396686a3a117382a31dba8d107707fab155d720c87362f2eaed7`. Runtime source is unchanged from the c43 escrow correction; only benchmark timing and explicit activation controls differ.

## Eager-prepare and native-broker candidate submission (5afdccb7)

Exact commit `5afdccb7c55cb5038e17d44b0a5f57a149651c44` freezes the eager native
preparation/paid target-admission path and independent native budget broker. Local
78/78 selected tests and five affected actual-MPI cases pass, with emergency
counters zero in those final cases. This does not establish performance success.
The intermediate 00ece idle-credit-return E1 defect and f772 diagnostics remain
in `global_validation/eager-broker-00ece204-local-summary.json` and
`global_validation/eager-broker-f77222b9-local-summary.json`. The final gate is
in `global_validation/eager-broker-local-summary.json`; it does not replace
those earlier observations.

Matrix job **3655586** contains 134 launches; funded/recovery job **3655587**
contains 36, for **170** total. Each requests two nodes, 32 ranks × six threads,
30 minutes, eligible for PVC or SPR. At submission both were pending for Priority
with no start estimate; the next planned status check was 2026-10-29 00:42:27 UTC.
No hardware outcome is inferred from submission or the local functional gate.

Both exact source archives have SHA256
`cf929731e7fa64a23e172b32f8f342bd7bd7946aed58d6ea0a06e059f59eca1d`
and input-manifest SHA256
`5648b84a0932bb8774fd136b4daf58b0649186639d463c818b1c2e6aa70a88a8`.
All 161 inventoried source/test files and ten benchmark inputs were checked
against the commit, with no working-tree overlay. Matrix stage is
`/scratch/05450/wangyinz/codex-tests/peak-global-eager-broker-final-matrix-20261028/20261028-193220-matrix-5993e6e6`;
funded/recovery stage is
`/scratch/05450/wangyinz/codex-tests/peak-global-eager-broker-final-funded_recovery-20261028/20261028-193220-funded_recovery-132c23e4`.

All original matrix/funded cases and their 90-second launch limits are unchanged.
The separate earned-recovery-long fixture uses fixed real native work and two
fixed 512-step target bursts, with a 180-second limit. The old work1 calibration
is retained as calibration only. Default meter policy removes an explicit policy
environment override: local uses its ordinary default, global its prepare/paid
feature; these are intentionally different physical admission paths. The new
analysis checks the actual path, startup charge, broker-authenticated startup
seal, native PID/CPU and final proofs. Only root requires all final replies
written/reaped; peers require their own cohort acknowledgment and may legally
retain a zero broker-final-written bit. Actual timing will be reported separately
for exec-inclusive, main, native/burst stages, startup and report/finalization.
No samples or failed/outlier runs will be selectively removed.
