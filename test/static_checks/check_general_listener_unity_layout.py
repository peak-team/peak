#!/usr/bin/env python3

import re
import sys
from pathlib import Path


EXPECTED_FRAGMENTS = (
    "config.inc",
    "accounting.inc",
    "callbacks.inc",
    "controller_state.inc",
    "controller.inc",
    "heartbeat.inc",
    "callback_runtime.inc",
    "attach.inc",
    "output.inc",
    "shutdown.inc",
)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_general_listener_unity_layout.py <source-root>",
              file=sys.stderr)
        return 2

    root = Path(sys.argv[1]).resolve()
    source = root / "src" / "general_listener.c"
    fragment_dir = root / "src" / "general_listener"
    source_text = source.read_text(encoding="utf-8")
    include_pattern = re.compile(
        r'^#include "general_listener/([^"/]+\.inc)"$', re.MULTILINE)
    included = tuple(include_pattern.findall(source_text))
    errors = []

    if included != EXPECTED_FRAGMENTS:
        errors.append(
            "general_listener.c: unity fragment order differs: "
            f"expected {EXPECTED_FRAGMENTS!r}, got {included!r}")
    if "unity implementation fragments, not independently compiled" not in source_text:
        errors.append("general_listener.c: missing unity-fragment contract")
    if not (fragment_dir / "README.md").is_file():
        errors.append("src/general_listener/README.md: missing layout contract")

    for fragment_name in EXPECTED_FRAGMENTS:
        fragment = fragment_dir / fragment_name
        if not fragment.is_file():
            errors.append(f"{fragment}: missing expected unity fragment")
            continue
        text = fragment.read_text(encoding="utf-8")
        if re.search(r"^\s*#\s*include\b", text, re.MULTILINE):
            errors.append(
                f"{fragment.relative_to(root)}: headers belong in general_listener.c")

    for path in (root / "src").rglob("*"):
        if not path.is_file() or path == source:
            continue
        if path.suffix not in {".c", ".h", ".inc"}:
            continue
        text = path.read_text(encoding="utf-8")
        if include_pattern.search(text):
            errors.append(
                f"{path.relative_to(root)}: only general_listener.c may include unity fragments")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1

    print(f"general_listener_unity_layout_ok fragments={len(included)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
