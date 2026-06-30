#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 4 ]; then
    echo "usage: $0 <label> <threads> <cpuset> <label:backend:detach|reattach>..." >&2
    exit 2
fi

SYSTEM_LABEL=$1
THREADS=$2
CPUSET=$3
shift 3

RUN_DURATION=${PEAK_HPC_DURATION:-3}
OVERHEAD_RATIO=${PEAK_HPC_OVERHEAD_RATIO:-0.000001}
PEAK_COST=${PEAK_HPC_PEAK_COST:-0.000000000001}
DETACH_COUNT=${PEAK_HPC_DETACH_COUNT:-1}
STRESS_SAMPLES=${PEAK_HPC_SAMPLES:-8}
BASELINE_SAMPLES=${PEAK_HPC_BASELINE_SAMPLES:-3}
BUILD_DIR=${PEAK_HPC_BUILD_DIR:-build-hpc-extreme}
OUT=${PEAK_HPC_OUT:-extreme-${SYSTEM_LABEL}-results}
BUILD_JOBS=${PEAK_HPC_BUILD_JOBS:-8}
THREAD_SLACK=${PEAK_HPC_THREAD_SLACK:-64}
MAX_THREADS=${PEAK_HPC_MAX_THREADS:-0}
ENABLE_MPI=${PEAK_HPC_ENABLE_MPI:-OFF}
FAIL_ON_TRANSITION_SKIPS=${PEAK_HPC_FAIL_ON_TRANSITION_SKIPS:-1}
EXPECTED_CPUSET_CPUS=${PEAK_HPC_EXPECTED_CPUSET_CPUS:-}

cpuset_cardinality()
{
    awk -v cpuset="$1" '
        function add_cpu(cpu) {
            if (!(cpu in seen)) {
                seen[cpu] = 1;
                total++;
            }
        }
        BEGIN {
            if (cpuset == "") {
                exit 2;
            }
            n = split(cpuset, parts, ",");
            total = 0;
            for (i = 1; i <= n; i++) {
                entry = parts[i];
                gsub(/^[[:space:]]+/, "", entry);
                gsub(/[[:space:]]+$/, "", entry);
                if (entry ~ /^[0-9][0-9]*$/) {
                    add_cpu(entry + 0);
                } else if (entry ~ /^[0-9][0-9]*-[0-9][0-9]*$/) {
                    split(entry, bounds, "-");
                    first = bounds[1] + 0;
                    last = bounds[2] + 0;
                    if (last < first) {
                        exit 2;
                    }
                    for (cpu = first; cpu <= last; cpu++) {
                        add_cpu(cpu);
                    }
                } else {
                    exit 2;
                }
            }
            print total;
        }
    '
}

if ! [[ "$THREADS" =~ ^[0-9]+$ ]] || [ "$THREADS" -le 0 ]; then
    echo "invalid positive thread count: ${THREADS}" >&2
    exit 2
fi
if ! CPUSET_COUNT=$(cpuset_cardinality "$CPUSET"); then
    echo "invalid simple comma/range cpuset: ${CPUSET}" >&2
    exit 2
fi
TASKSET_ALLOWED=$(taskset -c "$CPUSET" bash -c "awk '/Cpus_allowed_list/ {print \$2}' /proc/self/status")
if ! TASKSET_COUNT=$(cpuset_cardinality "$TASKSET_ALLOWED"); then
    echo "could not parse taskset allowed CPU list: ${TASKSET_ALLOWED}" >&2
    exit 2
fi
if [ "$THREADS" -ne "$CPUSET_COUNT" ]; then
    echo "EXTREME_CPUSET_SUMMARY label=${SYSTEM_LABEL} requested_threads=${THREADS} cpuset=${CPUSET} cpuset_cpus=${CPUSET_COUNT} taskset_allowed=${TASKSET_ALLOWED} taskset_cpus=${TASKSET_COUNT} status=thread_cpuset_mismatch"
    echo "THREADS (${THREADS}) must match cpuset cardinality (${CPUSET_COUNT}) for cpuset ${CPUSET}" >&2
    exit 2
fi
if [ "$CPUSET_COUNT" -ne "$TASKSET_COUNT" ]; then
    echo "EXTREME_CPUSET_SUMMARY label=${SYSTEM_LABEL} requested_threads=${THREADS} cpuset=${CPUSET} cpuset_cpus=${CPUSET_COUNT} taskset_allowed=${TASKSET_ALLOWED} taskset_cpus=${TASKSET_COUNT} status=taskset_cpuset_mismatch"
    echo "taskset allowed CPU cardinality (${TASKSET_COUNT}) did not match requested cpuset cardinality (${CPUSET_COUNT})" >&2
    exit 2
fi
if [ -n "$EXPECTED_CPUSET_CPUS" ]; then
    if ! [[ "$EXPECTED_CPUSET_CPUS" =~ ^[0-9]+$ ]] || [ "$EXPECTED_CPUSET_CPUS" -le 0 ]; then
        echo "invalid PEAK_HPC_EXPECTED_CPUSET_CPUS: ${EXPECTED_CPUSET_CPUS}" >&2
        exit 2
    fi
    if [ "$CPUSET_COUNT" -ne "$EXPECTED_CPUSET_CPUS" ]; then
        echo "EXTREME_CPUSET_SUMMARY label=${SYSTEM_LABEL} requested_threads=${THREADS} cpuset=${CPUSET} cpuset_cpus=${CPUSET_COUNT} expected_cpuset_cpus=${EXPECTED_CPUSET_CPUS} taskset_allowed=${TASKSET_ALLOWED} taskset_cpus=${TASKSET_COUNT} status=expected_cpuset_mismatch"
        echo "cpuset cardinality (${CPUSET_COUNT}) did not match expected full-node cardinality (${EXPECTED_CPUSET_CPUS})" >&2
        exit 2
    fi
fi
echo "EXTREME_CPUSET_SUMMARY label=${SYSTEM_LABEL} requested_threads=${THREADS} cpuset=${CPUSET} cpuset_cpus=${CPUSET_COUNT} expected_cpuset_cpus=${EXPECTED_CPUSET_CPUS:-unset} taskset_allowed=${TASKSET_ALLOWED} taskset_cpus=${TASKSET_COUNT} status=ok"

mkdir -p "$OUT"

echo "EXTREME_CONFIG label=${SYSTEM_LABEL} threads=${THREADS} cpuset=${CPUSET} expected_cpuset_cpus=${EXPECTED_CPUSET_CPUS:-unset} duration=${RUN_DURATION} overhead_ratio=${OVERHEAD_RATIO} peak_cost=${PEAK_COST} detach_count=${DETACH_COUNT} stress_samples=${STRESS_SAMPLES} baseline_samples=${BASELINE_SAMPLES} build_jobs=${BUILD_JOBS} enable_mpi=${ENABLE_MPI} fail_on_transition_skips=${FAIL_ON_TRANSITION_SKIPS}"
echo "EXTREME_HOST host=$(hostname)"
echo "EXTREME_DATE $(date -Is)"
echo "EXTREME_MODULES_BEGIN"
module list 2>&1 || true
echo "EXTREME_MODULES_END"
lscpu | grep -E "Architecture|CPU\\(s\\)|Thread\\(s\\) per core|Core\\(s\\) per socket|Socket\\(s\\)|Model name" || true
echo "EXTREME_CPUSET_ALLOWED_LIST ${TASKSET_ALLOWED}"

rm -rf "$BUILD_DIR"
cmake -S . -B "$BUILD_DIR" ${PEAK_HPC_CMAKE_ARGS:-} \
    -DCMAKE_BUILD_TYPE=Release \
    -DPEAK_BUILD_BENCHMARKS=ON \
    -DPEAK_ENABLE_MPI="$ENABLE_MPI" \
    -DBUILD_CUDA_PROFILE=OFF
cmake --build "$BUILD_DIR" --target peak peak_detach_helper benchmark_detach_hotloop "-j${BUILD_JOBS}"

EXE="${BUILD_DIR}/benchmarks/detach/benchmark_detach_hotloop"
LIBPEAK="${BUILD_DIR}/src/libpeak.so"
RUNNER="benchmarks/detach/run_detach_hotloop_stress.py"
PYTHON_BIN=${PEAK_HPC_PYTHON:-python3}

if [ ! -x "$EXE" ]; then
    echo "missing benchmark executable: ${EXE}" >&2
    exit 2
fi
if [ ! -f "$LIBPEAK" ]; then
    echo "missing libpeak: ${LIBPEAK}" >&2
    exit 2
fi

summarize_calls()
{
    local kind=$1
    local label=$2
    local path=$3
    awk -v kind="$kind" -v label="$label" '
        /calls_per_sec=/ {
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^calls_per_sec=/) {
                    split($i, a, "=");
                    v = a[2] + 0.0;
                    n++;
                    sum += v;
                    if (n == 1 || v < min) {
                        min = v;
                    }
                    if (v > max) {
                        max = v;
                    }
                }
            }
        }
        END {
            if (n > 0) {
                printf "%s_CALLS_SUMMARY label=%s samples=%d min=%.3f avg=%.3f max=%.3f\n",
                    kind, label, n, min, sum / n, max;
            } else {
                printf "%s_CALLS_SUMMARY label=%s samples=0\n", kind, label;
            }
        }
    ' "$path"
}

summarize_trace()
{
    local label=$1
    local files=("${OUT}/${label}-trace-"*.csv)
    if [ ! -e "${files[0]}" ]; then
        echo "TRACE_SUMMARY label=${label} trace_files=0"
        return 0
    fi

    awk -F, -v label="$label" '
        {
            op = $4;
            if (op != "detach" && op != "reattach") {
                next;
            }
            total[op]++;
            if ($5 == "success" && $6 == "1" && $7 == "safe") {
                success_safe[op]++;
            }
            if ($7 == "classify-failed" || $13 == "classify-failed") {
                classify_failed[op]++;
            }
            if ($5 == "prepare-failed") {
                prepare_failed[op]++;
            }
            if ($5 == "gum-failed") {
                gum_failed[op]++;
            }
            if ($11 + 0.0 > 0.0) {
                stop_n[op]++;
                stop_sum[op] += $11 + 0.0;
                if (($11 + 0.0) > stop_max[op]) {
                    stop_max[op] = $11 + 0.0;
                }
            }
            if ($10 + 0 > 0) {
                batch_n[op]++;
                batch_sum[op] += $10 + 0;
                if (($10 + 0) > batch_max[op]) {
                    batch_max[op] = $10 + 0;
                }
            }
            if ($8 + 0 > 0) {
                retry_rows[op]++;
            }
        }
        END {
            for (i = 1; i <= 2; i++) {
                op = (i == 1) ? "detach" : "reattach";
                avg_stop = stop_n[op] ? stop_sum[op] / stop_n[op] : 0.0;
                avg_batch = batch_n[op] ? batch_sum[op] / batch_n[op] : 0.0;
                printf "TRACE_SUMMARY label=%s op=%s total=%d success_safe=%d classify_failed=%d prepare_failed=%d gum_failed=%d avg_stop_us=%.3f max_stop_us=%.3f avg_batch=%.3f max_batch=%d retry_rows=%d\n",
                    label, op, total[op], success_safe[op], classify_failed[op],
                    prepare_failed[op], gum_failed[op], avg_stop, stop_max[op],
                    avg_batch, batch_max[op], retry_rows[op];
            }
        }
    ' "${files[@]}"
}

BASELINE_LOG="${OUT}/baseline.log"
: > "$BASELINE_LOG"
echo "RUN_BASELINE"
for sample in $(seq 1 "$BASELINE_SAMPLES"); do
    echo "BASELINE_SAMPLE sample=${sample}"
    taskset -c "$CPUSET" "$EXE" --threads "$THREADS" --seconds "$RUN_DURATION" 2>&1 | tee -a "$BASELINE_LOG"
done
summarize_calls "BASELINE" "${SYSTEM_LABEL}" "$BASELINE_LOG"

run_stress()
{
    local spec=$1
    local label backend operation mode_arg
    IFS=: read -r label backend operation <<EOF_SPEC
${spec}
EOF_SPEC

    if [ -z "$label" ] || [ -z "$backend" ] || [ -z "$operation" ]; then
        echo "invalid stress spec: ${spec}" >&2
        exit 2
    fi
    if [ "$operation" = "detach" ]; then
        mode_arg="--disable-reattach"
    elif [ "$operation" = "reattach" ]; then
        mode_arg="--require-reattach"
    else
        echo "invalid operation in stress spec: ${spec}" >&2
        exit 2
    fi

    local max_threads=$MAX_THREADS
    if [ "$max_threads" -le 0 ]; then
        max_threads=$((THREADS + THREAD_SLACK))
    fi

    local log_path="${OUT}/${label}.log"
    local -a transition_args=()
    if [ "$FAIL_ON_TRANSITION_SKIPS" = "1" ]; then
        transition_args+=(--fail-on-transition-skips)
    fi
    if [ -n "${PEAK_HPC_MAX_CLASSIFY_FAILED:-}" ]; then
        transition_args+=(--max-classify-failed "${PEAK_HPC_MAX_CLASSIFY_FAILED}")
    fi
    if [ -n "${PEAK_HPC_MAX_PREPARE_FAILED:-}" ]; then
        transition_args+=(--max-prepare-failed "${PEAK_HPC_MAX_PREPARE_FAILED}")
    fi
    if [ -n "${PEAK_HPC_MAX_GUM_FAILED:-}" ]; then
        transition_args+=(--max-gum-failed "${PEAK_HPC_MAX_GUM_FAILED}")
    fi
    if [ -n "${PEAK_HPC_MAX_DETACH_CLASSIFY_FAILED:-}" ]; then
        transition_args+=(--max-detach-classify-failed "${PEAK_HPC_MAX_DETACH_CLASSIFY_FAILED}")
    fi
    if [ -n "${PEAK_HPC_MAX_DETACH_PREPARE_FAILED:-}" ]; then
        transition_args+=(--max-detach-prepare-failed "${PEAK_HPC_MAX_DETACH_PREPARE_FAILED}")
    fi
    if [ -n "${PEAK_HPC_MAX_DETACH_GUM_FAILED:-}" ]; then
        transition_args+=(--max-detach-gum-failed "${PEAK_HPC_MAX_DETACH_GUM_FAILED}")
    fi
    if [ -n "${PEAK_HPC_MAX_REATTACH_CLASSIFY_FAILED:-}" ]; then
        transition_args+=(--max-reattach-classify-failed "${PEAK_HPC_MAX_REATTACH_CLASSIFY_FAILED}")
    fi
    if [ -n "${PEAK_HPC_MAX_REATTACH_PREPARE_FAILED:-}" ]; then
        transition_args+=(--max-reattach-prepare-failed "${PEAK_HPC_MAX_REATTACH_PREPARE_FAILED}")
    fi
    if [ -n "${PEAK_HPC_MAX_REATTACH_GUM_FAILED:-}" ]; then
        transition_args+=(--max-reattach-gum-failed "${PEAK_HPC_MAX_REATTACH_GUM_FAILED}")
    fi
    local -a cmd=(
        "$PYTHON_BIN" "$RUNNER"
        --exe "$EXE"
        --libpeak "$LIBPEAK"
        --threads "$THREADS"
        --seconds "$RUN_DURATION"
        --samples "$STRESS_SAMPLES"
        --timeout "${PEAK_HPC_TIMEOUT:-240}"
        --trace-prefix "${OUT}/${label}-trace"
        --stats-prefix "${OUT}/${label}-stats"
        --peak-max-threads "$max_threads"
        --thread-slack "$THREAD_SLACK"
        --require-trace-diagnostics
        --overhead-ratio "$OVERHEAD_RATIO"
        --peak-cost "$PEAK_COST"
        --detach-count "$DETACH_COUNT"
        "$mode_arg"
        "${transition_args[@]}"
    )
    if [ "$backend" != "auto" ]; then
        cmd+=(--detach-backend "$backend")
    fi

    echo "RUN_STRESS label=${label} backend=${backend} operation=${operation} max_threads=${max_threads}"
    taskset -c "$CPUSET" "${cmd[@]}" 2>&1 | tee "$log_path"
    summarize_calls "STRESS" "$label" "$log_path"
    summarize_trace "$label"
}

for spec in "$@"; do
    run_stress "$spec"
done

echo "EXTREME_DONE label=${SYSTEM_LABEL}"
