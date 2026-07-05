#!/usr/bin/env python3

import argparse
import csv
import errno
import fcntl
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


TARGET = "peak_exec_chain_hot_target"


MODE_TO_APP_ARG = {
    "execv_success_checkpoint": "execv-success",
    "exec_checkpoint_write_target_no_reentry": "execv-write-target-success",
    "execv_zero_call_checkpoint": "execv-zero-call",
    "exec_checkpoint_disabled": "execv-success",
    "execve_custom_env_injection": "execve-custom-env",
    "execve_preflight_unavailable_injection": "execve-custom-env",
    "execve_child_peak_env_only_injection": "execve-child-peak-env-only",
    "syscall_execve_custom_env_injection": "syscall-execve-custom-env",
    "syscall_execve_preflight_unavailable_injection": (
        "syscall-execve-custom-env"
    ),
    "syscall_execve_failure_non_destructive": "syscall-execve-failure",
    "syscall_execve_bad_env_failure_non_destructive": "syscall-execve-bad-env",
    "syscall_execve_bad_env_preflight_unknown_non_destructive": (
        "syscall-execve-bad-env"
    ),
    "syscall_execve_bad_argv_failure_non_destructive": (
        "syscall-execve-bad-argv"
    ),
    "execve_bad_env_failure_non_destructive": "execve-bad-env",
    "execve_bad_env_preflight_unknown_non_destructive": "execve-bad-env",
    "execve_bad_argv_failure_non_destructive": "execve-bad-argv",
    "execve_chain_disabled": "execve-custom-env",
    "execve_propagate_disabled": "execve-custom-env",
    "execve_secure_skip": "execve-custom-env",
    "execve_explicit_peak_env_preserved": "execve-explicit-peak-env",
    "execve_null_env_injection": "execve-null-env",
    "fexecve_custom_env_injection": "fexecve-custom-env",
    "fexecve_preflight_unavailable_injection": "fexecve-custom-env",
    "fexecve_failure_non_destructive": "fexecve-failure",
    "fexecve_bad_env_failure_non_destructive": "fexecve-bad-env",
    "fexecve_bad_env_preflight_unknown_non_destructive": "fexecve-bad-env",
    "fexecve_bad_argv_failure_non_destructive": "fexecve-bad-argv",
    "execveat_custom_env_injection": "execveat-custom-env",
    "execveat_empty_path_custom_env": "execveat-empty-path-custom-env",
    "execveat_empty_path_bad_env_failure_non_destructive": (
        "execveat-empty-path-bad-env"
    ),
    "execveat_empty_path_bad_env_preflight_unknown_non_destructive": (
        "execveat-empty-path-bad-env"
    ),
    "execveat_empty_path_bad_argv_failure_non_destructive": (
        "execveat-empty-path-bad-argv"
    ),
    "syscall_execveat_custom_env_injection": "syscall-execveat-custom-env",
    "execveat_failure_non_destructive": "execveat-failure",
    "execveat_bad_env_failure_non_destructive": "execveat-bad-env",
    "execveat_bad_env_preflight_unknown_non_destructive": "execveat-bad-env",
    "execveat_bad_argv_failure_non_destructive": "execveat-bad-argv",
    "syscall_execveat_failure_non_destructive": "syscall-execveat-failure",
    "syscall_execveat_bad_env_failure_non_destructive": "syscall-execveat-bad-env",
    "syscall_execveat_bad_env_preflight_unknown_non_destructive": (
        "syscall-execveat-bad-env"
    ),
    "syscall_execveat_bad_argv_failure_non_destructive": (
        "syscall-execveat-bad-argv"
    ),
    "helper_named_exec_still_checkpointed": "helper-named-exec",
    "configured_helper_named_exec_still_profiled": "configured-helper-named-exec",
    "execl_success_checkpoint": "execl-success",
    "execlp_path_search": "execlp-path-search",
    "execle_custom_env_injection": "execle-custom-env",
    "fork_execl_success_checkpoint": "fork-execl-success",
    "fork_execlp_path_search": "fork-execlp-path-search",
    "fork_execle_custom_env_injection": "fork-execle-custom-env",
    "exec_bad_stats_path_nonfatal": "execv-success",
    "exec_bad_trace_path_nonfatal": "execv-success",
    "exec_trace_fifo_path_nonblocking": "execv-success",
    "exec_trace_locked_path_nonblocking": "execv-success",
    "vfork_exec_trace_fifo_path_nonblocking": "vfork-exec-failure-trace",
    "vfork_exec_trace_locked_path_nonblocking": "vfork-exec-failure-trace",
    "exec_failure_non_destructive": "exec-failure",
    "no_duplicate_ld_preload": "duplicate-preload",
    "no_duplicate_ld_preload_whitespace": "duplicate-preload-whitespace",
    "no_duplicate_ld_preload_env_entries": "duplicate-preload-entry",
    "fork_execve_custom_env_injection": "fork-execve-custom-env",
    "fork_child_work_exec_checkpoint": "fork-child-work-exec",
    "fork_exec_failure_trace": "fork-exec-failure-trace",
    "syscall_clone_execve_custom_env_injection": "syscall-clone-execve-custom-env",
    "syscall_clone_syscall_execve_custom_env_injection": (
        "syscall-clone-syscall-execve-custom-env"
    ),
    "syscall_clone_exec_failure_trace": "syscall-clone-exec-failure-trace",
    "syscall_clone3_execve_custom_env_injection": (
        "syscall-clone3-execve-custom-env"
    ),
    "syscall_clone3_bad_pointer_failure_non_destructive": (
        "syscall-clone3-bad-pointer"
    ),
    "vfork_execve_custom_env_injection": "vfork-execve-custom-env",
    "vfork_exec_failure_trace": "vfork-exec-failure-trace",
    "vfork_execvp_enoexec_fallback": "vfork-execvp-enoexec",
    "vfork_execvp_empty_path_component": "vfork-execvp-empty-path",
    "vfork_large_child_env_injection": "vfork-large-child-env",
    "vfork_native_optout_parent_exec": "vfork-native-optout-parent-exec",
    "fork_large_peak_env_injection": "fork-large-peak-env",
    "fork_large_peak_target_file_env_injection": "fork-large-peak-env",
    "vfork_large_peak_env_injection": "vfork-large-peak-env",
    "vfork_large_peak_target_file_env_injection": "vfork-large-peak-env",
    "vfork_huge_peak_env_injection": "vfork-huge-peak-env",
    "vfork_overflow_peak_env_injection": "vfork-overflow-peak-env",
    "vfork_duplicate_ld_preload": "vfork-duplicate-preload",
    "clone_vfork_execve_custom_env_injection": "clone-vfork-execve-custom-env",
    "clone_vm_execve_custom_env_injection": "clone-vm-execve-custom-env",
    "clone_private_child_work_exec_checkpoint": "clone-private-child-work-exec",
    "clone_vfork_large_peak_env_injection": "clone-vfork-large-peak-env",
    "clone_vfork_large_peak_target_file_env_injection": "clone-vfork-large-peak-env",
    "clone_vfork_huge_peak_env_injection": "clone-vfork-huge-peak-env",
    "clone_vfork_overflow_peak_env_injection": "clone-vfork-overflow-peak-env",
    "clone_private_exec_failure_trace": "clone-private-exec-failure-trace",
    "clone_vm_exec_failure_trace": "clone-vm-exec-failure-trace",
    "clone_vfork_exec_failure_trace": "clone-vfork-exec-failure-trace",
    "clone_vfork_exec_trace_fifo_path_nonblocking": (
        "clone-vfork-exec-failure-trace"
    ),
    "clone_vfork_exec_trace_locked_path_nonblocking": (
        "clone-vfork-exec-failure-trace"
    ),
    "clone_vfork_execvp_enoexec_fallback": "clone-vfork-execvp-enoexec",
    "clone_vfork_execvp_empty_path_component": "clone-vfork-execvp-empty-path",
    "clone_vfork_duplicate_ld_preload": "clone-vfork-duplicate-preload",
    "clone_vfork_parent_exec_after_child": "clone-vfork-then-parent-exec",
    "execvp_path_search": "execvp-path-search",
    "execvp_empty_path_component": "execvp-path-search",
    "execvpe_child_env_path_used_fallback": "execvpe-child-env-path-used",
    "no_duplicate_ld_preload_execvpe_path_fallback": (
        "execvpe-child-path-duplicate-preload"
    ),
    "execvp_path_search_eacces_then_success": "execvp-path-search",
    "execvp_path_search_eacces_then_success_fallback": "execvp-path-search",
    "execvp_path_search_fallback": "execvp-path-search",
    "execvp_enoent_failure": "execvp-enoent",
    "execvp_enotdir_enoent_failure": "execvp-enoent",
    "execvp_enotdir_enoent_fallback": "execvp-enoent",
    "execvp_eacces_failure": "execvp-eacces",
    "execvp_eacces_fallback": "execvp-eacces",
    "execvp_enoent_fallback": "execvp-enoent",
    "execvp_enoexec_fallback": "execvp-enoexec",
    "fork_execvp_enoexec_fallback": "fork-execvp-enoexec",
    "posix_spawn_custom_env_injection": "posix-spawn-custom-env",
    "posix_spawn_null_env_injection": "posix-spawn-null-env",
    "posix_spawn_bad_env_failure_non_destructive": "posix-spawn-bad-env",
    "posix_spawn_bad_env_preflight_unknown_non_destructive": (
        "posix-spawn-bad-env"
    ),
    "posix_spawn_bad_argv_failure_non_destructive": "posix-spawn-bad-argv",
    "posix_spawn_preflight_unavailable_injection": (
        "posix-spawn-preflight-unavailable"
    ),
    "posix_spawn_duplicate_ld_preload": "posix-spawn-duplicate-preload",
    "posix_spawn_actions_attrs_injection": "posix-spawn-actions-attrs",
    "posix_spawn_actions_close_stderr": "posix-spawn-actions-close-stderr",
    "posix_spawn_usevfork_custom_env": "posix-spawn-usevfork-custom-env",
    "posix_spawn_child_peak_env_only_injection": "posix-spawn-child-peak-env-only",
    "posix_spawn_explicit_peak_env_preserved": "posix-spawn-explicit-peak-env",
    "posix_spawn_chain_disabled": "posix-spawn-custom-env",
    "posix_spawn_failure_non_destructive": "posix-spawn-failure",
    "posix_spawnp_path_search": "posix-spawnp-path-search",
    "posix_spawnp_child_env_path_ignored": "posix-spawnp-child-env-path-ignored",
    "posix_spawnp_empty_path_component": "posix-spawnp-path-search",
    "posix_spawnp_bad_env_failure_non_destructive": "posix-spawnp-bad-env",
    "posix_spawnp_bad_env_preflight_unknown_non_destructive": (
        "posix-spawnp-bad-env"
    ),
    "posix_spawnp_bad_argv_failure_non_destructive": "posix-spawnp-bad-argv",
    "text_output_not_corrupted": "exec-failure",
}

SPAWN_MODES = {
    "posix_spawn_custom_env_injection",
    "posix_spawn_null_env_injection",
    "posix_spawn_duplicate_ld_preload",
    "posix_spawn_actions_attrs_injection",
    "posix_spawn_actions_close_stderr",
    "posix_spawn_usevfork_custom_env",
    "posix_spawn_child_peak_env_only_injection",
    "posix_spawn_preflight_unavailable_injection",
    "posix_spawn_explicit_peak_env_preserved",
    "posix_spawn_chain_disabled",
    "posix_spawnp_path_search",
    "posix_spawnp_child_env_path_ignored",
    "posix_spawnp_empty_path_component",
}

SPAWN_CHILD_PROFILE_MODES = {
    "posix_spawn_custom_env_injection",
    "posix_spawn_null_env_injection",
    "posix_spawn_duplicate_ld_preload",
    "posix_spawn_actions_attrs_injection",
    "posix_spawn_actions_close_stderr",
    "posix_spawn_usevfork_custom_env",
    "posix_spawn_child_peak_env_only_injection",
    "posix_spawn_preflight_unavailable_injection",
    "posix_spawnp_path_search",
    "posix_spawnp_child_env_path_ignored",
    "posix_spawnp_empty_path_component",
}

FORK_EXEC_MODES = {
    "fork_execve_custom_env_injection",
    "fork_execl_success_checkpoint",
    "fork_execlp_path_search",
    "fork_execle_custom_env_injection",
    "fork_child_work_exec_checkpoint",
    "syscall_clone_execve_custom_env_injection",
    "syscall_clone_syscall_execve_custom_env_injection",
    "syscall_clone3_execve_custom_env_injection",
    "clone_private_child_work_exec_checkpoint",
    "vfork_execve_custom_env_injection",
    "vfork_large_child_env_injection",
    "clone_vfork_execve_custom_env_injection",
    "fork_large_peak_env_injection",
    "fork_large_peak_target_file_env_injection",
    "vfork_large_peak_env_injection",
    "vfork_large_peak_target_file_env_injection",
    "vfork_huge_peak_env_injection",
    "vfork_duplicate_ld_preload",
    "vfork_execvp_empty_path_component",
    "clone_vm_execve_custom_env_injection",
    "clone_vfork_large_peak_env_injection",
    "clone_vfork_large_peak_target_file_env_injection",
    "clone_vfork_huge_peak_env_injection",
    "clone_vfork_duplicate_ld_preload",
    "clone_vfork_execvp_empty_path_component",
}

SHARED_VM_EXEC_MODES = {
    "vfork_execve_custom_env_injection",
    "vfork_large_child_env_injection",
    "vfork_large_peak_env_injection",
    "vfork_large_peak_target_file_env_injection",
    "vfork_huge_peak_env_injection",
    "vfork_overflow_peak_env_injection",
    "vfork_duplicate_ld_preload",
    "vfork_execvp_empty_path_component",
    "clone_vm_execve_custom_env_injection",
    "clone_vfork_execve_custom_env_injection",
    "clone_vfork_large_peak_env_injection",
    "clone_vfork_large_peak_target_file_env_injection",
    "clone_vfork_huge_peak_env_injection",
    "clone_vfork_overflow_peak_env_injection",
    "clone_vfork_duplicate_ld_preload",
    "clone_vfork_execvp_empty_path_component",
}

CHAIN_DISABLED_MODES = {
    "execve_chain_disabled",
    "posix_spawn_chain_disabled",
}

OPTIONAL_EXEC_ENV_TRACE_MODES = {
    "exec_checkpoint_write_target_no_reentry",
}

NO_CHILD_PROFILE_MODES = {
    "execve_chain_disabled",
    "execve_propagate_disabled",
    "execve_secure_skip",
    "execve_explicit_peak_env_preserved",
    "helper_named_exec_still_checkpointed",
}

CHILD_PROFILE_EXEC_MODES = {
    "execv_success_checkpoint",
    "execv_zero_call_checkpoint",
    "exec_checkpoint_disabled",
    "execve_custom_env_injection",
    "execve_preflight_unavailable_injection",
    "execve_child_peak_env_only_injection",
    "syscall_execve_custom_env_injection",
    "syscall_execve_preflight_unavailable_injection",
    "execve_null_env_injection",
    "fexecve_custom_env_injection",
    "fexecve_preflight_unavailable_injection",
    "execveat_custom_env_injection",
    "execveat_empty_path_custom_env",
    "syscall_execveat_custom_env_injection",
    "configured_helper_named_exec_still_profiled",
    "execl_success_checkpoint",
    "execlp_path_search",
    "execle_custom_env_injection",
    "execvp_path_search",
    "execvp_empty_path_component",
    "execvpe_child_env_path_used_fallback",
    "execvp_path_search_eacces_then_success",
    "execvp_path_search_eacces_then_success_fallback",
    "execvp_path_search_fallback",
    "exec_bad_trace_path_nonfatal",
    "exec_trace_fifo_path_nonblocking",
    "exec_trace_locked_path_nonblocking",
    "no_duplicate_ld_preload",
    "no_duplicate_ld_preload_whitespace",
    "no_duplicate_ld_preload_env_entries",
    "clone_vfork_parent_exec_after_child",
    "vfork_native_optout_parent_exec",
}

FAILED_EXEC_MODES = {
    "exec_failure_non_destructive",
    "syscall_execve_failure_non_destructive",
    "fexecve_failure_non_destructive",
    "execveat_failure_non_destructive",
    "syscall_execveat_failure_non_destructive",
    "fork_exec_failure_trace",
    "syscall_clone_exec_failure_trace",
    "vfork_exec_failure_trace",
    "vfork_exec_trace_fifo_path_nonblocking",
    "vfork_exec_trace_locked_path_nonblocking",
    "clone_vm_exec_failure_trace",
    "clone_private_exec_failure_trace",
    "clone_vfork_exec_failure_trace",
    "clone_vfork_exec_trace_fifo_path_nonblocking",
    "clone_vfork_exec_trace_locked_path_nonblocking",
    "execvp_enoent_failure",
    "execvp_enotdir_enoent_failure",
    "execvp_enotdir_enoent_fallback",
    "execvp_eacces_failure",
    "execvp_eacces_fallback",
    "execvp_enoent_fallback",
    "text_output_not_corrupted",
}

FAILED_EXEC_ENOENT_MODES = {
    "exec_failure_non_destructive",
    "syscall_execve_failure_non_destructive",
    "execveat_failure_non_destructive",
    "syscall_execveat_failure_non_destructive",
    "fork_exec_failure_trace",
    "syscall_clone_exec_failure_trace",
    "vfork_exec_failure_trace",
    "vfork_exec_trace_fifo_path_nonblocking",
    "vfork_exec_trace_locked_path_nonblocking",
    "clone_vm_exec_failure_trace",
    "clone_private_exec_failure_trace",
    "clone_vfork_exec_failure_trace",
    "clone_vfork_exec_trace_fifo_path_nonblocking",
    "clone_vfork_exec_trace_locked_path_nonblocking",
    "execvp_enoent_failure",
    "execvp_enotdir_enoent_failure",
    "execvp_enotdir_enoent_fallback",
    "execvp_enoent_fallback",
    "text_output_not_corrupted",
}

SPAWN_FAILURE_MODES = {
    "posix_spawn_failure_non_destructive",
    "posix_spawnp_bad_env_failure_non_destructive",
    "posix_spawnp_bad_env_preflight_unknown_non_destructive",
}

NO_CHECKPOINT_MODES = {
    "exec_checkpoint_disabled",
    "execve_child_peak_env_only_injection",
    "syscall_execve_bad_env_failure_non_destructive",
    "syscall_execve_bad_env_preflight_unknown_non_destructive",
    "syscall_execve_bad_argv_failure_non_destructive",
    "execve_bad_env_failure_non_destructive",
    "execve_bad_env_preflight_unknown_non_destructive",
    "execve_bad_argv_failure_non_destructive",
    "fexecve_bad_env_failure_non_destructive",
    "fexecve_bad_env_preflight_unknown_non_destructive",
    "fexecve_bad_argv_failure_non_destructive",
    "execveat_bad_env_failure_non_destructive",
    "execveat_bad_env_preflight_unknown_non_destructive",
    "execveat_bad_argv_failure_non_destructive",
    "execveat_empty_path_bad_env_failure_non_destructive",
    "execveat_empty_path_bad_env_preflight_unknown_non_destructive",
    "execveat_empty_path_bad_argv_failure_non_destructive",
    "syscall_execveat_bad_env_failure_non_destructive",
    "syscall_execveat_bad_env_preflight_unknown_non_destructive",
    "syscall_execveat_bad_argv_failure_non_destructive",
    "syscall_clone3_bad_pointer_failure_non_destructive",
    "vfork_execvp_enoexec_fallback",
    "clone_vfork_execvp_enoexec_fallback",
    *SHARED_VM_EXEC_MODES,
    "vfork_exec_failure_trace",
    "vfork_exec_trace_fifo_path_nonblocking",
    "vfork_exec_trace_locked_path_nonblocking",
    "clone_vfork_exec_trace_fifo_path_nonblocking",
    "clone_vfork_exec_trace_locked_path_nonblocking",
    "clone_vm_exec_failure_trace",
    "clone_vfork_exec_failure_trace",
    *SPAWN_MODES,
    *SPAWN_FAILURE_MODES,
    "posix_spawn_bad_env_failure_non_destructive",
    "posix_spawn_bad_env_preflight_unknown_non_destructive",
    "posix_spawn_bad_argv_failure_non_destructive",
    "posix_spawnp_bad_env_failure_non_destructive",
    "posix_spawnp_bad_env_preflight_unknown_non_destructive",
    "posix_spawnp_bad_argv_failure_non_destructive",
}

BAD_STATS_MODES = {
    "exec_bad_stats_path_nonfatal",
}

NO_TRACE_MODES = {
    "exec_bad_trace_path_nonfatal",
    "exec_trace_fifo_path_nonblocking",
    "exec_trace_locked_path_nonblocking",
    "vfork_exec_trace_fifo_path_nonblocking",
    "vfork_exec_trace_locked_path_nonblocking",
    "clone_vfork_exec_trace_fifo_path_nonblocking",
    "clone_vfork_exec_trace_locked_path_nonblocking",
    "execve_child_peak_env_only_injection",
    "posix_spawn_child_peak_env_only_injection",
    "syscall_clone3_bad_pointer_failure_non_destructive",
}

LOCKED_TRACE_MODES = {
    "exec_trace_locked_path_nonblocking",
    "vfork_exec_trace_locked_path_nonblocking",
    "clone_vfork_exec_trace_locked_path_nonblocking",
}

PREFLIGHT_EXEC_FAILURE_MODES = {
    "syscall_execve_bad_env_failure_non_destructive",
    "syscall_execve_bad_env_preflight_unknown_non_destructive",
    "syscall_execve_bad_argv_failure_non_destructive",
    "execve_bad_env_failure_non_destructive",
    "execve_bad_env_preflight_unknown_non_destructive",
    "execve_bad_argv_failure_non_destructive",
    "fexecve_bad_env_failure_non_destructive",
    "fexecve_bad_env_preflight_unknown_non_destructive",
    "fexecve_bad_argv_failure_non_destructive",
    "execveat_bad_env_failure_non_destructive",
    "execveat_bad_env_preflight_unknown_non_destructive",
    "execveat_bad_argv_failure_non_destructive",
    "execveat_empty_path_bad_env_failure_non_destructive",
    "execveat_empty_path_bad_env_preflight_unknown_non_destructive",
    "execveat_empty_path_bad_argv_failure_non_destructive",
    "syscall_execveat_bad_env_failure_non_destructive",
    "syscall_execveat_bad_env_preflight_unknown_non_destructive",
    "syscall_execveat_bad_argv_failure_non_destructive",
    "posix_spawn_bad_env_failure_non_destructive",
    "posix_spawn_bad_env_preflight_unknown_non_destructive",
    "posix_spawn_bad_argv_failure_non_destructive",
    "posix_spawnp_bad_env_failure_non_destructive",
    "posix_spawnp_bad_env_preflight_unknown_non_destructive",
    "posix_spawnp_bad_argv_failure_non_destructive",
}

PREFLIGHT_UNAVAILABLE_MODES = {
    "execve_preflight_unavailable_injection",
    "syscall_execve_preflight_unavailable_injection",
    "fexecve_preflight_unavailable_injection",
    "posix_spawn_preflight_unavailable_injection",
}

PREFLIGHT_UNKNOWN_BAD_ENV_MODES = {
    "syscall_execve_bad_env_preflight_unknown_non_destructive",
    "execve_bad_env_preflight_unknown_non_destructive",
    "fexecve_bad_env_preflight_unknown_non_destructive",
    "execveat_bad_env_preflight_unknown_non_destructive",
    "execveat_empty_path_bad_env_preflight_unknown_non_destructive",
    "syscall_execveat_bad_env_preflight_unknown_non_destructive",
    "posix_spawn_bad_env_preflight_unknown_non_destructive",
    "posix_spawnp_bad_env_preflight_unknown_non_destructive",
}

FALLBACK_EXECVP_MODES = {
    "execvp_path_search_fallback",
    "execvpe_child_env_path_used_fallback",
    "no_duplicate_ld_preload_execvpe_path_fallback",
    "execvp_path_search_eacces_then_success_fallback",
    "execvp_enoent_fallback",
    "execvp_enotdir_enoent_fallback",
    "execvp_eacces_fallback",
    "execvp_enoexec_fallback",
    "fork_execvp_enoexec_fallback",
}


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", required=True, choices=sorted(MODE_TO_APP_ARG))
    parser.add_argument("--exe", required=True)
    parser.add_argument("--libpeak", required=True)
    return parser.parse_args()


def merge_preload(libpeak, old):
    return libpeak if not old else f"{libpeak}:{old}"


def terminate_process_group(proc, sig):
    try:
        os.killpg(proc.pid, sig)
    except ProcessLookupError:
        pass


def process_group_exists(pgid):
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def wait_process_group_exit(pgid, timeout):
    deadline = time.monotonic() + timeout
    while True:
        if not process_group_exists(pgid):
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.01)


def run_fixture(args, tmpdir):
    env = os.environ.copy()
    env["LD_PRELOAD"] = merge_preload(args.libpeak, env.get("LD_PRELOAD"))
    env["PEAK_TARGET"] = TARGET
    env["PEAK_HEARTBEAT_INTERVAL"] = "0"
    env["PEAK_TEXT_OUTPUT"] = "1" if args.mode == "text_output_not_corrupted" else "0"
    env["PEAK_STATSLOG_PATH"] = str(tmpdir / "peak-stats")
    env["PEAK_EXEC_TRACE_PATH"] = str(tmpdir / "exec-trace.csv")
    env["PEAK_VERBOSITY"] = "warn"
    env["PATH"] = (
        f"{tmpdir}{os.pathsep}{Path(args.exe).parent}"
        f"{os.pathsep}{env.get('PATH', '')}"
    )
    if args.mode == "exec_checkpoint_write_target_no_reentry":
        env["PEAK_TARGET"] = "write"
        env["PEAK_ENABLE_PER_TARGET_HEARTBEAT"] = "1"
        env["PEAK_COST"] = "0.000000000001"
        env["PEAK_OVERHEAD_RATIO"] = "0.000001"
        env["PEAK_HEARTBEAT_INTERVAL"] = "0.005"
        env["PEAK_HIBERNATION_CYCLE"] = "1"
    if args.mode == "execve_child_peak_env_only_injection":
        env["EXEC_CHAIN_CHILD_STATS_PREFIX"] = str(tmpdir / "peak-stats")
        for name in list(env):
            if name.startswith("PEAK_") and name not in {
                "PEAK_TARGET",
                "PEAK_HEARTBEAT_INTERVAL",
            }:
                env.pop(name, None)
    if args.mode == "posix_spawn_child_peak_env_only_injection":
        env["EXEC_CHAIN_CHILD_STATS_PREFIX"] = str(tmpdir / "peak-stats")
    if args.mode in {"posix_spawn_actions_attrs_injection",
                     "posix_spawn_actions_close_stderr"}:
        env["EXEC_CHAIN_SPAWN_ACTIONS_OUT"] = str(tmpdir / "spawn-actions.out")
    if args.mode in CHAIN_DISABLED_MODES:
        env["PEAK_EXEC_CHAIN"] = "0"
    if args.mode == "exec_checkpoint_disabled":
        env["PEAK_EXEC_CHECKPOINT"] = "0"
    if args.mode == "execve_propagate_disabled":
        env["PEAK_EXEC_PROPAGATE_PEAK_ENV"] = "0"
    if args.mode == "execve_secure_skip":
        env["PEAK_TEST_EXEC_AT_SECURE"] = "1"
    if args.mode in FALLBACK_EXECVP_MODES:
        env["PEAK_TEST_EXECVPE_FALLBACK"] = "1"
    if args.mode in PREFLIGHT_UNAVAILABLE_MODES | PREFLIGHT_UNKNOWN_BAD_ENV_MODES:
        env["PEAK_TEST_EXEC_PREFLIGHT_UNAVAILABLE"] = "1"
        env["PEAK_TEST_EXEC_PREFLIGHT_NO_PROC_MEM"] = "1"
    if args.mode in BAD_STATS_MODES:
        env["PEAK_STATSLOG_PATH"] = str(tmpdir / "missing" / "peak-stats")
    if (args.mode in NO_TRACE_MODES and
            args.mode not in {"exec_trace_fifo_path_nonblocking",
                              "vfork_exec_trace_fifo_path_nonblocking"} and
            args.mode not in LOCKED_TRACE_MODES):
        env["PEAK_EXEC_TRACE_PATH"] = str(tmpdir / "missing" / "exec-trace.csv")
    if args.mode in {"exec_trace_fifo_path_nonblocking",
                     "vfork_exec_trace_fifo_path_nonblocking",
                     "clone_vfork_exec_trace_fifo_path_nonblocking"}:
        fifo_path = tmpdir / "exec-trace.csv"
        os.mkfifo(fifo_path)
        env["PEAK_EXEC_TRACE_PATH"] = str(fifo_path)
    if args.mode in {
        "fork_large_peak_target_file_env_injection",
        "vfork_large_peak_target_file_env_injection",
        "clone_vfork_large_peak_target_file_env_injection",
    }:
        target_file = tmpdir / "peak-targets.txt"
        target_file.write_text(f"{TARGET}\n", encoding="utf-8")
        env.pop("PEAK_TARGET", None)
        env["PEAK_TARGET_FILE"] = str(target_file)

    if args.mode in {
        "execvp_enoexec_fallback",
        "fork_execvp_enoexec_fallback",
        "vfork_execvp_enoexec_fallback",
        "clone_vfork_execvp_enoexec_fallback",
    }:
        script = tmpdir / "peak-enoexec-script"
        script.write_text(
            '[ "$1" = "alpha" ] || exit 38\n'
            '[ "$2" = "beta" ] || exit 39\n'
            "exit 37\n",
            encoding="utf-8",
        )
        script.chmod(0o755)
    if args.mode in {
        "execvp_empty_path_component",
        "vfork_execvp_empty_path_component",
        "clone_vfork_execvp_empty_path_component",
        "posix_spawnp_empty_path_component",
    }:
        (tmpdir / "test_exec_chain").symlink_to(args.exe)
        env["PATH"] = f"{os.pathsep}{tmpdir / 'missing-after-empty'}"
    if args.mode == "execvpe_child_env_path_used_fallback":
        env["PATH"] = "/definitely/not/a/peak/parent/path"
        env["EXEC_CHAIN_TEST_CHILD_PATH"] = str(Path(args.exe).parent)
    if args.mode == "no_duplicate_ld_preload_execvpe_path_fallback":
        env["PATH"] = "/definitely/not/a/peak/parent/path"
        env["EXEC_CHAIN_TEST_CHILD_PATH"] = str(Path(args.exe).parent)
        env["PEAK_EXEC_PROPAGATE_PEAK_ENV"] = "0"
    if args.mode in {
        "execvp_path_search_eacces_then_success",
        "execvp_path_search_eacces_then_success_fallback",
    }:
        blocked = tmpdir / "blocked-path"
        blocked.mkdir()
        blocked_child = blocked / "test_exec_chain"
        blocked_child.write_text("#!/bin/sh\nexit 99\n", encoding="utf-8")
        blocked_child.chmod(0o644)
        env["PATH"] = f"{blocked}{os.pathsep}{Path(args.exe).parent}"
    if args.mode in {"execvp_eacces_failure", "execvp_eacces_fallback"}:
        blocked = tmpdir / "blocked-only"
        blocked.mkdir()
        blocked_child = blocked / "peak-noexec-child"
        blocked_child.write_text("#!/bin/sh\nexit 99\n", encoding="utf-8")
        blocked_child.chmod(0o644)
        env["PATH"] = str(blocked)
    if args.mode in {
        "execvp_enotdir_enoent_failure",
        "execvp_enotdir_enoent_fallback",
    }:
        not_dir = tmpdir / "not-a-dir"
        not_dir.write_text("not a directory\n", encoding="utf-8")
        env["PATH"] = f"{not_dir}{os.pathsep}{tmpdir / 'missing-dir'}"
    if args.mode in {
        "helper_named_exec_still_checkpointed",
        "configured_helper_named_exec_still_profiled",
    }:
        helper_name = (
            "custom_detach_helper"
            if args.mode == "configured_helper_named_exec_still_profiled"
            else "peak_detach_helper"
        )
        helper = tmpdir / helper_name
        helper.symlink_to(args.exe)
        if args.mode == "configured_helper_named_exec_still_profiled":
            env["PEAK_DETACH_HELPER"] = str(helper)

    fixture_timeout = 45.0 if args.mode in {
        "vfork_huge_peak_env_injection",
        "vfork_overflow_peak_env_injection",
        "clone_vfork_huge_peak_env_injection",
        "clone_vfork_overflow_peak_env_injection",
    } else 15.0

    locked_trace = None
    if args.mode in LOCKED_TRACE_MODES:
        locked_trace_path = tmpdir / "exec-trace.csv"
        locked_trace = locked_trace_path.open("a+", encoding="utf-8")
        fcntl.flock(locked_trace.fileno(), fcntl.LOCK_EX)
        env["PEAK_EXEC_TRACE_PATH"] = str(locked_trace_path)

    try:
        command = [args.exe, MODE_TO_APP_ARG[args.mode]]
        output_path = tmpdir / "fixture-output.log"
        with output_path.open("w", encoding="utf-8") as output_handle:
            proc = subprocess.Popen(
                command,
                cwd=tmpdir,
                env=env,
                stdout=output_handle,
                stderr=subprocess.STDOUT,
                text=True,
                start_new_session=True,
            )
            try:
                proc.wait(timeout=fixture_timeout)
                result = subprocess.CompletedProcess(
                    command,
                    proc.returncode,
                    stdout="",
                    stderr=None,
                )
                result.timed_out = False
                result.process_group_leaked = False
                result.timeout = fixture_timeout
                if not wait_process_group_exit(proc.pid, 0.25):
                    terminate_process_group(proc, signal.SIGTERM)
                    if not wait_process_group_exit(proc.pid, 2.0):
                        terminate_process_group(proc, signal.SIGKILL)
                        wait_process_group_exit(proc.pid, 1.0)
                    result.process_group_leaked = True
            except subprocess.TimeoutExpired:
                terminate_process_group(proc, signal.SIGTERM)
                try:
                    proc.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    terminate_process_group(proc, signal.SIGKILL)
                    proc.wait()
                if process_group_exists(proc.pid):
                    terminate_process_group(proc, signal.SIGKILL)
                    wait_process_group_exit(proc.pid, 1.0)
                result = subprocess.CompletedProcess(
                    command,
                    proc.returncode,
                    stdout="",
                    stderr=None,
                )
                result.timed_out = True
                result.process_group_leaked = False
                result.timeout = fixture_timeout
        result.stdout = output_path.read_text(encoding="utf-8",
                                              errors="replace")
        return result
    finally:
        if locked_trace is not None:
            fcntl.flock(locked_trace.fileno(), fcntl.LOCK_UN)
            locked_trace.close()
    return proc


def read_stats(path):
    with path.open(newline="", encoding="utf-8", errors="replace") as handle:
        return list(csv.DictReader(handle))


def target_count(paths, function_name=TARGET):
    total = 0
    for path in paths:
        for row in read_stats(path):
            if row.get("function") == function_name:
                total += int(float(row.get("count", "0") or 0))
    return total


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def require_trace(tmpdir, mode, expect_failed, expected_exec_errno=None):
    if mode in NO_TRACE_MODES:
        return

    trace = tmpdir / "exec-trace.csv"
    require(trace.exists(), "exec trace was not written")
    with trace.open(newline="", encoding="utf-8", errors="replace") as handle:
        rows = list(csv.DictReader(handle))
    events = [row.get("event") for row in rows]
    require("exec-before" in events, "trace missing exec-before")
    if mode in PREFLIGHT_EXEC_FAILURE_MODES:
        require("exec-failed" in events, "trace missing exec-failed")
        failed_rows = [row for row in rows if row.get("event") == "exec-failed"]
        require(any(row.get("errno") == "14" for row in failed_rows),
                "preflight exec-failed row did not preserve EFAULT")
        return
    if mode not in OPTIONAL_EXEC_ENV_TRACE_MODES:
        require(
            "exec-env-injected" in events or "exec-env-unchanged" in events,
            "trace missing exec env event",
        )
    if mode in BAD_STATS_MODES:
        require("exec-checkpoint-failed" in events,
                "trace missing checkpoint failure")
    elif mode not in NO_CHECKPOINT_MODES:
        require("exec-checkpoint-ok" in events, "trace missing checkpoint ok")
    if expect_failed:
        require("exec-failed" in events, "trace missing exec-failed")
        failed_rows = [row for row in rows if row.get("event") == "exec-failed"]
        if expected_exec_errno is not None:
            require(any(row.get("errno") == str(expected_exec_errno)
                        for row in failed_rows),
                    f"exec-failed row did not preserve errno {expected_exec_errno}")
    if mode in CHAIN_DISABLED_MODES:
        env_rows = [
            row for row in rows
            if row.get("event") in {"exec-env-injected", "exec-env-unchanged"}
        ]
        require(env_rows, "missing chain-disabled env row")
        require(env_rows[-1].get("ld_preload_action") == "disabled",
                "chain-disabled run modified LD_PRELOAD")
    if mode == "execve_secure_skip":
        env_rows = [
            row for row in rows
            if row.get("event") in {"exec-env-injected", "exec-env-unchanged"}
        ]
        require(any(row.get("ld_preload_action") == "secure-skip"
                    for row in env_rows),
                "secure-exec test did not suppress LD_PRELOAD injection")
    if mode in SPAWN_MODES:
        require("posix-spawn-ok" in events, "trace missing posix-spawn-ok")
    if mode in SPAWN_FAILURE_MODES:
        require("posix-spawn-failed" in events,
                "trace missing posix-spawn-failed")
    if mode in {
        "vfork_overflow_peak_env_injection",
        "clone_vfork_overflow_peak_env_injection",
    }:
        env_rows = [
            row for row in rows
            if row.get("event") in {"exec-env-injected", "exec-env-unchanged"}
        ]
        require(any(row.get("ld_preload_action") == "partial-peak-env"
                    for row in env_rows),
                "overflow run did not trace partial PEAK env propagation")


def dump_failure_artifacts(tmpdir):
    print(f"exec_chain_tmpdir={tmpdir}")
    try:
        entries = sorted(
            path.relative_to(tmpdir).as_posix()
            for path in tmpdir.rglob("*")
            if path.is_file() or path.is_symlink()
        )
        print("exec_chain_tmpdir_files_begin")
        for entry in entries[:200]:
            print(entry)
        if len(entries) > 200:
            print(f"... {len(entries) - 200} more files")
        print("exec_chain_tmpdir_files_end")
    except Exception as exc:
        print(f"exec_chain_tmpdir_files_error={exc}")

    trace = tmpdir / "exec-trace.csv"
    if trace.exists():
        try:
            print("exec_chain_trace_begin")
            print(trace.read_text(encoding="utf-8", errors="replace"), end="")
            print("exec_chain_trace_end")
        except Exception as exc:
            print(f"exec_chain_trace_error={exc}")
    else:
        print("exec_chain_trace_missing")

    try:
        stats_files = sorted(tmpdir.glob("peak-stats*.csv"))
        print("exec_chain_stats_files_begin")
        for path in stats_files:
            try:
                size = path.stat().st_size
            except OSError:
                size = -1
            print(f"{path.name} size={size}")
        print("exec_chain_stats_files_end")
    except Exception as exc:
        print(f"exec_chain_stats_files_error={exc}")


def check_mode(args, proc, tmpdir):
    output = proc.stdout
    if getattr(proc, "timed_out", False):
        raise AssertionError(
            f"fixture timed out after {proc.timeout:g}s\n{output}"
        )
    if getattr(proc, "process_group_leaked", False):
        raise AssertionError(
            f"fixture process group remained alive after leader exit\n{output}"
        )

    if args.mode == "execvp_enoexec_fallback":
        require(proc.returncode == 37,
                f"ENOEXEC shell fallback returned {proc.returncode}\n{output}")
    else:
        require(proc.returncode == 0,
                f"fixture returned {proc.returncode}\n{output}")

    if (args.mode == "syscall_clone3_execve_custom_env_injection" and
            "clone3_unavailable" in output):
        return

    stats_files = sorted(tmpdir.glob("peak-stats-p*.csv"))
    exec_files = [path for path in stats_files if "-exec" in path.name]
    final_files = [path for path in stats_files if "-exec" not in path.name]

    if args.mode in PREFLIGHT_UNAVAILABLE_MODES:
        if args.mode == "posix_spawn_preflight_unavailable_injection":
            require("posix_spawn_preflight_unavailable_ok" in output,
                    f"missing preflight-unavailable marker\n{output}")
        else:
            require("exec_child_ok" in output,
                    f"preflight-unavailable exec did not reach child\n{output}")

    if args.mode in LOCKED_TRACE_MODES:
        locked_trace_path = tmpdir / "exec-trace.csv"
        locked_trace_text = locked_trace_path.read_text(
            encoding="utf-8", errors="replace"
        )
        require(locked_trace_text == "",
                f"locked trace path was written despite LOCK_NB:\n"
                f"{locked_trace_text}")

    if args.mode in BAD_STATS_MODES:
        require("exec_child_ok" in output, f"missing child marker\n{output}")
        require(not stats_files,
                f"bad stats path should not write CSV files: {stats_files}")
        require_trace(tmpdir, args.mode, expect_failed=False)
        return

    if args.mode == "exec_checkpoint_write_target_no_reentry":
        require("exec_child_ok" in output, f"missing child marker\n{output}")
        require(exec_files, "missing exec checkpoint while profiling write")
        require(target_count(exec_files, "write") > 0,
                "exec checkpoint did not contain parent write calls")
        require(final_files, "missing child final CSV while profiling write")
        require(target_count(final_files, "write") > 0,
                "child final CSV did not profile write")
        require_trace(tmpdir, args.mode, expect_failed=False)
        return

    if args.mode in CHILD_PROFILE_EXEC_MODES | NO_CHILD_PROFILE_MODES:
        require("exec_child_ok" in output or "ld_preload_libpeak_count=" in output,
                f"missing child marker\n{output}")

    if args.mode in SPAWN_MODES | FORK_EXEC_MODES:
        require("exec_child_ok" in output or
                "ld_preload_libpeak_count=" in output or
                "posix_spawn_actions_attrs_ok" in output or
                "posix_spawn_actions_close_stderr_ok" in output or
                "child_env_PEAK_MEMLOG_PATH=" in output,
                f"missing spawned child marker\n{output}")

    if args.mode in SPAWN_MODES | SHARED_VM_EXEC_MODES:
        require(not exec_files,
                f"{args.mode} should not create exec checkpoints: {exec_files}")

    if args.mode in NO_CHECKPOINT_MODES:
        require(not exec_files,
                f"{args.mode} should not write exec checkpoint CSVs: {exec_files}")
    else:
        require(exec_files, "missing parent exec checkpoint CSV")
        if args.mode == "execv_zero_call_checkpoint":
            require(target_count(exec_files) == 0,
                    "zero-call checkpoint unexpectedly has target calls")
        else:
            require(target_count(exec_files) > 0,
                    "checkpoint CSV has no target calls")

    if args.mode in CHILD_PROFILE_EXEC_MODES:
        require(final_files, "missing child final CSV")
        require(target_count(final_files) > 0, "child final CSV has no target calls")

    if args.mode in {
        "posix_spawn_custom_env_injection",
        "posix_spawn_null_env_injection",
        "posix_spawn_duplicate_ld_preload",
        "posix_spawn_actions_attrs_injection",
        "posix_spawn_actions_close_stderr",
        "posix_spawn_usevfork_custom_env",
        "posix_spawn_child_peak_env_only_injection",
        "posix_spawn_preflight_unavailable_injection",
        "posix_spawnp_path_search",
        "posix_spawnp_child_env_path_ignored",
        "posix_spawnp_empty_path_component",
        *FORK_EXEC_MODES,
    }:
        require(len(final_files) >= 2,
                f"expected parent and spawned child final CSVs, got {final_files}")
        if args.mode in {
            "fork_child_work_exec_checkpoint",
            "clone_private_child_work_exec_checkpoint",
        }:
            expected_total = 9
        elif args.mode in {
            "syscall_clone_execve_custom_env_injection",
            "syscall_clone_syscall_execve_custom_env_injection",
            "syscall_clone3_execve_custom_env_injection",
        }:
            expected_total = 9
        else:
            expected_total = 12
        require(target_count(final_files) >= expected_total,
                "spawn parent+child final CSVs missed expected target calls")

    if args.mode in NO_CHILD_PROFILE_MODES:
        require(not final_files,
                f"child should not be profiled in {args.mode}: {final_files}")

    if args.mode in SPAWN_CHILD_PROFILE_MODES:
        require(final_files,
                f"expected spawned child final CSVs, got {final_files}")
        require(target_count(final_files) >= 5,
                "spawn child-profile mode missed expected target calls")

    if args.mode == "posix_spawn_chain_disabled":
        require(len(final_files) == 1,
                f"chain disabled spawn should only profile the parent: {final_files}")
        require(target_count(final_files) >= 5,
                "chain disabled spawn parent final CSV missed target calls")

    if args.mode == "posix_spawn_explicit_peak_env_preserved":
        require(len(final_files) == 1,
                f"explicit child PEAK env should only profile the parent: {final_files}")
        require(target_count(final_files) >= 5,
                "explicit child PEAK env parent final CSV missed target calls")

    if args.mode == "exec_failure_non_destructive":
        require("exec_failure_errno=2" in output, f"missing failure marker\n{output}")
        require(final_files, "missing normal final CSV after failed exec")
        require(target_count(final_files) >= 8,
                "normal final CSV missed calls after failed exec")

    if args.mode == "fork_child_work_exec_checkpoint":
        require(exec_files, "missing fork-child pre-exec checkpoint CSV")
        require(target_count(exec_files) >= 8,
                "fork-child checkpoint missed pre-exec child calls")

    if args.mode == "clone_private_child_work_exec_checkpoint":
        require(exec_files, "missing private-clone pre-exec checkpoint CSV")
        require(target_count(exec_files) >= 6,
                "private-clone checkpoint missed pre-exec child calls")

    if args.mode == "fork_exec_failure_trace":
        require("fork_exec_failure_errno=2" in output,
                f"missing fork exec failure marker\n{output}")
        require(exec_files, "missing fork-child failed-exec checkpoint CSV")
        require(target_count(exec_files) >= 5,
                "fork-child failed-exec checkpoint missed target calls")

    if args.mode == "syscall_clone_exec_failure_trace":
        require("syscall_clone_exec_failure_errno=2" in output,
                f"missing syscall-clone exec failure marker\n{output}")
        require(exec_files, "missing syscall-clone failed-exec checkpoint CSV")
        require(target_count(exec_files) >= 5,
                "syscall-clone failed-exec checkpoint missed target calls")

    if args.mode == "clone_private_exec_failure_trace":
        require("clone_private_exec_failure_errno=2" in output,
                f"missing private-clone exec failure marker\n{output}")
        require(exec_files, "missing private-clone failed-exec checkpoint CSV")
        require(target_count(exec_files) >= 7,
                "private-clone failed-exec checkpoint missed target calls")
        require(final_files,
                "missing private-clone final CSV after failed exec")
        require(target_count(final_files) >= 4,
                "private-clone final CSV missed calls after failed exec")

    if args.mode in {"vfork_exec_failure_trace",
                     "vfork_exec_trace_fifo_path_nonblocking",
                     "vfork_exec_trace_locked_path_nonblocking"}:
        require("vfork_exec_failure_errno=2" in output,
                f"missing vfork exec failure marker\n{output}")
        require(final_files, "missing parent final CSV after vfork failed exec")
        require(target_count(final_files) >= 2,
                "parent final CSV missed calls after vfork failed exec")

    if args.mode == "clone_vm_exec_failure_trace":
        require("clone_vm_exec_failure_errno=2" in output,
                f"missing clone-vm exec failure marker\n{output}")
        require(final_files, "missing parent final CSV after clone-vm failed exec")
        require(target_count(final_files) >= 8,
                "parent final CSV missed calls after clone-vm failed exec")

    if args.mode == "clone_vfork_exec_failure_trace":
        require("clone_vfork_exec_failure_errno=2" in output,
                f"missing clone-vfork exec failure marker\n{output}")

    if args.mode in {
        "fork_execvp_enoexec_fallback",
        "vfork_execvp_enoexec_fallback",
        "clone_vfork_execvp_enoexec_fallback",
    }:
        marker = {
            "fork_execvp_enoexec_fallback": "fork_execvp_enoexec_exit=37",
            "vfork_execvp_enoexec_fallback": "vfork_execvp_enoexec_exit=37",
            "clone_vfork_execvp_enoexec_fallback": (
                "clone_vfork_execvp_enoexec_exit=37"
            ),
        }[args.mode]
        require(marker in output, f"missing ENOEXEC fallback marker\n{output}")
        require(final_files, "missing parent final CSV after ENOEXEC")
        require(target_count(final_files) >= 2,
                "parent final CSV missed calls after ENOEXEC")

    if args.mode in {
        "vfork_overflow_peak_env_injection",
        "clone_vfork_overflow_peak_env_injection",
    }:
        require("child_env_PEAK_MEMLOG_PATH=exec-overflow-sentinel" in output,
                f"overflow child did not receive priority PEAK env\n{output}")
        require(final_files, "missing parent final CSV after overflow env")
        require(target_count(final_files) >= 5,
                "parent final CSV missed calls after overflow env")

    if args.mode == "syscall_execve_failure_non_destructive":
        require("syscall_execve_failure_errno=2" in output,
                f"missing syscall execve failure marker\n{output}")
        require(final_files, "missing final CSV after failed syscall execve")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after failed syscall execve")

    if args.mode in {
        "syscall_execve_bad_env_failure_non_destructive",
        "syscall_execve_bad_env_preflight_unknown_non_destructive",
    }:
        require("syscall_execve_bad_env_errno=14" in output,
                f"missing syscall execve EFAULT marker\n{output}")
        require(final_files, "missing final CSV after bad-env syscall execve")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after bad-env syscall execve")
        require(not exec_files,
                f"bad-env syscall execve should not write exec checkpoints: {exec_files}")
        require_trace(tmpdir, args.mode, expect_failed=True, expected_exec_errno=14)
        return

    if args.mode == "syscall_execve_bad_argv_failure_non_destructive":
        require("syscall_execve_bad_argv_errno=14" in output,
                f"missing syscall execve bad-argv marker\n{output}")
        require(final_files, "missing final CSV after bad-argv syscall execve")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after bad-argv syscall execve")
        require(not exec_files,
                f"bad-argv syscall execve should not write exec checkpoints: {exec_files}")
        require_trace(tmpdir, args.mode, expect_failed=True, expected_exec_errno=14)
        return

    if args.mode in {
        "execve_bad_env_failure_non_destructive",
        "execve_bad_env_preflight_unknown_non_destructive",
    }:
        require("execve_bad_env_errno=14" in output,
                f"missing execve EFAULT marker\n{output}")
        require(final_files, "missing final CSV after bad-env execve")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after bad-env execve")
        require(not exec_files,
                f"bad-env execve should not write exec checkpoints: {exec_files}")
        require_trace(tmpdir, args.mode, expect_failed=True, expected_exec_errno=14)
        return

    if args.mode == "execve_bad_argv_failure_non_destructive":
        require("execve_bad_argv_errno=14" in output,
                f"missing execve bad-argv marker\n{output}")
        require(final_files, "missing final CSV after bad-argv execve")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after bad-argv execve")
        require(not exec_files,
                f"bad-argv execve should not write exec checkpoints: {exec_files}")
        require_trace(tmpdir, args.mode, expect_failed=True, expected_exec_errno=14)
        return

    if args.mode == "syscall_clone3_bad_pointer_failure_non_destructive":
        if "clone3_unavailable" in output:
            return
        require("syscall_clone3_bad_pointer_errno=14" in output,
                f"missing clone3 EFAULT marker\n{output}")
        require(final_files, "missing final CSV after failed clone3")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after failed clone3")
        require(not exec_files,
                f"clone3 bad pointer should not write exec checkpoints: {exec_files}")
        return

    if args.mode in {"execvp_enoent_failure",
                     "execvp_enotdir_enoent_failure",
                     "execvp_enotdir_enoent_fallback",
                     "execvp_enoent_fallback"}:
        require("execvp_enoent_errno=2" in output,
                f"missing execvp ENOENT marker\n{output}")
        require(final_files, "missing final CSV after execvp ENOENT")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after execvp ENOENT")

    if args.mode in {"execvp_eacces_failure", "execvp_eacces_fallback"}:
        require("execvp_eacces_errno=13" in output,
                f"missing execvp EACCES marker\n{output}")
        require(final_files, "missing final CSV after execvp EACCES")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after execvp EACCES")

    if args.mode == "fexecve_failure_non_destructive":
        require(f"fexecve_failure_errno={errno.EBADF}" in output,
                f"missing fexecve failure marker\n{output}")
        require(final_files, "missing final CSV after failed fexecve")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after failed fexecve")

    if args.mode in {
        "fexecve_bad_env_failure_non_destructive",
        "fexecve_bad_env_preflight_unknown_non_destructive",
    }:
        require("fexecve_bad_env_errno=14" in output,
                f"missing fexecve bad-env marker\n{output}")
        require(final_files, "missing final CSV after bad-env fexecve")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after bad-env fexecve")
        require(not exec_files,
                f"bad-env fexecve should not write exec checkpoints: {exec_files}")
        require_trace(tmpdir, args.mode, expect_failed=True, expected_exec_errno=14)
        return

    if args.mode == "fexecve_bad_argv_failure_non_destructive":
        require("fexecve_bad_argv_errno=14" in output,
                f"missing fexecve bad-argv marker\n{output}")
        require(final_files, "missing final CSV after bad-argv fexecve")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after bad-argv fexecve")
        require(not exec_files,
                f"bad-argv fexecve should not write exec checkpoints: {exec_files}")
        require_trace(tmpdir, args.mode, expect_failed=True, expected_exec_errno=14)
        return

    if args.mode == "execveat_failure_non_destructive":
        require("execveat_failure_errno=2" in output,
                f"missing execveat failure marker\n{output}")
        require(final_files, "missing final CSV after failed execveat")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after failed execveat")

    if args.mode in {
        "execveat_bad_env_failure_non_destructive",
        "execveat_bad_env_preflight_unknown_non_destructive",
    }:
        if "execveat_unavailable" in output:
            return
        require("execveat_bad_env_errno=14" in output,
                f"missing execveat bad-env marker\n{output}")
        require(final_files, "missing final CSV after bad-env execveat")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after bad-env execveat")
        require(not exec_files,
                f"bad-env execveat should not write exec checkpoints: {exec_files}")
        require_trace(tmpdir, args.mode, expect_failed=True, expected_exec_errno=14)
        return

    if args.mode == "execveat_bad_argv_failure_non_destructive":
        if "execveat_unavailable" in output:
            return
        require("execveat_bad_argv_errno=14" in output,
                f"missing execveat bad-argv marker\n{output}")
        require(final_files, "missing final CSV after bad-argv execveat")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after bad-argv execveat")
        require(not exec_files,
                f"bad-argv execveat should not write exec checkpoints: {exec_files}")
        require_trace(tmpdir, args.mode, expect_failed=True, expected_exec_errno=14)
        return

    if args.mode in {
        "execveat_empty_path_bad_env_failure_non_destructive",
        "execveat_empty_path_bad_env_preflight_unknown_non_destructive",
    }:
        if "execveat_unavailable" in output:
            return
        require("execveat_empty_path_bad_env_errno=14" in output,
                f"missing empty-path execveat bad-env marker\n{output}")
        require(final_files, "missing final CSV after empty-path bad-env execveat")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after empty-path bad-env execveat")
        require(not exec_files,
                "empty-path bad-env execveat should not write exec checkpoints: "
                f"{exec_files}")
        require_trace(tmpdir, args.mode, expect_failed=True, expected_exec_errno=14)
        return

    if args.mode == "execveat_empty_path_bad_argv_failure_non_destructive":
        if "execveat_unavailable" in output:
            return
        require("execveat_empty_path_bad_argv_errno=14" in output,
                f"missing empty-path execveat bad-argv marker\n{output}")
        require(final_files, "missing final CSV after empty-path bad-argv execveat")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after empty-path bad-argv execveat")
        require(not exec_files,
                "empty-path bad-argv execveat should not write exec checkpoints: "
                f"{exec_files}")
        require_trace(tmpdir, args.mode, expect_failed=True, expected_exec_errno=14)
        return

    if args.mode == "syscall_execveat_failure_non_destructive":
        require("syscall_execveat_failure_errno=2" in output,
                f"missing syscall execveat failure marker\n{output}")
        require(final_files, "missing final CSV after failed syscall execveat")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after failed syscall execveat")

    if args.mode in {
        "syscall_execveat_bad_env_failure_non_destructive",
        "syscall_execveat_bad_env_preflight_unknown_non_destructive",
    }:
        if "execveat_unavailable" in output:
            return
        require("syscall_execveat_bad_env_errno=14" in output,
                f"missing syscall execveat EFAULT marker\n{output}")
        require(final_files, "missing final CSV after bad-env syscall execveat")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after bad-env syscall execveat")
        require(not exec_files,
                f"bad-env syscall execveat should not write exec checkpoints: {exec_files}")
        require_trace(tmpdir, args.mode, expect_failed=True, expected_exec_errno=14)
        return

    if args.mode == "syscall_execveat_bad_argv_failure_non_destructive":
        if "execveat_unavailable" in output:
            return
        require("syscall_execveat_bad_argv_errno=14" in output,
                f"missing syscall execveat bad-argv marker\n{output}")
        require(final_files, "missing final CSV after bad-argv syscall execveat")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after bad-argv syscall execveat")
        require(not exec_files,
                f"bad-argv syscall execveat should not write exec checkpoints: {exec_files}")
        require_trace(tmpdir, args.mode, expect_failed=True, expected_exec_errno=14)
        return

    if args.mode == "posix_spawn_failure_non_destructive":
        require("posix_spawn_failure_result=2" in output,
                f"missing posix_spawn failure marker\n{output}")
        require(final_files, "missing final CSV after failed posix_spawn")
        require(target_count(final_files) >= 6,
                "normal final CSV missed calls after failed posix_spawn")

    if args.mode in {
        "posix_spawn_bad_env_failure_non_destructive",
        "posix_spawn_bad_env_preflight_unknown_non_destructive",
        "posix_spawnp_bad_env_failure_non_destructive",
        "posix_spawnp_bad_env_preflight_unknown_non_destructive",
    }:
        marker = (
            "posix_spawnp_bad_env_result=14"
            if args.mode.startswith("posix_spawnp_")
            else "posix_spawn_bad_env_result=14"
        )
        label = "posix_spawnp" if args.mode.startswith("posix_spawnp_") \
            else "posix_spawn"
        require(marker in output,
                f"missing {label} bad-env marker\n{output}")
        require(final_files, f"missing final CSV after bad-env {label}")
        require(target_count(final_files) >= 6,
                f"normal final CSV missed calls after bad-env {label}")
        require(not exec_files,
                f"bad-env {label} should not write exec checkpoints: {exec_files}")
        require_trace(tmpdir, args.mode, expect_failed=True, expected_exec_errno=14)
        return

    if args.mode in {
        "posix_spawn_bad_argv_failure_non_destructive",
        "posix_spawnp_bad_argv_failure_non_destructive",
    }:
        marker = (
            "posix_spawnp_bad_argv_result=14"
            if args.mode.startswith("posix_spawnp_")
            else "posix_spawn_bad_argv_result=14"
        )
        label = "posix_spawnp" if args.mode.startswith("posix_spawnp_") \
            else "posix_spawn"
        require(marker in output,
                f"missing {label} bad-argv marker\n{output}")
        require(final_files, f"missing final CSV after bad-argv {label}")
        require(target_count(final_files) >= 6,
                f"normal final CSV missed calls after bad-argv {label}")
        require(not exec_files,
                f"bad-argv {label} should not write exec checkpoints: {exec_files}")
        require_trace(tmpdir, args.mode, expect_failed=True, expected_exec_errno=14)
        return

    if args.mode in {"no_duplicate_ld_preload",
                     "no_duplicate_ld_preload_whitespace",
                     "no_duplicate_ld_preload_env_entries",
                     "no_duplicate_ld_preload_execvpe_path_fallback",
                     "vfork_duplicate_ld_preload",
                     "clone_vfork_duplicate_ld_preload",
                     "posix_spawn_duplicate_ld_preload"}:
        match = re.search(r"ld_preload_libpeak_count=(\d+)", output)
        require(match is not None, f"missing LD_PRELOAD count\n{output}")
        require(int(match.group(1)) == 1,
                f"LD_PRELOAD contains libpeak {match.group(1)} times")
        env_match = re.search(r"ld_preload_env_entries=(\d+)", output)
        require(env_match is not None, f"missing LD_PRELOAD env entry count\n{output}")
        require(int(env_match.group(1)) == 1,
                f"child env contains {env_match.group(1)} LD_PRELOAD entries")
        if args.mode in {"no_duplicate_ld_preload_env_entries",
                         "vfork_duplicate_ld_preload",
                         "clone_vfork_duplicate_ld_preload",
                         "posix_spawn_duplicate_ld_preload"}:
            extra_match = re.search(r"ld_preload_extra_count=(\d+)", output)
            require(extra_match is not None,
                    f"missing extra LD_PRELOAD token count\n{output}")
            require(int(extra_match.group(1)) == 2,
                    "distinct non-PEAK LD_PRELOAD token was not preserved")

    if args.mode == "posix_spawn_actions_attrs_injection":
        spawn_output = tmpdir / "spawn-actions.out"
        require(spawn_output.exists(),
                "posix_spawn file action did not create redirected output")
        text = spawn_output.read_text(encoding="utf-8", errors="replace")
        match = re.search(r"ld_preload_libpeak_count=(\d+)", text)
        require(match is not None,
                f"spawn action output missing LD_PRELOAD count\n{text}")
        require(int(match.group(1)) == 1,
                f"spawn action output contains libpeak {match.group(1)} times")
        env_match = re.search(r"ld_preload_env_entries=(\d+)", text)
        require(env_match is not None,
                f"spawn action output missing LD_PRELOAD env entry count\n{text}")
        require(int(env_match.group(1)) == 1,
                f"spawn action output has {env_match.group(1)} LD_PRELOAD entries")

    if args.mode == "posix_spawn_actions_close_stderr":
        spawn_output = tmpdir / "spawn-actions.out"
        require(spawn_output.exists(),
                "posix_spawn close-stderr action did not create stdout output")
        text = spawn_output.read_text(encoding="utf-8", errors="replace")
        require("child_stdout_after_stderr_close" in text,
                f"close-stderr child stdout marker missing\n{text}")
        require("child_stderr_sentinel_after_spawn_close" not in output,
                f"closed stderr sentinel leaked to parent output\n{output}")
        require("child_stderr_sentinel_after_spawn_close" not in text,
                f"closed stderr sentinel leaked to redirected stdout\n{text}")

    if args.mode == "text_output_not_corrupted":
        require("exec_failure_errno=2" in output, f"missing failure marker\n{output}")
        require("PEAK Library" in output, "text output was not printed")
        require(TARGET in output, "text output lost demangled target name")
        require(final_files, "missing final CSV for text-output run")

    if args.mode == "execvp_enoexec_fallback":
        require(not final_files,
                f"ENOEXEC shell fallback should not write PEAK final CSVs: {final_files}")

    require_trace(
        tmpdir,
        args.mode,
        expect_failed=args.mode in FAILED_EXEC_MODES,
        expected_exec_errno=(
            2 if args.mode in FAILED_EXEC_ENOENT_MODES
            else 13 if args.mode in {"execvp_eacces_failure",
                                      "execvp_eacces_fallback"}
            else None
        ),
    )


def main():
    args = parse_args()
    with tempfile.TemporaryDirectory(
        prefix=f"peak-exec-chain-{args.mode}-",
        dir=os.getcwd(),
    ) as tmp:
        tmpdir = Path(tmp)
        proc = run_fixture(args, tmpdir)
        try:
            check_mode(args, proc, tmpdir)
        except Exception as exc:
            sys.stdout.write(proc.stdout)
            dump_failure_artifacts(tmpdir)
            print(f"exec_chain_check_failed mode={args.mode}: {exc}")
            raise
    print(f"exec_chain_check_ok mode={args.mode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
