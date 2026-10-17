"""Require late-provider semantics and real profiling on an existing thread."""
import argparse
import csv
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
for name in ("application", "provider", "libpeak"):
    parser.add_argument("--" + name, required=True, type=Path)
args = parser.parse_args()
env = {k: v for k, v in os.environ.items()
       if not k.startswith(("PEAK_", "FLAME_"))
       and k not in ("LD_PRELOAD", "LD_AUDIT")}
with tempfile.TemporaryDirectory(prefix="peak-throw-loading-") as directory:
    env.update(
        LD_PRELOAD=str(args.libpeak.resolve()),
        PEAK_TARGET="peak_cxx_throw_outer,peak_cxx_throw_inner,peak_cxx_throw_recursive",
        PEAK_MAX_NUM_THREADS="8", PEAK_COST="0", PEAK_HEARTBEAT_INTERVAL="0",
        PEAK_ENABLE_GLOBAL_HEARTBEAT="0", PEAK_ENABLE_PER_TARGET_HEARTBEAT="0",
        PEAK_ENABLE_REATTACH="0", PEAK_DETACH_BACKEND="signal",
        PEAK_TEXT_OUTPUT="false", PEAK_OUTPUT_AGGREGATION="local",
        PEAK_STATSLOG_PATH=str(Path(directory) / "stats"),
    )
    result = subprocess.run(
        [str(args.application.resolve()), str(args.provider.resolve())],
        env=env, capture_output=True, text=True, timeout=8)
    print(result.stdout, end="")
    assert result.returncode == 0 and not result.stderr, result.stderr
    assert "cxx_throw_late_provider_ok threads=2 calls=480 propagated=96 caught_inside=48 recursive=192" in result.stdout
    paths = list(Path(directory).glob("stats*.csv"))
    assert len(paths) == 1, paths
    with paths[0].open() as handle:
        rows = {row["function"]: row for row in csv.DictReader(handle)}
    for name, count in (("peak_cxx_throw_outer", 144),
                        ("peak_cxx_throw_inner", 144),
                        ("peak_cxx_throw_recursive", 192)):
        assert int(rows[name]["count"]) == count, rows[name]
        assert int(rows[name]["dropped_calls"]) == 0, rows[name]
        assert int(rows[name]["dropped_threads"]) == 0, rows[name]
    assert int(rows["PEAK_CAPABILITY_cpu-target"]["capability_active"]) == 1
print("cxx_throw_late_provider_profiled counts=144,144,192 drops=0")
