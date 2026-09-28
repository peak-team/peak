#!/usr/bin/env python3
"""Route only Gum's code-allocator near-page reference through PEAK policy."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess

ORIGINAL = "gum_try_alloc_n_pages_near"
ROUTED = "peak_gum_try_alloc_n_pages_near_main_low_gap"
CALLER = "gumcodeallocator.c.o"
BACKEND = "backend-posix_gummemory-posix.c.o"


def run(command, cwd=None):
    child = None
    mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
    try:
        try:
            child = subprocess.Popen(
                command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                start_new_session=True,
                preexec_fn=lambda: signal.pthread_sigmask(signal.SIG_SETMASK, mask))
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, mask)
        stdout, stderr = child.communicate(timeout=15)
        if child.returncode:
            raise RuntimeError(stderr.decode(errors="replace"))
        return stdout
    finally:
        if child is not None:
            # Also remove descendants after a tool's parent exited or failed.
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.communicate()


def symbols(nm, path):
    result = []
    for line in run([nm, str(path)]).decode().splitlines():
        fields = line.split()
        if len(fields) >= 2:
            result.append((fields[-2], fields[-1]))
    return result


def require_symbol(table, name, kind):
    actual = [entry for entry in table if entry[1] == name]
    expected = [] if kind is None else [(kind, name)]
    if actual != expected:
        raise RuntimeError("unexpected binding for {}: {}".format(name, actual))


def main():
    parser = argparse.ArgumentParser()
    for name in ["library", "helper", "work-dir", "ar", "nm", "objcopy"]:
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    library = Path(args.library).resolve()
    helper = Path(args.helper).resolve()
    work = Path(args.work_dir).resolve()
    work.mkdir(parents=True, exist_ok=True)
    members = run([args.ar, "t", str(library)]).decode().splitlines()
    if (members.count(CALLER) != 1 or members.count(BACKEND) != 1 or
            helper.name in members):
        raise RuntimeError("expected unique caller/backend and absent helper")
    require_symbol(symbols(args.nm, library), ROUTED, None)
    run([args.ar, "x", str(library), CALLER, BACKEND], cwd=work)
    caller = work / CALLER
    backend = work / BACKEND
    before = hashlib.sha256(backend.read_bytes()).hexdigest()
    require_symbol(symbols(args.nm, caller), ORIGINAL, "U")
    require_symbol(symbols(args.nm, caller), ROUTED, None)
    require_symbol(symbols(args.nm, backend), ORIGINAL, "T")
    require_symbol(symbols(args.nm, backend), ROUTED, None)
    require_symbol(symbols(args.nm, helper), ROUTED, "T")
    require_symbol(symbols(args.nm, helper), ORIGINAL, "U")

    run([args.objcopy, "--redefine-sym", ORIGINAL + "=" + ROUTED, str(caller)])
    require_symbol(symbols(args.nm, caller), ORIGINAL, None)
    require_symbol(symbols(args.nm, caller), ROUTED, "U")
    run([args.ar, "r", str(library), str(caller), str(helper)])
    run([args.ar, "s", str(library)])
    run([args.ar, "x", str(library), BACKEND], cwd=work)
    after = hashlib.sha256(backend.read_bytes()).hexdigest()
    if before != after:
        raise RuntimeError("backend member bytes changed")
    require_symbol(symbols(args.nm, backend), ORIGINAL, "T")
    require_symbol(symbols(args.nm, backend), ROUTED, None)
    definitions = [entry for entry in symbols(args.nm, library)
                   if entry[0] != "U"]
    require_symbol(definitions, ORIGINAL, "T")
    require_symbol(definitions, ROUTED, "T")
    print(json.dumps({"caller": CALLER, "undefined_original": ORIGINAL,
                      "routed": ROUTED, "helper": helper.name,
                      "backend_sha_before": before, "backend_sha_after": after,
                      "archive_sha": hashlib.sha256(library.read_bytes()).hexdigest()},
                     sort_keys=True))


if __name__ == "__main__":
    def cancelled(signum, frame):
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        raise KeyboardInterrupt("near-page archive patch cancelled")

    signal.signal(signal.SIGTERM, cancelled)
    signal.signal(signal.SIGINT, cancelled)
    main()
