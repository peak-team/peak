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
    return {}


def expected_attached_records(mode):
    if mode in ("two-generations", "two-generations-heartbeat"):
        return 2
    return 1


def find_stats_csv(stats_prefix, pid):
    expected = f"{stats_prefix}-p{pid}.csv"
    if os.path.exists(expected):
        return expected

    candidates = sorted(glob.glob(f"{stats_prefix}-p*.csv"))
    if len(candidates) == 1:
        return candidates[0]
    return None


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
            extra_env_for_mode(mode),
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
    if expects_positive_count(mode):
        if stats_csv is None:
            trace = read_text(trace_path)
            raise AssertionError(
                f"{mode} JIT run did not create stats csv for pid {pid}\n"
                f"expected prefix: {stats_prefix}\ntrace={trace}\n{output}"
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
