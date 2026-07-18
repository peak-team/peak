#!/usr/bin/env python3

import re
import sys
from pathlib import Path


def expected_guard(path: Path) -> str:
    stem = re.sub(r"[^A-Za-z0-9]+", "_", path.stem).upper()
    return f"PEAK_{stem}_H"


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_header_guard_style.py <source-root>",
              file=sys.stderr)
        return 2

    source_root = Path(sys.argv[1]).resolve()
    headers = sorted(
        path
        for directory in ("include", "src", "test")
        for path in (source_root / directory).rglob("*.h")
        if path.is_file()
    )
    errors = []
    guards = {}

    for path in headers:
        relative_path = path.relative_to(source_root)
        guard = expected_guard(path)
        lines = path.read_text(encoding="utf-8").splitlines()
        expected_open = [f"#ifndef {guard}", f"#define {guard}"]
        expected_close = f"#endif /* {guard} */"

        if lines[:2] != expected_open:
            errors.append(
                f"{relative_path}: expected first lines {expected_open!r}")
        if not lines or lines[-1] != expected_close:
            errors.append(
                f"{relative_path}: expected final line {expected_close!r}")

        previous_path = guards.get(guard)
        if previous_path is not None:
            errors.append(
                f"duplicate guard {guard}: {previous_path} and {relative_path}")
        else:
            guards[guard] = relative_path

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1

    print(f"header_guard_style_ok headers={len(headers)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
