#!/usr/bin/env python3
import os
import subprocess
import sys


def run(program, preload, target, mode, legacy=False):
    env = os.environ.copy()
    env.update({
        "LD_PRELOAD": preload,
        "PEAK_TARGET": target,
        "PEAK_TARGET_FILE": "",
        "PEAK_TARGET_GROUP": "",
        "PEAK_HEARTBEAT_INTERVAL": "0",
        "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "0",
        "PEAK_ENABLE_GLOBAL_HEARTBEAT": "0",
        "PEAK_ENABLE_REATTACH": "0",
    })
    if legacy:
        env["PEAK_ENABLE_CXX_SYMBOL_SCAN"] = "1"
    try:
        completed = subprocess.run([program, mode], env=env,
                                   capture_output=True, text=True, timeout=20)
    except subprocess.TimeoutExpired as error:
        raise AssertionError(f"timed out: {mode}") from error
    if completed.returncode != 0:
        raise AssertionError(completed.stdout + completed.stderr)
    return completed.stdout, completed.stderr


def main():
    program, module_a, module_b = sys.argv[1:]
    selector = "peak_test::Widget::func(int,double)"
    for target in (f"{module_a}!{selector}",
                   f"{module_a}!_ZN9peak_test6Widget4funcEid",
                   f"{module_a}!peak_test::Widget::operator!() const"):
        stdout, _ = run(program, module_a, target, "startup-unique")
        assert "startup_unique_selector_ok" in stdout

    # The resolver unit test covers all three legacy extraction regressions.
    # The destructor also has a listener-safe production prologue, so exercise
    # that target through the complete startup attach path.
    stdout, _ = run(program, module_a, "~Widget", "startup-unique",
                    legacy=True)
    assert "startup_unique_selector_ok" in stdout

    dual_target = ",".join((
        f"{module_a}!_ZN9peak_test6Widget4funcEid",
        f"{module_b}!_ZN9peak_test6Widget4funcEid",
    ))
    stdout, _ = run(program, f"{module_a}:{module_b}", dual_target,
                    "startup-dual-module")
    assert "startup_dual_module_report_names_ok" in stdout

    stdout, stderr = run(program, f"{module_a}:{module_b}", selector,
                         "startup-ambiguous")
    assert "startup_ambiguous_terminal_ok" in stdout
    assert stderr.count("ambiguous target selector; refusing to attach") == 1
    assert stderr.count(f"module={module_a}") == 1
    assert stderr.count(f"module={module_b}") == 1

    stdout, stderr = run(program, module_a,
                         "peak_symbol_fixture_invoke+0X10",
                         "startup-invalid")
    assert "startup_invalid_selector_ok" in stdout
    assert stderr.count("invalid target selector; refusing to attach") == 1
    print("startup_selectors_test_ok")


if __name__ == "__main__":
    main()
