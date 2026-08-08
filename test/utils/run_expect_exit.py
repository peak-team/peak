#!/usr/bin/env python3
"""Run a command and require one precise exit status plus a stderr marker."""

import argparse
import re
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exit-code", type=int, required=True)
    parser.add_argument("--stderr-regex", required=True)
    parser.add_argument("--stderr-count-regex")
    parser.add_argument("--stderr-count", type=int)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command or args.command[0] != "--" or len(args.command) == 1:
        parser.error("provide command after --")
    completed = subprocess.run(args.command[1:], text=True, capture_output=True)
    sys.stdout.write(completed.stdout)
    sys.stderr.write(completed.stderr)
    if completed.returncode != args.exit_code:
        return 1
    if re.search(args.stderr_regex, completed.stderr) is None:
        return 1
    if ((args.stderr_count_regex is None) != (args.stderr_count is None)):
        parser.error("--stderr-count-regex and --stderr-count must be used together")
    if args.stderr_count_regex is not None:
        if len(re.findall(args.stderr_count_regex, completed.stderr)) != args.stderr_count:
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
