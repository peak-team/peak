# Opt-in global-budget prototype validation

This record evaluates the opt-in `PEAK_MPI_GLOBAL_BUDGET=1` prototype. It does not
replace the [local-budget matrix results](SYNC_VALIDATION.md), and protocol/data
validity does not establish an application slowdown bound. The default remains
mode off. The two-node run below supplies actual native/off/on measurements; the
prototype remains experimental because its overall timing criterion is not met.

## Frozen implementation and scope

The local validated library SHA256 is
`dc1d50602b7b4e9814a20d48ff80f39c41f22c9deb6e1a35e133bf5f67075685`.
The frozen HPC source archive SHA256 is
`e1419b5f652be802b8978c81c96f24b52f672703c1e84ea954796abc4445e9c9`.
Each of its four production-source hashes matches the local validation manifest:

| Source | SHA256 |
| --- | --- |
| `src/general_listener.c` | `a4dc49e981519bae3a956ca2b92bf86dba3d9b569608dc4e31ae52ce4158d69c` |
| `src/mpi_interceptor.c` | `9da7b62e49f1be6c3a82ead09b1922b9b83235990252815c0b8b196f028414c6` |
| `src/global_budget.c` | `ab85242cb572df42cda9769147a673912baffe017ae16495c2791d1aded4662b` |
| `include/internal/global_budget.h` | `cdf2973773f3963aba71e441a1d20165743f70eb18afa2afee3fc02ed676b411` |

The MPI-init application thread exchanges the TCP endpoint once. Background
coordination uses POSIX sockets, without calling MPI from the service thread or
requesting an upgrade to MPI's thread support. The local real-MPI bootstrap logs
confirm requested/provided thread level 1 (`MPI_THREAD_FUNNELED`) on all eight
ranks. The HPC benchmark also requests FUNNELED; its bootstrap diagnostics must
be checked independently rather than inferred from local runs.

The root charges a conservative sum of reported rank costs, including startup
and transport accounting. `wave_span_seconds` measures root GO through all-DONE
as a diagnostic; the prototype does **not** replace summed control charges with
that span. Root `budget_seconds` is distinct from rank-local `allowance_seconds`.
Outstanding reservations remain charged; `remaining_seconds` deducts reservations
and the combined control/grace reserve. That reserve is 3 ms plus 500 us per rank
(28 ms for eight ranks; 112 ms for 32 ranks). This is a conservative risk policy,
not a proof that arbitrary safe-stop latency fits a fixed reservation.

`view_epoch`/`decision_sequence` identify a root view; `completed_wave` is a local
acknowledgment, whereas `root_completed_wave` identifies root-confirmed global
completion. Snapshots from different epochs/ranks must not be summed into an
invented job-wide ledger. Final `fresh=0` is expected after stopping the service;
`ever_active`, `shutdown_confirmed`, degradation and accounting diagnostics carry
separate meanings.

## Completed local evidence

The final implementation passed 34 focused CTest cases plus static checks.
Five real MPICH runs used eight ranks × two OpenMP threads and the original
rotating workload (`pressure.c`, 512 epochs, 2000 calls per epoch, default true
integer-compute warmup). All launcher receipts were zero; both arithmetic/MPI
phase checks passed and every final oracle was PASS. Local affinity resources
were shared, so these runs provide functional evidence only.

The unique evidence directory is `/tmp/peak-global-validated-dc1d50602b7b`, with
`PROVENANCE.json`, each case's `receipt.json`, and separate per-rank stdout/stderr.

| Local case | Target | Positive grants | Restore/revisit evidence | Shutdown settlement |
| --- | --- | --- | --- | --- |
| Off | .05 | Global mode disabled | Local policy only | No global protocol |
| Funded | .05 | 8/8 ranks | Three ranks reattached/revisited | 8/8 confirmed |
| Tight | .01 | No positive grants | Global wave 1; root all-DONE 1 | 8/8 confirmed |
| Injected disconnect | .05 | No grants after degradation | Conservative fallback observed | Not confirmed |
| Injected invalid accounting | .05 | No grants after degradation | Invalid-accounting/degraded diagnostics | Not confirmed |

The funded case retained root wave 0; its restore/revisit observations therefore
do not independently establish restoration after a completed coordinated wave.
The tight case completed such a wave but did not receive positive grants.

The tight case is **not** evidence of a hard budget bound: rank 0's last root
view reported `global_spent_seconds=0.289389147` and
`budget_seconds=0.210975999`, with remaining pool zero. Root-confirmed wave span
was 48.026852 ms. The measured accounting overdraw remains visible; successful
shutdown and a completed wave do not erase already spent cost or establish
performance acceptance.

`grace_used_seconds` measures the bounded session grace (at most 500 us per rank
in these local runs). The largest funded value was 499.8 us, while its separate
reported `unfunded_seconds` reached 1.0098 ms because that diagnostic can include
closing-tail cost. The wire field historically called unfunded carries bounded
grace-used accounting, not the larger final tail diagnostic. These quantities
must not be conflated. Emergency fallback counts are also separate from degraded
transport/accounting samples: credit exhaustion can invoke a local fallback even
while the coordinated view remains valid.

The fault-injection shim SHA256 was
`13477e18405beaae46499efe02d55e0365b9f1144ac3bd1faf520c3dead10b43`.
Disconnect and invalid-accounting cases deliberately lose global settlement;
their correctness passes do not qualify them as healthy global-budget runs.

## Completed two-node performance experiment

Slurm job **3652532** completed on PVC: two nodes, 32 ranks × six OpenMP threads, scheduler elapsed **05:33**, exit **0:0**. The job used 33 explicit sequential MPI launches: five matched cases × six arms, two short smoke checks, and one current-local `a87a89f` compilation-reference arm. All launch receipts and `body.rc` were zero. The decoded archive SHA256 is `c2b80e8a133a96540caa0976c82187c8787b7d4337f08614d610df0db2676982`.

The allocation-built prototype library SHA256 is `6b6b017a9f7760d0c2b74f1db2347f5d642bd307b7f9248472b496ecb7a4599c`; its helper SHA256 is `cd1c53865e23419197d95b1067cea964a3b34fa090ab5326ed9c8a55861ff312`. Different local/cluster library hashes reflect different build environments; their four frozen production-source hashes match the manifest above. The source archive records working-tree changes over `61800e39c42645ee02301ba01b65a5b36010902b`.

Each matched case used `N0 L0 G0 G1 L1 N1`: native N, and the same freshly built prototype library with `PEAK_MPI_GLOBAL_BUDGET=0/1` for L/G. Original cases retained `--epochs 2048 --calls 2000`, default true warmup of 10000 calls × 131072 integer steps per thread. The separate funded control changed only `--warmup-calls 60000`, identically across its arms. It does not replace any original short-warmup case.

Strict analysis reported `valid=true`, no errors: actual receipts, independent thread/MPI/overlap oracles, paired configuration/operation counts/checksums, distinct CPU bindings, and all 32 final local application reports passed. All ten G arms reported 32/32 ever-active participants, requested/provided MPI thread level **1/1**, root-confirmed wave 1, and 32/32 shutdown acknowledgments. No runtime-degraded or invalid-accounting samples were reported. Final `fresh=0` reflects normal service shutdown. These protocol observations are not performance acceptance.

Primary main wall ends after the common phase-completion barrier. Warmup-plus-main measures cumulative useful-work phases. Main-through-finalize begins at main entry and stops after MPI_Finalize; pre-main loader/constructor work and launcher wall are excluded. These intervals answer different questions. Two samples per arm are descriptive; apparent speedups are not established statistically.

| Case | N main s | L main s | G main s | G/N main | G/N warmup+main | G/N main-through-finalize |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| allreduce-rotating | 0.056001 | 0.102171 | 0.029372 | 0.5245 | 0.9033 | 1.0478 |
| allreduce-slack | 3.202383 | 3.232915 | 3.070961 | 0.9590 | 0.9696 | 1.0584 |
| bcast-slack | 2.731836 | 2.733333 | 2.743345 | 1.0042 | 0.9824 | 1.0664 |
| iallreduce-critical | 3.220393 | 3.281705 | 3.155757 | 0.9799 | 1.0065 | 1.1019 |
| funded-allreduce-rotating | 0.026120 | 0.129902 | 0.479172 | 18.3449 | 1.0292 | 1.0636 |

**The five global warmup-plus-main means are within five percent of native** (largest ratio 1.0292). The funded short main phase at 18.34× native therefore does not, by itself, violate the accepted cumulative-work interval: credit accumulated during its real 12-second warmup is available for subsequent sampling. The broader **main-through-finalize criterion remains unmet in four of five cases**, reaching +10.19% in Iallreduce critical. Neither protocol correctness nor cumulative-work means certify a universal overall bound.

The original rotating native samples were 83.347374 and 28.654897 ms, while G was 28.984726 and 29.759081 ms. Mean application-thread MPI sync time was 68.7 ms in native N0 versus 16.2 ms in N1 and 16.5/17.2 ms in G. Thus G/N=0.5245 is driven by a native outlier; it is not evidence of a reproducible twofold speedup.

### Credit, coverage and actual controls

All four original short-warmup G cases issued **zero positive grants**, recorded only **64 selected calls in total** and sampled one rank-target pair out of 512 possible configured rank-target pairs. Their coordinated wave stopped all sixteen targets on every rank, with 32 local control windows in total. This is effective early stopping with very poor sampling utility, not proof of a useful funded profiling path. The 112 ms combined reserve and charged startup/management costs leave no usable pool in these short cases.

| Case / arm | Positive-grant ranks | Selected calls | Profiled rank-target pairs / 512 | Local windows summed | Reattached targets summed | Emergency-fallback ranks |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| allreduce-rotating G0 | 0 | 64 | 1 | 32 | 0 | 0 |
| allreduce-rotating G1 | 0 | 64 | 1 | 32 | 0 | 0 |
| allreduce-slack G0 | 0 | 64 | 1 | 32 | 0 | 0 |
| allreduce-slack G1 | 0 | 64 | 1 | 32 | 0 | 0 |
| bcast-slack G0 | 0 | 64 | 1 | 32 | 0 | 0 |
| bcast-slack G1 | 0 | 64 | 1 | 32 | 0 | 0 |
| iallreduce-critical G0 | 0 | 64 | 1 | 32 | 0 | 0 |
| iallreduce-critical G1 | 0 | 64 | 1 | 32 | 0 | 0 |
| funded-allreduce-rotating G0 | 32 | 1378230 | 510 | 32 | 0 | 26 |
| funded-allreduce-rotating G1 | 32 | 1241840 | 486 | 32 | 0 | 19 |

The funded control obtained positive grants on all 32 ranks and sampled 510/486 rank-target pairs, versus 32 pairs and 4096 calls per local-mode arm. It recorded 1,378,230/1,241,840 calls and **no reattach**. It covers positive credit and much broader initial sampling, but does not establish post-wave restoration on the cluster. Emergency fallback occurred on 26/19 ranks while runtime-degraded samples remained zero: these are distinct states.

The funded main means were 26.120162 ms native, 129.902198 ms local, and 479.171549 ms global. Global estimated profile sums were 0.262163/0.235693 seconds, versus approximately 0.000776 seconds locally. Application compute CPU summed across all 192 threads rose by about 0.263 CPU seconds from native to global, consistent in scale with the additional callback cost. Global mean application-thread MPI sync elapsed was 432/471 ms versus 13.7/13.5 ms native. The observed cost of doing more profiling and synchronization propagation is therefore a supported mechanism indication. On every funded emergency rank, the policy counter recorded two rank-wide fan-outs but only one physical window and no reattach; other ranks recorded one fan-out. Controller source order processes local emergency fan-out before its later root-wave fan-out. This supports an ordering inference that some local stopping precedes root coordination, but the logs do not timestamp physical-window spacing or prove which local windows lie inside the reported GO-to-all-DONE span. Consequently that span must not be used to deduplicate these measured costs. The remaining difference is not independently attributed to TCP, credit delivery or scheduling; CPU seconds, overlapping waits and phase wall must not be added together.

Root GO-to-all-DONE spans ranged from about 3.6 to 16.3 ms, but summed local control charges remain separate. The funded root-0 final view reported S=0.625880/0.611851 seconds against B=0.664964/0.651978 seconds. Its remaining pool was zero after outstanding reservations and the 112 ms combined reserve. Original rotating root-0 S=0.176254/0.186058 exceeded B=0.115462/0.111235 seconds, despite zero grants. Already spent cost and closing work are visible; coordinated stopping does not erase them.

All cluster grace-used values stayed below 500 us per rank (largest 499.708 us); funded final unfunded-tail diagnostics reached about 511.896 us. These remain separate from the bounded wire grace-used field. No data gate was relaxed to treat a positive grace or tail diagnostic as zero.

### Funded timing-boundary decomposition

| Boundary / interval mean s | Native | Local mode | Global mode | G minus N |
| --- | ---: | ---: | ---: | ---: |
| init_boundary | 0.664142 | 1.255193 | 0.940729 | +0.276587 |
| warmup_main | 12.691329 | 13.032339 | 13.061313 | +0.369984 |
| finalize_boundary | 1.003713 | 1.366686 | 1.357855 | +0.354142 |
| other_gaps | 0.174151 | 0.095923 | 0.098247 | -0.075904 |
| main_to_finalize | 14.533334 | 15.750141 | 15.458144 | +0.924810 |

About **0.631 seconds of the 0.925-second broader-interval increase lies in the timestamped MPI-init/finalize boundaries**; cumulative work accounts for another +0.370 seconds, offset by about −0.076 seconds in other gaps. This identifies where time was observed, not a unique implementation cause. Reported global bootstrap time was only about 9–27 ms per rank and shutdown flush tens of microseconds, so assigning the entire +0.631 seconds specifically to TCP/bootstrap or report printing would be unsupported. Local mode also has substantial init/finalize cost. Further tuning requires event-level timing that isolates MPI runtime work, support hooks, report preparation/output and coordinator activity. Credit low-watermark notification and earlier refill are candidate follow-up mechanisms to evaluate with those timestamps, not performance fixes established by this run.

### Complete two-sample timing record

| Case / mode | Main samples s | Warmup+main samples s | Main-through-finalize samples s |
| --- | --- | --- | --- |
| allreduce-rotating native | 0.083347374, 0.028654897 | 2.471999684, 2.314724881 | 4.291902000, 4.307207000 |
| allreduce-rotating local | 0.102269698, 0.102071376 | 2.410860776, 2.319006832 | 4.867242000, 4.661389000 |
| allreduce-rotating global | 0.028984726, 0.029759081 | 2.174462431, 2.149329708 | 4.527124000, 4.482654000 |
| allreduce-slack native | 3.331382130, 3.073383795 | 5.442734550, 5.116823086 | 7.238658000, 6.894508000 |
| allreduce-slack local | 3.172311420, 3.293519252 | 5.512832423, 5.806949210 | 8.246764000, 8.081367000 |
| allreduce-slack global | 3.102565522, 3.039356817 | 5.153126556, 5.085461024 | 7.542433000, 7.416702000 |
| bcast-slack native | 2.730040759, 2.733630965 | 4.945278451, 5.192605084 | 6.776402000, 7.099342000 |
| bcast-slack local | 2.733123151, 2.733542824 | 4.694727802, 4.927700898 | 7.168179000, 7.133258000 |
| bcast-slack global | 2.733916111, 2.752773927 | 4.950504450, 5.008728295 | 7.257677000, 7.539605000 |
| iallreduce-critical native | 3.220925210, 3.219861672 | 5.318212355, 5.387537563 | 7.061509000, 7.080104000 |
| iallreduce-critical local | 3.308011681, 3.255397386 | 5.278850119, 5.381397341 | 7.510588000, 7.624275000 |
| iallreduce-critical global | 3.183406724, 3.128107825 | 5.432182285, 5.343662821 | 7.692804000, 7.890108000 |
| funded-allreduce-rotating native | 0.026232242, 0.026008081 | 12.849902832, 12.532754505 | 14.657299000, 14.409370000 |
| funded-allreduce-rotating local | 0.158044192, 0.101760203 | 13.004229342, 13.060448423 | 15.743361000, 15.756922000 |
| funded-allreduce-rotating global | 0.459049256, 0.499293841 | 13.203064457, 12.919561197 | 15.645996000, 15.270292000 |

## Reproduction and acceptance limits

Decoded evidence is in `/tmp/peak-global-prototype-results`; the source/command package is `/tmp/peak-global-budget-prototype-20261028`. The tested launch order and fixed arguments are preserved in [global_validation/commands.json](global_validation/commands.json), with the per-rank wrapper and reproduction contract in [global_validation/README.md](global_validation/README.md). No source archives or binaries are committed.

```sh
python3 benchmarks/mpi_rank_budget/global_analyze.py RESULTS > global-summary.json
```

The compact [measured-summary.json](global_validation/measured-summary.json) preserves both samples, root-view accounting, coverage and timing decomposition. Full raw rank reports remain in the SHA-verified archive. Do not sum different ranks’ replicated global snapshots or compare rank-local allowance with the job-wide budget.

This prototype demonstrates real cross-node participation, positive credits in the funded case and coordinated stopping with authenticated completion diagnostics. It does **not** yet demonstrate satisfactory overall application overhead or good utility in the short cases, and the cluster run did not exercise funded post-wave reattachment. Outstanding acceptance work is event-level init/finalize attribution, useful short-run grants without unaccounted startup/closing costs, restoration under coordinated budget, and repeated experiments sufficient to separate effects from run variability. Production tuning was not changed after observing these results.
