#!/usr/bin/env python3
import argparse
import ctypes
import os
import sys
import time

def load_library(lib_candidates, mode=None):
    """
    Try a list of names/paths and return (CDLL, chosen_name).
    mode: passed to ctypes.CDLL(..., mode=) if given (Linux only).
    """
    last_err = None
    for name in lib_candidates:
        try:
            if mode is None:
                lib = ctypes.CDLL(name)
            else:
                lib = ctypes.CDLL(name, mode=mode)
            return lib, name
        except OSError as e:
            last_err = e
    raise OSError(f"Failed to load any of {lib_candidates}: {last_err}")

# ---- Call helpers for libA exports ----
def call_a_call(libA):
    fn = libA.a_call
    fn.argtypes = []
    fn.restype  = None
    fn()

# ---- Call helpers for libB export ----
def call_b_dynamic(libB):
    fn = libB.b_dynamic
    fn.argtypes = []
    fn.restype  = None
    fn()

def main():
    p = argparse.ArgumentParser(description="ctypes test for libA/libB dynamic loading demo.")
    p.add_argument("--delay", type=float, default=0.5,
                   help="Seconds to sleep before first dlopen (default 0.5).")
    p.add_argument("--which", default="A", choices=["A","B","both"],
                   help="Which library API to exercise: libA, libB, or both. Default=A.")
    p.add_argument("--a-variant", default="dynB", choices=["dynB","staB"],
                   help="Which libA shared-library variant to load. Default=dynB.")
    p.add_argument("--duration", type=float, default=0.0,
                   help="Loop calls until this many seconds have elapsed. Default calls once.")
    p.add_argument("--b-func", default="b_dynamic",
                   help="Function in libB to call (currently only b_dynamic).")
    args = p.parse_args()

    print(f"[py] PID={os.getpid()} sleeping {args.delay:.1f}s before dlopen …")
    time.sleep(args.delay)

    # On Linux, you can optionally control RTLD flags:
    # from os import RTLD_NOW, RTLD_LOCAL
    # mode = RTLD_NOW  # or RTLD_LOCAL|RTLD_NOW
    mode = None

    if args.which in ("A", "both"):
        libA_name = "libA_dynB.so" if args.a_variant == "dynB" else "libA_staB.so"
        libA, chosenA = load_library([f"./{libA_name}", libA_name], mode=mode)
        print(f"[py] dlopen success: {chosenA}")

        print("[py] calling libA.a_call()")
        deadline = time.time() + args.duration
        calls = 0
        while calls == 0 or time.time() < deadline:
            call_a_call(libA)
            calls += 1
        print(f"[py] libA.a_call calls={calls}")

    if args.which in ("B", "both"):
        # Load libB directly and call its exported function
        libB, chosenB = load_library(["./libB.so", "libB.so"], mode=mode)
        print(f"[py] dlopen success: {chosenB}")
        if args.b_func != "b_dynamic":
            print(f"[py] unknown libB func: {args.b_func}", file=sys.stderr)
        else:
            print("[py] calling libB.b_dynamic()")
            call_b_dynamic(libB)

    print("[py] done.")

if __name__ == "__main__":
    main()
