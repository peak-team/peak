#!/usr/bin/env python3
"""Validate one rank's complete saved observation before computing any totals."""

import argparse
from collections import Counter
import json
from pathlib import Path
import re


FIELDS = {
    "WINDOW": "schema rank start_ns end_ns elapsed_ns",
    "WINDOW_API": "schema rank index kind arg wall0 cpu0 cpu1 wall1 result errors",
    "WINDOW_READER_CPU": "schema rank reader cut initialized id_rc lo cpu hi rc lo_rc hi_rc",
    "WINDOW_OBSERVE_END": "schema rank api_count dropped",
}
WORK_FIELDS = set("published_calls_lo published_calls_hi target_calls_lo "
                  "target_calls_hi publication_lag_bound lifetime_calls".split())


def decode(text, rank, expected_counts):
    """Fail closed: incomplete input returns measurements=None, never zero costs.

    expected_counts maps kind 1 (detach), 2 (reattach), 3 (drain) to counts
    fixed by the caller's protocol, independently of the collected output.
    Other ranks and unrelated application lines are ignored.
    """
    errors = []
    records = {name: [] for name in FIELDS}
    if (type(rank) is not int or rank < 0 or set(expected_counts) != {1, 2, 3}
            or any(type(n) is not int or n < 0 for n in expected_counts.values())
            or not 0 < sum(expected_counts.values()) <= 128):
        return {"valid": False, "errors": ["invalid expected protocol"],
                "measurements": None}
    for number, line in enumerate(text.splitlines(), 1):
        tokens = line.split()
        if not tokens or tokens[0] not in FIELDS:
            continue
        name = tokens[0]
        try:
            pairs = [token.split("=") for token in tokens[1:]]
            if any(len(pair) != 2 for pair in pairs):
                raise ValueError("malformed field")
            if len({pair[0] for pair in pairs}) != len(pairs):
                raise ValueError("duplicate field")
            raw = dict(pairs)
            if "rank" not in raw or not re.fullmatch(r"\d+", raw["rank"]):
                raise ValueError("missing/invalid rank")
            if int(raw["rank"]) != rank:
                continue
            required = set(FIELDS[name].split())
            if name == "WINDOW" and set(raw) & WORK_FIELDS:
                required |= WORK_FIELDS
            if set(raw) != required:
                raise ValueError("missing/unknown field")
            if any(not re.fullmatch(r"-?\d+", value) for value in raw.values()):
                raise ValueError("noninteger field")
            row = {key: int(value) for key, value in raw.items()}
            if any(value > 2**64 - 1 for value in row.values()):
                raise ValueError("integer exceeds uint64 range")
            if row["schema"] != 1:
                raise ValueError("unsupported schema")
            records[name].append(row)
        except ValueError as error:
            errors.append("line {}: {}".format(number, error))

    def require(condition, message):
        if not condition:
            errors.append(message)

    windows, apis, readers, footers = (records[name] for name in FIELDS)
    require(len(windows) == 1, "exactly one WINDOW required")
    require(len(footers) == 1, "exactly one footer required")
    require(len(apis) == sum(expected_counts.values()), "API count mismatch")
    require(len(readers) == 4, "four reader cuts required")
    if errors:
        return {"valid": False, "errors": errors, "measurements": None}
    window, footer = windows[0], footers[0]
    start, end = window["start_ns"], window["end_ns"]
    require(0 < start < end and end - start == window["elapsed_ns"],
            "invalid WINDOW endpoints/duration")
    if WORK_FIELDS.issubset(window):
        require(all(window[key] >= 0 for key in WORK_FIELDS) and
                window["published_calls_lo"] <= window["published_calls_hi"] and
                window["target_calls_lo"] <= window["target_calls_hi"],
                "invalid work-count bounds")
    require(footer["api_count"] == len(apis) and footer["dropped"] == 0,
            "incomplete/overflowed observation")
    require([row["index"] for row in apis] == list(range(len(apis))),
            "API indices must be ordered, unique and contiguous")
    actual_counts = Counter(row["kind"] for row in apis)
    require(set(actual_counts).issubset(expected_counts) and all(
            actual_counts[kind] == count for kind, count in expected_counts.items()),
            "API kind counts mismatch")
    previous_end = 0
    previous_cpu_end = 0
    for row in apis:
        require(row["result"] == 1 and row["errors"] == 0,
                "API failure or clock error")
        require(row["arg"] >= 0 and 0 < row["wall0"] <= row["wall1"]
                and 0 <= row["cpu0"] <= row["cpu1"], "invalid API clocks/argument")
        require(previous_end <= row["wall0"], "main-thread API scopes overlap/reorder")
        require(previous_cpu_end <= row["cpu0"], "main-thread CPU decreases across scopes")
        previous_end = row["wall1"]
        previous_cpu_end = row["cpu1"]
    cuts = {}
    for row in readers:
        key = row["reader"], row["cut"]
        require(key in {(0, 0), (0, 1), (1, 0), (1, 1)} and key not in cuts,
                "missing/duplicate/unknown reader cut")
        cuts[key] = row
        require(row["initialized"] == 1 and all(row[key] == 0 for key in
                ("id_rc", "rc", "lo_rc", "hi_rc")), "reader clock unavailable")
        require(0 < row["lo"] <= row["hi"] and row["cpu"] >= 0,
                "invalid reader clock bracket")
    if set(cuts) == {(0, 0), (0, 1), (1, 0), (1, 1)}:
        for reader in range(2):
            before, after = cuts[reader, 0], cuts[reader, 1]
            require(before["hi"] <= start and after["lo"] >= end,
                    "reader cuts must bracket the entire WINDOW")
            require(after["cpu"] >= before["cpu"], "reader CPU decreases")
    if errors:
        return {"valid": False, "errors": errors, "measurements": None}

    # Serialized main-thread envelopes are disjoint; clip each before summing.
    union = sum(max(0, min(end, row["wall1"]) - max(start, row["wall0"]))
                for row in apis)
    contained = all(start <= row["wall0"] <= row["wall1"] <= end for row in apis)
    by_kind = {}
    for kind in (1, 2, 3):
        rows = [row for row in apis if row["kind"] == kind]
        by_kind[str(kind)] = {
            "count": len(rows),
            "clipped_outer_wall_ns": sum(max(0, min(end, row["wall1"])
                - max(start, row["wall0"])) for row in rows),
            "inner_cpu_ns": (sum(row["cpu1"] - row["cpu0"] for row in rows)
                if all(start <= row["wall0"] <= row["wall1"] <= end for row in rows)
                else None),
        }
    reader_samples = []
    for reader in range(2):
        before, after = cuts[reader, 0], cuts[reader, 1]
        reader_samples.append({
            "reader": reader,
            "sample_cpu_delta_ns": after["cpu"] - before["cpu"],
            "before_bracket_ns": [before["lo"], before["hi"]],
            "after_bracket_ns": [after["lo"], after["hi"]],
            "max_wall_extension_before_ns": start - before["lo"],
            "max_wall_extension_after_ns": after["hi"] - end,
        })
    return {"valid": True, "errors": [], "rank": rank, "measurements": {
        "window_ns": end - start,
        "window_work_counts": ({key: window[key] for key in sorted(WORK_FIELDS)}
                               if WORK_FIELDS.issubset(window) else None),
        "api_clipped_outer_wall_union_ns": union,
        "window_outside_api_union_ns": end - start - union,
        "api_all_fully_contained": contained,
        "api_inner_cpu_sum_ns": (sum(row["cpu1"] - row["cpu0"] for row in apis)
                                  if contained else None),
        "api_by_kind": by_kind, "reader_samples": reader_samples,
    }, "limits": [
        "Clock values are recorded samples; no calibrated physical-time error bound.",
        "API wall envelopes include instrumentation; inner CPU excludes outer clocks.",
        "Clipped API CPU is unknown, never proportionally apportioned.",
        "Reader samples bracket W, not exact W CPU; overlapping thread CPU is not wall time.",
        "W outside API envelopes still includes worker/reader/scheduling and other work.",
    ]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--rank", type=int, required=True)
    for name in ("detach", "reattach", "drain"):
        parser.add_argument("--{}-count".format(name), type=int, required=True)
    args = parser.parse_args()
    try:
        result = decode(args.log.read_text(), args.rank,
                        {1: args.detach_count, 2: args.reattach_count, 3: args.drain_count})
    except (OSError, UnicodeError) as error:
        result = {"valid": False, "errors": [str(error)], "measurements": None}
    print(json.dumps(result, indent=2))
    return 0 if result["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
