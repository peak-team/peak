#!/usr/bin/env python3
"""Run the mmap JIT fixture under PEAK and validate statslog output."""

import argparse
import csv
import glob
import os
import re
import subprocess
import sys
import tempfile


SKIP_RETURN_CODE = 77
JIT_SYMBOL = "peak_jit_hot"
PID_RE = re.compile(r"\bpid=([0-9]+)\b")


def merge_preload(libpeak):
    existing = os.environ.get("LD_PRELOAD")
    if existing:
        return f"{libpeak}:{existing}"
    return libpeak


def make_env(libpeak, stats_prefix, map_path, trace_path, extra_env=None):
    env = os.environ.copy()
    env.update(
        {
            "LD_PRELOAD": merge_preload(libpeak),
            "PEAK_TARGET": JIT_SYMBOL,
            "PEAK_STATSLOG_PATH": stats_prefix,
            "PEAK_JIT_ENABLE": "1",
            "PEAK_JIT_PROVIDER": "perfmap",
            "PEAK_JIT_MAP_PATH": map_path,
            "PEAK_JIT_TRACE_PATH": trace_path,
            "PEAK_HEARTBEAT_INTERVAL": "0",
            "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "0",
            "PEAK_ENABLE_GLOBAL_HEARTBEAT": "0",
            "PEAK_ENABLE_REATTACH": "0",
            "PEAK_COST": "0",
        }
    )
    if extra_env:
        env.update(extra_env)
    return env


def mode_flag(mode):
    if mode in (
        "positive",
        "final-drain",
        "retry-attach",
        "final-drain-retry",
        "v8-js-optimized",
        "v8-js-csv-name",
        "v8-lazycompile-optimized",
    ):
        return "--with-perf-map"
    if mode == "partial-record":
        return "--with-partial-perf-map"
    if mode == "pre-exec":
        return "--with-pre-exec-perf-map"
    if mode == "two-generations":
        return "--with-two-generations"
    if mode == "two-generations-heartbeat":
        return "--with-two-generations"
    if mode == "stale-then-valid":
        return "--with-stale-then-valid"
    if mode == "stale-then-valid-default-timeout":
        return "--with-stale-then-valid"
    if mode == "final-drain-stale-then-valid":
        return "--with-stale-then-valid"
    if mode == "duplicate-record":
        return "--with-duplicate-perf-map"
    if mode == "malformed-then-valid":
        return "--with-malformed-then-valid"
    if mode == "overlong-then-valid":
        return "--with-overlong-then-valid"
    if mode in ("bounded-queue", "allocation-failure", "shutdown-full-queue"):
        return "--with-bounded-queue-then-valid"
    if mode == "pending-round-robin":
        return "--with-pending-round-robin"
    if mode == "pending-timeout-backlog":
        return "--with-pending-timeout-backlog"
    if mode == "truncated-generation":
        return "--with-truncated-generation"
    if mode == "truncated-during-drain-generation":
        return "--with-truncated-during-drain-generation"
    if mode == "replaced-generation":
        return "--with-replaced-generation"
    if mode == "pending-replaced-generation":
        return "--with-pending-replaced-generation"
    if mode == "attach-retry-timeout":
        return "--with-perf-map"
    if mode == "negative":
        return "--without-metadata"
    raise ValueError(f"unknown mode: {mode}")


def metadata_symbol(mode):
    if mode == "v8-js-optimized":
        return f"JS:*{JIT_SYMBOL} /tmp/peak_jit_fixture.js:1:25"
    if mode == "v8-js-csv-name":
        return f"JS:*{JIT_SYMBOL} /tmp/peak,jit \"fixture\".js:1:25"
    if mode == "v8-lazycompile-optimized":
        return f"LazyCompile:*{JIT_SYMBOL} /tmp/peak_jit_fixture.js:1:25"
    return JIT_SYMBOL


def expects_attached_record(mode):
    return mode in (
        "positive",
        "final-drain",
        "retry-attach",
        "pre-exec",
        "two-generations",
        "stale-then-valid",
        "stale-then-valid-default-timeout",
        "final-drain-stale-then-valid",
        "duplicate-record",
        "malformed-then-valid",
        "overlong-then-valid",
        "final-drain-retry",
        "partial-record",
        "v8-js-optimized",
        "v8-js-csv-name",
        "v8-lazycompile-optimized",
        "two-generations-heartbeat",
        "bounded-queue",
        "pending-round-robin",
        "allocation-failure",
        "shutdown-full-queue",
        "truncated-generation",
        "truncated-during-drain-generation",
        "replaced-generation",
        "pending-replaced-generation",
    )


def expects_positive_count(mode):
    return mode in (
        "positive",
        "retry-attach",
        "pre-exec",
        "two-generations",
        "stale-then-valid",
        "stale-then-valid-default-timeout",
        "duplicate-record",
        "malformed-then-valid",
        "overlong-then-valid",
        "partial-record",
        "v8-js-optimized",
        "v8-js-csv-name",
        "v8-lazycompile-optimized",
        "two-generations-heartbeat",
        "bounded-queue",
        "pending-round-robin",
        "allocation-failure",
        "truncated-generation",
        "truncated-during-drain-generation",
        "replaced-generation",
        "pending-replaced-generation",
    )


def extra_env_for_mode(mode):
    if mode in ("retry-attach", "final-drain-retry"):
        return {"PEAK_JIT_TEST_ATTACH_SEQUENCE": "retry,real"}
    if mode == "stale-then-valid":
        return {"PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS": "20"}
    if mode == "two-generations-heartbeat":
        return {
            "PEAK_HEARTBEAT_INTERVAL": "0.01",
            "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "1",
            "PEAK_ENABLE_GLOBAL_HEARTBEAT": "0",
            "PEAK_ENABLE_REATTACH": "0",
            "PEAK_COST": "0",
            "PEAK_JIT_DRAIN_RECORD_BUDGET": "1",
        }
    if mode in ("bounded-queue", "shutdown-full-queue"):
        return {
            "PEAK_JIT_PENDING_CAPACITY": "2",
            "PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS": "10000",
            "PEAK_JIT_DRAIN_RECORD_BUDGET": "1",
        }
    if mode == "pending-round-robin":
        return {
            "PEAK_JIT_PENDING_CAPACITY": "2",
            "PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS": "10000",
            "PEAK_JIT_DRAIN_RECORD_BUDGET": "1",
        }
    if mode == "pending-timeout-backlog":
        return {
            "PEAK_JIT_PENDING_CAPACITY": "4",
            "PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS": "20",
            "PEAK_JIT_DRAIN_RECORD_BUDGET": "1",
        }
    if mode == "allocation-failure":
        return {"PEAK_JIT_TEST_FAIL_PENDING_ALLOCATION": "1"}
    if mode == "attach-retry-timeout":
        return {
            "PEAK_JIT_TEST_ATTACH_SEQUENCE": "always-retry",
            "PEAK_JIT_ATTACH_RETRY_TIMEOUT_MS": "20",
        }
    return {}


def expected_attached_records(mode):
    if mode in ("two-generations", "two-generations-heartbeat"):
        return 2
    return 1


def find_stats_csv(stats_prefix, pid):
    expression = re.compile(
        rf"^{re.escape(stats_prefix)}-j[A-Za-z0-9_-]+-s[A-Za-z0-9_-]+-"
        rf"h[A-Za-z0-9_.-]+-r[A-Za-z0-9_-]+-p{pid}-q[0-9a-f]{{16}}\.csv$"
    )
    artifacts = sorted(glob.glob(f"{stats_prefix}-*.csv"))
    unexpected = [path for path in artifacts if expression.fullmatch(path) is None]
    if unexpected:
        raise AssertionError(f"unexpected statistics CSV artifacts: {unexpected}")
    candidates = [path for path in artifacts if expression.fullmatch(path)]
    if not candidates:
        return None
    if len(candidates) != 1:
        raise AssertionError(f"expected exactly one statistics CSV, got {candidates}")
    return candidates[0]


def parse_stats_csv(path):
    if path is None:
        return []
    with open(path, newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def symbol_count(rows, symbol):
    total = 0
    for row in rows:
        if row.get("function") != symbol:
            continue
        try:
            total += int(row.get("count", "0"))
        except ValueError:
            raise AssertionError(f"invalid count in stats row: {row}") from None
    return total


def jit_diagnostics(rows):
    matches = [row for row in rows if row.get("function") == "PEAK_JIT_DIAGNOSTICS"]
    if len(matches) != 1:
        raise AssertionError(f"expected exactly one JIT diagnostics row, got {matches}")
    return matches[0]


def diagnostic_int(row, name):
    try:
        return int(row.get(name, ""))
    except ValueError:
        raise AssertionError(f"invalid {name} in JIT diagnostics: {row}") from None


def read_text(path):
    if not os.path.exists(path):
        return ""
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def read_trace(path):
    text = read_text(path)
    rows = list(csv.reader(text.splitlines()))
    return text, rows


def trace_rows_with_result(rows, result):
    return [
        row
        for row in rows
        if len(row) >= 7
        and row[1] == "perfmap-record"
        and row[2] == "perfmap"
        and row[6] == result
    ]


def trace_has_result(rows, result):
    return bool(trace_rows_with_result(rows, result))


def cleanup_perf_map(pid):
    path = f"/tmp/perf-{pid}.map"
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    except OSError as exc:
        print(f"warning: failed to remove {path}: {exc}", file=sys.stderr)


def run_one(args, tmpdir, mode):
    stats_prefix = os.path.join(tmpdir, f"peak-jit-{mode}")
    map_path = os.path.join(tmpdir, f"perf-{mode}.map")
    trace_path = os.path.join(tmpdir, f"jit-{mode}-trace.csv")
    extra_env = extra_env_for_mode(mode)
    if mode == "truncated-during-drain-generation":
        extra_env = dict(extra_env)
        extra_env["PEAK_JIT_TEST_PRE_FINAL_STAT_BARRIER"] = os.path.join(
            tmpdir, "pre-final-stat.barrier"
        )
    if mode == "pending-replaced-generation":
        extra_env = dict(extra_env)
        extra_env["PEAK_JIT_TEST_PRE_SOURCE_OBSERVE_BARRIER"] = os.path.join(
            tmpdir, "pre-source-observe.barrier"
        )
    try:
        os.unlink(map_path)
    except FileNotFoundError:
        pass
    command = [
        args.exe,
        mode_flag(mode),
        "--iterations",
        str(args.iterations),
        "--metadata-sleep-us",
        "0" if mode in ("final-drain", "final-drain-retry", "final-drain-stale-then-valid") else str(args.metadata_sleep_us),
        "--symbol",
        metadata_symbol(mode),
    ]
    completed = subprocess.run(
        command,
        env=make_env(
            args.libpeak,
            stats_prefix,
            map_path,
            trace_path,
            extra_env,
        ),
        cwd=tmpdir,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=args.timeout,
        check=False,
    )

    output = completed.stdout + completed.stderr
    pid_match = PID_RE.search(output)
    pid = int(pid_match.group(1)) if pid_match else None

    if completed.returncode == SKIP_RETURN_CODE:
        print(output, end="")
        return {
            "mode": mode,
            "skipped": True,
            "count": 0,
            "stats_csv": None,
            "pid": pid,
        }

    if completed.returncode != 0:
        raise AssertionError(
            f"{mode} fixture failed with rc={completed.returncode}\n{output}"
        )

    if pid is None:
        raise AssertionError(f"{mode} fixture output did not include pid=\n{output}")

    stats_csv = find_stats_csv(stats_prefix, pid)
    rows = parse_stats_csv(stats_csv)
    count = symbol_count(rows, JIT_SYMBOL)
    diagnostics = jit_diagnostics(rows)
    cleanup_perf_map(pid)
    try:
        os.unlink(map_path)
    except FileNotFoundError:
        pass

    if expects_attached_record(mode):
        trace, trace_rows = read_trace(trace_path)
        attached_records = trace_rows_with_result(trace_rows, "attached")
        expected_attached = expected_attached_records(mode)
        if len(attached_records) != expected_attached:
            raise AssertionError(
                f"expected exactly {expected_attached} attached perf-map record(s), got "
                f"{len(attached_records)}\ntrace={trace}\n{output}"
            )
        if mode in ("retry-attach", "final-drain-retry"):
            retry_indexes = [
                index
                for index, row in enumerate(trace_rows)
                if len(row) >= 7
                and row[1] == "perfmap-record"
                and row[2] == "perfmap"
                and row[6] == "attach-retry"
            ]
            attached_index = trace_rows.index(attached_records[0])
            if not retry_indexes or retry_indexes[0] >= attached_index:
                raise AssertionError(
                    f"{mode} did not retain the perf-map record across an "
                    f"attach retry\ntrace={trace}\n{output}"
                )
        if mode == "partial-record" and not trace_has_result(trace_rows, "partial-record"):
            raise AssertionError(
                "partial-record mode did not observe a partial perf-map row "
                f"before attach\ntrace={trace}\n{output}"
            )
        if mode == "pre-exec" and not trace_has_result(trace_rows, "not-executable-retry"):
            raise AssertionError(
                "pre-exec mode did not retain a matching perf-map row while "
                f"the code was still non-executable\ntrace={trace}\n{output}"
            )
        if mode in ("stale-then-valid", "final-drain-stale-then-valid") and not trace_has_result(trace_rows, "not-executable-timeout"):
            raise AssertionError(
                f"{mode} mode did not time out a stale non-executable "
                f"target row before attaching the valid generation\ntrace={trace}\n{output}"
            )
        if mode == "stale-then-valid-default-timeout" and not trace_has_result(trace_rows, "not-executable-retry"):
            raise AssertionError(
                "stale-then-valid-default-timeout did not retain the stale "
                f"row as pending while still scanning later metadata\ntrace={trace}\n{output}"
            )
        if mode == "duplicate-record" and not trace_has_result(trace_rows, "not-matched"):
            raise AssertionError(
                "duplicate-record mode did not observe a no-op duplicate row "
                f"after attach\ntrace={trace}\n{output}"
            )
        if mode == "overlong-then-valid" and not trace_has_result(trace_rows, "overlong-record"):
            raise AssertionError(
                "overlong-then-valid mode did not skip an overlong complete "
                f"row before attaching the valid generation\ntrace={trace}\n{output}"
            )
        if mode == "v8-js-csv-name":
            names = [row[3] for row in trace_rows if len(row) >= 7]
            expected = metadata_symbol(mode)
            if expected not in names:
                raise AssertionError(
                    "v8-js-csv-name did not preserve a comma/quote-heavy "
                    f"symbol as one CSV field\nexpected={expected}\n"
                    f"rows={trace_rows}\ntrace={trace}\n{output}"
                )
        if mode in (
            "truncated-generation",
            "truncated-during-drain-generation",
            "replaced-generation",
        ):
            refreshed_records = trace_rows_with_result(
                trace_rows, "generation-refreshed"
            )
            generations = {
                row[7]
                for row in attached_records + refreshed_records
                if len(row) >= 8
            }
            if len(refreshed_records) != 1:
                raise AssertionError(
                    f"{mode} did not report exactly one same-address generation "
                    f"refresh\nrows={trace_rows}\ntrace={trace}"
                )
            if len(generations) != 2:
                raise AssertionError(
                    f"{mode} did not process the repeated address/name "
                    f"as a new provider lifetime\nrows={trace_rows}\ntrace={trace}"
                )
    if mode == "attach-retry-timeout":
        trace, trace_rows = read_trace(trace_path)
        if trace_has_result(trace_rows, "attached") or not trace_has_result(
            trace_rows, "attach-retry-timeout"
        ):
            raise AssertionError(
                "attach retry did not expire without attaching\n"
                f"trace={trace}\n{output}"
            )
        if diagnostic_int(diagnostics, "jit_attach_retry_timeout") != 1:
            raise AssertionError(f"missing attach retry timeout diagnostic: {diagnostics}")
    if mode == "pending-timeout-backlog":
        trace, trace_rows = read_trace(trace_path)
        timeout_rows = trace_rows_with_result(
            trace_rows, "not-executable-timeout"
        )
        backlog_end_rows = [
            row
            for row in trace_rows
            if len(row) >= 7 and row[3] == "peak_jit_backlog_63"
        ]
        if len(timeout_rows) != 1 or len(backlog_end_rows) != 1 or not (
            trace_rows.index(timeout_rows[0]) < trace_rows.index(backlog_end_rows[0])
        ):
            raise AssertionError(
                "budget-1 pending timeout did not make progress before the "
                f"metadata backlog reached EOF\ntrace={trace}\n{output}"
            )
    if mode in ("bounded-queue", "shutdown-full-queue"):
        if diagnostic_int(diagnostics, "jit_pending_queue_full") < 1:
            raise AssertionError(f"missing queue-full diagnostic: {diagnostics}")
        if diagnostic_int(diagnostics, "jit_pending_high_water") != 2:
            raise AssertionError(f"pending queue exceeded configured bound: {diagnostics}")
        trace, trace_rows = read_trace(trace_path)
        if not trace_has_result(trace_rows, "pending-queue-full"):
            raise AssertionError(
                f"queue-full record did not report its final drop outcome\n{trace}"
            )
    if mode == "pending-round-robin" and diagnostic_int(
        diagnostics, "jit_pending_high_water"
    ) != 2:
        raise AssertionError(f"round-robin fixture did not fill both slots: {diagnostics}")
    if mode in ("stale-then-valid", "final-drain-stale-then-valid") and diagnostic_int(
        diagnostics, "jit_non_executable_timeout"
    ) < 1:
        raise AssertionError(f"missing non-executable timeout diagnostic: {diagnostics}")
    if mode == "allocation-failure" and diagnostic_int(
        diagnostics, "jit_allocation_failure"
    ) != 1:
        raise AssertionError(f"missing allocation-failure diagnostic: {diagnostics}")
    if mode == "allocation-failure":
        trace, trace_rows = read_trace(trace_path)
        if not trace_has_result(trace_rows, "pending-allocation-failure"):
            raise AssertionError(
                "allocation-failure record did not report its final drop "
                f"outcome\n{trace}"
            )
    if mode in (
        "truncated-generation",
        "truncated-during-drain-generation",
        "replaced-generation",
        "pending-replaced-generation",
    ) and diagnostic_int(diagnostics, "jit_provider_generation") < 2:
        raise AssertionError(f"provider generation did not advance: {diagnostics}")
    if expects_positive_count(mode):
        if stats_csv is None:
            raise AssertionError(
                f"{mode} JIT run did not create stats csv for pid {pid}\n"
                f"expected prefix: {stats_prefix}\n{output}"
            )
        if count <= 0:
            trace = read_text(trace_path)
            raise AssertionError(
                f"{mode} JIT run did not record {JIT_SYMBOL}; "
                f"stats_csv={stats_csv}\nrows={rows}\ntrace={trace}\n{output}"
            )
    elif mode == "negative":
        if count != 0:
            raise AssertionError(
                f"negative JIT run unexpectedly recorded {JIT_SYMBOL}; "
                f"stats_csv={stats_csv} count={count}\nrows={rows}\n{output}"
            )

    print(
        f"jit_profiling_{mode}_ok pid={pid} "
        f"stats_csv={stats_csv or '<none>'} {JIT_SYMBOL}_count={count}"
    )
    return {
        "mode": mode,
        "skipped": False,
        "count": count,
        "stats_csv": stats_csv,
        "pid": pid,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    parser.add_argument(
        "--mode",
        choices=(
            "positive",
            "negative",
            "both",
            "final-drain",
            "final-drain-retry",
            "final-drain-stale-then-valid",
            "retry-attach",
            "partial-record",
            "pre-exec",
            "two-generations",
            "stale-then-valid",
            "stale-then-valid-default-timeout",
            "duplicate-record",
            "malformed-then-valid",
            "overlong-then-valid",
            "v8-js-optimized",
            "v8-js-csv-name",
            "v8-lazycompile-optimized",
            "two-generations-heartbeat",
            "bounded-queue",
            "pending-round-robin",
            "allocation-failure",
            "attach-retry-timeout",
            "shutdown-full-queue",
            "truncated-generation",
            "truncated-during-drain-generation",
            "replaced-generation",
            "pending-replaced-generation",
            "pending-timeout-backlog",
        ),
        default="both",
    )
    parser.add_argument("--iterations", type=int, default=1000000)
    parser.add_argument("--metadata-sleep-us", type=int, default=50000)
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()

    args.exe = os.path.abspath(args.exe)
    args.libpeak = os.path.abspath(args.libpeak)

    if args.iterations <= 0:
        print("--iterations must be positive", file=sys.stderr)
        return 2
    if args.metadata_sleep_us < 0:
        print("--metadata-sleep-us must be non-negative", file=sys.stderr)
        return 2
    if not os.path.exists(args.exe):
        print(f"missing fixture executable: {args.exe}", file=sys.stderr)
        return 2
    if not os.path.exists(args.libpeak):
        print(f"missing libpeak: {args.libpeak}", file=sys.stderr)
        return 2

    modes = ["positive", "negative"] if args.mode == "both" else [args.mode]
    with tempfile.TemporaryDirectory(prefix="peak-jit-profile-") as tmpdir:
        results = [run_one(args, tmpdir, mode) for mode in modes]

    if any(result["skipped"] for result in results):
        return SKIP_RETURN_CODE

    counts = {result["mode"]: result["count"] for result in results}
    print(
        "jit_profiling_check_ok "
        + " ".join(f"{mode}_count={count}" for mode, count in counts.items())
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
