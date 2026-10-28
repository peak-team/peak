# MPI rank-budget validation

Initial implementation: `fd12d0b` (`Bound MPI profiling risk with shared rank
budgets and burst detection`); round 2 additionally tests the subsequent working-tree
rank-wide batch-detach optimization. This record separates functional evidence from
application performance evidence; it does not claim a strict 5% slowdown bound.

## Completed local checks

The initial implementation's 24 related CTest cases passed (24/24). The batch-detach
revision subsequently passed 27/27 related local CTest cases. Final verification
passed 28/28 cases: the new zero-cooldown admission guard regression plus all 27
previous cases. The admission correction prevents a later burst wave from entering
before accounting for the preceding wave is published. It did not change callback,
batch-detach or stop-window paths, and the same accounting path retains its prior
behavior. This final admission correction was not followed by another two-node
performance run; the distributed measurements below cover the preceding revision. Benchmark checks also
passed: GCC compilation with the serial-only smoke shim, 256 distinct exported
functions with sixteen entry NOPs, default and coarse-work thread oracles,
independent routing/oracle simulation across 32 ranks × six threads, and offline
analysis accepting complete stdout fixtures while rejecting timeout receipts and
oracle failures. The smoke shim does not establish MPI correctness.

A separate real MPICH 4.1.1 functional test ran eight ranks × two OpenMP threads.
All four launcher receipts reported exit code 0, both warmup/main records had
`oracle_bad=0 mpi_bad=0`, and every final oracle reported `PASS`.

| Case | Native | Candidate | Successful candidate burst detaches |
| --- | --- | --- | --- |
| Rotating pressure | Correctness passed | Correctness passed | 128 (16 per rank) |
| Coarse control | Correctness passed | Correctness passed | 0 |

These counts were checked against the launcher receipts and every rank's detach
trace, including the per-rank transition summary. Pressure had no non-burst
detaches; coarse had neither burst nor non-burst detaches. Pressure burst requests
occurred at process elapsed times 0.675795449–4.544537776 seconds, before the
configured 10-second heartbeat interval. All 128 pressure physical detach
operations succeeded. Neither candidate trace validation found suspicious lines.

The real-MPI workload used the source and target list in this directory, with
`OMP_NUM_THREADS=2`, dynamic sizing disabled, close/core binding, and:

| Parameter | Pressure | Coarse |
| --- | --- | --- |
| `--epochs` | 256 | 8 |
| `--calls` | 2000 | 128 |
| `--main-work` | 1 | 262144 |
| `--warmup-calls` | 128 | 128 |
| `--warmup-work` | 4096 | 4096 |

Candidate settings included `PEAK_GLOBAL_OVERHEAD_RATIO=.05`,
`PEAK_OVERHEAD_RATIO=.005`, detach/reattach factors `1.2/.95`, per-target heartbeat
disabled, global heartbeat enabled, reattach disabled, heartbeat interval `10`,
`PEAK_HB_MAX_US=10000000`, strict safe detach with the signal backend, local output
aggregation, and `PEAK_COST=0`. All eight ranks reported world size 8 from
`PMI_SIZE`, effective rank target approximately `.00625`, and burst guard stride
64. This functional configuration deliberately isolates burst-triggered actions
from ordinary heartbeat actions; see [README.md](README.md) for the original
pressure experiment and distributed comparison configuration.

The local ranks shared affinity resources. Their elapsed times support no local
performance conclusion; only correctness, rank-budget activation, successful
burst-triggered detach, and the coarse negative control are established here.

## Two-node HPC validation: round 1

Slurm job `3651692` completed with intermediate implementation `fd12d0b`.
The six default-workload runs used two nodes, 32 MPI ranks × six OpenMP threads.
All six exit receipts were zero; the offline analyzer accepted matching workload
counts/checksums, clean warmup/main oracles and MPI checks, and final `PASS` records.
The execution order was `N0 B0 C0 C1 B1 N1` (native/baseline/candidate).

| Run | Warmup wall rank mean (s) | Main wall rank mean (s) |
| --- | ---: | ---: |
| N0 | 2.042663 | 0.125819 |
| N1 | 2.043826 | 0.120147 |
| B0 | 2.041832 | 3.839126 |
| B1 | 2.057053 | 3.842869 |
| C0 | 2.042394 | 0.844437 |
| C1 | 2.042243 | 0.846384 |

The two-run main wall means were 0.122983 seconds native, 3.840998 seconds
baseline, and 0.845411 seconds candidate. Candidate main wall remained **6.874×
native**; the measured warmup-plus-main wall mean remained approximately **33.3%
above native**. These are descriptive ratios of directly measured phase wall rank
means, not sums of independently selected rank maxima or process-lifetime ratios.
Two samples do not establish statistical significance.

Candidate aggregate reports recorded 65,536 and 65,463 calls, versus 16,384,000
potential profiled calls in each run. Both reported 16 distinct targets detached,
zero reattached and zero revisited. The maximum-control owner rank reported 16
control windows totaling 0.038009 and 0.038097 seconds respectively. The aggregate
16-target coverage counts unique target identities; it is not a count of all
physical detach operations across ranks.

This intermediate version greatly reduced callbacks but **did not meet the
application wall-time objective**. Individual detach control windows can still
occur in sequence as the active profiled rank rotates, extending the MPI
synchronization critical path. The reported approximately 38 ms belongs to one
owner rank and cannot be treated as the application's complete control penalty.
This round is not evidence that the problem is fixed. The next section records
the subsequent batch-detach experiment; these round-1 numbers are retained as
evidence of the intermediate version's limitation.

Reproduce the offline correctness and wall comparison using the six stdout and
exit-code receipt pairs with:

```sh
python3 benchmarks/mpi_rank_budget/analyze.py RESULTS \
  --labels N0 B0 C0 B1 C1 N1
```

This argument order assigns each label to its native/baseline/candidate group;
it does not change the recorded chronological execution order above. See
[README.md](README.md) for workload and Slurm execution configuration. Profiler
accounting and transition evidence must additionally be read from aggregate
stderr reports; application correctness alone does not certify those reports.


## Two-node HPC validation: round 2

Slurm job `3651730` completed on two PVC nodes with 32 MPI ranks × six OpenMP
threads, scheduler elapsed `1:22`, exit `0:0`. It tested the rank-wide batch-detach
working-tree revision, not solely the committed round-1 implementation. The
strict bundle summary reported `valid=true`, with no errors, for the six primary
runs, coarse pair and policy-disabled control. Exit receipts, oracle/MPI checks,
operation counts, matching checksums and affinity were reconciled.

| Run | Warmup wall rank mean (s) | Main wall rank mean (s) | Process elapsed (s) |
| --- | ---: | ---: | ---: |
| N0 | 1.950549 | 0.109326 | 3.759643 |
| N1 | 2.453245 | 0.108094 | 4.232174 |
| B0 | 2.040628 | 4.075473 | 8.034442 |
| B1 | 2.073242 | 3.883662 | 8.025286 |
| C0 | 2.171264 | 0.213221 | 4.261402 |
| C1 | 2.021961 | 0.185533 | 4.041732 |
| Native mean | 2.201897 | 0.108710 | 3.9959085 |
| Baseline mean | 2.056935 | 3.9795675 | 8.029864 |
| Candidate mean | 2.0966125 | 0.199377 | 4.151567 |

Candidate process elapsed averaged **3.90% above native**, compared with about
101% above native for baseline. Candidate main wall still averaged **1.834×
native** (83.4% higher), although it fell substantially from round 1. Process
elapsed includes initialization, warmup, reporting and finalization and must not
be presented as main-phase overhead. N1 warmup was 2.453245 seconds versus N0's
1.950549 seconds, a substantial variation which affects the native process mean.
The measured warmup-plus-main means were 2.310607 seconds native, 6.0365025
seconds baseline and 2.2959895 seconds candidate. With two primary samples, these
are descriptive observations, not a significance test or a strict 5% application
slowdown guarantee.

Each candidate run recorded 4096 calls and profiled **one of 16** instrumented
target identities, then detached all 16 (zero reattached/revisited). The rank-wide
burst response batches targets instead of opening a separate control window for
each target. Each HPC aggregate maximum-owner report showed one control window,
lasting 2.771841 ms and 2.789204 ms respectively. These aggregate reports do not
provide separate trace evidence for every one of the 32 ranks; the local real-MPI
wave separately verified the per-rank burst behavior.
This is an explicit coverage tradeoff: 15 targets can be detached before collecting
samples, so reduced overhead does not imply preservation of all-target profiles.
The original main workload still executed all 16,384,000 potential profiled calls
and passed its complete useful-work oracle.

| Candidate diagnostic | C0 | C1 |
| --- | ---: | ---: |
| Maximum raw profile+control ratio | 0.0881495% | 0.0936370% |
| Maximum profile+control risk ratio | 1.3989254% | 1.4859227% |
| Owner-rank control windows | 1 | 1 |
| Failed control windows | 0 | 0 |

Both aggregate accounting snapshots were valid. Raw accounting combines estimated profile cost (recorded calls times calibrated
per-call cost, plus management accounting) with measured control seconds on the
reported rank; risk accounting multiplies
control seconds by 16 local ranks to reflect possible contention amplification.
These are distinct policy diagnostics with profiler elapsed denominators, not
measured application slowdown. The configured world target was approximately
0.05; the effective per-rank target was 0.0015625 for 32 ranks, with stride 64.
Independently selected maxima are never added.

The coarse negative control (`--epochs 64 --calls 128 --main-work 262144`, default
warmup) measured main wall 3.350006 seconds native and 3.456031 seconds candidate.
Candidate recorded all 8192 potential calls, profiled the two exercised targets,
and reported zero detaches and zero control windows. The policy-disabled control
recorded all 16,384,000 potential calls across 16 targets and reported no detach or
control windows, confirming that burst protection was disabled in that control.

Raw launcher outputs were preserved unchanged. In several stdout files a complete
`config ranks=... expected_warmup_calls=...` record interrupted rank 31's affinity
record. Analysis removed only that complete config record from a copy and joined
the literal affinity prefix and immediately following continuation; it inferred
no CPU label or value. The reconstruction yielded all ranks 0–31 with six
single-CPU thread bindings each and 96 distinct CPUs per node. This lossless
reassembly is documented in the result bundle's `ANALYSIS_NOTES`; the strict
summary is `mpi-budget-summary.json`.

The batch-detach revision also passed a second real local MPICH 4.1.1 wave of the
same four eight-rank/two-thread cases documented above. Every exit receipt was
zero and every warmup/main oracle and MPI check passed. Candidate pressure had
128 successful physical detaches, one rank-wide burst wave and 15 unsampled
targets reported on each rank; coarse had zero burst waves, unsampled targets or
detaches. Shared local affinity again precludes a local performance conclusion.
