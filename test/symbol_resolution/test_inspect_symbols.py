#!/usr/bin/env python3
import os
import subprocess
import sys
import tempfile


def run(*args, expected, env=None):
    try:
        completed = subprocess.run(args, capture_output=True, text=True,
                                   env=env, timeout=20)
    except subprocess.TimeoutExpired as error:
        raise AssertionError(f"timed out: {args}") from error
    if completed.returncode != expected:
        raise AssertionError(
            f"expected {expected}, got {completed.returncode}: {completed.stderr}")
    return completed.stdout


def probe_optional_demangle(*args):
    """Return the platform demangler capability without weakening its result."""
    try:
        completed = subprocess.run(args, capture_output=True, text=True,
                                   timeout=20)
    except subprocess.TimeoutExpired as error:
        raise AssertionError(f"timed out: {args}") from error
    if completed.returncode not in (0, 1):
        raise AssertionError(
            f"expected resolver result 0 or 1, got {completed.returncode}: "
            f"{completed.stderr}")
    expected_candidates = 1 if completed.returncode == 0 else 0
    assert f"candidates={expected_candidates}" in completed.stdout
    return completed.returncode


def main():
    peak, module_a, module_b = sys.argv[1:]
    output = run(
        peak, "inspect-symbols", "--module", module_a, "--module", module_b,
        "peak_test::Widget::func(int,double)", expected=1)
    assert "candidates=2" in output
    assert f"module={module_a}" in output
    assert f"module={module_b}" in output
    assert "mangled=_ZN9peak_test6Widget4funcEid" in output
    assert "demangled=peak_test::Widget::func(int, double)" in output

    run(peak, "inspect-symbols",
        f"{module_a}!_ZN9peak_test6Widget4funcEid+0x0", expected=2)
    run(peak, "inspect-symbols", f"{module_a}!peak_symbol_strong_alias+0x1",
        expected=2)

    output = run(
        peak, "inspect-symbols",
        f"{module_a}!peak_test::Widget::operator!() const", expected=0)
    assert "candidates=1" in output
    assert "operator!() const" in output

    output = run(
        peak, "inspect-symbols",
        f"{module_a}!peak_test::Widget::operator !() const", expected=0)
    assert "candidates=1" in output
    assert "operator!() const" in output

    output = run(
        peak, "inspect-symbols",
        f"{module_a}!peak_test::Widget::operator[](int) const", expected=0)
    assert "candidates=1" in output
    assert "operator[](int) const" in output

    output = run(
        peak, "inspect-symbols",
        f"{module_a}!peak_test::operator<<<int>(peak_test::TemplateOperatorValue const&, int)",
        expected=0)
    assert "candidates=1" in output
    assert "operator<<" in output
    output = run(
        peak, "inspect-symbols",
        f"{module_a}!peak_test::operator >> <int>(peak_test::TemplateOperatorValue const&, int)",
        expected=0)
    assert "candidates=1" in output
    assert "operator>>" in output
    output = run(
        peak, "inspect-symbols",
        f"{module_a}!peak_test::operator+<int>(peak_test::TemplateOperatorValue const&, int)",
        expected=0)
    assert "candidates=1" in output
    assert "operator+" in output
    for selector in (
            "peak_test::operator\"\" _peak_literal(unsigned long long)",
            "peak_test::operator\"\"_peak_literal(unsigned long long)"):
        output = run(peak, "inspect-symbols", f"{module_a}!{selector}",
                     expected=0)
        assert "candidates=1" in output
        assert "operator\"\" _peak_literal" in output

    for selector in (
            "multiword_return<int>(int)",
            "unsigned long multiword_return<int>(int)",
            "const_ref_return<int>()", "int const& const_ref_return<int>()"):
        output = run(peak, "inspect-symbols", f"{module_a}!{selector}",
                     expected=0)
        assert "candidates=1" in output

    for selector in (
            "function_pointer_return<int>(int)",
            "int (*function_pointer_return<int>(int))(int)",
            "noexcept_pointer_return<2,1>()",
            "int (*noexcept_pointer_return<2,1>())() noexcept",
            "array_return<2,1>()", "int (*array_return<2,1>()) [((2)>(1))]",
            "plain_operator_type_return<int>(int)",
            "operator_type plain_operator_type_return<int>(int)",
            "allocation_outer<CallbackType>(CallbackType)",
            "decltype (new CallbackType()) allocation_outer<CallbackType>(CallbackType)",
            "peak_test::QualifierOnly::const_method(int) const",
            "peak_test::QualifierOnly::lref_method(int) &",
            "peak_test::QualifierOnly::noexcept_method(int)",
            "less_return<int,int>(int,int)",
            "decltype ({parm#1}<{parm#2}) less_return<int,int>(int,int)",
            "shift_return<int>(int)",
            "decltype ({parm#1}<<(1)) shift_return<int>(int)",
            "nested_pointer_return<int>(int)",
            "int (*(*nested_pointer_return<int>(int))())(int)",
            "probe::concrete_angle_plus_return<int>(int)",
            "probe::Holder<&probe::operator+> probe::concrete_angle_plus_return<int>(int)",
            "peak_test::OperatorSurface::operator new(unsigned long)",
            "peak_test::OperatorSurface::operator delete(void*)",
            "peak_test::OperatorSurface::operator bool() const",
            "peak_test::OperatorSurface::operator unsigned long() const"):
        output = run(peak, "inspect-symbols", f"{module_a}!{selector}",
                     expected=0)
        assert "candidates=1" in output

    # Older libstdc++ demanglers return this ABI spelling unchanged.
    conversion_mangled = (
        "_Z25conversion_pointer_returnIN9peak_test23ConversionPointerSourceEEDTadsrT_oncviES2_")
    conversion_selectors = (
        "conversion_pointer_return<peak_test::ConversionPointerSource>(peak_test::ConversionPointerSource)",
        "decltype (&peak_test::ConversionPointerSource::operator int) conversion_pointer_return<peak_test::ConversionPointerSource>(peak_test::ConversionPointerSource)")
    output = run(peak, "inspect-symbols", f"{module_a}!{conversion_mangled}",
                 expected=0)
    assert "candidates=1" in output
    # Exact mangled lookup deliberately avoids demangling, so probe the human
    # selector path itself. Older Frontera libstdc++ demanglers return this ABI
    # spelling unchanged; both human spellings must then be consistently
    # unavailable while the exact mangled selector above remains usable.
    conversion_expected = probe_optional_demangle(
        peak, "inspect-symbols", f"{module_a}!{conversion_selectors[0]}")
    for selector in conversion_selectors[1:]:
        output = run(peak, "inspect-symbols", f"{module_a}!{selector}",
                     expected=conversion_expected)
        expected_candidates = 1 if conversion_expected == 0 else 0
        assert f"candidates={expected_candidates}" in output

    for selector in (
            "const", "(int) const",
            "<int>(peak_test::TemplateOperatorValue const&, int)",
            "long multiword_return<int>(int)",
            "longlong multiword_return<int>(int)",
            "long function_pointer_return<int>(int)",
            "type plain_operator_type_return<int>(int)",
            "CallbackType()",
            "peak_test::QualifierOnly::const_method(int)",
            "peak_test::QualifierOnly::lref_method(int)",
            "peak_test::QualifierOnly::noexcept_method(int) noexcept",
            "new(unsigned long)", "delete(void*)", "bool() const",
            "unsigned long() const", "long() const",
            "_peak_literal(unsigned long long)", "int() const",
            "operator+(probe::OperatorValue)",
            "const& const_ref_return<int>()"):
        output = run(peak, "inspect-symbols", f"{module_a}!{selector}",
                     expected=1)
        assert "candidates=0" in output

    for expression in (
            "N > 0", "N>0", "N >0", "N> 0", "N>M", "N >M", "N> M",
            "N < M", "N<M", "N <M", "N< M", "sizeof(T)>sizeof(U)",
            "N>=0", "N >=0", "N>= 0", "N >= 0"):
        output = run(
            peak, "inspect-symbols",
            f"{module_a}!f<std::enable_if_t<({expression}),int>>()",
            expected=1)
        assert "candidates=0" in output

    for expression in ("N>M", "N >M", "N> M", "N > M",
                       "N<M", "N <M", "N< M", "N < M"):
        output = run(peak, "inspect-symbols",
                     f"{module_a}!foo(decltype({expression}))", expected=1)
        assert "candidates=0" in output
    output = run(peak, "inspect-symbols",
                 f"{module_a}!f<decltype(std::vector<int>{{}})>()", expected=1)
    assert "candidates=0" in output
    output = run(peak, "inspect-symbols",
                 f"{module_a}!int (*array_bound_return<2,1>()) [((2)>(1))]",
                 expected=0)
    assert "candidates=1" in output
    for selector in (
            "decltype(values[((2)>(1))]) selector_array_value()",
            "decltype(Box{((2)>(1))}) selector_brace_value()"):
        output = run(peak, "inspect-symbols", f"{module_a}!{selector}",
                     expected=1)
        assert "candidates=0" in output

    run(peak, "inspect-symbols", "symbol+0xZZ", expected=2)
    run(peak, "inspect-symbols", "symbol+0x-1", expected=2)
    run(peak, "inspect-symbols", "symbol+0x+1", expected=2)
    run(peak, "inspect-symbols", "symbol+0x 1", expected=2)
    run(peak, "inspect-symbols", "symbol+0x10+0x20", expected=2)
    run(peak, "inspect-symbols", "symbol+0X1", expected=2)
    output = run(peak, "inspect-symbols", f"{module_a}!f<(N+0x10)>()",
                 expected=1)
    assert "candidates=0" in output
    output = run(peak, "inspect-symbols", "--module", module_a,
                 "peak_symbol_does_not_exist", expected=1)
    assert "candidates=0" in output
    run(peak, "inspect-symbols", "--module", "/no/such/peak-module.so",
        "peak_test::Widget::func(int,double)", expected=2)
    with tempfile.TemporaryDirectory() as directory:
        marker = os.path.join(directory, "loaded")
        env = os.environ.copy()
        env["PEAK_SYMBOL_CLI_CONSTRUCTOR_MARKER"] = marker
        run(peak, "inspect-symbols", "--module", expected=2, env=env)
        run(peak, "inspect-symbols", "--module", module_a,
            expected=2, env=env)
        run(peak, "inspect-symbols", f"{module_a}!symbol!extra",
            expected=2, env=env)
        run(peak, "inspect-symbols", f"{module_a}!foo(",
            expected=2, env=env)
        run(peak, "inspect-symbols", f"{module_a}!foo)",
            expected=2, env=env)
        run(peak, "inspect-symbols", f"{module_a}!foo<int",
            expected=2, env=env)
        run(peak, "inspect-symbols", f"{module_a}!foo>",
            expected=2, env=env)
        run(peak, "inspect-symbols", f"{module_a}!foo(std::vector<int)",
            expected=2, env=env)
        run(peak, "inspect-symbols", f"{module_a}!foo(int>)",
            expected=2, env=env)
        run(peak, "inspect-symbols",
            f"{module_a}!f<std::enable_if_t<(N=>M),int>>()",
            expected=2, env=env)
        run(peak, "inspect-symbols",
            f"{module_a}!f<foo(FixedString<3)>()",
            expected=2, env=env)
        run(peak, "inspect-symbols", f"{module_a}!foo([)]",
            expected=2, env=env)
        run(peak, "inspect-symbols", f"{module_a}!foo([x",
            expected=2, env=env)
        run(peak, "inspect-symbols", f"{module_a}!foo({{x",
            expected=2, env=env)
        assert not os.path.exists(marker)
    print("inspect_symbols_test_ok")


if __name__ == "__main__":
    main()
