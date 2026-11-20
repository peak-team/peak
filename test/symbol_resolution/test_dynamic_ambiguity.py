#!/usr/bin/env python3
import os
import subprocess
import sys


def run(program, mode, env):
    try:
        completed = subprocess.run([program, mode], env=env,
                                   capture_output=True, text=True, timeout=20)
    except subprocess.TimeoutExpired as error:
        raise AssertionError(f"timed out: {mode}") from error
    if completed.returncode != 0:
        raise AssertionError(completed.stdout + completed.stderr)
    return completed


def main():
    program, module_a, _ = sys.argv[1:]
    env = os.environ.copy()
    env.pop("PEAK_VERBOSITY", None)
    env.update({
        "PEAK_ENABLE_CXX_SYMBOL_SCAN": "0",
        "PEAK_TARGET_FILE": "",
        "PEAK_TARGET_GROUP": "",
        "PEAK_HEARTBEAT_INTERVAL": "0",
        "PEAK_ENABLE_PER_TARGET_HEARTBEAT": "0",
        "PEAK_ENABLE_GLOBAL_HEARTBEAT": "0",
        "PEAK_ENABLE_REATTACH": "0",
    })
    default_target = "peak_test::legacy_short_unique"
    env["PEAK_TARGET"] = default_target
    capability = run(program, "check-owned-module-lookup", env).stdout
    assert ("owned_module_lookup=0" in capability or
            "owned_module_lookup=1" in capability), capability
    fallback = run(program, "owned-module-unavailable", env)
    assert "dynamic_owned_module_unavailable_ok" in fallback.stdout
    assert fallback.stderr.count(
        f"cannot resolve late-loaded target '{default_target}'") == 1, fallback.stderr
    assert "path-qualified module!symbol scope" in fallback.stderr, fallback.stderr
    if "owned_module_lookup=0" in capability:
        print("dynamic_owned_module_fallback_test_ok")
        return

    for mode, selector, verbosity, expected, is_ambiguous in (
        ("unique", default_target, None, "dynamic_unique_selector_ok", False),
        ("unique", f"{os.path.basename(module_a)}!peak_test::legacy_short_unique",
         None, "dynamic_unique_selector_ok", False),
        ("first-provider", "peak_test::Widget::func(int,double)", None,
         "dynamic_first_provider_ok", False),
        ("ambiguous", "peak_test::Widget::func", None, "dynamic_ambiguous_terminal_ok", True),
        ("ambiguous-sequential", "peak_test::Widget::func", "info",
         "dynamic_ambiguous_terminal_ok", True),
    ):
        run_env = env.copy()
        run_env["PEAK_TARGET"] = selector
        if verbosity is not None:
            run_env["PEAK_VERBOSITY"] = verbosity
        completed = run(program, mode, run_env)
        assert expected in completed.stdout
        diagnostic = "ambiguous dynamic target selector"
        assert completed.stderr.count(diagnostic) == int(is_ambiguous), completed.stderr
        if is_ambiguous:
            assert "(2 candidates" in completed.stderr, completed.stderr
            assert selector in completed.stderr, completed.stderr
            assert completed.stderr.count("address=") == (2 if verbosity == "info" else 0)
    print("dynamic_ambiguity_test_ok")


if __name__ == "__main__":
    main()
