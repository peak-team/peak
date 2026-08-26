#!/usr/bin/env python3
"""Check the fixed JIT pending queue's controller-work invariants."""

import argparse
import pathlib


def function_body(source, name):
    marker = f"\n{name}("
    start = source.find(marker)
    if start < 0:
        raise AssertionError(f"missing {name}")
    opening = source.find("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated {name}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    args = parser.parse_args()
    source = pathlib.Path(args.source).read_text(encoding="utf-8")
    add = function_body(source, "peak_jit_pending_record_add")
    remove = function_body(source, "peak_jit_pending_record_remove_index")

    full = add.find(
        "peak_jit_pending_record_count >= configured_jit_pending_capacity"
    )
    duplicate_scan = add.find("peak_jit_pending_record_find(")
    if full < 0 or duplicate_scan < 0 or full >= duplicate_scan:
        raise AssertionError("full pending queues must reject before duplicate scan")
    if "memmove(" in remove:
        raise AssertionError("pending removal must not shift the remaining array")
    if (
        "peak_jit_pending_records[index] =" not in remove
        or "peak_jit_pending_records[peak_jit_pending_record_count]" not in remove
    ):
        raise AssertionError("pending removal must replace from the final slot")

    print("jit_bounded_work_ok")


if __name__ == "__main__":
    main()
