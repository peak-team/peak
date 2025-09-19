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
def call_a_dynamic_calls_b_dynamic(libA):
    fn = libA.a_dynamic_calls_b_dynamic
    fn.argtypes = []
    fn.restype  = None
    fn()

def call_a_dynamic_calls_a_static(libA):
    fn = libA.a_dynamic_calls_a_static
    fn.argtypes = []
    fn.restype  = None
    fn()

def call_a_test_try_resolve_b_static(libA):
    fn = libA.a_test_try_resolve_b_static
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
    p.add_argument("--a-funcs", default="all",
                   help="Comma list from {adb, aas, arst}. "
                        "adb=a_dynamic_calls_b_dynamic, "
                        "aas=a_dynamic_calls_a_static, "
                        "arst=a_test_try_resolve_b_static. Default=all.")
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
        # Load libA (this will, on first call, dlopen libB internally)
        libA, chosenA = load_library(["./libA.so", "libA.so"], mode=mode)
        print(f"[py] dlopen success: {chosenA}")

        # Decide which A functions to call
        if args.a_funcs == "all":
            a_calls = ["adb", "aas", "arst"]
        else:
            a_calls = [x.strip() for x in args.a_funcs.split(",") if x.strip()]

        print(f"[py] calling libA funcs: {a_calls}")
        for tag in a_calls:
            if tag == "adb":
                call_a_dynamic_calls_b_dynamic(libA)
            elif tag == "aas":
                call_a_dynamic_calls_a_static(libA)
            elif tag == "arst":
                call_a_test_try_resolve_b_static(libA)
            else:
                print(f"[py] unknown libA func tag: {tag}", file=sys.stderr)

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