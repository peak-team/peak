"""Bounded native injection timeout/cancellation, using a silent valid agent."""
import argparse
from collections import namedtuple
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


Process = namedtuple("Process", "parent group uid born")


def census():
    snapshot = subprocess.run(["ps", "-axo", "pid=,ppid=,pgid=,uid=,lstart="],
                              capture_output=True, text=True, timeout=2, check=True)
    rows = {}
    for line in snapshot.stdout.splitlines():
        fields = line.split(maxsplit=4)
        if len(fields) == 5:
            pid, parent, group, uid = map(int, fields[:4])
            rows[pid] = Process(parent, group, uid, fields[4])
    return rows


def collect_owned(host_pid, tracked, snapshot):
    # Record ancestry while it exists; Core's helper later creates its own
    # session and can outlive/reparent away from the isolated injector worker.
    parents = {host_pid}
    parents.update(matching_owned(tracked, snapshot))
    changed = True
    while changed:
        changed = False
        for pid, process in snapshot.items():
            if process.parent in parents and pid not in parents:
                tracked[pid] = process
                parents.add(pid)
                changed = True


def matching_owned(tracked, snapshot):
    return {pid: process for pid, process in tracked.items()
            if pid in snapshot and snapshot[pid][1:] == process[1:]}


def surviving_groups(tracked):
    groups = set()
    for group in {process.group for process in tracked.values()}:
        try:
            os.killpg(group, 0)
        except ProcessLookupError:
            continue
        except PermissionError:
            pass
        groups.add(group)
    return groups


def cleanup_owned(tracked):
    # Observe foreign-UID descendants for leak checks, but never signal them.
    notes = []
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            snapshot = census()
            live = matching_owned(tracked, snapshot)
        except Exception as error:
            notes.append(f"cleanup census failed: {error}")
            break
        for pid, process in live.items():
            if process.uid != os.geteuid():
                notes.append(f"not signaling foreign-UID descendant pid={pid} "
                             f"uid={process.uid} group={process.group}")
                continue
            try:
                foreign_group_member = any(item.group == process.group and
                                           item.uid != os.geteuid()
                                           for item in snapshot.values())
                if pid == process.group and not foreign_group_member:
                    os.killpg(process.group, sig)
                else:
                    os.kill(pid, sig)
            except ProcessLookupError:
                pass
            except OSError as error:
                notes.append(f"cleanup signal failed for pid={pid}: {error}")
        if live and sig == signal.SIGTERM:
            time.sleep(0.1)
    return notes


def cleanup_host(process):
    try:
        reap(process)
        return []
    except Exception as error:
        return [f"host cleanup failed: {error}"]


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
                        workers = {pid for pid, process in snapshot.items()
                                   if process.parent == host.pid and pid in tracked}
                        saw_worker |= bool(workers)
                        saw_helper |= any(process.parent in workers for process in snapshot.values())
                        if saw_worker and (sys.platform != "darwin" or saw_helper):
                            break
                        time.sleep(0.01)
                    assert saw_worker, "did not observe isolated injection worker"
                    if sys.platform == "darwin":
                        assert saw_helper, ("did not observe Core helper before cancellation/timeout", tracked)
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
                    cleanup_notes = cleanup_owned(tracked) + cleanup_host(host)
                    try:
                        stdout, stderr = host.communicate(timeout=1)
                    except subprocess.TimeoutExpired as partial:
                        stdout, stderr = partial.stdout, partial.stderr
                    except Exception as capture_error:
                        stdout, stderr = "", f"capture failed: {capture_error}"
                    def decoded(value):
                        return value.decode(errors="replace") if isinstance(value, bytes) else value or ""
                    raise AssertionError(
                        f"{error}\ncleanup notes: {cleanup_notes!r}\nCLI stdout:\n{decoded(stdout)}\n"
                        f"CLI stderr:\n{decoded(stderr)}\n"
                        f"fixture log:\n{(root / 'target.log').read_text()}") from error
                finally:
                    # Signal tracked children while their parent still owns
                    # them, then reap the host; no broad process-name cleanup.
                    cleanup_notes = cleanup_owned(tracked) + cleanup_host(host)
                    host.stdout.close()
                    host.stderr.close()
                    if cleanup_notes:
                        if sys.exc_info()[0] is not None:
                            print(f"Additional cleanup diagnostics: {cleanup_notes!r}", file=sys.stderr)
                        else:
                            raise AssertionError(f"cleanup failed: {cleanup_notes!r}")
                assert target.poll() is None, (root / "target.log").read_text()
                wait_progress(path, progress(path))
            target.stdin.write(b"Q")
            target.stdin.flush()
            assert target.wait(timeout=5) == 0
        finally:
            reap(target)
            target.stdin.close()
print("attach_transport_ok")
