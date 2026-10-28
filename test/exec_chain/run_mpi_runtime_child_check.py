#!/usr/bin/env python3
"""Exercise scoped MPI utility suppression without an MPI runtime dependency."""
import argparse
import os
from pathlib import Path
import signal
import subprocess

p = argparse.ArgumentParser()
p.add_argument("--exe", type=Path, required=True)
p.add_argument("--libpeak", type=Path, required=True)
a = p.parse_args()
base = {k: v for k, v in os.environ.items()
        if not k.startswith(("PEAK_", "PMI", "PMIX", "OMPI_", "SLURM_"))}
base.update(LD_PRELOAD=str(a.libpeak.resolve()), PEAK_TARGET="utility_target",
            PEAK_TEXT_OUTPUT="1", PEAK_OUTPUT_AGGREGATION="local",
            PEAK_HEARTBEAT_INTERVAL="0", PEAK_MAX_NUM_THREADS="16")
for global_mode, mode in [("1", "system"), ("1", "popen"),
                          ("1", "exec"), ("1", "spawn"),
                          ("1", "descendant"),
                          ("1", "custom-global-off"),
                          ("1", "failure"), ("1", "null"), ("1", "normal"),
                          ("0", "system")]:
    env = dict(base, PEAK_MPI_GLOBAL_BUDGET=global_mode)
    proc = subprocess.Popen([str(a.exe.resolve()), mode], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, start_new_session=True)
    try:
        out, err = proc.communicate(timeout=15)
    finally:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait()
    assert proc.returncode == 0, (global_mode, mode, out, err)
    assert "parent environment unchanged" in out, (mode, out, err)
    if mode not in ("failure", "null"):
        expected = int(mode in ("normal", "custom-global-off") or global_mode == "0")
        assert f"child active={expected} marker=0" in out, (mode, out, err)
        if mode == "descendant":
            assert "child active=1 marker=0" in out, (mode, out, err)
        if mode == "custom-global-off":
            assert out.count("child active=1 marker=0") == 2, (mode, out, err)
    if global_mode == "1" and mode not in ("normal", "null", "descendant", "custom-global-off"):
        assert err.count("PEAK Library Performance Report") == 1, (mode, out, err)
print("mpi_runtime_child_check_ok")
