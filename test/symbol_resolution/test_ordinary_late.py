"""Production late raw-name regression using CMake-built fixtures."""
import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--lib", required=True, type=Path)
parser.add_argument("--application", required=True, type=Path)
for kind in ("dynamic", "hidden", "local", "duplicate"):
    parser.add_argument("--" + kind, required=True, type=Path)
parser.add_argument("--objcopy", required=True)
parser.add_argument("--logs", type=Path)
args = parser.parse_args()

def run(command, env=None):
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout + result.stderr

env = {key: value for key, value in os.environ.items()
       if not key.startswith(("PEAK_", "FLAME_")) and key != "LD_PRELOAD"}
env.update(
    LD_PRELOAD=str(args.lib.resolve()),
    PEAK_TARGET="cblas_sdot",
    PEAK_HEARTBEAT_INTERVAL="0",
    PEAK_ENABLE_GLOBAL_HEARTBEAT="false",
    PEAK_ENABLE_PER_TARGET_HEARTBEAT="false",
    PEAK_DETACH_BACKEND="signal",
    OMP_NUM_THREADS="1",
    OPENBLAS_NUM_THREADS="1",
)
with tempfile.TemporaryDirectory(prefix="peak-ordinary-late-") as directory:
    for kind in ("dynamic", "hidden", "local", "duplicate", "stripped"):
        module = getattr(args, "local" if kind == "stripped" else kind)
        if kind == "stripped":
            module = Path(directory) / module.name
            shutil.copy2(args.local, module)
            run([args.objcopy, "--strip-all", str(module)])
        symbols = run(["readelf", "--dyn-syms", "-W", str(module)])
        assert ("cblas_sdot" in symbols) == (kind == "dynamic")
        output = run([str(args.application), str(module), kind], env)
        if args.logs:
            args.logs.mkdir(parents=True, exist_ok=True)
            (args.logs / (kind + ".log")).write_text(output)
        assert "ORDINARY_LATE_ORIGINAL_PASS calls=100" in output
        match = re.search(r"Recorded calls: (\d+)", output)
        assert match, output
        assert int(match[1]) == (0 if kind == "stripped" else 100), output
        print("PASS ordinary late " + kind + " originals=100 recorded=" + match[1], flush=True)
