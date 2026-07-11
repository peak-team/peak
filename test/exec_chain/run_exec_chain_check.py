#!/usr/bin/env python3

import argparse
import csv
import errno
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path


TARGET = "peak_exec_chain_hot_target"

MODE_TO_APP_ARG = {
    "execv_success_checkpoint": "execv-success",
    "exec_checkpoint_write_target_no_reentry": "execv-write-target-success",
    "execv_zero_call_checkpoint": "execv-zero-call",
    "exec_checkpoint_disabled": "execv-success",
    "exec_bad_stats_path_nonfatal": "execv-success",
    "execve_custom_env_injection": "execve-custom-env",
    "raw_syscall_execve_custom_env_injection": "raw-syscall-execve-custom-env",
    "raw_syscall_execve_backend_disabled_injection": "raw-syscall-execve-custom-env",
    "raw_syscall_execve_failure_non_destructive": "raw-syscall-execve-failure",
    "raw_syscall_nonexec_passthrough": "raw-syscall-nonexec",
    "execve_preflight_unavailable_injection": "execve-custom-env",
    "execve_child_peak_env_only_injection": "execve-child-peak-env-only",
    "execve_null_env_injection": "execve-null-env",
    "execve_large_env_injection": "execve-large-env",
    "execve_env_build_failure_passthrough": "execve-custom-env",
    "execve_bad_env_failure_non_destructive": "execve-bad-env",
    "execve_bad_env_preflight_unknown_non_destructive": "execve-bad-env",
    "execve_bad_argv_failure_non_destructive": "execve-bad-argv",
    "execve_explicit_peak_env_preserved": "execve-explicit-peak-env",
    "execve_child_chain_disabled": "execve-child-chain-disabled",
    "execve_child_checkpoint_disabled": "execve-child-checkpoint-disabled",
    "execve_child_propagate_disabled": "execve-child-propagate-disabled",
    "execve_propagate_disabled": "execve-custom-env",
    "execve_chain_disabled": "execve-custom-env",
    "execve_secure_skip": "execve-custom-env",
    "fexecve_custom_env_injection": "fexecve-custom-env",
    "fexecve_preflight_unavailable_injection": "fexecve-custom-env",
    "fexecve_bad_env_failure_non_destructive": "fexecve-bad-env",
    "fexecve_bad_env_preflight_unknown_non_destructive": "fexecve-bad-env",
    "fexecve_bad_argv_failure_non_destructive": "fexecve-bad-argv",
    "execveat_custom_env_injection": "execveat-custom-env",
    "raw_syscall_execveat_custom_env_injection": "raw-syscall-execveat-custom-env",
    "execveat_bad_env_failure_non_destructive": "execveat-bad-env",
    "execveat_bad_env_preflight_unknown_non_destructive": "execveat-bad-env",
    "execveat_bad_argv_failure_non_destructive": "execveat-bad-argv",
    "execl_success_checkpoint": "execl-success",
    "execlp_path_search": "execlp-path-search",
    "execle_custom_env_injection": "execle-custom-env",
    "exec_failure_non_destructive": "exec-failure",
    "execvp_path_search": "execvp-path-search",
    "execvp_empty_path_component": "execvp-path-search",
    "helper_named_exec_still_checkpointed": "helper-named-exec",
    "execvpe_caller_path_child_env_ignored": "execvpe-child-env-path-ignored",
    "no_duplicate_ld_preload_execvpe_path_fallback": (
        "execvpe-child-path-duplicate-preload"
    ),
    "execvp_enoent_failure": "execvp-enoent",
    "execvp_enotdir_enoent_failure": "execvp-enoent",
    "execvp_enotdir_enoent_fallback": "execvp-enoent",
    "execvp_eacces_failure": "execvp-eacces",
    "execvp_eacces_fallback": "execvp-eacces",
    "execvp_enoent_fallback": "execvp-enoent",
    "execvp_enoexec_fallback": "execvp-enoexec",
    "fork_child_exec_parent_exec": "fork-child-exec-parent-exec",
    "vfork_child_exec_parent_exec": "vfork-child-exec-parent-exec",
    "vfork_child_failed_exec_parent_exec": (
        "vfork-child-failed-exec-parent-exec"
    ),
    "vfork_execl_parent_exec": "vfork-execl-parent-exec",
    "vfork_execlp_parent_exec": "vfork-execlp-parent-exec",
    "vfork_execle_parent_exec": "vfork-execle-parent-exec",
    "fork_custom_env_execve_chain": "fork-custom-env-execve",
    "vfork_custom_env_execve_chain": "vfork-custom-env-execve",
    "vfork_custom_env_execle_chain": "vfork-custom-env-execle",
    "fork_custom_env_chain_disabled": "fork-custom-env-disabled",
    "vfork_custom_env_chain_disabled": "vfork-custom-env-disabled",
    "vfork_custom_env_execvpe_chain": "vfork-custom-env-execvpe",
    "vfork_custom_env_execvp_chain": "vfork-custom-env-execvp",
    "vfork_custom_env_execlp_chain": "vfork-custom-env-execlp",
    "vfork_custom_env_fexecve_chain": "vfork-custom-env-fexecve",
    "vfork_custom_env_execveat_chain": "vfork-custom-env-execveat",
    "fork_raw_syscall_custom_env_chain": "fork-raw-syscall-custom-env",
    "fork_raw_syscall_chain_disabled": "fork-raw-syscall-chain-disabled",
    "vfork_raw_syscall_execveat_chain": "vfork-raw-syscall-execveat",
    "fork_bad_env_vector_efault": "fork-bad-env-vector",
    "vfork_bad_env_vector_efault": "vfork-bad-env-vector",
    "fork_bad_env_string_efault": "fork-bad-env-string",
    "vfork_bad_env_string_efault": "vfork-bad-env-string",
    # The child fast path has 255 usable stack env slots and an 8 KiB preload
    # buffer.  Inputs beyond either bound deliberately use libc's original envp.
    "vfork_env_slots_fallback": "vfork-env-slots-fallback",
    "vfork_long_preload_fallback": "vfork-long-preload-fallback",
    "no_duplicate_ld_preload": "duplicate-preload",
    "no_duplicate_ld_preload_whitespace": "duplicate-preload-whitespace",
    "no_duplicate_ld_preload_env_entries": "duplicate-preload-entry",
    "posix_spawn_custom_env_injection": "posix-spawn-custom-env",
    "posix_spawn_env_build_failure_passthrough": "posix-spawn-custom-env",
    "posix_spawn_null_env_injection": "posix-spawn-null-env",
    "posix_spawn_bad_env_failure_non_destructive": "posix-spawn-bad-env",
    "posix_spawn_bad_env_preflight_unknown_non_destructive": (
        "posix-spawn-bad-env"
    ),
    "posix_spawn_bad_argv_failure_non_destructive": "posix-spawn-bad-argv",
    "posix_spawn_preflight_unavailable_injection": (
        "posix-spawn-preflight-unavailable"
    ),
    "posix_spawn_preflight_unavailable_bad_env_delegates": (
        "posix-spawn-bad-env"
    ),
    "posix_spawn_duplicate_ld_preload": "posix-spawn-duplicate-preload",
    "posix_spawn_actions_attrs_injection": "posix-spawn-actions-attrs",
    "posix_spawn_actions_close_stderr": "posix-spawn-actions-close-stderr",
    "posix_spawn_usevfork_custom_env": "posix-spawn-usevfork-custom-env",
    "posix_spawn_child_peak_env_only_injection": (
        "posix-spawn-child-peak-env-only"
    ),
    "posix_spawn_chain_disabled": "posix-spawn-custom-env",
    "posix_spawn_failure_non_destructive": "posix-spawn-failure",
    "posix_spawn_resolver_null_preserves_errno": "posix-spawn-resolver-null",
    "posix_spawnp_path_search": "posix-spawnp-path-search",
    "posix_spawnp_env_build_failure_passthrough": "posix-spawnp-custom-env",
    "posix_spawnp_resolver_null_preserves_errno": "posix-spawnp-resolver-null",
    "posix_spawnp_empty_path_component": "posix-spawnp-path-search",
    "posix_spawnp_child_env_path_ignored": "posix-spawnp-child-env-path-ignored",
    "posix_spawnp_preflight_unavailable_delegates": "posix-spawnp-path-search",
    "posix_spawnp_preflight_unavailable_bad_env_delegates": (
        "posix-spawnp-bad-env"
    ),
    "posix_spawnp_bad_env_failure_non_destructive": "posix-spawnp-bad-env",
    "posix_spawnp_bad_env_preflight_unknown_non_destructive": (
        "posix-spawnp-bad-env"
    ),
    "posix_spawnp_bad_argv_failure_non_destructive": "posix-spawnp-bad-argv",
    "posix_spawn_explicit_peak_env_preserved": "posix-spawn-explicit-peak-env",
    "exec_checkpoint_concurrent_fini_callbacks": "exec-concurrent-fini-callbacks",
    "exec_checkpoint_fork_child_fini": "exec-checkpoint-fork-child-fini",
    "text_output_not_corrupted": "exec-failure",
}

SPAWN_MODES = {
    "posix_spawn_custom_env_injection",
    "posix_spawn_env_build_failure_passthrough",
    "posix_spawn_null_env_injection",
    "posix_spawn_duplicate_ld_preload",
    "posix_spawn_actions_attrs_injection",
    "posix_spawn_actions_close_stderr",
    "posix_spawn_usevfork_custom_env",
    "posix_spawn_child_peak_env_only_injection",
    "posix_spawn_preflight_unavailable_injection",
    "posix_spawn_preflight_unavailable_bad_env_delegates",
    "posix_spawn_chain_disabled",
    "posix_spawn_failure_non_destructive",
    "posix_spawn_resolver_null_preserves_errno",
    "posix_spawnp_path_search",
    "posix_spawnp_env_build_failure_passthrough",
    "posix_spawnp_resolver_null_preserves_errno",
    "posix_spawnp_empty_path_component",
    "posix_spawnp_child_env_path_ignored",
    "posix_spawnp_preflight_unavailable_delegates",
    "posix_spawnp_preflight_unavailable_bad_env_delegates",
    "posix_spawn_explicit_peak_env_preserved",
}

NO_EXEC_CHECKPOINT_MODES = {
    "raw_syscall_execve_backend_disabled_injection",
    "exec_checkpoint_disabled",
    "exec_checkpoint_concurrent_fini_callbacks",
    "execve_child_checkpoint_disabled",
    "exec_bad_stats_path_nonfatal",
    "raw_syscall_nonexec_passthrough",
    "execve_bad_env_failure_non_destructive",
    "execve_bad_env_preflight_unknown_non_destructive",
    "execve_bad_argv_failure_non_destructive",
    "fexecve_bad_env_failure_non_destructive",
    "fexecve_bad_env_preflight_unknown_non_destructive",
    "fexecve_bad_argv_failure_non_destructive",
    "execveat_bad_env_failure_non_destructive",
    "execveat_bad_env_preflight_unknown_non_destructive",
    "execveat_bad_argv_failure_non_destructive",
    "posix_spawn_bad_env_failure_non_destructive",
    "posix_spawn_bad_env_preflight_unknown_non_destructive",
    "posix_spawn_preflight_unavailable_bad_env_delegates",
    "posix_spawn_bad_argv_failure_non_destructive",
    "posix_spawnp_bad_env_failure_non_destructive",
    "posix_spawnp_bad_env_preflight_unknown_non_destructive",
    "posix_spawnp_preflight_unavailable_bad_env_delegates",
    "posix_spawnp_bad_argv_failure_non_destructive",
    *SPAWN_MODES,
}

CAPACITY_PASSTHROUGH_MODES = {
    "vfork_env_slots_fallback": ("env-slots", "large-env", 0),
    "vfork_long_preload_fallback": ("long-preload", "postfork-long-preload", 1),
}

NATIVE_REFERENCE_MODES = {
    "execve_bad_env_failure_non_destructive",
    "execve_bad_env_preflight_unknown_non_destructive",
    "execve_bad_argv_failure_non_destructive",
    "fexecve_bad_env_failure_non_destructive",
    "fexecve_bad_env_preflight_unknown_non_destructive",
    "fexecve_bad_argv_failure_non_destructive",
    "execveat_bad_env_failure_non_destructive",
    "execveat_bad_env_preflight_unknown_non_destructive",
    "execveat_bad_argv_failure_non_destructive",
    "execvpe_caller_path_child_env_ignored",
    "execvp_empty_path_component",
    "execvp_enotdir_enoent_failure",
    "execvp_eacces_fallback",
    "posix_spawnp_child_env_path_ignored",
    "posix_spawnp_empty_path_component",
    "posix_spawn_bad_env_failure_non_destructive",
    "posix_spawn_bad_env_preflight_unknown_non_destructive",
    "posix_spawn_preflight_unavailable_bad_env_delegates",
    "posix_spawnp_bad_env_failure_non_destructive",
    "posix_spawnp_bad_env_preflight_unknown_non_destructive",
    "posix_spawnp_preflight_unavailable_bad_env_delegates",
}

PREFLIGHT_UNAVAILABLE_MODES = {
    "execve_preflight_unavailable_injection",
    "fexecve_preflight_unavailable_injection",
    "posix_spawn_preflight_unavailable_injection",
    "posix_spawn_preflight_unavailable_bad_env_delegates",
    "posix_spawnp_preflight_unavailable_delegates",
    "posix_spawnp_preflight_unavailable_bad_env_delegates",
}

PREFLIGHT_UNKNOWN_BAD_ENV_MODES = {
    "execve_bad_env_preflight_unknown_non_destructive",
    "fexecve_bad_env_preflight_unknown_non_destructive",
    "execveat_bad_env_preflight_unknown_non_destructive",
    "posix_spawn_bad_env_preflight_unknown_non_destructive",
    "posix_spawnp_bad_env_preflight_unknown_non_destructive",
}

PREFLIGHT_ALL_UNAVAILABLE_MODES = {
    "posix_spawn_preflight_unavailable_bad_env_delegates",
    "posix_spawnp_preflight_unavailable_delegates",
    "posix_spawnp_preflight_unavailable_bad_env_delegates",
    *PREFLIGHT_UNKNOWN_BAD_ENV_MODES,
}

SPAWN_OBSERVATION_MODES = {
    "posix_spawn_bad_env_failure_non_destructive",
    "posix_spawn_bad_env_preflight_unknown_non_destructive",
    "posix_spawn_preflight_unavailable_bad_env_delegates",
    "posix_spawn_bad_argv_failure_non_destructive",
    "posix_spawn_failure_non_destructive",
    "posix_spawnp_bad_env_failure_non_destructive",
    "posix_spawnp_bad_env_preflight_unknown_non_destructive",
    "posix_spawnp_preflight_unavailable_bad_env_delegates",
    "posix_spawnp_bad_argv_failure_non_destructive",
}

FALLBACK_EXECVP_MODES = {
    "no_duplicate_ld_preload_execvpe_path_fallback",
    "execvp_enotdir_enoent_fallback",
    "execvp_eacces_fallback",
    "execvp_enoent_fallback",
}


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", required=True, choices=sorted(MODE_TO_APP_ARG))
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    return parser.parse_args()


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def require_native_reference(args, proc):
    require(proc.returncode == 0,
            f"native reference failed with {proc.returncode}\n{proc.stdout}")
    mode = args.mode
    output = proc.stdout
    if mode in SPAWN_OBSERVATION_MODES:
        observation = parse_spawn_observation(output)
        require_valid_spawn_observation(observation, "native")
        return observation
    if mode in {
        "execve_bad_env_failure_non_destructive",
        "execve_bad_env_preflight_unknown_non_destructive",
    }:
        require(f"execve_bad_env_errno={errno.EFAULT}" in output,
                f"native execve bad-env missed EFAULT\n{output}")
        return
    if mode == "execve_bad_argv_failure_non_destructive":
        require(f"execve_bad_argv_errno={errno.EFAULT}" in output,
                f"native execve bad-argv missed EFAULT\n{output}")
        return
    if mode in {
        "fexecve_bad_env_failure_non_destructive",
        "fexecve_bad_env_preflight_unknown_non_destructive",
    }:
        require(f"fexecve_bad_env_errno={errno.EFAULT}" in output,
                f"native fexecve bad-env missed EFAULT\n{output}")
        return
    if mode == "fexecve_bad_argv_failure_non_destructive":
        require(f"fexecve_bad_argv_errno={errno.EFAULT}" in output,
                f"native fexecve bad-argv missed EFAULT\n{output}")
        return
    if mode in {
        "execveat_bad_env_failure_non_destructive",
        "execveat_bad_env_preflight_unknown_non_destructive",
    }:
        require(f"execveat_bad_env_errno={errno.EFAULT}" in output,
                f"native execveat bad-env missed EFAULT\n{output}")
        return
    if mode == "execveat_bad_argv_failure_non_destructive":
        require(f"execveat_bad_argv_errno={errno.EFAULT}" in output,
                f"native execveat bad-argv missed EFAULT\n{output}")
        return
    if mode in {
        "posix_spawn_bad_env_failure_non_destructive",
        "posix_spawn_bad_env_preflight_unknown_non_destructive",
        "posix_spawn_preflight_unavailable_bad_env_delegates",
    }:
        require(f"posix_spawn_bad_env_result={errno.EFAULT}" in output,
                f"native posix_spawn bad-env missed EFAULT\n{output}")
        return
    if mode == "posix_spawn_bad_argv_failure_non_destructive":
        require(f"posix_spawn_bad_argv_result={errno.EFAULT}" in output,
                f"native posix_spawn bad-argv missed EFAULT\n{output}")
        return
    if mode in {
        "posix_spawnp_bad_env_failure_non_destructive",
        "posix_spawnp_bad_env_preflight_unknown_non_destructive",
        "posix_spawnp_preflight_unavailable_bad_env_delegates",
    }:
        require(f"posix_spawnp_bad_env_result={errno.EFAULT}" in output,
                f"native posix_spawnp bad-env missed EFAULT\n{output}")
        return
    if mode == "posix_spawnp_bad_argv_failure_non_destructive":
        require(f"posix_spawnp_bad_argv_result={errno.EFAULT}" in output,
                f"native posix_spawnp bad-argv missed EFAULT\n{output}")
        return
    if mode in {
        "execvp_enotdir_enoent_failure",
    }:
        require(f"execvp_enoent_errno={errno.ENOENT}" in output,
                f"native execvp ENOTDIR/ENOENT missed ENOENT\n{output}")
        return
    if mode == "execvp_eacces_fallback":
        require(f"execvp_eacces_errno={errno.EACCES}" in output,
                f"native execvp EACCES missed EACCES\n{output}")
        return
    require("exec_child_ok" in output,
            f"native PATH reference missed child\n{output}")


def parse_spawn_observation(output):
    match = re.search(
        r"spawn_observation operation=(?P<operation>\S+) "
        r"result=(?P<result>-?\d+) errno=(?P<errno>-?\d+) "
        r"pid_created=(?P<pid_created>[01]) "
        r"invalid_success=(?P<invalid_success>[01]) "
        r"wait_status=(?P<wait_status>-?\d+) "
        r"child_exit=(?P<child_exit>-?\d+) "
        r"child_signal=(?P<child_signal>-?\d+) "
        r"wait_errno=(?P<wait_errno>-?\d+) "
        r"continued_sink=(?P<continued_sink>\d+)",
        output,
    )
    require(match is not None, f"missing spawn observation\n{output}")
    observation = match.groupdict()
    for name in observation:
        if name != "operation":
            observation[name] = int(observation[name])
    return observation


def require_valid_spawn_observation(observation, label):
    require(observation["pid_created"] == int(observation["result"] == 0),
            f"{label} spawn child creation did not follow its return value\n"
            f"{observation}")
    require(not observation["invalid_success"],
            f"{label} posix_spawn returned success without a usable child PID\n"
            f"{observation}")


def install_path_fixture(tmpdir: Path, exe: Path):
    bindir = tmpdir / "bin"
    bindir.mkdir()
    target = bindir / "test_exec_chain"
    try:
        target.symlink_to(exe)
    except OSError:
        shutil.copy2(exe, target)
        target.chmod(target.stat().st_mode | stat.S_IXUSR)

    helper_named = bindir / "peak_detach_helper"
    try:
        helper_named.symlink_to(exe)
    except OSError:
        shutil.copy2(exe, helper_named)
        helper_named.chmod(helper_named.stat().st_mode | stat.S_IXUSR)

    enoexec = bindir / "enoexec-script"
    enoexec.write_text("echo enoexec_script_ran\nexit 37\n", encoding="utf-8")
    enoexec.chmod(0o755)

    cwd_target = tmpdir / "test_exec_chain"
    try:
        cwd_target.symlink_to(exe)
    except OSError:
        shutil.copy2(exe, cwd_target)
        cwd_target.chmod(cwd_target.stat().st_mode | stat.S_IXUSR)

    blocked_dir = tmpdir / "blocked"
    blocked_dir.mkdir()
    blocked = blocked_dir / "blocked-exec"
    blocked.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    blocked.chmod(0o644)
    return bindir, blocked_dir


def base_env(args, tmpdir: Path, bindir: Path, blocked_dir: Path, preload: bool):
    env = os.environ.copy()
    for name in list(env):
        if name.startswith("PEAK_"):
            env.pop(name, None)
    if preload:
        env["LD_PRELOAD"] = str(Path(args.libpeak).resolve())
        env["PEAK_TARGET"] = TARGET
        env["PEAK_STATSLOG_PATH"] = "peak_stats"
        env["PEAK_HEARTBEAT_INTERVAL"] = "0"
        env["PEAK_TEXT_OUTPUT"] = "0"
    else:
        env.pop("LD_PRELOAD", None)
    if args.mode == "raw_syscall_execve_backend_disabled_injection":
        env.pop("PEAK_TARGET", None)

    old_path = env.get("PATH", "/bin:/usr/bin")
    env["PATH"] = f"{bindir}{os.pathsep}{old_path}"
    if args.mode == "execvp_eacces_failure":
        env["PATH"] = str(blocked_dir)
    if args.mode in {
        "posix_spawn_actions_attrs_injection",
        "posix_spawn_actions_close_stderr",
    }:
        env["EXEC_CHAIN_SPAWN_ACTIONS_OUT"] = str(tmpdir / "spawn-actions.out")
    if args.mode in {
        "execve_child_peak_env_only_injection",
        "posix_spawn_child_peak_env_only_injection",
    }:
        env["EXEC_CHAIN_CHILD_STATS_PREFIX"] = "peak_stats"
    if args.mode == "exec_checkpoint_disabled":
        env["PEAK_EXEC_CHECKPOINT"] = "0"
    if args.mode == "execve_propagate_disabled":
        env["PEAK_EXEC_PROPAGATE_PEAK_ENV"] = "0"
    if args.mode in {"execve_chain_disabled", "posix_spawn_chain_disabled"}:
        env["PEAK_EXEC_CHAIN"] = "0"
    if args.mode == "execve_secure_skip":
        env["PEAK_TEST_EXEC_AT_SECURE"] = "1"
    if args.mode in {
        "execve_env_build_failure_passthrough",
        "posix_spawn_env_build_failure_passthrough",
        "posix_spawnp_env_build_failure_passthrough",
    }:
        env["PEAK_TEST_EXEC_ENV_BUILD_FAIL"] = "1"
    if args.mode in {
        "posix_spawn_resolver_null_preserves_errno",
        "posix_spawnp_resolver_null_preserves_errno",
    }:
        env["PEAK_TEST_EXEC_SPAWN_RESOLVER_NULL"] = "1"
    if args.mode in PREFLIGHT_UNAVAILABLE_MODES | PREFLIGHT_UNKNOWN_BAD_ENV_MODES:
        env["PEAK_TEST_EXEC_PREFLIGHT_UNAVAILABLE"] = "1"
    if args.mode in PREFLIGHT_ALL_UNAVAILABLE_MODES:
        env["PEAK_TEST_EXEC_PREFLIGHT_NO_PROC_MEM"] = "1"
        env["PEAK_TEST_EXEC_PREFLIGHT_MAPS_UNAVAILABLE"] = "1"
    if args.mode in FALLBACK_EXECVP_MODES:
        env["PEAK_TEST_EXECVPE_FALLBACK"] = "1"
    if args.mode == "exec_checkpoint_write_target_no_reentry":
        env["PEAK_TARGET"] = "write"
    if args.mode == "exec_bad_stats_path_nonfatal":
        env["PEAK_STATSLOG_PATH"] = str(tmpdir / "missing-dir" / "peak_stats")
    if args.mode == "text_output_not_corrupted":
        env["PEAK_TEXT_OUTPUT"] = "1"
    if args.mode in {"execvp_empty_path_component", "posix_spawnp_empty_path_component"}:
        env["PATH"] = f"{os.pathsep}{tmpdir / 'missing-after-empty'}"
    if args.mode == "no_duplicate_ld_preload_execvpe_path_fallback":
        env["PATH"] = str(bindir)
        env["EXEC_CHAIN_TEST_CHILD_PATH"] = str(bindir)
        env["PEAK_EXEC_PROPAGATE_PEAK_ENV"] = "0"
    if args.mode in {
        "execvp_enotdir_enoent_failure",
        "execvp_enotdir_enoent_fallback",
    }:
        not_dir = tmpdir / "not-a-dir"
        not_dir.write_text("not a directory\n", encoding="utf-8")
        env["PATH"] = f"{not_dir}{os.pathsep}{tmpdir / 'missing-dir'}"
    if args.mode in {"execvp_eacces_fallback", "execvp_enoent_fallback"}:
        env["PATH"] = str(blocked_dir if args.mode == "execvp_eacces_fallback"
                          else tmpdir / "missing-dir")
    return env


def run_fixture(args, tmpdir: Path, preload=True):
    exe = Path(args.exe).resolve()
    tmpdir.mkdir(parents=True, exist_ok=True)
    bindir, blocked_dir = install_path_fixture(tmpdir, exe)
    env = base_env(args, tmpdir, bindir, blocked_dir, preload=preload)
    app_arg = MODE_TO_APP_ARG[args.mode]
    return subprocess.run(
        [str(exe), app_arg],
        cwd=tmpdir,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=20,
    )


def csv_files(tmpdir: Path):
    stats_name = re.compile(
        r"^(?:peak_stats|peak_statslog)-p\d+(?P<checkpoint>-exec\d+)?\.csv$"
    )
    all_csv = sorted(tmpdir.glob("*.csv"))
    matched_csv = [(path, stats_name.fullmatch(path.name)) for path in all_csv]
    unexpected_csv = [path for path, match in matched_csv if match is None]
    require(not unexpected_csv,
            f"unexpected CSV artifacts in {tmpdir}: {unexpected_csv}")
    exec_files = [path for path, match in matched_csv if match["checkpoint"]]
    final_files = [path for path, match in matched_csv if not match["checkpoint"]]
    return exec_files, final_files


def target_count(paths, function=TARGET):
    total = 0
    for path in paths:
        with path.open(newline="", encoding="utf-8", errors="replace") as fp:
            reader = csv.DictReader(fp)
            for row in reader:
                if row.get("function") == function:
                    total += int(row.get("count", "0"))
    return total


def require_bad_env_artifacts(mode, final_files, exec_files, operation):
    require(final_files, f"missing final CSV after bad-env {operation}")
    if mode in PREFLIGHT_UNKNOWN_BAD_ENV_MODES:
        require(len(final_files) == 1,
                f"expected one final CSV after preflight-unknown {operation}: "
                f"{final_files}")
    require(target_count(final_files) >= 6,
            f"normal final CSV missed calls after bad-env {operation}")
    require(not exec_files,
            f"bad-env {operation} should not write exec checkpoints: {exec_files}")


def require_spawn_observation(args, output, final_files, exec_files,
                              native_observation):
    observation = parse_spawn_observation(output)
    require(native_observation is not None,
            f"missing native spawn reference for {args.mode}")
    require_valid_spawn_observation(native_observation, "native")
    require_valid_spawn_observation(observation, "PEAK")
    require(observation == native_observation,
            "PEAK spawn behavior diverged from the same-host native reference\n"
            f"native={native_observation}\npeak={observation}\n{output}")
    require(observation["continued_sink"] >= 6,
            f"parent did not continue profiling after spawn call\n{output}")
    if observation["pid_created"]:
        require(observation["wait_status"] >= 0,
                f"created spawn child was not waited\n{output}")
        require(observation["wait_errno"] == 0,
                f"waiting for created spawn child failed\n{output}")
    else:
        require(observation["wait_status"] == -1,
                f"spawn reported no child but recorded a wait status\n{output}")
    require(len(final_files) == 1,
            f"expected exactly one final CSV after spawn failure: {final_files}")
    require(target_count(final_files) >= 6,
            "normal final CSV missed calls after spawn failure")
    require(not exec_files,
            f"spawn failure should not write exec checkpoints: {exec_files}")


def require_ld_count(output, expected):
    match = re.search(r"ld_preload_libpeak_count=(\d+)", output)
    require(match is not None, f"missing LD_PRELOAD count\n{output}")
    require(int(match.group(1)) == expected,
            f"expected {expected} libpeak LD_PRELOAD entries\n{output}")


def require_single_ld_env(output):
    match = re.search(r"ld_preload_env_entries=(\d+)", output)
    require(match is not None, f"missing LD_PRELOAD env entry count\n{output}")
    require(int(match.group(1)) == 1,
            f"expected one LD_PRELOAD env entry\n{output}")


def check_common(args, proc, tmpdir: Path, native_observation=None):
    output = proc.stdout
    exec_files, final_files = csv_files(tmpdir)

    if args.mode == "exec_checkpoint_concurrent_fini_callbacks":
        require("checkpoint_reader_gate_held=1 fini_waiting=1" in output,
                f"checkpoint/fini lifetime-gate ordering was not proven\n{output}")
        require(final_files,
                f"fini did not complete after releasing checkpoint reader\n{output}")

    if args.mode == "exec_checkpoint_fork_child_fini":
        match = re.search(
            r"fork_child_checkpoint_skipped=1 failed_exec_exit=1 "
            r"normal_return_exit=1 parent_pid=(\d+)",
            output,
        )
        require(match is not None,
                f"fork-child lifecycle invariants were not proven\n{output}")
        parent_pid = match.group(1)
        require(len(exec_files) == 1,
                f"expected one parent checkpoint, found {exec_files}\n{output}")
        require(len(final_files) == 1,
                f"expected one parent final CSV, found {final_files}\n{output}")
        require(all(f"-p{parent_pid}" in path.name
                    for path in exec_files + final_files),
                f"fork child emitted PEAK artifacts: "
                f"{exec_files + final_files}\n{output}")

    if args.mode == "execvp_enoexec_fallback":
        require(proc.returncode == 37,
                f"ENOEXEC fallback returned {proc.returncode}\n{output}")
        require("enoexec_script_ran" in output,
                f"missing shell fallback output\n{output}")
        require(exec_files, "missing checkpoint before ENOEXEC shell handoff")
        require(target_count(exec_files) >= 5,
                f"ENOEXEC checkpoint missed parent calls: {exec_files}")
        return

    require(proc.returncode == 0,
            f"fixture exited {proc.returncode}\n{output}")

    if args.mode in CAPACITY_PASSTHROUGH_MODES:
        kind, marker, ld_preload_env_entries = CAPACITY_PASSTHROUGH_MODES[args.mode]
        require(len(exec_files) <= 1,
                f"capacity passthrough fabricated child/extra checkpoints: "
                f"{exec_files}\n{output}")
        if exec_files:
            require(target_count(exec_files) == 5,
                    "capacity passthrough checkpoint includes child calls or "
                    f"misses the parent exec boundary: {exec_files}")
        require(f"postfork_capacity_passthrough=1 kind={kind}" in output,
                f"capacity fallback was not proven\n{output}")
        require(f"marker={marker}" in output,
                f"capacity fallback did not preserve the native child marker\n{output}")
        require_ld_count(output, 0)
        require(f"ld_preload_env_entries={ld_preload_env_entries}" in output,
                f"capacity fallback changed the native LD_PRELOAD environment\n{output}")
        for name in (
            "peak_target",
            "peak_statslog",
            "peak_exec_chain",
            "peak_exec_checkpoint",
            "peak_exec_propagate",
        ):
            require(f"{name}=<missing>" in output,
                    f"capacity fallback injected {name}\n{output}")
        require("exec_child_ok" in output,
                f"capacity fallback parent exec did not retain native success\n{output}")
        return

    if args.mode not in NO_EXEC_CHECKPOINT_MODES:
        require(exec_files, f"missing exec checkpoint for {args.mode}\n{output}")
    else:
        require(not exec_files,
                f"unexpected exec checkpoint for {args.mode}: {exec_files}\n{output}")

    if args.mode == "execv_zero_call_checkpoint":
        require(target_count(exec_files) == 0,
                f"zero-call checkpoint recorded target calls: {exec_files}")

    if args.mode == "exec_checkpoint_write_target_no_reentry":
        require("parent_write_before_exec" in output,
                f"missing parent write marker\n{output}")
        require(target_count(exec_files, "write") == 1,
                "checkpoint I/O re-entered the profiled write target")

    if args.mode in {
        "execv_success_checkpoint",
        "execve_custom_env_injection",
        "raw_syscall_execve_custom_env_injection",
        "execve_preflight_unavailable_injection",
        "execve_null_env_injection",
        "execve_large_env_injection",
        "fexecve_custom_env_injection",
        "fexecve_preflight_unavailable_injection",
        "execveat_custom_env_injection",
        "raw_syscall_execveat_custom_env_injection",
        "execl_success_checkpoint",
        "execlp_path_search",
        "execle_custom_env_injection",
        "execvp_path_search",
        "execvp_empty_path_component",
        "helper_named_exec_still_checkpointed",
        "execvpe_caller_path_child_env_ignored",
        "execve_child_peak_env_only_injection",
        "execve_child_chain_disabled",
        "execve_child_propagate_disabled",
        "fork_child_exec_parent_exec",
        "vfork_child_exec_parent_exec",
        "vfork_child_failed_exec_parent_exec",
        "vfork_execl_parent_exec",
        "vfork_execlp_parent_exec",
        "vfork_execle_parent_exec",
        "fork_custom_env_execve_chain",
        "vfork_custom_env_execve_chain",
        "vfork_custom_env_execle_chain",
        "fork_custom_env_chain_disabled",
        "vfork_custom_env_chain_disabled",
        "vfork_custom_env_execvpe_chain",
        "vfork_custom_env_execvp_chain",
        "vfork_custom_env_execlp_chain",
        "vfork_custom_env_fexecve_chain",
        "vfork_custom_env_execveat_chain",
        "fork_raw_syscall_custom_env_chain",
        "fork_raw_syscall_chain_disabled",
        "vfork_raw_syscall_execveat_chain",
        "fork_bad_env_vector_efault",
        "vfork_bad_env_vector_efault",
        "fork_bad_env_string_efault",
        "vfork_bad_env_string_efault",
        "vfork_env_slots_fallback",
        "vfork_long_preload_fallback",
        "no_duplicate_ld_preload",
        "no_duplicate_ld_preload_whitespace",
        "no_duplicate_ld_preload_env_entries",
    }:
        require(target_count(exec_files) >= 5,
                f"checkpoint missed parent calls: {exec_files}")

    if args.mode in {
        "execve_env_build_failure_passthrough",
        "posix_spawn_env_build_failure_passthrough",
        "posix_spawnp_env_build_failure_passthrough",
    }:
        require_ld_count(output, 0)
        expected_marker = {
            "execve_env_build_failure_passthrough": "custom-env",
            "posix_spawn_env_build_failure_passthrough": "posix-spawn-env",
            "posix_spawnp_env_build_failure_passthrough": "posix-spawnp-env",
        }[args.mode]
        require(f"marker={expected_marker}" in output,
                f"original envp was not passed through\n{output}")

    if args.mode in {
        "posix_spawn_resolver_null_preserves_errno",
        "posix_spawnp_resolver_null_preserves_errno",
    }:
        expected_path_search = "1" if args.mode.startswith("posix_spawnp_") else "0"
        require(
            f"posix_spawn_resolver_null_ok path_search={expected_path_search}" in output,
            f"spawn resolver-null fallback did not preserve errno\n{output}",
        )

    if args.mode in {
        "execve_custom_env_injection",
        "raw_syscall_execve_custom_env_injection",
        "raw_syscall_execve_backend_disabled_injection",
        "execve_preflight_unavailable_injection",
        "execve_null_env_injection",
        "execve_large_env_injection",
        "fexecve_custom_env_injection",
        "fexecve_preflight_unavailable_injection",
        "execveat_custom_env_injection",
        "raw_syscall_execveat_custom_env_injection",
        "execle_custom_env_injection",
        "execve_child_checkpoint_disabled",
        "execve_child_propagate_disabled",
        "no_duplicate_ld_preload",
        "no_duplicate_ld_preload_whitespace",
        "no_duplicate_ld_preload_env_entries",
        "no_duplicate_ld_preload_execvpe_path_fallback",
        "posix_spawn_custom_env_injection",
        "posix_spawn_null_env_injection",
        "posix_spawn_duplicate_ld_preload",
        "posix_spawn_actions_attrs_injection",
        "posix_spawn_child_peak_env_only_injection",
        "fork_custom_env_execve_chain",
        "vfork_custom_env_execve_chain",
        "vfork_custom_env_execle_chain",
        "fork_raw_syscall_custom_env_chain",
    }:
        check_text = output
        if args.mode in {
            "posix_spawn_actions_attrs_injection",
            "posix_spawn_actions_close_stderr",
        }:
            redirected = tmpdir / "spawn-actions.out"
            require(redirected.exists(), "spawn action did not create output")
            check_text = redirected.read_text(encoding="utf-8", errors="replace")
            if args.mode == "posix_spawn_actions_attrs_injection":
                require("posix_spawn_actions_attrs_ok" in output,
                        f"missing spawn action parent marker\n{output}")
            else:
                require("posix_spawn_actions_close_stderr_ok" in output,
                        f"missing close-stderr parent marker\n{output}")
        require_ld_count(check_text, 1)
        require_single_ld_env(check_text)

    if args.mode in {
        "fork_custom_env_chain_disabled",
        "vfork_custom_env_chain_disabled",
        "fork_raw_syscall_chain_disabled",
    }:
        require_ld_count(output, 0)
        require("peak_exec_chain=0" in output,
                f"child PEAK_EXEC_CHAIN=0 was not preserved\n{output}")

    if args.mode in {
        "fork_custom_env_execve_chain",
        "vfork_custom_env_execve_chain",
        "vfork_custom_env_execle_chain",
        "fork_custom_env_chain_disabled",
        "vfork_custom_env_chain_disabled",
        "fork_raw_syscall_custom_env_chain",
        "fork_raw_syscall_chain_disabled",
    }:
        require("parent_env_unchanged=1" in output,
                f"post-fork child changed parent environment\n{output}")

    if args.mode in {
        "vfork_custom_env_execvpe_chain",
        "vfork_custom_env_execvp_chain",
        "vfork_custom_env_execlp_chain",
        "vfork_custom_env_fexecve_chain",
        "vfork_custom_env_execveat_chain",
        "vfork_raw_syscall_execveat_chain",
    }:
        require_ld_count(output, 1)
        require_single_ld_env(output)
        require("marker=postfork-api-custom" in output,
                f"post-fork custom env was not preserved\n{output}")
        require("peak_target=explicit_child_target" in output,
                f"post-fork explicit PEAK_TARGET was overwritten\n{output}")
        require("postfork_api_parent_env_unchanged=1" in output,
                f"post-fork wrapper changed parent environ\n{output}")

    if args.mode in {
        "fork_bad_env_vector_efault",
        "vfork_bad_env_vector_efault",
        "fork_bad_env_string_efault",
        "vfork_bad_env_string_efault",
    }:
        require("postfork_bad_env_efault=1" in output,
                f"post-fork bad env did not preserve EFAULT\n{output}")

    if args.mode in {
        "no_duplicate_ld_preload",
        "no_duplicate_ld_preload_whitespace",
        "no_duplicate_ld_preload_env_entries",
        "no_duplicate_ld_preload_execvpe_path_fallback",
        "posix_spawn_duplicate_ld_preload",
    }:
        match = re.search(r"ld_preload_extra_count=(\d+)", output)
        require(match is not None, f"missing extra preload count\n{output}")
        require(int(match.group(1)) == 2,
                f"extra LD_PRELOAD entries were not preserved\n{output}")

    if args.mode == "posix_spawn_actions_attrs_injection":
        text = (tmpdir / "spawn-actions.out").read_text(
            encoding="utf-8",
            errors="replace",
        )
        match = re.search(r"ld_preload_extra_count=(\d+)", text)
        require(match is not None, f"missing redirected preload count\n{text}")

    if args.mode in {
        "execve_explicit_peak_env_preserved",
        "posix_spawn_explicit_peak_env_preserved",
    }:
        require("child_env_PEAK_TARGET=explicit_child_target" in output,
                f"explicit child PEAK_TARGET was not preserved\n{output}")

    if args.mode in {
        "execve_child_peak_env_only_injection",
        "posix_spawn_child_peak_env_only_injection",
    }:
        require("marker=child-peak-env" in output,
                f"child-only PEAK env marker missing\n{output}")
        require_ld_count(output, 1)

    if args.mode == "execve_propagate_disabled":
        require_ld_count(output, 1)
        require("peak_target=<missing>" in output,
                f"disabled propagation leaked PEAK_TARGET\n{output}")

    if args.mode == "execve_child_checkpoint_disabled":
        require_ld_count(output, 1)
        require("peak_exec_checkpoint=0" in output,
                f"child PEAK_EXEC_CHECKPOINT=0 was not preserved\n{output}")

    if args.mode == "execve_child_propagate_disabled":
        require_ld_count(output, 1)
        require("peak_target=<missing>" in output,
                f"child PEAK_EXEC_PROPAGATE_PEAK_ENV=0 leaked target\n{output}")
        require("peak_exec_propagate=0" in output,
                f"child PEAK_EXEC_PROPAGATE_PEAK_ENV=0 was not preserved\n{output}")

    if args.mode == "execve_child_chain_disabled":
        require_ld_count(output, 0)
        require("peak_exec_chain=0" in output,
                f"child PEAK_EXEC_CHAIN=0 was not preserved\n{output}")

    if args.mode in {
        "execve_chain_disabled",
        "execve_secure_skip",
        "posix_spawn_chain_disabled",
    }:
        require_ld_count(output, 0)

    if args.mode == "exec_bad_stats_path_nonfatal":
        require("exec_child_ok" in output,
                f"bad checkpoint path made exec fail\n{output}")

    if args.mode == "exec_failure_non_destructive":
        require(f"exec_failure_errno={errno.ENOENT}" in output,
                f"missing exec failure marker\n{output}")
        require(final_files, "missing final CSV after failed exec")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after failed exec")

    if args.mode == "raw_syscall_execve_failure_non_destructive":
        require(f"raw_syscall_execve_errno={errno.ENOENT}" in output,
                f"missing raw syscall exec failure marker\n{output}")
        require(final_files, "missing final CSV after failed raw syscall exec")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after raw syscall exec failure")

    if args.mode == "raw_syscall_nonexec_passthrough":
        match = re.search(r"raw_syscall_getpid=(\d+) expected=(\d+) errno=(\d+)",
                          output)
        require(match is not None and match.group(1) == match.group(2) and
                int(match.group(3)) == errno.EALREADY,
                f"non-exec syscall was not transparently forwarded\n{output}")
        require("raw_syscall_write=ok" in output,
                f"multi-argument syscall was not transparently forwarded\n{output}")

    if args.mode in {
        "execvp_enoent_failure",
        "execvp_enotdir_enoent_failure",
        "execvp_enotdir_enoent_fallback",
        "execvp_enoent_fallback",
    }:
        require(f"execvp_enoent_errno={errno.ENOENT}" in output,
                f"missing execvp ENOENT marker\n{output}")
        require(final_files, "missing final CSV after execvp ENOENT")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after execvp ENOENT")

    if args.mode in {"execvp_eacces_failure", "execvp_eacces_fallback"}:
        require(f"execvp_eacces_errno={errno.EACCES}" in output,
                f"missing execvp EACCES marker\n{output}")
        require(final_files, "missing final CSV after execvp EACCES")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after execvp EACCES")

    if args.mode in {
        "execve_bad_env_failure_non_destructive",
        "execve_bad_env_preflight_unknown_non_destructive",
    }:
        require(f"execve_bad_env_errno={errno.EFAULT}" in output,
                f"missing execve bad-env marker\n{output}")
        require_bad_env_artifacts(args.mode, final_files, exec_files, "execve")

    if args.mode == "execve_bad_argv_failure_non_destructive":
        require(f"execve_bad_argv_errno={errno.EFAULT}" in output,
                f"missing execve bad-argv marker\n{output}")
        require(final_files, "missing final CSV after bad-argv execve")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after bad-argv execve")
        require(not exec_files,
                f"bad-argv execve should not write exec checkpoints: {exec_files}")

    if args.mode in {
        "fexecve_bad_env_failure_non_destructive",
        "fexecve_bad_env_preflight_unknown_non_destructive",
    }:
        require(f"fexecve_bad_env_errno={errno.EFAULT}" in output,
                f"missing fexecve bad-env marker\n{output}")
        require_bad_env_artifacts(args.mode, final_files, exec_files, "fexecve")

    if args.mode == "fexecve_bad_argv_failure_non_destructive":
        require(f"fexecve_bad_argv_errno={errno.EFAULT}" in output,
                f"missing fexecve bad-argv marker\n{output}")
        require(final_files, "missing final CSV after bad-argv fexecve")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after bad-argv fexecve")
        require(not exec_files,
                f"bad-argv fexecve should not write exec checkpoints: {exec_files}")

    if args.mode in {
        "execveat_bad_env_failure_non_destructive",
        "execveat_bad_env_preflight_unknown_non_destructive",
    }:
        require(f"execveat_bad_env_errno={errno.EFAULT}" in output,
                f"missing execveat bad-env marker\n{output}")
        require_bad_env_artifacts(args.mode, final_files, exec_files, "execveat")

    if args.mode == "execveat_bad_argv_failure_non_destructive":
        require(f"execveat_bad_argv_errno={errno.EFAULT}" in output,
                f"missing execveat bad-argv marker\n{output}")
        require(final_files, "missing final CSV after bad-argv execveat")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after bad-argv execveat")
        require(not exec_files,
                f"bad-argv execveat should not write exec checkpoints: {exec_files}")

    if args.mode in SPAWN_OBSERVATION_MODES:
        require_spawn_observation(args,
                                  output,
                                  final_files,
                                  exec_files,
                                  native_observation)

    if args.mode in {
        "posix_spawnp_path_search",
        "posix_spawnp_preflight_unavailable_delegates",
        "posix_spawnp_child_env_path_ignored",
        "posix_spawnp_empty_path_component",
    }:
        require("exec_child_ok" in output,
                f"spawnp child did not run\n{output}")

    if args.mode in {
        "execvpe_caller_path_child_env_ignored",
        "posix_spawnp_child_env_path_ignored",
        "execvp_empty_path_component",
        "posix_spawnp_empty_path_component",
    }:
        require("exec_child_ok" in output,
                f"caller PATH search did not find fixture\n{output}")

    if args.mode == "posix_spawn_actions_close_stderr":
        text = (tmpdir / "spawn-actions.out").read_text(
            encoding="utf-8",
            errors="replace",
        )
        require("child_stdout_after_stderr_close" in text,
                f"close-stderr child stdout marker missing\n{text}")
        require("child_stderr_sentinel_after_spawn_close" not in output,
                f"closed stderr sentinel leaked to parent output\n{output}")
        require("child_stderr_sentinel_after_spawn_close" not in text,
                f"closed stderr sentinel leaked to redirected stdout\n{text}")

    if args.mode == "posix_spawn_usevfork_custom_env":
        require("posix_spawn_usevfork_ok" in output,
                f"missing usevfork marker\n{output}")

    if args.mode == "text_output_not_corrupted":
        require("exec_failure_errno=2" in output,
                f"missing failed exec marker\n{output}")
        require("PEAK Library" in output,
                f"text PEAK output missing\n{output}")
        require(TARGET in output,
                f"text PEAK output lost target name\n{output}")


def main():
    args = parse_args()
    with tempfile.TemporaryDirectory(
        prefix=f"peak-exec-chain-{args.mode}-",
        dir=os.getcwd(),
    ) as tmp:
        tmpdir = Path(tmp)
        native_observation = None
        if args.mode in NATIVE_REFERENCE_MODES | SPAWN_OBSERVATION_MODES:
            native_proc = run_fixture(args, tmpdir / "native", preload=False)
            native_observation = require_native_reference(args, native_proc)
        peak_tmp = tmpdir / "peak"
        peak_tmp.mkdir()
        proc = run_fixture(args, peak_tmp, preload=True)
        try:
            check_common(args, proc, peak_tmp, native_observation)
        except Exception:
            sys.stdout.write(proc.stdout)
            for path in sorted(peak_tmp.glob("*")):
                if path.is_file():
                    print(f"artifact {path.name}:")
                    try:
                        data = path.read_bytes()
                        if b"\0" in data[:4096] or len(data) > 32768:
                            print(f"<skipped binary/large artifact size={len(data)}>")
                            continue
                        print(data.decode("utf-8", errors="replace"))
                    except Exception as exc:
                        print(f"<unreadable: {exc}>")
            raise
    print(f"exec_chain_check_ok mode={args.mode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
