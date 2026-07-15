#!/usr/bin/env python3

import ctypes
import os


def main():
    child = os.fork()
    if child == 0:
        library = ctypes.CDLL("./libB.so")
        library.b_dynamic()
        os._exit(0)

    waited, status = os.waitpid(child, 0)
    if waited != child or not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
        raise RuntimeError(f"fork child failed: waited={waited} status={status}")
    print("fork_child_dlopen_unsupported_ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
