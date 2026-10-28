"""Exercise the public attach executable against a non-preloaded live process."""
import argparse
import csv
import os
from pathlib import Path
import signal
import shutil
import subprocess
import tempfile
import time


def reap(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)


def run(command, env, timeout=20):
    process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        out, err = process.communicate(timeout=timeout)
        return process.returncode, out + err
    finally:
        reap(process)


def wait_for(predicate, timeout=10):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError("timed out waiting for attach test condition")


def counter(path):
    try:
        return int(path.read_text().strip())
    except (OSError, ValueError):
        return 0


def alive(process, progress):
    assert process.poll() is None, "profiling terminated the target"
    before = counter(progress)
    wait_for(lambda: counter(progress) > before, 3)


def report(prefix):
    paths = list(prefix.parent.glob(prefix.name + "*.csv"))
    assert paths, f"no normal PEAK CSV for {prefix}"
    rows = []
    for path in paths:
        with path.open() as stream:
            rows.extend(csv.DictReader(stream))
    targets = [r for r in rows if r.get("function") == "peak_attach_existing_target"]
    assert targets, rows
    assert sum(int(r["count"]) for r in targets) > 0, targets
    for row in targets:
        for column in ("overhead_s", "ever_detached", "ever_reattached",
                       "dropped_calls", "dropped_threads"):
            assert column in row, (column, row)
        assert float(row["total_s"]) > 0, row
    assert any(r.get("function") == "peak_attach_churn_target"
               and int(r["count"]) > 0 for r in rows), rows
    assert any(r.get("function") == "PEAK_CAPABILITY_cpu-target"
               and int(r["capability_active"]) == 1 for r in rows), rows


parser = argparse.ArgumentParser()
parser.add_argument("--cli", type=Path, required=True)
parser.add_argument("--agent", type=Path)
parser.add_argument("--fixture", type=Path)
parser.add_argument("--require-injection", action="store_true")
parser.add_argument("--mode", choices=("cli", "runtime", "memory"), required=True)
args = parser.parse_args()
env = {k: v for k, v in os.environ.items()
       if not k.startswith(("PEAK_", "FLAME_"))
       and k not in ("LD_PRELOAD", "LD_AUDIT", "DYLD_INSERT_LIBRARIES")}
cli = str(args.cli.resolve())
if args.mode == "cli":
    code, output = run([cli, "--help"], env)
    assert code == 0 and "Usage:" in output, (code, output)
    for bad in ([], ["0"], ["-1"], ["garbage"],
                ["123", "--target", "x", "--duration", "nan"],
                ["123", "--target", "x", "--duration", "0"],
                ["123", "--target", ""], ["123", "--unknown"]):
        code, output = run([cli] + bad, env)
        assert code != 0 and output.strip(), (bad, code, output)
    code, output = run([cli, "123", "--target", "x", "--agent",
                        "/nonexistent/peak-agent"], env)
    assert code != 0 and "agent" in output.lower(), (code, output)
    print("attach_cli_ok")
else:
    with tempfile.TemporaryDirectory(prefix="peak-attach-test-") as directory:
        root = Path(directory)
        progress = root / "progress"
        log = (root / "fixture.log").open("w+")
        target = subprocess.Popen([str(args.fixture.resolve()), str(progress)],
                                  env=env, stdin=subprocess.PIPE, stdout=log, stderr=log,
                                  start_new_session=True)
        try:
            wait_for(lambda: counter(progress) > 20 or target.poll() is not None)
            if target.poll() == 77:
                print("SKIP: fixture cannot enable Linux ptrace permission")
                raise SystemExit(77)
            alive(target, progress)
            agent_options = ["--agent", str(args.agent.resolve())] if args.agent else []
            base = [cli, str(target.pid)] + agent_options
            env.update(PEAK_HEARTBEAT_INTERVAL="0.05", PEAK_COST="0.01",
                       PEAK_ENABLE_PER_TARGET_HEARTBEAT="1",
                       PEAK_ENABLE_GLOBAL_HEARTBEAT="1", PEAK_ENABLE_REATTACH="1",
                       PEAK_HIBERNATION_CYCLE="1", PEAK_REATTACH_COOLDOWN_MS="0",
                       PEAK_OVERHEAD_RATIO="0.00000001",
                       PEAK_GLOBAL_OVERHEAD_RATIO="0.00000001",
                       PEAK_DETACH_BACKEND="signal", PEAK_REQUIRE_SAFE_DETACH="1",
                       PEAK_DETACH_TRACE_PATH=str(root / "detach.csv"),
                       PEAK_OUTPUT_AGGREGATION="local", PEAK_TEXT_OUTPUT="false")
            if args.mode == "memory":
                env.update(PEAK_MEMORY_PROFILE="TRUE", PEAK_MEMORY_TRACK_ALL="TRUE",
                           PEAK_MEMLOG_PATH=str(root / "memlog"),
                           PEAK_ENABLE_PER_TARGET_HEARTBEAT="0",
                           PEAK_ENABLE_GLOBAL_HEARTBEAT="0")
            prefix = root / "stats"
            code, output = run(base + ["--target",
                                      "peak_attach_existing_target,peak_attach_churn_target",
                                      "--output", str(prefix)], env)
            # Only explicit OS security failures may skip real injection coverage.
            denied = ("ptrace operation not permitted", "permission denied",
                      "operation not permitted", "not allowed to attach",
                      "unable to access process with pid")
            if code and any(reason in output.lower() for reason in denied):
                if args.require_injection:
                    raise AssertionError("native injection required but OS denied it:\n" + output)
                print("SKIP: platform denied native attach:\n" + output)
                raise SystemExit(77)
            assert code == 0, (code, output, target.poll(), (root / "fixture.log").read_text())
            alive(target, progress)
            code, output = run(base + ["--target", "peak_attach_existing_target"], env)
            assert code != 0 and output.strip(), (code, output)
            alive(target, progress)
            # A byte-for-byte copied agent has different image identity too.
            if args.agent:
                copied_agent = root / ("arbitrary-copy" + args.agent.suffix)
                shutil.copy2(args.agent, copied_agent)
                code, output = run([cli, str(target.pid), "--agent", str(copied_agent),
                                    "--target", "peak_attach_existing_target"], env)
                assert code != 0 and "already" in output.lower(), (code, output)
                alive(target, progress)
            time.sleep(1.5)
            target.stdin.write(b"Q")
            target.stdin.flush()
            assert target.wait(timeout=20) == 0, (root / "fixture.log").read_text()
            assert "statfs_errors=0" in (root / "fixture.log").read_text(), (root / "fixture.log").read_text()
            report(prefix)
            if args.mode == "runtime":
                with (root / "detach.csv").open() as stream:
                    transitions = list(csv.reader(stream))
                for operation in ("detach", "reattach"):
                    assert any(len(row) >= 7 and row[3] == operation
                               and row[4:7] == ["success", "1", "safe"]
                               for row in transitions), (operation, transitions)
            else:
                memory_logs = list(root.glob("memlog*.csv"))
                assert memory_logs, list(root.iterdir())
                memory_events = []
                for path in memory_logs:
                    with path.open() as stream:
                        memory_events.extend(csv.DictReader(stream))
                allocated = [row for row in memory_events if int(row["delta"]) == 256]
                freed = [row for row in memory_events if int(row["delta"]) == -256]
                assert allocated and freed, (memory_events[:20], (root / "fixture.log").read_text())
                assert len({row["tid"] for row in allocated}) >= 3, allocated[:20]
            # Unresolved selectors remain available for future dlopen activity.
            other_progress = root / "missing-progress"
            other = subprocess.Popen([str(args.fixture.resolve()), str(other_progress)],
                                     env=env, stdin=subprocess.PIPE, stdout=log,
                                     stderr=log, start_new_session=True)
            try:
                wait_for(lambda: counter(other_progress) > 20)
                code, output = run([cli, str(other.pid)] + agent_options + ["--target",
                                    "peak_attach_missing_target"], env)
                assert code == 0 and output.strip(), (code, output)
                assert "pending" in output.lower(), output
                alive(other, other_progress)
                other.stdin.write(b"Q")
                other.stdin.flush()
                assert other.wait(timeout=20) == 0
            finally:
                reap(other)
                other.stdin.close()
            # PID known to have exited avoids machine-specific PID limits.
            dead = subprocess.Popen(["/bin/true"], start_new_session=True)
            dead.wait(timeout=3)
            code, output = run([cli, str(dead.pid)] + agent_options +
                               ["--target", "x"], env)
            assert code != 0 and output.strip(), (code, output)
        finally:
            reap(target)
            if target.stdin:
                target.stdin.close()
            log.close()
    print("attach_" + args.mode + "_ok")
