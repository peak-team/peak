"""A startup hook owns its plugin after the application closes it."""
import csv
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile

launcher, agent, fixture, library = map(lambda arg: str(Path(arg).resolve()), sys.argv[1:])
env = {key: value for key, value in os.environ.items()
       if not key.startswith("PEAK_") and key not in ("LD_PRELOAD", "LD_AUDIT")}

with tempfile.TemporaryDirectory(prefix="peak-attach-module-") as dirname:
    root = Path(dirname)
    with (root / "fixture.log").open("w+") as log:
        target = subprocess.Popen([fixture, library], env=env, stdout=subprocess.PIPE,
                                  stderr=log, stdin=subprocess.PIPE,
                                  start_new_session=True)
        try:
            assert select.select([target.stdout], [], [], 5)[0], "fixture startup timed out"
            assert target.stdout.readline() == b"ready\n", target.poll()
            cli = subprocess.Popen(
                [launcher, str(target.pid), "--agent", agent, "--target",
                 f"{library}!peak_attach_plugin_target", "--output", str(root / "profile")],
                env={**env, "PEAK_HEARTBEAT_INTERVAL": "0"}, stdout=log, stderr=log,
                start_new_session=True,
            )
            try:
                assert cli.wait(timeout=20) == 0
            finally:
                if cli.poll() is None:
                    os.killpg(cli.pid, signal.SIGTERM)
                    try:
                        cli.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(cli.pid, signal.SIGKILL)
                        cli.wait(timeout=5)
            target.stdin.write(b"Q")
            target.stdin.flush()
            assert target.wait(timeout=10) == 0
            report, = root.glob("profile*.csv")
            with report.open() as stream:
                rows = [row for row in csv.DictReader(stream)
                        if "peak_attach_plugin_target" in row["function"]]
                assert sum(int(row["count"]) for row in rows) == 10, rows
        except AssertionError:
            log.seek(0)
            print(log.read(), file=sys.stderr)
            raise
        finally:
            if target.poll() is None:
                os.killpg(target.pid, signal.SIGKILL)
                target.wait(timeout=5)
            target.stdout.close()
            target.stdin.close()
