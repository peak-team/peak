#!/usr/bin/env python3
"""Run reversed fixed-work MPI comparisons and audit rank-local coverage."""

import argparse
import csv
import math
import os
from pathlib import Path
import re
import shlex
import signal
import statistics
import subprocess
import sys


RANK_ID = re.compile(r"-r(\d+)-p(\d+)-")
TARGETS = {"fixture_hot_a", "fixture_hot_b", "fixture_sentinel"}
WORK_KEYS = ("rounds", "base", "calls", "compute_checksum", "leaf_checksum", "sentinel_checksum")


def launch(command, environment, log, timeout):
    with log.open("w") as output, subprocess.Popen(
        command, env=environment, stdout=output, stderr=subprocess.STDOUT, start_new_session=True
    ) as process:
        try:
            status = process.wait(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                pass
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
            raise
    if status:
        raise ValueError(f"launcher exited {status}; see {log}")


def read_work(log, mode, rounds, base, hot):
    ranks = {}
    for line in log.read_text().splitlines():
        if not line.startswith("fixed "):
            continue
        row = dict(field.split("=", 1) for field in line.split()[1:])
        rank = int(row["rank"])
        if (rank in ranks or row["mode"] != mode or int(row["rounds"]) != rounds
                or int(row["base"]) != base or int(row["calls"]) != rounds * hot // 2
                or int(row["elapsed_ns"]) <= 0):
            raise ValueError(f"invalid work record in {log}: {line}")
        ranks[rank] = row
    if set(ranks) != {0, 1}:
        raise ValueError(f"expected two unique rank work records in {log}")
    return ranks


def read_coverage(prefix, ranks, rounds, full):
    reports = {}
    for path in prefix.parent.glob(prefix.name + "-*-ranklocal-*.csv"):
        match = RANK_ID.search(path.name)
        if match is None:
            raise ValueError(f"unexpected report name {path}")
        rank = int(match[1])
        if rank not in ranks or rank in reports or match[2] != ranks[rank]["pid"]:
            raise ValueError(f"unmatched or duplicate report {path}")
        with path.open(newline="") as stream:
            reports[rank] = {}
            for row in csv.DictReader(stream):
                target = row["function"]
                if target in reports[rank]:
                    raise ValueError(f"duplicate report function in {path}: {target}")
                reports[rank][target] = row
    if reports.keys() != ranks.keys():
        raise ValueError(f"expected one CSV report from each rank for {prefix}")

    coverage = []
    for rank, report in sorted(reports.items()):
        if report.get("PEAK_CAPABILITY_cpu-target", {}).get("capability_active") != "1":
            raise ValueError(f"CPU targets unavailable on rank {rank} for {prefix}")
        offered = {"fixture_sentinel": rounds,
                   ("fixture_hot_a", "fixture_hot_b")[rank]: int(ranks[rank]["calls"])}
        for target in sorted(TARGETS):
            limit = offered.get(target, 0)
            row = report.get(target)
            if not limit and row and int(row["count"]):
                raise ValueError(f"unexpected calls to {target} on rank {rank} for {prefix}")
            if not limit:
                continue
            values = dict(count=int(row["count"]) if row else 0)
            values.update({key: int(row[key]) if row else "" for key in
                           ("dropped_calls", "dropped_threads", "ever_detached", "ever_reattached")})
            if not 0 <= values["count"] <= limit:
                raise ValueError(f"invalid coverage for {target} on rank {rank} for {prefix}")
            status = "reported_no_drops"
            if row is None:
                status = "missing_target_row"
            elif values["dropped_calls"] or values["dropped_threads"]:
                status = "reported_drops"
            if full and (values["count"] != limit or any(values[k] for k in values if k != "count")):
                raise ValueError(f"full control had incomplete coverage or a transition for {target} on rank {rank}")
            coverage.append(dict(rank=rank, target=target, offered=limit,
                                 coverage=values["count"] / limit, report_row_present=bool(row),
                                 coverage_status=status, **values))
    return coverage


def write_csv(path, rows):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(dict.fromkeys(k for row in rows for k in row)))
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--libpeak", type=Path, required=True)
    parser.add_argument("--launcher", default="mpiexec -n 2")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--rounds", type=int, required=True)
    parser.add_argument("--base", type=int, required=True)
    parser.add_argument("--hot", type=int, required=True)
    parser.add_argument("--cooldown-ms", type=int, default=100,
                        help="q5/q10 reattach cooldown in milliseconds (default: 100)")
    parser.add_argument("--no-reattach", action="store_true",
                        help="disable reattach in q5/q10 for first-detach diagnostics")
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    if (args.rounds <= 0 or args.rounds % 2 or args.hot <= 0 or args.hot % 2
            or args.base <= 0 or not math.isfinite(args.timeout) or args.timeout <= 0):
        parser.error("rounds and hot must be positive even integers, and base and timeout must be positive")
    if not 1 <= args.cooldown_ms <= 2**32 - 1:
        parser.error("cooldown-ms must be a positive 32-bit unsigned integer")
    launcher = shlex.split(args.launcher)
    if not launcher:
        parser.error("launcher must name a two-rank MPI launcher")
    exe, library, directory = (path.resolve() for path in (args.exe, args.libpeak, args.out))
    if not exe.is_file() or not library.is_file():
        parser.error("exe and libpeak must refer to existing files")
    directory.mkdir(parents=True, exist_ok=False)

    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("PEAK_") and key not in ("LD_PRELOAD", "LD_AUDIT")}
    launches, ranks, coverage = [], [], []
    expected_work = None
    for mode in ("rotating", "common"):
        for block, arms in enumerate((("full", "q5", "q10"), ("q10", "q5", "full")), 1):
            block_rows = []
            for arm in ("native-before", *arms, "native-after"):
                context = dict(mode=mode, block=block, arm=arm)
                name = f"{mode}-{block}-{arm}"
                log, prefix = directory / f"{name}.log", directory / f"{name}.csv"
                controlled = arm in ("q5", "q10")
                reattach = controlled and not args.no_reattach
                params = [] if arm.startswith("native") else [
                    f"LD_PRELOAD={library}", "PEAK_TARGET=fixture_hot_a,fixture_hot_b,fixture_sentinel",
                    "PEAK_ENABLE_PER_TARGET_HEARTBEAT=0", f"PEAK_ENABLE_REATTACH={int(reattach)}",
                    "PEAK_MPI_ACTIVE_CLOCK=1", "PEAK_HEARTBEAT_INTERVAL=0.1",
                    "PEAK_OUTPUT_AGGREGATION=local", f"PEAK_STATSLOG_PATH={prefix}", "PEAK_TEXT_OUTPUT=0",
                    f"PEAK_ENABLE_GLOBAL_HEARTBEAT={int(arm != 'full')}",
                    f"PEAK_OVERHEAD_RATIO={0.05 if arm == 'q5' else 0.10}",
                    f"PEAK_GLOBAL_OVERHEAD_RATIO={0.05 if arm == 'q5' else 0.10}",
                ]
                if reattach:
                    params.append(f"PEAK_REATTACH_COOLDOWN_MS={args.cooldown_ms}")
                command = [*launcher, "env", *params, str(exe), mode,
                           str(args.rounds), str(args.base), str(args.hot)]
                (directory / f"{name}.command").write_text(shlex.join(command) + "\n")
                launch(command, environment, log, args.timeout)
                work = read_work(log, mode, args.rounds, args.base, args.hot)
                checksum = {rank: tuple(row[key] for key in WORK_KEYS) for rank, row in work.items()}
                if expected_work is None:
                    expected_work = checksum
                elif checksum != expected_work:
                    raise ValueError(f"fixed work or checksum mismatch in {log}")
                ranks.extend({**row, **context} for row in work.values())
                launch_row = dict(context, critical_rank_ns=max(int(row["elapsed_ns"])
                                                                for row in work.values()))
                if params:
                    target_rows = read_coverage(prefix, work, args.rounds, arm == "full")
                    coverage.extend(dict(context, **row) for row in target_rows)
                    missing = sum(row["coverage_status"] == "missing_target_row" for row in target_rows)
                    with_drops = sum(row["coverage_status"] == "reported_drops" for row in target_rows)
                    launch_row.update(
                        reattach_enabled=reattach,
                        cooldown_ms=args.cooldown_ms if reattach else "",
                        missing_target_rows=missing,
                        target_rows_with_reported_drops=with_drops,
                    )
                    if missing or with_drops:
                        print(f"{name}: {missing} missing target rows, {with_drops} rows reporting drops; "
                              f"see {directory / 'coverage.csv'}", file=sys.stderr)
                block_rows.append(launch_row)
            native = [block_rows[i]["critical_rank_ns"] for i in (0, -1)]
            for row in block_rows[1:-1]:
                elapsed = row["critical_rank_ns"]
                row.update(native_before_ns=native[0], native_after_ns=native[1],
                           overhead_pct=100 * (elapsed / statistics.mean(native) - 1),
                           endpoint_low_pct=100 * (elapsed / max(native) - 1),
                           endpoint_high_pct=100 * (elapsed / min(native) - 1))
            launches.extend(block_rows)
    write_csv(directory / "launches.csv", launches)
    write_csv(directory / "ranks.csv", ranks)
    write_csv(directory / "coverage.csv", coverage)


if __name__ == "__main__":
    main()
