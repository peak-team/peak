#!/usr/bin/env python3

import argparse
import tempfile
import unittest
from pathlib import Path

import run_exec_chain_check as checker


class ExecChainCheckContractsTest(unittest.TestCase):
    def check_mode(self, mode, output, workdir):
        proc = argparse.Namespace(returncode=0, stdout=output)
        args = argparse.Namespace(mode=mode)
        checker.check_common(args, proc, workdir)

    def test_capacity_passthrough_requires_native_env_and_no_checkpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            workdir = Path(directory)
            output = (
                "ld_preload_libpeak_count=0 ld_preload_env_entries=1 "
                "ld_preload_extra_count=0 peak_target=<missing> "
                "peak_statslog=<missing> marker=postfork-long-preload "
                "peak_exec_chain=<missing> peak_exec_checkpoint=<missing> "
                "peak_exec_propagate=<missing> ld_library_path_env_entries=0 "
                "ld_library_path_0=<missing> ld_library_path_1=<missing> "
                f"path={workdir / 'no-external-commands'} "
                "observer_mode=capacity-long-preload "
                "child_pad_validated_count=0 child_pad_mismatch_count=0 "
                "ld_preload_length=8192 ld_preload_all_x=1 "
                "loader_observer=<missing> secure_test_hook=<missing>\n"
                "postfork_capacity_passthrough=1 kind=long-preload "
                "input_entries=3 input_pad_validated_count=0 "
                "input_pad_mismatch_count=0 input_preload_entries=1 "
                "input_preload_length=8192 input_preload_all_x=1 "
                "input_path_entries=1 input_loader_path_entries=0 "
                "input_terminator_null=1\n"
                "exec_child_ok sink=0\n"
            )
            self.check_mode("vfork_long_preload_fallback", output, workdir)
            (workdir / "peak_stats-p1-exec1.csv").write_text(
                "function,count\npeak_exec_chain_hot_target,7\n",
                encoding="utf-8",
            )
            with self.assertRaises(AssertionError):
                self.check_mode("vfork_long_preload_fallback", output, workdir)

    def test_injected_path_still_requires_an_exec_checkpoint(self):
        output = "exec_child_ok sink=0\n"
        with tempfile.TemporaryDirectory() as directory:
            workdir = Path(directory)
            with self.assertRaises(AssertionError):
                self.check_mode("execv_success_checkpoint", output, workdir)
            (workdir / "peak_stats-p1-exec1.csv").write_text(
                "function,count\npeak_exec_chain_hot_target,5\n",
                encoding="utf-8",
            )
            self.check_mode("execv_success_checkpoint", output, workdir)


if __name__ == "__main__":
    unittest.main()
