#!/usr/bin/env python3
import argparse
import errno
import os
import re
import subprocess
import tempfile
from pathlib import Path


RESULT_RE = re.compile(
    r"^preinit_case=(\w+) result=(-?\d+) errno=(\d+)$",
    re.MULTILINE,
)
REENTRY_RE = re.compile(
    r"^resolver_reentry_case=(\w+) result=(-?\d+) errno=(\d+)$",
    re.MULTILINE,
)
SPAWN_REENTRY_RE = re.compile(
    r"^spawn_resolver_reentry_case=(\w+) result=(-?\d+) errno=(\d+)$",
    re.MULTILINE,
)
SPAWN_PUBLICATION_REENTRY_RE = re.compile(
    r"^spawn_publication_reentry_case=(\w+) result=(-?\d+) errno=(\d+)$",
    re.MULTILINE,
)
SPAWN_PUBLICATION_WAITED_RE = re.compile(
    r"^spawn_publication_waited_case=(\w+) ok=(\d+)$", re.MULTILINE
)
SPAWN_RAW_FORK_CHILD_RE = re.compile(
    r"^spawn_raw_fork_child_case=(\w+) ok=(\d+)$", re.MULTILINE
)
FORK_CHILD_RE = re.compile(r"^resolver_raw_fork_child_ok=(\d+)$", re.MULTILINE)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def parse_results(output):
    results = {}
    for name, result, error_number in RESULT_RE.findall(output):
        require(name not in results, f"duplicate preinit case {name}\n{output}")
        results[name] = (int(result), int(error_number))
    return results


def run_case(exe: Path, libpeak: Path, workdir: Path, preload: bool):
    env = os.environ.copy()
    for name in list(env):
        if name.startswith("PEAK_"):
            env.pop(name, None)
    env.pop("LD_PRELOAD", None)
    empty_path = workdir / "empty-path"
    eacces_path = workdir / "eacces-path"
    eloop_path = workdir / "eloop-path"
    empty_path.mkdir(exist_ok=True)
    eacces_path.mkdir(exist_ok=True)
    eloop_path.mkdir(exist_ok=True)
    blocked_command = eacces_path / "peak-preinit-eacces-eloop"
    blocked_command.write_text("not executable\n", encoding="utf-8")
    blocked_command.chmod(0o644)
    loop_command = eloop_path / "peak-preinit-eacces-eloop"
    if not loop_command.is_symlink():
        loop_command.symlink_to(loop_command.name)
    env["PATH"] = os.pathsep.join(
        (str(eacces_path), str(eloop_path), str(empty_path))
    )
    env["PEAK_EXEC_CHAIN"] = "1"
    env["PEAK_EXEC_CHECKPOINT"] = "1"
    env["PEAK_EXEC_TRACE_PATH"] = str(workdir / "unexpected-exec-trace")
    env["PEAK_TEST_EXEC_PREFLIGHT_TRAP"] = "1"
    env["PEAK_HEARTBEAT_INTERVAL"] = "0"
    env["PEAK_TEXT_OUTPUT"] = "0"
    if preload:
        env["LD_PRELOAD"] = str(libpeak)

    proc = subprocess.run(
        [str(exe)],
        cwd=workdir,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=10,
    )
    label = "preloaded" if preload else "native"
    require(proc.returncode == 0,
            f"{label} preinit failed with {proc.returncode}:\n{proc.stdout}")
    results = parse_results(proc.stdout)
    require(results, f"missing preinit results in {label} output:\n{proc.stdout}")
    reentry = REENTRY_RE.findall(proc.stdout)
    spawn_reentry = SPAWN_REENTRY_RE.findall(proc.stdout)
    spawn_publication_reentry = SPAWN_PUBLICATION_REENTRY_RE.findall(proc.stdout)
    spawn_publication_waited = SPAWN_PUBLICATION_WAITED_RE.findall(proc.stdout)
    spawn_raw_fork_child = SPAWN_RAW_FORK_CHILD_RE.findall(proc.stdout)
    fork_child = FORK_CHILD_RE.findall(proc.stdout)
    return (results, reentry, spawn_reentry, spawn_publication_reentry,
            spawn_publication_waited,
            spawn_raw_fork_child, fork_child, proc.stdout)


def require_native_contract(results, output):
    enoent_exec_cases = {
        "execve",
        "execv",
        "execl",
        "execle",
        "execvp",
        "execlp",
        "execvpe",
        "execveat",
        "raw_execve",
        "raw_execveat",
    }
    for name in enoent_exec_cases & results.keys():
        require(results[name] == (-1, errno.ENOENT),
                f"native {name} contract changed: {results[name]}\n{output}")
    if "execvp_eacces_eloop" in results:
        require(results["execvp_eacces_eloop"] == (-1, errno.ELOOP),
                "native execvp masked terminal ELOOP: "
                f"{results['execvp_eacces_eloop']}\n{output}")
    if "fexecve" in results:
        # Older glibc implements fexecve via /proc/self/fd/<fd>, so its
        # invalid-fd fallback can report ENOENT instead of EBADF/EINVAL.
        require(results["fexecve"][0] == -1 and
                results["fexecve"][1] in {
                    errno.EBADF, errno.EINVAL, errno.ENOENT},
                f"native fexecve contract changed: {results['fexecve']}\n"
                f"{output}")
    for name in ("posix_spawn", "posix_spawnp"):
        require(results[name][0] in {0, errno.ENOENT},
                f"native {name} contract changed: {results[name]}\n{output}")


def require_spawn_results(cases, expected_result, label, output):
    require([(name, int(result)) for name, result, _ in cases] == [
                ("posix_spawn", expected_result),
                ("posix_spawnp", expected_result)],
            f"{label} changed: {cases}\n{output}")


def spawn_results_match_native(cases, native):
    expected = []
    for name in ("posix_spawn", "posix_spawnp"):
        native_result = native.get(name)
        if native_result is None:
            return False
        expected.append((name, native_result[0]))
    return [(name, int(result)) for name, result, _ in cases] == expected


def require_spawn_results_match_native(cases, native, label, output):
    require(spawn_results_match_native(cases, native),
            f"{label} differs from native spawn results: "
            f"native={native} actual={cases}\n{output}")


def check_spawn_publication_result_contract():
    modern_native = {
        "posix_spawn": (errno.ENOENT, errno.EALREADY),
        "posix_spawnp": (errno.ENOENT, errno.EALREADY),
    }
    old_glibc_native = {
        "posix_spawn": (0, errno.EALREADY),
        "posix_spawnp": (0, errno.EALREADY),
    }
    modern_results = [
        ("posix_spawn", str(errno.ENOENT), "0"),
        ("posix_spawnp", str(errno.ENOENT), "0"),
    ]
    old_glibc_results = [
        ("posix_spawn", "0", str(errno.ENOENT)),
        ("posix_spawnp", "0", str(errno.ENOENT)),
    ]
    require(spawn_results_match_native(modern_results, modern_native),
            "modern spawn publication result contract rejected ENOENT")
    require(spawn_results_match_native(old_glibc_results, old_glibc_native),
            "old-glibc spawn publication result contract rejected 0")
    require(not spawn_results_match_native(old_glibc_results, modern_native),
            "spawn publication result contract accepted a native mismatch")


def require_preloaded_contract(native, preloaded, output):
    require(set(preloaded) == set(native),
            f"preloaded preinit cases differ: native={sorted(native)} "
            f"preloaded={sorted(preloaded)}\n{output}")
    for name, native_result in native.items():
        if name in {"posix_spawn", "posix_spawnp"}:
            require(preloaded[name][0] == native_result[0],
                    f"preloaded {name} result differs: "
                    f"native={native_result} preloaded={preloaded[name]}\n{output}")
        else:
            require(preloaded[name] == native_result,
                    f"preloaded {name} differs: native={native_result} "
                    f"preloaded={preloaded[name]}\n{output}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    parser.add_argument("--expected-cases", required=True)
    args = parser.parse_args()
    exe = Path(args.exe).resolve()
    libpeak = Path(args.libpeak).resolve()
    expected_cases = set(args.expected_cases.split(","))
    check_spawn_publication_result_contract()

    with tempfile.TemporaryDirectory(prefix="peak-exec-preinit-") as tmp:
        workdir = Path(tmp)
        (native, native_reentry, native_spawn_reentry,
         native_spawn_publication_reentry, native_spawn_publication_waited,
         native_spawn_raw_fork_child, native_fork_child, native_output) = run_case(
            exe, libpeak, workdir, False)
        (preloaded, preloaded_reentry, preloaded_spawn_reentry,
         preloaded_spawn_publication_reentry, preloaded_spawn_publication_waited,
         preloaded_spawn_raw_fork_child, preloaded_fork_child,
         preloaded_output) = run_case(
            exe, libpeak, workdir, True)
        require(set(native) == expected_cases,
                f"native preinit cases differ: expected={sorted(expected_cases)} "
                f"actual={sorted(native)}\n{native_output}")
        require_native_contract(native, native_output)
        require_preloaded_contract(native, preloaded, preloaded_output)
        require(native_reentry == [("execve", "0", "0")],
                f"native unexpectedly ran resolver hook: {native_reentry}")
        require(preloaded_reentry == [("execve", "-1", str(errno.ENOENT))],
                "resolver-contention reentry did not take the native execve "
                f"failure path: {preloaded_reentry}\n{preloaded_output}")
        require_spawn_results(native_spawn_reentry, 0,
                              "native spawn resolver hooks", native_output)
        require_spawn_results(preloaded_spawn_reentry, errno.ENOSYS,
                              "spawn resolver nonblocking fallback",
                              preloaded_output)
        require_spawn_results(native_spawn_publication_reentry, 0,
                              "native spawn publication hooks", native_output)
        require_spawn_results_match_native(
            preloaded_spawn_publication_reentry, native,
            "concurrent resolver native spawn behavior", preloaded_output)
        require(native_spawn_publication_waited == [
                    ("posix_spawn", "0"), ("posix_spawnp", "0")],
                "native unexpectedly ran spawn publication wait hooks: "
                f"{native_spawn_publication_waited}")
        require(preloaded_spawn_publication_waited == [
                    ("posix_spawn", "1"), ("posix_spawnp", "1")],
                "concurrent resolver caller completed before READY publication: "
                f"{preloaded_spawn_publication_waited}\n{preloaded_output}")
        require(native_spawn_raw_fork_child == [("posix_spawn", "0"),
                                                 ("posix_spawnp", "0")],
                "native unexpectedly ran spawn raw-fork hooks: "
                f"{native_spawn_raw_fork_child}")
        require(preloaded_spawn_raw_fork_child == [("posix_spawn", "1"),
                                                    ("posix_spawnp", "1")],
                "raw-fork child did not fail closed from an in-progress resolver: "
                f"{preloaded_spawn_raw_fork_child}\n{preloaded_output}")
        require(native_fork_child == ["0"],
                "native unexpectedly ran resolver fork hook: "
                f"{native_fork_child}\n{native_output}")
        require(preloaded_fork_child == ["1"],
                "raw-fork child inherited a blocking resolver state: "
                f"{preloaded_fork_child}\n{preloaded_output}")
        require(not (workdir / "unexpected-exec-trace").exists(),
                "pre-constructor exec entered trace handling")
        require(not list(workdir.glob("*-exec*.csv")),
                "pre-constructor exec created a checkpoint")

    print("exec_preinit_contract_ok")


if __name__ == "__main__":
    main()
