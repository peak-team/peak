#!/usr/bin/env python3

import importlib.util
import pathlib
import sys
import sysconfig
import time


def load_variant(variant):
    suffix = sysconfig.get_config_var("EXT_SUFFIX") or ".so"
    path = pathlib.Path.cwd() / f"{variant}{suffix}"
    spec = importlib.util.spec_from_file_location("customCpackage", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to create import spec for {path}")

    module = importlib.util.module_from_spec(spec)
    sys.modules["customCpackage"] = module
    spec.loader.exec_module(module)
    return module


def main():
    if len(sys.argv) not in (2, 3):
        print("usage: import_customCpackage_variant.py <variant> [duration_s]",
              file=sys.stderr)
        return 2

    variant = sys.argv[1]
    duration = float(sys.argv[2]) if len(sys.argv) == 3 else 1.0
    module = load_variant(variant)

    deadline = time.time() + duration
    calls = 0
    while calls == 0 or time.time() < deadline:
        module.a_call()
        calls += 1

    print(f"{variant} calls={calls}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
