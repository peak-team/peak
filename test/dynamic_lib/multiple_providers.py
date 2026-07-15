#!/usr/bin/env python3

import ctypes
import importlib.util
import pathlib
import sys


def main():
    direct = ctypes.CDLL("./libB.so")
    direct.b_dynamic()

    extension_path = next(
        pathlib.Path.cwd().glob("customCpackage_staA_staB*.so")
    )
    spec = importlib.util.spec_from_file_location(
        "customCpackage", extension_path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load {extension_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["customCpackage"] = module
    spec.loader.exec_module(module)
    module.a_call()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
