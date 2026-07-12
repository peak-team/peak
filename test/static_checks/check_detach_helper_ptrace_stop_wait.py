#!/usr/bin/env python3

import pathlib
import re
import sys


def function_body(source: str, name: str) -> str:
    start = source.index(f"{name}(")
    open_brace = source.index("{", start)
    depth = 0
    for index in range(open_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[open_brace + 1:index]
    raise ValueError(f"unterminated {name}")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_detach_helper_ptrace_stop_wait.py <source-root>", file=sys.stderr)
        return 2

    source = (pathlib.Path(sys.argv[1]) / "src" / "detach_helper.c").read_text()
    body = function_body(source, "wait_for_ptrace_stop")
    if "sigtimedwait(" not in body or "SIGCHLD" not in body:
        print("ptrace stop wait must wait for SIGCHLD", file=sys.stderr)
        return 1
    initial_wait = body.find("waitpid(tid, &status, __WALL | WNOHANG)")
    signal_wait = body.find("sigtimedwait(")
    if initial_wait < 0 or signal_wait < 0 or initial_wait > signal_wait:
        print("ptrace stop wait must recheck waitpid before waiting for SIGCHLD", file=sys.stderr)
        return 1
    if "usleep(" in body or "PEAK_DETACH_HELPER_PTRACE_STOP_RETRY_SLEEP_US" in source:
        print("ptrace stop wait must not use fixed sleep polling", file=sys.stderr)
        return 1
    if "ETIMEDOUT" not in body:
        print("ptrace stop wait lost its bounded deadline", file=sys.stderr)
        return 1
    if ("deadline - monotonic_milliseconds()" not in body or
            "tv_sec" not in body or "tv_nsec" not in body):
        print("ptrace stop wait must derive sigtimedwait timeout from deadline", file=sys.stderr)
        return 1
    if not re.search(r"errno\s*==\s*EAGAIN\s*\)\s*\{\s*errno\s*=\s*ETIMEDOUT",
                     body):
        print("ptrace stop wait must map timed signal waits to ETIMEDOUT", file=sys.stderr)
        return 1
    if not re.search(r"sigaddset\s*\([^;]*SIGCHLD", body):
        print("ptrace stop wait must construct a SIGCHLD mask", file=sys.stderr)
        return 1

    main_body = function_body(source, "main")
    reset = main_body.find("sigaction(SIGCHLD")
    block = main_body.find("sigprocmask(SIG_BLOCK")
    serve = main_body.find("serve_protocol(")
    if reset < 0 or "SIG_DFL" not in main_body or block < 0 or serve < 0:
        print("helper must reset and block SIGCHLD before protocol service", file=sys.stderr)
        return 1
    if not (reset < block < serve):
        print("SIGCHLD setup must precede protocol service", file=sys.stderr)
        return 1

    print("detach_helper_ptrace_stop_wait_ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
