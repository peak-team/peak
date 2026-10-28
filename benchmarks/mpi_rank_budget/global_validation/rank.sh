#!/usr/bin/env bash
set -euo pipefail
arm=$1; dest=$2; shift 2
rank=${PMI_RANK:-${PMIX_RANK:-${OMPI_COMM_WORLD_RANK:-${SLURM_PROCID:?}}}}
unset LD_PRELOAD LD_AUDIT
export PEAK_MPI_GLOBAL_BUDGET=0
if [[ $arm == A ]]; then export LD_PRELOAD="$CURRENT_LOCAL_LIB" PEAK_DETACH_HELPER="$CURRENT_LOCAL_HELPER"; fi
if [[ $arm == L || $arm == G ]]; then export LD_PRELOAD="$PROTOTYPE_LIB" PEAK_DETACH_HELPER="$PROTOTYPE_HELPER"; fi
if [[ $arm == G ]]; then export PEAK_MPI_GLOBAL_BUDGET=1; fi
exec ./pressure "$@" > "$dest/rank-$rank.out" 2> "$dest/rank-$rank.err"
