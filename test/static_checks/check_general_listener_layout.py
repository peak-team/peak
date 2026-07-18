#!/usr/bin/env python3

import re
import sys
from pathlib import Path


EXPECTED_MODULES = (
    "attach_policy",
    "exec_checkpoint_writer",
    "mpi_report_transport",
    "report_formatter",
    "report_maxima",
    "report_model",
    "report_snapshot",
    "runtime_config",
    "socket_report_transport",
)

EXPECTED_SECTIONS = (
    "Configuration and controller declarations",
    "Runtime and controller accounting",
    "Invocation listener and thread-pause primitives",
    "Controller state transitions",
    "Controller execution",
    "Heartbeat policy",
    "Invocation callbacks",
    "Listener attachment",
    "Checkpoint and report snapshot capture",
    "Final reporting and shutdown",
)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_general_listener_layout.py <source-root>",
              file=sys.stderr)
        return 2

    root = Path(sys.argv[1]).resolve()
    source = root / "src" / "general_listener.c"
    module_dir = root / "src" / "general_listener"
    module_headers = root / "include" / "internal" / "general_listener"
    source_text = source.read_text(encoding="utf-8")
    errors = []

    misplaced_headers = sorted(
        path.relative_to(root)
        for path in (root / "src").rglob("*.h")
        if path.is_file()
    )
    if misplaced_headers:
        errors.append(
            f"headers under src instead of include: {misplaced_headers!r}")
    misplaced_sources = sorted(
        path.relative_to(root)
        for suffix in ("*.c", "*.cc", "*.cpp", "*.cxx")
        for path in (root / "include").rglob(suffix)
        if path.is_file()
    )
    if misplaced_sources:
        errors.append(
            f"sources under include instead of src: {misplaced_sources!r}")

    inc_files = sorted(
        path.relative_to(root)
        for directory in ("src", "include", "test", "cmake")
        for path in (root / directory).rglob("*.inc")
        if path.is_file()
    )
    if inc_files:
        errors.append(f"implementation fragments remain: {inc_files!r}")
    if re.search(r'^\s*#\s*include\s+[<\"][^>\"]+\.(?:inc|c)[>\"]',
                 source_text,
                 re.MULTILINE):
        errors.append(
            "general_listener.c: implementation files must not be included")

    section_offsets = []
    for section in EXPECTED_SECTIONS:
        marker = f"/* {section}. */"
        count = source_text.count(marker)
        if count != 1:
            errors.append(
                f"general_listener.c: expected one section marker {marker!r}, "
                f"found {count}")
            continue
        section_offsets.append(source_text.index(marker))
    if section_offsets != sorted(section_offsets):
        errors.append("general_listener.c: lifecycle section order changed")
    if source_text.count("G_DEFINE_TYPE_EXTENDED(") != 1:
        errors.append(
            "general_listener.c: listener type definition must remain unique")
    if "peak_exec_checkpoint_write_rows(" not in source_text:
        errors.append(
            "general_listener.c: checkpoint capture must hand immutable rows "
            "to the writer module")

    for module_name in EXPECTED_MODULES:
        source_module = module_dir / f"{module_name}.c"
        header_module = module_headers / f"{module_name}.h"
        if not source_module.is_file():
            errors.append(
                f"src/general_listener/{module_name}.c: missing module source")
        else:
            module_source = source_module.read_text(encoding="utf-8")
            expected_header = (
                '#include "internal/general_listener/'
                f'{module_name}.h"')
            if expected_header not in module_source:
                errors.append(
                    f"src/general_listener/{module_name}.c: must include its "
                    "private interface")
            if re.search(r'^\s*#\s*include\s+[<\"][^>\"]+\.(?:inc|c)[>\"]',
                         module_source,
                         re.MULTILINE):
                errors.append(
                    f"src/general_listener/{module_name}.c: must not include "
                    "implementation files")
        if not header_module.is_file():
            errors.append(
                "include/internal/general_listener/"
                f"{module_name}.h: missing private module interface")
        misplaced_header = module_dir / f"{module_name}.h"
        if misplaced_header.exists():
            errors.append(
                f"{misplaced_header.relative_to(root)}: private headers belong "
                "under include/internal/general_listener")

    writer = module_dir / "exec_checkpoint_writer.c"
    if writer.is_file():
        writer_text = writer.read_text(encoding="utf-8")
        for forbidden in (
            "PeakGeneralListener",
            "array_listener",
            "peak_general_overhead",
            "pthread_mutex",
        ):
            if forbidden in writer_text:
                errors.append(
                    "src/general_listener/exec_checkpoint_writer.c: writer "
                    f"must not own listener state ({forbidden})")

    for module_name in (
        "mpi_report_transport",
        "report_formatter",
        "report_snapshot",
        "socket_report_transport",
    ):
        module_text = (module_dir / f"{module_name}.c").read_text(
            encoding="utf-8")
        for forbidden in (
            "array_listener",
            "peak_demangled_strings",
            "peak_hook_states",
        ):
            if forbidden in module_text:
                errors.append(
                    f"src/general_listener/{module_name}.c: snapshot consumer "
                    f"must not read lifecycle state ({forbidden})")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1

    print(
        "general_listener_layout_ok "
        f"modules={len(EXPECTED_MODULES)} sections={len(EXPECTED_SECTIONS)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
