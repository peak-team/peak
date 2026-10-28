# Global-budget experiment command manifest

`commands.json` preserves all 33 launches in the actual chronological order.
N uses no preload; L/G use the **same** prototype library/helper with
`PEAK_MPI_GLOBAL_BUDGET=0/1`; A uses the previously built `a87a89f` library/helper.
The two S0 arms are smoke checks, not matched performance samples. `funded-*`
retains its own 60000-call true warmup and must not be mixed with short cases.

`rank.sh` is the tested per-rank wrapper. Supply `PROTOTYPE_LIB`,
`PROTOTYPE_HELPER`, `CURRENT_LOCAL_LIB`, and `CURRENT_LOCAL_HELPER` using verified
absolute allocation-visible paths. Set `PEAK_OUTPUT_AGGREGATION=local`,
`PEAK_TEXT_OUTPUT=1` and the [profiler policy settings](../README.md), with
`OMP_NUM_THREADS=6`, fixed distinct CPU binding and two nodes/32 MPI ranks.
Build the prototype inside the allocation using the exact recorded source;
record the resulting library/helper hashes, workload hash and Slurm resources.

Materialize each manifest entry as a direct sequential launcher command:

```sh
mkdir -p results/CASE/LABEL
set +e
timeout --signal=TERM --kill-after=10s 90s ibrun -n 32 ./rank.sh MODE \
  results/CASE/LABEL --sync SYNC --layout LAYOUT --epochs EPOCHS \
  --calls CALLS --warmup-calls WARMUP_CALLS
rc=$?
printf '%s\n' "$rc" > results/CASE/LABEL.rc
set -e
if ((rc != 0)); then exit "$rc"; fi
```

The submitted job contained explicit sequential commands, not a scheduling loop.
The outer wrapper recorded `body.rc`, archived results/manifests/build logs even
on failure, wrote an archive SHA256 and ASCII base64, then retained the original
failure status. No subsequent test arm was executed after a nonzero launch.
Keep raw outputs unchanged. Archive/base64 transport is not performance data;
verify decoded bytes against the archive SHA before analysis. Run
`python3 ../global_analyze.py RESULTS` only after retrieving all required arms.
The complete evidence and exact prototype hashes belong in
[GLOBAL_VALIDATION.md](../GLOBAL_VALIDATION.md).
