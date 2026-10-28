# MPI synchronization and rank-imbalance matrix

The expanded experiment does **not** establish that all MPI synchronization patterns remain below five-percent application overhead. All 120 measured arms passed correctness and data-integrity gates; those gates are distinct from performance acceptance.

## Provenance and reproduction

- Stampede3 job `3651921`: PVC, two nodes, 32 MPI ranks × six OpenMP threads, scheduler elapsed `16:03`, exit `0:0`.
- Candidate production commit: `a87a89f`; pre-fix baseline: `df586c0`. The allocation built the candidate and reused the previously verified baseline and Gum provider.
- Workload: `sync_pressure.c`, SHA256 `f2641ff45a62ab730745b16b81d51c0b4f582a54a6950f51d4de53b63fddb27b`; candidate library SHA256 `10c84f16fff4ddd362a4342a9097c2531efd22b2f95ca042fe61b1bdc11d63cb`.
- Retrieved result archive SHA256: `0daca4cd6db0b762023ec5b495e0ef21b72772ed5192231584c591fb921dfcd4`.
- Every case ran `N0 B0 C0 C1 B1 N1` in the same allocation. Parameters: `--epochs 2048 --calls 2000`, default warmup 10000 calls × 131072 arithmetic steps per thread. Five sync modes × four layouts produce 20 cases and 120 measured arms; three additional native probes preceded the matrix.

Profiler settings match the [distributed configuration](README.md): five-percent configured job risk target, effective rank target `.05/32`, stride 64, heartbeat `.01`, per-target/global heartbeat and reattach enabled, 100 ms reattach cooldown, strict signal-backend detach. Each rank wrote a separate local text report.

```sh
python3 benchmarks/mpi_rank_budget/sync_summary.py RESULTS > matrix-summary.json
```

The complete local result tree is `/tmp/peak-mpi-sync-matrix-results/results`; the strict summary is `/tmp/peak-mpi-sync-matrix-summary.json`. Archive/manifests and library/workload hashes were reconciled before analysis. The summary reported `valid=true`, no errors: all real launcher receipts zero; independent arithmetic, overlap and MPI oracles passed; fixed-work config/counts/checksums matched across paired runs; all ranks had complete metrics and matching distinct single-CPU thread bindings on two hosts. CPU IDs alone establish logical binding, not physical-core topology. Each of the 32 profiled-rank reports per arm had a final 16-target local scope, valid accounting and zero failed windows; zero-target helper reports were excluded.

## Timing boundaries and two-sample observations

Main wall is rank 0’s directly measured phase, ending after one common phase-completion barrier. That final barrier is outside the selected per-epoch synchronization operation. `completion_wait` is separately recorded and included in wall. Oracle-validation collectives are outside measured wall. Main-through-finalize starts at main entry and stops after MPI_Finalize: pre-main loader/LD_PRELOAD construction and launcher elapsed are excluded. Reporting and finalization can contribute to the supplementary interval; their causal costs were not independently isolated here.

Ratios below divide two-run means. Two samples are descriptive, with no significance or universal bound claim; means below native should not be read as established profiler speedups.

| Case | Native main s | Baseline main s | Candidate main s | C/N main | C/N warmup+main | C/N main-through-finalize |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| allreduce-rotating | 0.028099 | 0.988536 | 0.118717 | 4.2250 | 1.0168 | 1.1705 |
| allreduce-slack | 3.055937 | 3.637618 | 3.106075 | 1.0164 | 1.0372 | 1.0790 |
| allreduce-critical | 3.088314 | 4.019821 | 3.184896 | 1.0313 | 0.9891 | 1.0592 |
| allreduce-aligned | 3.096270 | 3.284652 | 3.158732 | 1.0202 | 1.0247 | 1.0725 |
| barrier-rotating | 0.028501 | 0.996562 | 0.105017 | 3.6846 | 1.0375 | 1.0913 |
| barrier-slack | 3.111029 | 3.114517 | 3.167242 | 1.0181 | 1.0367 | 1.1415 |
| barrier-critical | 3.090720 | 4.019900 | 3.143621 | 1.0171 | 0.9836 | 1.0384 |
| barrier-aligned | 3.093470 | 3.194742 | 3.130953 | 1.0121 | 1.0052 | 1.0776 |
| bcast-rotating | 0.028313 | 0.892162 | 0.099533 | 3.5154 | 0.9741 | 1.1076 |
| bcast-slack | 2.743414 | 2.744150 | 2.904032 | 1.0585 | 1.0475 | 1.1247 |
| bcast-critical | 2.895885 | 3.732711 | 2.959781 | 1.0221 | 0.9988 | 1.0568 |
| bcast-aligned | 3.142022 | 2.937249 | 2.803048 | 0.8921 | 0.9889 | 1.0380 |
| ring-rotating | 0.030597 | 0.829128 | 0.093989 | 3.0718 | 0.9073 | 1.0335 |
| ring-slack | 2.803102 | 2.757697 | 2.758283 | 0.9840 | 0.9971 | 1.0523 |
| ring-critical | 2.798372 | 3.951582 | 2.853158 | 1.0196 | 0.9822 | 1.0480 |
| ring-aligned | 2.788048 | 2.915454 | 2.789589 | 1.0006 | 1.0375 | 1.0922 |
| iallreduce-rotating | 0.035202 | 0.959380 | 0.114816 | 3.2616 | 1.1171 | 1.1072 |
| iallreduce-slack | 3.092498 | 3.083682 | 3.169316 | 1.0248 | 1.0004 | 1.0539 |
| iallreduce-critical | 3.125361 | 4.053251 | 3.145838 | 1.0066 | 0.9577 | 1.0312 |
| iallreduce-aligned | 3.674964 | 3.245390 | 3.108590 | 0.8459 | 0.8970 | 0.9716 |

The five rotating/tiny-compute main phases remain 3.07–4.22× native: native work lasts only 28–35 ms, while candidate main wall lasts 94–119 ms. For comparison, baseline Allreduce rotating main averaged 0.988536 s versus candidate 0.118717 s and native 0.028099 s. The fix strongly reduces that counterexample but leaves a fixed control response penalty.

Of the fifteen heavier/nonuniform main phases, fourteen candidate/native means are at most 1.032; Bcast slack is 1.0585. The large apparent reductions in Bcast aligned and Iallreduce aligned have only two samples and are not established speedups. Fourteen of twenty supplementary main-through-finalize means exceed native by five percent; worst cases are Allreduce rotating +17.05%, Barrier slack +14.15%, and Bcast slack +12.47%. Warmup-plus-main exceeds five percent in Iallreduce rotating (+11.71%). These supplementary changes cannot be assigned wholly to MPI synchronization or callback/control cost without isolating warmup, reporting, finalization and run variability.

## Per-rank imbalance and coverage

`slack` places the high-frequency profiled calls on rank 0 and the heavy native work on rank 31; `aligned` places both on rank 0. In these layouts, native target-loop arithmetic has exact max/min ratio 512 (Iallreduce adds the separately oracle-checked uniform 256-step overlap per epoch) and measured rank-mean compute CPU max/min about 309–319. Thus the tests include both overhead on a fast rank waiting behind a slow native rank and overhead on the native bottleneck. `critical` rotates the heavy work with the hotspot: cumulative work balances across ranks but every epoch has a heavily unequal pre-sync critical path.

In slack/aligned candidate arms, rank 0 recorded 128 calls and one control window; the other 31 ranks recorded zero selected calls and zero windows. This is a strongly unequal distribution of profiler cost as well as native work. Rotating/critical candidate arms usually recorded 128 calls and one window per rank (4096 calls, 32 windows summed across ranks). In Bcast critical C1, rank 14 instead recorded 249 calls, two sampled targets and two windows; the run total was 4217 calls and 33 windows, with no reattach and no failed window. Therefore a blanket “one window per rank in every mode” statement would be false.

All candidate raw profile-plus-control rank ratios were below the effective 0.15625% rank risk target; the largest was approximately 0.1049%. That remains a local estimated-profile/measured-control diagnostic divided by profiler elapsed, not an application wall-time guarantee. Its coexistence with larger main-wall and main-through-finalize ratios is part of the observed result. Per-rank costs are retained separately in the JSON and independently selected maxima are never summed into a claimed wall penalty.

High-pressure rank-wide stopping also sacrifices coverage: rotating/critical ranks normally sample one target then stop all sixteen; slack/aligned leave non-hot ranks with no selected samples. The workload still executes and validates all planned target-loop arithmetic and the separate Iallreduce overlap. `lcg_steps` counts target-loop steps; overlap is checked separately and is not included in that operation count.

## Raw samples

| Case | N main samples s | C main samples s | N main-through-finalize s | C main-through-finalize s |
| --- | --- | --- | --- | --- |
| allreduce-rotating | 0.027132606, 0.029064805 | 0.102274930, 0.135159008 | 3.837508000, 3.812755000 | 4.650093000, 4.304501000 |
| allreduce-slack | 3.033551197, 3.078323641 | 3.155336613, 3.056813377 | 6.857409000, 6.719039000 | 7.553106000, 7.095757000 |
| allreduce-critical | 3.134681206, 3.041946087 | 3.110598552, 3.259194409 | 7.071093000, 6.800132000 | 7.200593000, 7.491829000 |
| allreduce-aligned | 3.164567442, 3.027972712 | 3.098227096, 3.219236321 | 7.101690000, 6.880351000 | 7.268902000, 7.726802000 |
| barrier-rotating | 0.028199235, 0.028803554 | 0.105771641, 0.104262075 | 3.757899000, 3.913322000 | 4.273391000, 4.098370000 |
| barrier-slack | 3.156835088, 3.065222454 | 3.105116528, 3.229366934 | 7.036597000, 6.909171000 | 7.624454000, 8.293961000 |
| barrier-critical | 3.004694053, 3.176745854 | 3.085431108, 3.201811386 | 7.074970000, 7.213190000 | 7.346745000, 7.490064000 |
| barrier-aligned | 3.011665882, 3.175273265 | 3.140999561, 3.120906188 | 6.866781000, 7.115831000 | 7.759334000, 7.308468000 |
| bcast-rotating | 0.036760400, 0.019865710 | 0.094482420, 0.104583098 | 3.938491000, 3.897021000 | 4.306723000, 4.371580000 |
| bcast-slack | 2.742947037, 2.743880888 | 3.046213297, 2.761851313 | 6.427308000, 6.424724000 | 7.592808000, 6.862346000 |
| bcast-critical | 2.872656409, 2.919114156 | 2.941998109, 2.977564522 | 6.837443000, 6.599182000 | 7.112139000, 7.087514000 |
| bcast-aligned | 2.789235856, 3.494807685 | 2.810710508, 2.795385806 | 6.511072000, 7.131725000 | 7.127621000, 7.033825000 |
| ring-rotating | 0.016241923, 0.044952266 | 0.096221161, 0.091757803 | 4.158769000, 3.906048000 | 4.126961000, 4.208045000 |
| ring-slack | 2.813422902, 2.792781222 | 2.756661845, 2.759904532 | 6.539962000, 6.720551000 | 6.981826000, 6.972559000 |
| ring-critical | 2.805313829, 2.791430718 | 2.854905807, 2.851409303 | 6.954058000, 6.579074000 | 7.081807000, 7.100688000 |
| ring-aligned | 2.797442860, 2.778652769 | 2.796992510, 2.782184574 | 6.814072000, 6.444251000 | 7.249303000, 7.232051000 |
| iallreduce-rotating | 0.037121451, 0.033283214 | 0.107298155, 0.122333100 | 4.077061000, 3.884026000 | 4.363491000, 4.451178000 |
| iallreduce-slack | 3.125765627, 3.059231004 | 3.276089546, 3.062542176 | 6.989269000, 7.119504000 | 7.446107000, 7.423708000 |
| iallreduce-critical | 3.188366300, 3.062355781 | 3.175668249, 3.116007398 | 7.122902000, 7.015452000 | 7.323632000, 7.256375000 |
| iallreduce-aligned | 3.146412749, 4.203515965 | 3.113289261, 3.103891340 | 6.787816000, 8.274152000 | 7.175937000, 7.458238000 |
