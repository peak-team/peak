#!/usr/bin/env python3

import collections
import pathlib
import re
import sys


MUTATION_RE = re.compile(
    r"\bgum_interceptor_(attach|detach|replace|replace_fast|revert|"
    r"begin_transaction|end_transaction|flush)\b"
)
EXACT_ATTACH_BOUNDARY_RE = re.compile(
    r"\bpeak_general_listener_attach_exact\s*\("
)
EXACT_ATTACH_BACKEND_RE = re.compile(
    r"\bgum_interceptor_peak_attach_exact\s*\("
)

EXPECTED = {
    ("dlopen-controller", "src/dlopen_interceptor.c"): {
        "begin_transaction": 3,
        "end_transaction": 3,
        "flush": 2,
        "replace_fast": 1,
        "revert": 2,
    },
    ("support-init", "src/malloc_interceptor.c"): {
        "begin_transaction": 1,
        "end_transaction": 1,
        "replace_fast": 1,
    },
    ("support-shutdown-debt", "src/malloc_interceptor.c"): {
        "begin_transaction": 1,
        "end_transaction": 1,
        "flush": 1,
        "revert": 6,
    },
    ("strict-controller", "src/general_listener.c"): {
        "attach_exact_backend": 1,
        "attach_exact_boundary": 6,
        "detach": 5,
        "begin_transaction": 10,
        "end_transaction": 10,
        "flush": 2,
    },
    ("support-init", "src/mpi_interceptor.c"): {
        "begin_transaction": 1,
        "end_transaction": 1,
        "replace_fast": 1,
    },
    ("support-shutdown-debt", "src/mpi_interceptor.c"): {
        "begin_transaction": 2,
        "end_transaction": 2,
        "flush": 2,
        "revert": 2,
    },
    ("support-init", "src/pthread_listener.c"): {
        "attach": 1,
        "begin_transaction": 1,
        "end_transaction": 1,
        "replace_fast": 1,
    },
    ("support-shutdown-debt", "src/pthread_listener.c"): {
        "begin_transaction": 1,
        "detach": 1,
        "end_transaction": 1,
        "flush": 2,
        "revert": 1,
    },
    ("support-init", "src/cuda_interceptor.cpp"): {
        "begin_transaction": 1,
        "end_transaction": 1,
        "replace_fast": 10,
    },
    ("support-shutdown-debt", "src/cuda_interceptor.cpp"): {
        "begin_transaction": 1,
        "end_transaction": 1,
        "flush": 1,
        "revert": 10,
    },
    ("support-init", "src/peak.c"): {
        "begin_transaction": 1,
        "end_transaction": 1,
        "replace_fast": 1,
    },
    ("support-shutdown-debt", "src/peak.c"): {
        "begin_transaction": 1,
        "end_transaction": 1,
        "flush": 1,
        "revert": 1,
    },
}

FUNCTION_ANCHORS = {
    "src/general_listener.c": {
        "peak_general_controller_detach_if_requested_unlocked": "strict-controller",
        "peak_general_controller_reattach_if_requested_unlocked": "strict-controller",
        "peak_general_controller_shutdown_hook_unlocked": "strict-controller",
        "peak_general_controller_process_pending_batch_unlocked": "strict-controller",
        "peak_general_controller_flush_teardown": "strict-controller",
        "peak_general_overhead_bootstrapping": "strict-controller",
        "peak_general_listener_dynamic_attach_symbol": "strict-controller",
        "peak_general_listener_attach": "strict-controller",
    },
    "src/malloc_interceptor.c": {
        "DO_REPLACE_FAST": "support-init",
        "malloc_interceptor_attach": "support-init",
        "malloc_interceptor_detach": "support-shutdown-debt",
    },
    "src/pthread_listener.c": {
        "pthread_listener_attach": "support-init",
        "pthread_listener_flush_teardown": "support-shutdown-debt",
        "pthread_listener_dettach": "support-shutdown-debt",
    },
    "src/mpi_interceptor.c": {
        "mpi_interceptor_attach": "support-init",
        "mpi_interceptor_restore_finalize_for_direct_call": "support-shutdown-debt",
        "mpi_interceptor_dettach": "support-shutdown-debt",
    },
    "src/cuda_interceptor.cpp": {
        "cuda_interceptor_attach": "support-init",
        "cuda_interceptor_dettach": "support-shutdown-debt",
    },
    "src/peak.c": {
        "exit_interceptor_attach": "support-init",
        "exit_interceptor_detach": "support-shutdown-debt",
    },
}


def read_source(repo_root, rel, path):
    source = path.read_text(encoding="utf-8")
    if rel != "src/general_listener.c":
        return source

    def include_fragment(match):
        fragment = match.group(1)
        return (
            repo_root / "src/general_listener" / fragment
        ).read_text(encoding="utf-8")

    return re.sub(
        r'^#include "general_listener/([^"]+\.inc)"$',
        include_fragment,
        source,
        flags=re.MULTILINE,
    )


def classify_function(rel, line_no, function_starts):
    anchors = FUNCTION_ANCHORS.get(rel)
    if anchors is None:
        if rel == "src/dlopen_interceptor.c":
            return "dlopen-controller"
        return None

    active = None
    for start_line, function_name in function_starts.get(rel, []):
        if start_line > line_no:
            break
        active = function_name
    if active is None:
        return None
    return anchors.get(active)


def count_mutations(repo_root):
    function_starts = collections.defaultdict(list)
    source_paths = []

    for path in (repo_root / "src").glob("**/*"):
        if path.suffix not in (".c", ".cc", ".cpp", ".h", ".hpp"):
            continue

        rel = path.relative_to(repo_root).as_posix()
        source = read_source(repo_root, rel, path)
        source_paths.append((rel, source))
        anchors = FUNCTION_ANCHORS.get(rel, {})
        if anchors:
            for line_number, line in enumerate(source.splitlines(), 1):
                for function_name in anchors:
                    if function_name in line:
                        function_starts[rel].append(
                            (line_number, function_name)
                        )

    found = collections.defaultdict(collections.Counter)
    unclassified = []
    for rel, source in source_paths:
        for line_number, line in enumerate(source.splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*"):
                continue
            for match in MUTATION_RE.finditer(line):
                category = classify_function(rel, line_number, function_starts)
                if category is None:
                    unclassified.append(
                        f"{rel}:{line_number}: {match.group(0)}"
                    )
                    continue
                found[(category, rel)][match.group(1)] += 1
            for match in EXACT_ATTACH_BOUNDARY_RE.finditer(line):
                if rel != "src/general_listener.c":
                    unclassified.append(
                        f"{rel}:{line_number}: {match.group(0)}"
                    )
                    continue
                found[("strict-controller", rel)][
                    "attach_exact_boundary"
                ] += 1
            for match in EXACT_ATTACH_BACKEND_RE.finditer(line):
                if rel != "src/general_listener.c":
                    unclassified.append(
                        f"{rel}:{line_number}: {match.group(0)}"
                    )
                    continue
                found[("strict-controller", rel)][
                    "attach_exact_backend"
                ] += 1

    return {key: dict(counter) for key, counter in found.items()}, unclassified


def main():
    if len(sys.argv) != 2:
        print("usage: check_gum_mutation_allowlist.py <repo-root>",
              file=sys.stderr)
        return 2

    repo_root = pathlib.Path(sys.argv[1]).resolve()
    found, unclassified = count_mutations(repo_root)

    if unclassified or found != EXPECTED:
        print("Direct Gum mutation allowlist mismatch.", file=sys.stderr)
        print("Any new Gum or PEAK exact attach/detach/replace/revert/"
              "transaction/flush site needs an explicit strict-detach review.",
              file=sys.stderr)
        if unclassified:
            print(f"unclassified: {unclassified}", file=sys.stderr)
        print(f"expected: {EXPECTED}", file=sys.stderr)
        print(f"found:    {found}", file=sys.stderr)
        return 1

    print("gum_mutation_allowlist_ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
