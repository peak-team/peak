#!/usr/bin/env python3

import ctypes
import os
import threading


THREAD_COUNT = 8
WARNING_TEXT = "fork child dynamic dlopen profiling is unsupported until exec"


def child_main():
    barrier = threading.Barrier(THREAD_COUNT)
    errors = []
    errors_lock = threading.Lock()

    def load_from_child_thread():
        try:
            barrier.wait(timeout=5.0)
            library = ctypes.CDLL("./libB.so")
            library.b_dynamic()
        except BaseException as error:  # Report thread failures through exit status.
            with errors_lock:
                errors.append(repr(error))

    threads = [threading.Thread(target=load_from_child_thread)
               for _ in range(THREAD_COUNT)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    if errors:
        os.write(2, ("child thread failures: " + "; ".join(errors) + "\n").encode())
        return 1
    return 0


def main():
    read_fd, write_fd = os.pipe()
    child = os.fork()
    if child == 0:
        os.close(read_fd)
        os.dup2(write_fd, 2)
        os.close(write_fd)
        os._exit(child_main())

    os.close(write_fd)
    captured = bytearray()
    while True:
        chunk = os.read(read_fd, 4096)
        if not chunk:
            break
        captured.extend(chunk)
    os.close(read_fd)

    waited, status = os.waitpid(child, 0)
    if waited != child or not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
        raise RuntimeError(
            f"fork child failed: waited={waited} status={status} "
            f"stderr={captured.decode(errors='replace')!r}"
        )

    output = captured.decode(errors="replace")
    warning_count = output.count(WARNING_TEXT)
    if warning_count != 1:
        raise RuntimeError(
            f"expected one warning for the child process, got {warning_count}: "
            f"{output!r}"
        )

    print(output, end="")
    print("fork_child_dlopen_warning_count=1")
    print("fork_child_dlopen_unsupported_ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
