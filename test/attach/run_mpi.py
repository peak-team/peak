"""Attach to zero, one, or both ranks and verify real MPI finalization."""

import argparse
import csv
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time


def stop_group(proc):
    if proc.poll() is None:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait(timeout=5)


def read_pid(path, deadline):
    while time.monotonic() < deadline:
        if path.exists():
            contents = path.read_text().strip()
            if contents:
                return int(contents)
        time.sleep(0.02)
    raise AssertionError(f"MPI rank did not become ready: {path}")


def dump_rank_threads(pid):
    try:
        tasks = sorted(Path(f"/proc/{pid}/task").iterdir())
    except OSError:
        return
    for task in tasks[:16]:
        try:
            status = (task / "status").read_text()
            fields = [line for line in status.splitlines()
                      if line.startswith(("Name:", "State:", "TracerPid:"))]
            wchan = (task / "wchan").read_text().strip()
            print(f"rank pid={pid} tid={task.name} wchan={wchan} {' '.join(fields)}")
        except OSError:
            continue


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--agent", required=True)
    parser.add_argument("--ranks", type=int, required=True)
    parser.add_argument("--local", action="store_true")
    parser.add_argument("launch", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("PEAK_", "FLAME_")) and key not in ("LD_PRELOAD", "LD_AUDIT")}
    with tempfile.TemporaryDirectory(prefix="peak-attach-mpi-") as directory:
        root = Path(directory)
        log = root / "mpi.log"
        pids = []
        with log.open("w") as output:
            job = subprocess.Popen([*args.launch[1:], directory], env=env,
                                   stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
            try:
                deadline = time.monotonic() + 10
                pids = [read_pid(root / f"pid-{rank}", deadline) for rank in range(2)]
                attach_env = dict(env, PEAK_HEARTBEAT_INTERVAL="0",
                                  PEAK_MPI_REAL_FINALIZE="0")
                if args.local:
                    attach_env["PEAK_OUTPUT_AGGREGATION"] = "local"
                def attach(rank, config):
                    cmd = [args.cli, str(pids[rank]), "--agent", args.agent,
                           "--target", "peak_attach_mpi_target", "--output",
                           str(root / "report")]
                    cli = subprocess.Popen(cmd, env=config, stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, text=True,
                                           start_new_session=True)
                    try:
                        stdout, _ = cli.communicate(timeout=20)
                        return cli.returncode, stdout
                    finally:
                        stop_group(cli)

                if args.ranks == 1 and not args.local:
                    code, stdout = attach(0, dict(attach_env, PEAK_OUTPUT_AGGREGATION="mpi"))
                    assert code != 0 and "independent rank-local reports" in stdout, stdout
                for rank in range(args.ranks):
                    code, stdout = attach(rank, attach_env)
                    assert code == 0, stdout
                (root / "release").touch()
                assert job.wait(timeout=10) == 0, "MPI job exited unsuccessfully"
                contents = log.read_text()
                assert all(f"finalized rank={rank}" in contents for rank in range(2))
                reports = list(root.glob("report*.csv"))
                assert len(reports) == args.ranks, reports
                for rank in range(args.ranks):
                    paths = [path for path in reports if f"-p{pids[rank]}-" in path.name]
                    assert len(paths) == 1, reports
                    with paths[0].open() as report:
                        assert any(row["function"] == "peak_attach_mpi_target" and
                                   int(row["count"]) >= 100
                                   for row in csv.DictReader(report)), paths[0]
            except Exception:
                with log.open() as stream:
                    print(stream.read(64 * 1024))
                for pid in pids:
                    dump_rank_threads(pid)
                raise
            finally:
                stop_group(job)


if __name__ == "__main__":
    main()
