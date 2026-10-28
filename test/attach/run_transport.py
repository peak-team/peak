"""Bounded native injection timeout/cancellation, using a silent valid agent."""
import argparse
import os
from pathlib import Path
import signal
import sys
import subprocess
import tempfile
import time

p = argparse.ArgumentParser()
for name in ("cli", "fixture", "agent"):
    p.add_argument("--" + name, type=Path, required=True)
a = p.parse_args()
env = {k: v for k, v in os.environ.items() if not k.startswith(("PEAK_", "FLAME_"))
       and k not in ("LD_PRELOAD", "LD_AUDIT", "DYLD_INSERT_LIBRARIES")}

if sys.platform == "darwin":
    env["PEAK_ATTACH_DIAGNOSTICS"] = "1"


def census():
    snapshot = subprocess.run(["ps", "-axo", "pid=,ppid=,pgid=,lstart="],
                              capture_output=True, text=True, timeout=2, check=True)
    rows = {}
    for line in snapshot.stdout.splitlines():
        fields = line.split(maxsplit=3)
        if len(fields) == 4:
            pid, parent, group = map(int, fields[:3])
            rows[pid] = (parent, group, fields[3])
    return rows


def collect_owned(host_pid, tracked, snapshot):
    # Record ancestry while it exists; Core's helper later creates its own
    # session and can outlive/reparent away from the isolated injector worker.
    parents = {host_pid}
    parents.update(pid for pid, (_, born) in tracked.items()
                   if pid in snapshot and snapshot[pid][2] == born)
    changed = True
    while changed:
        changed = False
        for pid, (parent, group, born) in snapshot.items():
            if parent in parents and pid not in parents:
                tracked[pid] = (group, born)
                parents.add(pid)
                changed = True


def matching_owned(tracked, snapshot):
    return {pid: (group, born) for pid, (group, born) in tracked.items()
            if pid in snapshot and snapshot[pid][1:] == (group, born)}


def surviving_groups(tracked):
    groups = set()
    for group in {group for group, _ in tracked.values()}:
        try:
            os.killpg(group, 0)
        except ProcessLookupError:
            continue
        except PermissionError:
            pass
        groups.add(group)
    return groups


def cleanup_owned(tracked):
    # Never signal a recycled PID/group or a process selected by its name.
    for sig in (signal.SIGTERM, signal.SIGKILL):
        live = matching_owned(tracked, census())
        for pid, (group, _) in live.items():
            try:
                if pid == group:
                    os.killpg(group, sig)
                else:
                    os.kill(pid, sig)
            except ProcessLookupError:
                pass
        if live and sig == signal.SIGTERM:
            time.sleep(0.1)


def reap(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=3)


def progress(path):
    try:
        return int(path.read_text())
    except (OSError, ValueError):
        return 0


def wait_progress(path, before):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if progress(path) > before:
            return
        time.sleep(0.02)
    raise AssertionError("target stopped making independent progress")


with tempfile.TemporaryDirectory(prefix="peak-attach-transport-") as directory:
    root = Path(directory)
    path = root / "progress"
    with (root / "target.log").open("w+") as log:
        target = subprocess.Popen([str(a.fixture.resolve()), str(path)], env=env,
                                  stdin=subprocess.PIPE, stdout=log, stderr=log,
                                  start_new_session=True)
        try:
            wait_progress(path, 20)
            command = [str(a.cli.resolve()), str(target.pid), "--target",
                       "peak_attach_existing_target", "--agent", str(a.agent.resolve())]
            for cancel in (True, False):
                started = time.monotonic()
                host = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True,
                                        start_new_session=True)
                tracked = {}
                try:
                    deadline = time.monotonic() + 3
                    saw_worker = saw_helper = False
                    while time.monotonic() < deadline and host.poll() is None:
                        snapshot = census()
                        collect_owned(host.pid, tracked, snapshot)
                        workers = {pid for pid, (parent, _, _) in snapshot.items()
                                   if parent == host.pid and pid in tracked}
                        saw_worker |= bool(workers)
                        saw_helper |= any(parent in workers for parent, _, _ in snapshot.values())
                        if saw_worker and (sys.platform != "darwin" or saw_helper):
                            break
                        time.sleep(0.01)
                    assert saw_worker, "did not observe isolated injection worker"
                    if sys.platform == "darwin":
                        assert saw_helper, "did not observe Core helper before cancellation/timeout"
                    if cancel:
                        host.send_signal(signal.SIGINT)
                    # Continue observing ancestry during teardown, including
                    # children launched after the initial worker observation.
                    deadline = started + 20
                    while host.poll() is None and time.monotonic() < deadline:
                        collect_owned(host.pid, tracked, census())
                        time.sleep(0.02)
                    stdout, stderr = host.communicate(timeout=1)
                    assert host.returncode != 0, (stdout, stderr)
                    elapsed = time.monotonic() - started
                    assert elapsed < (9 if cancel else 19), elapsed
                    if not cancel:
                        assert elapsed >= 14, (elapsed, stderr)
                    assert "interrupted" in stderr or "timed out" in stderr, stderr
                    deadline = time.monotonic() + 3
                    live = matching_owned(tracked, census())
                    groups = surviving_groups(tracked)
                    while (live or groups) and time.monotonic() < deadline:
                        time.sleep(0.02)
                        live = matching_owned(tracked, census())
                        groups = surviving_groups(tracked)
                    assert not live and not groups, (
                        "owned injector/helper survived CLI exit", live, groups, stderr)
                except Exception as error:
                    cleanup_owned(tracked)
                    reap(host)
                    try:
                        stdout, stderr = host.communicate(timeout=1)
                    except subprocess.TimeoutExpired as partial:
                        stdout, stderr = partial.stdout, partial.stderr
                    def decoded(value):
                        return value.decode(errors="replace") if isinstance(value, bytes) else value or ""
                    raise AssertionError(
                        f"{error}\nCLI stdout:\n{decoded(stdout)}\n"
                        f"CLI stderr:\n{decoded(stderr)}\n"
                        f"fixture log:\n{(root / 'target.log').read_text()}") from error
                finally:
                    # Signal tracked children while their parent still owns
                    # them, then reap the host; no broad process-name cleanup.
                    cleanup_owned(tracked)
                    reap(host)
                    host.stdout.close()
                    host.stderr.close()
                assert target.poll() is None, (root / "target.log").read_text()
                wait_progress(path, progress(path))
            target.stdin.write(b"Q")
            target.stdin.flush()
            assert target.wait(timeout=5) == 0
        finally:
            reap(target)
            target.stdin.close()
print("attach_transport_ok")
