#!/usr/bin/env python3
"""Keep initialized exec checkpoints on the raw, non-blocking cache path."""

import pathlib
import sys


def body(source, name):
    start = source.index(name)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError(f"unterminated {name}")


def main(root):
    identity = (root / "src/general_listener/output_identity.c").read_text()
    writer = (root / "src/general_listener/exec_checkpoint_writer.c").read_text()
    ready = body(identity, "peak_output_identity_checkpoint_path")
    writer_path = body(writer, "peak_exec_checkpoint_path")
    forbidden = ("getenv", "pthread_once", "malloc", "free", "snprintf", "strlen",
                 "strcmp", "memcpy", "mutex", "lock")
    for token in forbidden:
        assert token not in ready, f"ready checkpoint path calls {token}"
    assert "_Atomic unsigned int peak_output_checkpoint_prefix_length" in identity
    assert "ATOMIC_INT_LOCK_FREE == 2" in identity
    ready = "if (identity_path == PEAK_OUTPUT_CHECKPOINT_READY) {\n        return 0;\n    }"
    unavailable = (
        "if (identity_path == PEAK_OUTPUT_CHECKPOINT_UNAVAILABLE) {\n"
        "        errno = EINVAL;\n        return -1;\n    }"
    )
    legacy = writer_path.index("base = getenv")
    assert ready in writer_path and unavailable in writer_path
    assert writer_path.index(ready) < legacy
    assert writer_path.index(unavailable) < legacy
    print("exec_checkpoint_output_contract_ok")


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]))
