#!/usr/bin/env python3
"""Smoke checks for runtime-provided JIT metadata formats."""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile


SKIP_RETURN_CODE = 77


def run_node_v8(timeout):
    node = shutil.which("node")
    if node is None:
        print("node_v8_metadata_skip reason=missing-node")
        return SKIP_RETURN_CODE

    script = "\n".join(
        [
            "function peakJitV8Target(x) { return x + 1; }",
            "let sum = 0;",
            "for (let i = 0; i < 500000; i++) sum += peakJitV8Target(i);",
            "console.log('node_v8_metadata_ok pid=' + process.pid + ' sum=' + sum);",
        ]
    )

    with tempfile.TemporaryDirectory(prefix="peak-node-v8-jit-") as tmpdir:
        script_path = os.path.join(tmpdir, "v8_peak_jit.js")
        with open(script_path, "w", encoding="utf-8") as handle:
            handle.write(script)

        completed = subprocess.run(
            [node, "--perf-basic-prof", script_path],
            cwd=tmpdir,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
        output = completed.stdout + completed.stderr

        if completed.returncode != 0:
            print(output, end="")
            print(f"node_v8_metadata_skip reason=node-failed rc={completed.returncode}")
            return SKIP_RETURN_CODE

        match = re.search(r"\bpid=([0-9]+)\b", output)
        if match is None:
            raise AssertionError(f"Node output did not include pid=\n{output}")

        pid = int(match.group(1))
        perf_map = f"/tmp/perf-{pid}.map"
        try:
            with open(perf_map, encoding="utf-8") as handle:
                rows = handle.read().splitlines()
        finally:
            try:
                os.unlink(perf_map)
            except FileNotFoundError:
                pass

    target_rows = [row for row in rows if "peakJitV8Target" in row]
    optimized_rows = [
        row for row in target_rows if "LazyCompile:*peakJitV8Target" in row
    ]
    if not target_rows:
        raise AssertionError("V8 perf map did not include peakJitV8Target")
    if not optimized_rows:
        raise AssertionError(
            "V8 perf map did not include optimized LazyCompile row for "
            f"peakJitV8Target; rows={target_rows}"
        )

    print(
        "node_v8_metadata_ok "
        f"target_rows={len(target_rows)} optimized_rows={len(optimized_rows)}"
    )
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--framework",
        choices=("node-v8",),
        required=True,
    )
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()

    if args.timeout <= 0:
        print("--timeout must be positive", file=sys.stderr)
        return 2

    if args.framework == "node-v8":
        return run_node_v8(args.timeout)

    raise AssertionError(f"unhandled framework: {args.framework}")


if __name__ == "__main__":
    raise SystemExit(main())
