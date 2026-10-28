"""Exercise the public attach executable against a non-preloaded live process."""
import argparse
import csv
from concurrent.futures import ThreadPoolExecutor
import os
import re
from pathlib import Path
import signal
import shutil
import sys
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


def run(command, env, timeout=20, fixture_log=None):
    process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        out, err = process.communicate(timeout=timeout)
        return process.returncode, out + err
    except subprocess.TimeoutExpired as expired:
        # communicate retains its buffers across retries. Replace each partial
        # buffer with the later complete one instead of duplicating output.
        out, err = expired.stdout, expired.stderr
        reap(process)
        try:
            out, err = process.communicate(timeout=1)
        except subprocess.TimeoutExpired as final:
            out = final.stdout if final.stdout is not None else out
            err = final.stderr if final.stderr is not None else err
        def decoded(value):
            return value.decode(errors="replace") if isinstance(value, bytes) else value or ""
        fixture = ""
        if fixture_log is not None:
            try:
                fixture = fixture_log.read_text(errors="replace")
            except OSError as error:
                fixture = str(error)
        raise AssertionError(
            f"attach command timed out after {timeout}s: {command!r}\n"
            f"CLI stdout:\n{decoded(out)}\nCLI stderr:\n{decoded(err)}\n"
            f"fixture log:\n{fixture}") from None
    finally:
        reap(process)
        process.stdout.close()
        process.stderr.close()


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


def alive(process, progress, fixture_log=None):
    try:
        assert process.poll() is None, "profiling terminated the target"
        before = counter(progress)
        wait_for(lambda: counter(progress) > before, 3)
    except AssertionError as error:
        detail = fixture_evidence(process, fixture_log) if fixture_log else ""
        raise AssertionError(f"{error}; target status={process.poll()}\n{detail}") from error


def fixture_evidence(process, log_path):
    evidence = log_path.read_text(errors="replace")
    status = process.poll()
    if sys.platform.startswith("linux"):
        tracers = set()
        for task in sorted(Path(f"/proc/{process.pid}/task").glob("*/status"))[:24]:
            try:
                lines = task.read_text().splitlines()
            except OSError:
                continue
            state = [line for line in lines if line.startswith(("Name:", "State:", "TracerPid:"))]
            evidence += f"\n{task}: {state!r}"
            tracers.update(int(line.split(":", 1)[1]) for line in state
                           if line.startswith("TracerPid:") and int(line.split(":", 1)[1]))
        for tracer in tracers:
            try:
                evidence += f"\ntracer {tracer} comm: " + Path(f"/proc/{tracer}/comm").read_text().strip()
            except OSError:
                pass
    if sys.platform != "darwin" or status is None or status >= 0:
        return evidence
    directories = [Path.home() / "Library/Logs/DiagnosticReports",
                   Path("/Library/Logs/DiagnosticReports")]
    started = getattr(process, "peak_started_at", time.time() - 60)
    deadline = time.monotonic() + 6
    expected_pid = re.compile(r'"pid"\s*:\s*' + str(process.pid) + r'\b|\[' + str(process.pid) + r'\]')
    while time.monotonic() < deadline:
        for directory in directories:
            try:
                candidates = list(directory.glob("*attach*"))
            except OSError:
                continue
            for report in candidates:
                try:
                    if report.suffix not in (".ips", ".crash") or report.stat().st_mtime < started - 2:
                        continue
                    content = report.read_text(errors="replace")[:262144]
                except OSError:
                    continue
                if expected_pid.search(content):
                    return evidence + f"\nNative crash report {report}:\n{content}"
        time.sleep(0.2)
    return evidence + (f"\nNo native crash report for PID {process.pid} signal {-status} "
                       f"within 6s; inspected {directories!r}")


def filesystem_phase(target, log_path, phase):
    marker = "statfs_phase=" + phase
    previous = log_path.read_text().count(marker)
    target.stdin.write(phase.encode())
    target.stdin.flush()
    wait_for(lambda: log_path.read_text().count(marker) > previous, 3)


def report(prefix):
    paths = list(prefix.parent.glob(prefix.name + "*.csv"))
    assert len(paths) == 1, f"expected one PEAK runtime report for {prefix}: {paths}"
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
if sys.platform == "darwin":
    env["PEAK_ATTACH_DIAGNOSTICS"] = "1"
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
        target.peak_started_at = time.time()
        try:
            wait_for(lambda: counter(progress) > 20 or target.poll() is not None)
            if target.poll() == 77:
                print("SKIP: fixture cannot enable Linux ptrace permission")
                raise SystemExit(77)
            alive(target, progress, root / "fixture.log")
            agent_options = ["--agent", str(args.agent.resolve())] if args.agent else []
            base = [cli, str(target.pid)] + agent_options
            env.update(PEAK_HEARTBEAT_INTERVAL="0.05", PEAK_COST="0.01",
                       PEAK_ENABLE_PER_TARGET_HEARTBEAT="1",
                       PEAK_ENABLE_GLOBAL_HEARTBEAT="1", PEAK_ENABLE_REATTACH="1",
                       PEAK_HIBERNATION_CYCLE="1", PEAK_REATTACH_COOLDOWN_MS="0",
                       PEAK_OVERHEAD_RATIO="0.00000001",
                       PEAK_GLOBAL_OVERHEAD_RATIO="1.0",
                       PEAK_GLOBAL_REATTACH_FACTOR="1.0",
                       PEAK_DETACH_BACKEND="signal", PEAK_REQUIRE_SAFE_DETACH="1",
                       PEAK_DETACH_TRACE_PATH=str(root / "detach.csv"),
                       PEAK_OUTPUT_AGGREGATION="local", PEAK_TEXT_OUTPUT="false")
            if args.mode == "memory":
                env.update(PEAK_MEMORY_PROFILE="TRUE", PEAK_MEMORY_TRACK_ALL="TRUE",
                           PEAK_MEMLOG_PATH=str(root / "memlog"),
                           PEAK_ENABLE_PER_TARGET_HEARTBEAT="0",
                           PEAK_ENABLE_GLOBAL_HEARTBEAT="0")
            if sys.platform == "darwin":
                # Exercise the libSystem-only loader before any PEAK runtime
                # state exists. A failed dlopen must leave this target usable
                # for the immediately following normal activation.
                invalid_agent = root / "readable-invalid-agent.dylib"
                invalid_agent.write_text("This is deliberately not a Mach-O image.\n")
                filesystem_phase(target, root / "fixture.log", "B")
                code, output = run([cli, str(target.pid), "--agent", str(invalid_agent),
                                    "--target", "peak_attach_existing_target"], env,
                                   fixture_log=root / "fixture.log")
                assert code != 0 and "Darwin loader" in output, (code, output)
                assert not any(word in output.lower() for word in
                               ("timed out", "connection was lost",
                                "activation was not confirmed")), output
                alive(target, progress, root / "fixture.log")
                filesystem_phase(target, root / "fixture.log", "B")
            # Correctable preflight failures must release their claim/config
            # without creating hooks, reports, or a second runtime instance.
            rejected_prefix = root / "rejected"
            retry_cases = [
                ("!", env, "invalid PEAK target selector"),
                ("peak_attach_existing_target", dict(env, PEAK_EXEC_CHAIN="1"),
                 "late attach cannot propagate PEAK across exec"),
            ]
            for selector, rejected_env, diagnostic in retry_cases:
                filesystem_phase(target, root / "fixture.log", "B")
                code, output = run(base + ["--target", selector, "--output",
                                          str(rejected_prefix)], rejected_env,
                                   fixture_log=root / "fixture.log")
                assert code != 0 and diagnostic in output, (
                    code, output, fixture_evidence(target, root / "fixture.log"))
                assert "already" not in output.lower(), output
                alive(target, progress, root / "fixture.log")
                assert not list(root.glob("rejected*.csv")), list(root.iterdir())
            filesystem_phase(target, root / "fixture.log", "B")
            prefix = root / "stats"
            code, output = run(base + ["--target",
                                      "peak_attach_existing_target,peak_attach_churn_target",
                                      "--output", str(prefix)], env,
                                   fixture_log=root / "fixture.log")
            # Only explicit OS security failures may skip real injection coverage.
            denied = ("ptrace operation not permitted", "permission denied",
                      "operation not permitted", "not allowed to attach",
                      "unable to access process with pid")
            if code and any(reason in output.lower() for reason in denied):
                if args.require_injection:
                    raise AssertionError("native injection required but OS denied it:\n" + output)
                print("SKIP: platform denied native attach:\n" + output)
                raise SystemExit(77)
            assert code == 0, (code, output, target.poll(), fixture_evidence(target, root / "fixture.log"))
            alive(target, progress, root / "fixture.log")
            filesystem_phase(target, root / "fixture.log", "G")
            alive(target, progress, root / "fixture.log")
            filesystem_phase(target, root / "fixture.log", "B")
            with ThreadPoolExecutor(max_workers=2) as clients:
                duplicates = [clients.submit(
                    run, base + ["--target", "peak_attach_existing_target"], env,
                    fixture_log=root / "fixture.log") for _ in range(2)]
                for duplicate in duplicates:
                    code, output = duplicate.result()
                    assert code != 0 and "already" in output.lower(), (
                        code, output, fixture_evidence(target, root / "fixture.log"))
            alive(target, progress, root / "fixture.log")
            # A byte-for-byte copied agent has different image identity too.
            if args.agent:
                copied_agent = root / ("arbitrary-copy" + args.agent.suffix)
                shutil.copy2(args.agent, copied_agent)
                filesystem_phase(target, root / "fixture.log", "B")
                code, output = run([cli, str(target.pid), "--agent", str(copied_agent),
                                    "--target", "peak_attach_existing_target"], env)
                assert code != 0 and "already" in output.lower(), (code, output)
                alive(target, progress, root / "fixture.log")
            filesystem_phase(target, root / "fixture.log", "G")
            if args.mode == "runtime":
                trace = root / "detach.csv"
                with trace.open() if trace.exists() else open(os.devnull) as stream:
                    earlier_rows = len(list(csv.reader(stream)))
                def completed_cycle():
                    trace = root / "detach.csv"
                    if not trace.exists():
                        return False
                    with trace.open() as stream:
                        rows = list(csv.reader(stream))[earlier_rows:]
                    return all(any(len(row) >= 7 and row[3] == operation
                                   and row[4:7] == ["success", "1", "safe"]
                                   for row in rows)
                               for operation in ("detach", "reattach"))
                wait_for(completed_cycle, 12)
            else:
                time.sleep(1.5)

            target.stdin.write(b"Q")
            target.stdin.flush()
            assert target.wait(timeout=20) == 0, fixture_evidence(target, root / "fixture.log")
            assert "statfs_errors=0" in (root / "fixture.log").read_text(), (root / "fixture.log").read_text()
            guarded = [line for line in (root / "fixture.log").read_text().splitlines()
                       if line.startswith("statfs_guarded_calls=")]
            assert guarded and int(guarded[-1].split("=")[1]) > 0, guarded
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
                alive(other, other_progress, root / "fixture.log")
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
