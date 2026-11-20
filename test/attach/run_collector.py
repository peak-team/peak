"""Feed the collector's complete output directly to live attach."""
import csv
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile


def reap(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
    process.wait(timeout=5)


collector, fixture, module, cli, agent = (str(Path(arg).resolve()) for arg in sys.argv[1:])
env = {key: value for key, value in os.environ.items()
       if not key.startswith("PEAK_") and key not in ("LD_PRELOAD", "LD_AUDIT")}


def check_paths(use_output_option):
    with tempfile.TemporaryDirectory(prefix="peak-attach-collector-") as directory:
        root = Path(directory)
        selectors = root / "targets.txt"
        subprocess.run([sys.executable, collector, module, "-o", str(selectors)],
                       env=env, check=True, timeout=10)
        target_dir = root / "target"
        target_dir.mkdir()
        (target_dir / selectors.name).write_text("attach_wrong_target\n")
        config = dict(
            env, PEAK_TARGET_FILE="./targets.txt", PEAK_HEARTBEAT_INTERVAL="0",
            PEAK_STATSLOG_PATH="./unused" if use_output_option else "./report",
            PEAK_ENABLE_GLOBAL_HEARTBEAT="0", PEAK_ENABLE_PER_TARGET_HEARTBEAT="0",
            PEAK_MEMORY_PROFILE="1", PEAK_MEMORY_TRACK_ALL="1",
            PEAK_MEMLOG_PATH=str(root / "memlog"),
        )
        with (root / "target.log").open("w+") as log:
            target = subprocess.Popen([fixture], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=log, env=env, cwd=target_dir, start_new_session=True)
            try:
                readable, _, _ = select.select([target.stdout], [], [], 5)
                assert readable, "fixture readiness timed out"
                if target.stdout.readline() != b"ready\n":
                    assert target.wait(timeout=5) == 77
                    sys.exit(77)
                command = [cli, str(target.pid), "--agent", agent]
                if use_output_option:
                    command += ["--output", "./report"]
                attach = subprocess.Popen(
                    command, cwd=root, env=config, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
                try:
                    output, _ = attach.communicate(timeout=20)
                    assert attach.returncode == 0, (output, (root / "target.log").read_text())
                    assert f'PEAK_TARGET_FILE="{root}/./targets.txt"'.encode() in output, output
                    assert f'PEAK_STATSLOG_PATH="{root}/./report"'.encode() in output, output
                finally:
                    reap(attach)
                target.stdin.write(b"G")
                target.stdin.flush()
                assert target.wait(timeout=5) == 0, (root / "target.log").read_text()
                reports = list(root.glob("report*.csv"))
                assert len(reports) == 1, reports
                expected = {"attach_collected_first", "attach_collected_second"}
                counts = {}
                with reports[0].open() as stream:
                    for row in csv.DictReader(stream):
                        name = row["function"].rsplit("!", 1)[-1]
                        if name in expected:
                            counts[name] = int(row["count"])
                assert counts == dict.fromkeys(expected, 2), (
                    counts, reports[0].read_text(), (root / "target.log").read_text())
                allocation_threads = set()
                for path in root.glob("memlog*.csv"):
                    with path.open() as stream:
                        allocation_threads.update(row["tid"] for row in csv.DictReader(stream)
                                                  if int(row["delta"]) == 256)
                assert len(allocation_threads) >= 2, allocation_threads
            finally:
                reap(target)


check_paths(use_output_option=False)
check_paths(use_output_option=True)
