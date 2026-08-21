#!/usr/bin/env python3
import csv
import sys


def validate(path):
    current = 0
    maximum = 0
    previous_timestamp = None
    events = 0
    with open(path, newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            timestamp = int(row["ts_ns"])
            if previous_timestamp is not None and timestamp < previous_timestamp:
                raise RuntimeError("memory events are not timestamp ordered")
            previous_timestamp = timestamp
            current += int(row["delta"])
            if current < 0 or int(row["current"]) != current:
                raise RuntimeError("memory event current total disagrees with deltas")
            maximum = max(maximum, current)
            events += 1
    return events, current, maximum


def main():
    if len(sys.argv) != 2:
        return 2
    events, current, maximum = validate(sys.argv[1])
    print(f"memlog_accounting_ok events={events} final={current} maximum={maximum}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
