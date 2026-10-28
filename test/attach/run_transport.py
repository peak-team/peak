"""Bounded native injection timeout/cancellation, using a silent valid agent."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

p = argparse.ArgumentParser()
for name in ("cli", "fixture", "agent"):
    p.add_argument("--" + name, type=Path, required=True)
a = p.parse_args()
env = {k: v for k, v in os.environ.items() if not k.startswith(("PEAK_", "FLAME_"))
       and k not in ("LD_PRELOAD", "LD_AUDIT", "DYLD_INSERT_LIBRARIES")}


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
                worker_groups = set()
                try:
                    deadline = time.monotonic() + 2
                    while time.monotonic() < deadline and host.poll() is None:
                        snapshot = subprocess.run(["ps", "-axo", "pid=,ppid=,pgid="],
                                                  capture_output=True, text=True,
                                                  timeout=2, check=True)
                        for line in snapshot.stdout.splitlines():
                            pid, parent, group = map(int, line.split())
                            if parent == host.pid:
                                worker_groups.add(group)
                        if worker_groups:
                            break
                        time.sleep(0.01)
                    assert worker_groups, "did not observe isolated injection worker"
                    if cancel:
                        host.send_signal(signal.SIGINT)
                    stdout, stderr = host.communicate(timeout=20)
                    assert host.returncode != 0, (stdout, stderr)
                    elapsed = time.monotonic() - started
                    assert elapsed < (6 if cancel else 19), elapsed
                    if not cancel:
                        assert elapsed >= 14, (elapsed, stderr)
                    assert "interrupted" in stderr or "timed out" in stderr, stderr
                    for group in worker_groups:
                        try:
                            os.killpg(group, 0)
                        except ProcessLookupError:
                            pass
                        else:
                            raise AssertionError(f"injector process group {group} survived")
                finally:
                    reap(host)
                assert target.poll() is None, (root / "target.log").read_text()
                wait_progress(path, progress(path))
            target.stdin.write(b"Q")
            target.stdin.flush()
            assert target.wait(timeout=5) == 0
        finally:
            reap(target)
            target.stdin.close()
print("attach_transport_ok")
