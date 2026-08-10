#include "internal/target_resolver.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

bool peak_truncate_function_name = false;

static int failures;

static void
expect_true(gboolean condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "not ok - %s\n", message);
        failures++;
    }
}

static PeakTargetResolveResult
resolve(const char* selector, PeakTargetResolution* resolution)
{
    return peak_target_resolver_resolve(selector, NULL, TRUE, resolution);
}

static gpointer
unique_address(const char* selector)
{
    PeakTargetResolution resolution = {0};
    PeakTargetResolveResult result = resolve(selector, &resolution);
    gpointer address = NULL;

    expect_true(result == PEAK_TARGET_RESOLVE_UNIQUE, selector);
    if (result == PEAK_TARGET_RESOLVE_UNIQUE) {
        address = ((PeakTargetSymbolCandidate*)
                   g_ptr_array_index(resolution.candidates, 0))->address;
    }
    peak_target_resolution_clear(&resolution);
    return address;
}

int
main(void)
{
    char* module_a_selector;
    char* module_b_selector;
    char* mangled_selector;
    char* offset_selector;
    char* weak_selector;
    char* strong_selector;
    char* temporary_directory;
    char* symlink_path;
    char* missing_path;
    char* basename_selector;
    const char* module_basename;
    const char* module_b_basename;
    PeakTargetResolution resolution = {0};
    gpointer weak_address;
    gpointer strong_address;

    gum_init_embedded();
    expect_true(dlopen(PEAK_TEST_SYMBOL_MODULE_A, RTLD_NOW | RTLD_LOCAL) != NULL,
                "load module a");
    expect_true(dlopen(PEAK_TEST_SYMBOL_MODULE_B, RTLD_NOW | RTLD_LOCAL) != NULL,
                "load module b");

    temporary_directory = g_dir_make_tmp("peak-symbol-resolution-XXXXXX", NULL);
    expect_true(temporary_directory != NULL, "create temporary directory");
    symlink_path = temporary_directory != NULL
        ? g_build_filename(temporary_directory, "symbol_fixture.so", NULL)
        : NULL;
    if (symlink_path != NULL) {
        expect_true(symlink(PEAK_TEST_SYMBOL_MODULE_A, symlink_path) == 0,
                    "create module symlink");
        expect_true(peak_target_resolver_module_matches(symlink_path,
                                                         PEAK_TEST_SYMBOL_MODULE_A),
                    "module path accepts canonical-equivalent symlink");
    }
    module_basename = strrchr(PEAK_TEST_SYMBOL_MODULE_A, '/');
    module_b_basename = strrchr(PEAK_TEST_SYMBOL_MODULE_B, '/');
    missing_path = g_strdup_printf("missing-directory/%s",
                                   module_basename != NULL ? module_basename + 1
                                                          : PEAK_TEST_SYMBOL_MODULE_A);
    expect_true(!peak_target_resolver_module_matches(missing_path,
                                                      PEAK_TEST_SYMBOL_MODULE_A),
                "unresolved path never falls back to basename matching");
    if (symlink_path != NULL) {
        unlink(symlink_path);
    }
    if (temporary_directory != NULL) {
        rmdir(temporary_directory);
    }
    g_free(missing_path);
    g_free(symlink_path);
    g_free(temporary_directory);

    expect_true(module_basename != NULL && module_b_basename != NULL &&
                    strcmp(module_basename + 1, module_b_basename + 1) == 0,
                "fixture DSOs have the same basename");
    basename_selector = g_strdup_printf(
        "%s!peak_test::Widget::func(int,double)", module_basename + 1);
    expect_true(resolve(basename_selector, &resolution) ==
                    PEAK_TARGET_RESOLVE_AMBIGUOUS &&
                    resolution.candidates->len == 2,
                "same-basename module selector retains both DSO candidates");
    peak_target_resolution_clear(&resolution);
    g_free(basename_selector);

    expect_true(resolve("peak_test::Widget::func(int,double)", &resolution) ==
                    PEAK_TARGET_RESOLVE_AMBIGUOUS &&
                    resolution.candidates->len == 2,
                "overloaded cross-DSO selector is ambiguous with both candidates");
    peak_target_resolution_clear(&resolution);

    expect_true(resolve("peak_test::Widget::func", &resolution) ==
                    PEAK_TARGET_RESOLVE_AMBIGUOUS &&
                    resolution.candidates->len == 4,
                "legacy namespace selector does not select an overload");
    peak_target_resolution_clear(&resolution);

    module_a_selector = g_strdup_printf(
        "%s!peak_test::Widget::func(int,double)", PEAK_TEST_SYMBOL_MODULE_A);
    module_b_selector = g_strdup_printf(
        "%s!peak_test::Widget::func(int,double)", PEAK_TEST_SYMBOL_MODULE_B);
    expect_true(unique_address(module_a_selector) != NULL,
                "fully qualified module selector resolves module a");
    expect_true(unique_address(module_b_selector) != NULL,
                "fully qualified module selector resolves module b");
    expect_true(peak_target_resolver_resolve(
                    module_a_selector, PEAK_TEST_SYMBOL_MODULE_A, TRUE,
                    &resolution) == PEAK_TARGET_RESOLVE_UNIQUE,
                "loaded-module resolver resolves the exact module path");
    peak_target_resolution_clear(&resolution);

    mangled_selector = g_strdup_printf(
        "%s!_ZN9peak_test6Widget4funcEid", PEAK_TEST_SYMBOL_MODULE_A);
    offset_selector = g_strdup_printf("%s+0x0", mangled_selector);
    expect_true(unique_address(mangled_selector) == unique_address(offset_selector),
                "mangled selector and zero offset resolve deterministically");
    {
        char* function_pointer_selector = g_strdup_printf(
            "%s!function_pointer_return<int>(int)", PEAK_TEST_SYMBOL_MODULE_A);
        char* clone_selector = g_strdup_printf(
            "%s!_Z23function_pointer_returnIiEPFiiET_.clone_for_test.0",
            PEAK_TEST_SYMBOL_MODULE_A);

        expect_true(unique_address(function_pointer_selector) != NULL,
                    "full selector excludes GCC IPA clone candidates");
        expect_true(resolve(clone_selector, &resolution) ==
                        PEAK_TARGET_RESOLVE_UNIQUE &&
                        strstr(((PeakTargetSymbolCandidate*)
                                g_ptr_array_index(resolution.candidates, 0))->demangled,
                               "[clone .clone_for_test.0]") != NULL,
                    "GCC IPA clone remains selectable by exact mangled name");
        peak_target_resolution_clear(&resolution);
        g_free(clone_selector);
        g_free(function_pointer_selector);
    }
    {
        char* target_selector = g_strdup_printf("%s!Derived::target(int)",
                                                PEAK_TEST_SYMBOL_MODULE_A);
        expect_true(unique_address(target_selector) != NULL,
                    "ordinary virtual override excludes ABI thunks");
        g_free(target_selector);
    }
    {
        char* target_selector = g_strdup_printf("%s!Derived::target",
                                                PEAK_TEST_SYMBOL_MODULE_A);
        expect_true(unique_address(target_selector) != NULL,
                    "legacy selector excludes ABI thunks");
        g_free(target_selector);
    }
#ifdef PEAK_TEST_EXPECT_GCC_THUNK
    {
        char* thunk_selector = g_strdup_printf(
            "%s!_ZThn8_N7Derived6targetEi", PEAK_TEST_SYMBOL_MODULE_A);
        expect_true(resolve(thunk_selector, &resolution) ==
                        PEAK_TARGET_RESOLVE_UNIQUE &&
                        strstr(((PeakTargetSymbolCandidate*)
                                g_ptr_array_index(resolution.candidates, 0))->demangled,
                               "non-virtual thunk to Derived::target(int)") != NULL,
                    "GCC ABI thunk remains selectable only by exact mangled name");
        peak_target_resolution_clear(&resolution);
        g_free(thunk_selector);
    }
#endif
    {
        char* out_of_module_selector = g_strdup_printf("%s+0x10000000",
                                                        mangled_selector);
        expect_true(resolve(out_of_module_selector, &resolution) ==
                        PEAK_TARGET_RESOLVE_NONE,
                    "out-of-module offset is not accepted");
        peak_target_resolution_clear(&resolution);
        g_free(out_of_module_selector);
    }
    {
        char* collision_selector = g_strdup_printf("%s!collision",
                                                    PEAK_TEST_SYMBOL_MODULE_A);
        expect_true(unique_address(collision_selector) != NULL,
                    "exact C symbol wins over a legacy C++ short-name candidate");
        g_free(collision_selector);
    }
    expect_true(resolve("peak_test::template_func<int>(int)", &resolution) ==
                    PEAK_TARGET_RESOLVE_AMBIGUOUS,
                "template selector keeps cross-DSO candidates ambiguous");
    peak_target_resolution_clear(&resolution);
    expect_true(resolve("peak_test::namespaced_template<int>(int)", &resolution) ==
                    PEAK_TARGET_RESOLVE_AMBIGUOUS,
                "template selector accepts a namespaced return type");
    peak_target_resolution_clear(&resolution);
    expect_true(resolve("peak_test::Widget::operator()", &resolution) ==
                    PEAK_TARGET_RESOLVE_AMBIGUOUS,
                "legacy qualified operator selector keeps all candidates");
    peak_target_resolution_clear(&resolution);
    {
        char* operator_selector = g_strdup_printf(
            "%s!peak_test::Widget::operator!() const",
            PEAK_TEST_SYMBOL_MODULE_A);
        expect_true(unique_address(operator_selector) != NULL,
                    "module-qualified operator bang selector resolves");
        g_free(operator_selector);
    }
    {
        char* literal_selector = g_strdup_printf(
            "%s!peak_test::operator\"\" _peak_literal(unsigned long long)",
            PEAK_TEST_SYMBOL_MODULE_A);
        char* literal_selector_without_space = g_strdup_printf(
            "%s!peak_test::operator\"\"_peak_literal(unsigned long long)",
            PEAK_TEST_SYMBOL_MODULE_A);
        char* literal_suffix = g_strdup_printf(
            "%s!_peak_literal(unsigned long long)",
            PEAK_TEST_SYMBOL_MODULE_A);

        expect_true(unique_address(literal_selector) != NULL,
                    "module-qualified user-defined literal selector resolves");
        expect_true(unique_address(literal_selector_without_space) != NULL,
                    "user-defined literal selector accepts omitted whitespace");
        expect_true(resolve(literal_suffix, &resolution) == PEAK_TARGET_RESOLVE_NONE,
                    "literal suffix is not an independent function id");
        peak_target_resolution_clear(&resolution);
        g_free(literal_suffix);
        g_free(literal_selector_without_space);
        g_free(literal_selector);
    }
    {
        char* name_selector = g_strdup_printf(
            "%s!probe::concrete_angle_plus_return<int>(int)",
            PEAK_TEST_SYMBOL_MODULE_A);
        char* full_selector = g_strdup_printf(
            "%s!probe::Holder<&probe::operator+> probe::concrete_angle_plus_return<int>(int)",
            PEAK_TEST_SYMBOL_MODULE_A);
        char* operator_suffix = g_strdup_printf(
            "%s!operator+(probe::OperatorValue)",
            PEAK_TEST_SYMBOL_MODULE_A);

        expect_true(unique_address(name_selector) != NULL,
                    "operator in a return template does not hide the function name");
        expect_true(unique_address(full_selector) != NULL,
                    "full return-template selector resolves");
        expect_true(resolve(operator_suffix, &resolution) == PEAK_TARGET_RESOLVE_NONE,
                    "operator in a return template is not an independent suffix");
        peak_target_resolution_clear(&resolution);
        g_free(operator_suffix);
        g_free(full_selector);
        g_free(name_selector);
    }
    {
        char* legacy_short_selector = g_strdup_printf(
            "%s!legacy_short_unique", PEAK_TEST_SYMBOL_MODULE_A);
        expect_true(unique_address(legacy_short_selector) != NULL,
                    "module-qualified legacy C++ short selector resolves");
        g_free(legacy_short_selector);
    }

    weak_selector = g_strdup_printf("%s!peak_symbol_weak_alias",
                                    PEAK_TEST_SYMBOL_MODULE_A);
    strong_selector = g_strdup_printf("%s!peak_symbol_strong_alias",
                                      PEAK_TEST_SYMBOL_MODULE_A);
    weak_address = unique_address(weak_selector);
    strong_address = unique_address(strong_selector);
    {
        char* c_offset_selector = g_strdup_printf("%s+0x1", strong_selector);
        expect_true(unique_address(c_offset_selector) != NULL,
                    "plain C selector accepts an in-module offset");
        g_free(c_offset_selector);
    }
#if defined(__ELF__)
    expect_true(weak_address == strong_address,
                "weak and strong aliases retain their shared address");
#else
    expect_true(weak_address != NULL && strong_address != NULL,
                "weak and strong alias symbols both resolve");
#endif

    expect_true(resolve("module!+0x10", &resolution) == PEAK_TARGET_RESOLVE_INVALID,
                "empty symbol is invalid");
    peak_target_resolution_clear(&resolution);
    expect_true(resolve("module!symbol+0xZZ", &resolution) == PEAK_TARGET_RESOLVE_INVALID,
                "malformed offset is invalid");
    peak_target_resolution_clear(&resolution);
    expect_true(resolve("module!symbol+0x-1", &resolution) == PEAK_TARGET_RESOLVE_INVALID,
                "signed offset is invalid");
    peak_target_resolution_clear(&resolution);
    expect_true(resolve("module!symbol+0x 1", &resolution) == PEAK_TARGET_RESOLVE_INVALID,
                "whitespace offset is invalid");
    peak_target_resolution_clear(&resolution);
    expect_true(resolve("module!symbol+0x10+0x20", &resolution) ==
                    PEAK_TARGET_RESOLVE_INVALID,
                "repeated offsets are invalid");
    peak_target_resolution_clear(&resolution);
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::Widget::operator!() const"),
                "operator bang does not create a second module separator");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::Widget::operator!=(int)"),
                "operator not-equal does not create a second module separator");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::Widget::operator !() const"),
                "spaced operator bang does not create a module separator");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::Widget::operator !=(int)"),
                "spaced operator not-equal does not create a module separator");
    expect_true(peak_target_resolver_validate_selector("foo<!false>()"),
                "template bang is not a module separator");
    expect_true(peak_target_resolver_validate_selector("noexcept(!B)"),
                "parameter bang is not a module separator");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::Widget::operator<(int)"),
                "operator less-than is not an unclosed template");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::Widget::operator<<(int)"),
                "operator shift-left is not an unclosed template");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::Widget::operator>(int)"),
                "operator greater-than is not an extra template close");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::Widget::operator>>(int)"),
                "operator shift-right is not an extra template close");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::f<std::pair<int,double>>(int)"),
                "nested templates and parameters remain valid");
    expect_true(!peak_target_resolver_validate_selector("module!foo("),
                "unclosed parameter list is invalid");
    expect_true(!peak_target_resolver_validate_selector("module!foo)"),
                "extra parameter close is invalid");
    expect_true(!peak_target_resolver_validate_selector("module!foo<int"),
                "unclosed template is invalid");
    expect_true(!peak_target_resolver_validate_selector("module!foo>"),
                "extra template close is invalid");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::Widget::operator->()"),
                "operator arrow is not an extra template close");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::Widget::operator->*(int)"),
                "operator arrow-star is not an extra template close");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::Widget::operator<=>(int)"),
                "operator spaceship is not an extra template close");
    expect_true(peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N > 0),int>>()"),
                "template relation does not close an outer template");
    expect_true(peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N>0),int>>()"),
                "template relation without whitespace remains valid");
    expect_true(peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N >0),int>>()"),
                "template relation with left whitespace remains valid");
    expect_true(peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N> 0),int>>()"),
                "template relation with right whitespace remains valid");
    expect_true(peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N<0),int>>()"),
                "less-than template relation remains valid");
    expect_true(peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N>M),int>>()") &&
                    peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N >M),int>>()") &&
                    peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N> M),int>>()") &&
                    peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N > M),int>>()"),
                "greater-than relation whitespace does not affect validation");
    expect_true(peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N<M),int>>()") &&
                    peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N <M),int>>()") &&
                    peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N< M),int>>()") &&
                    peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N < M),int>>()"),
                "less-than relation whitespace does not affect validation");
    expect_true(peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(sizeof(T)>sizeof(U)),int>>()"),
                "sizeof relation remains valid");
    expect_true(peak_target_resolver_validate_selector(
                    "module!foo(decltype(N>M))") &&
                    peak_target_resolver_validate_selector(
                    "module!foo(decltype(N >M))") &&
                    peak_target_resolver_validate_selector(
                    "module!foo(decltype(N> M))") &&
                    peak_target_resolver_validate_selector(
                    "module!foo(decltype(N > M))"),
                "decltype greater-than relation remains valid");
    expect_true(peak_target_resolver_validate_selector(
                    "module!foo(decltype(N<M))") &&
                    peak_target_resolver_validate_selector(
                    "module!foo(decltype(N <M))") &&
                    peak_target_resolver_validate_selector(
                    "module!foo(decltype(N< M))") &&
                    peak_target_resolver_validate_selector(
                    "module!foo(decltype(N < M))"),
                "decltype less-than relation remains valid");
    expect_true(peak_target_resolver_validate_selector(
                    "module!f<decltype(std::vector<int>{})>()"),
                "decltype nested type remains valid");
    expect_true(peak_target_resolver_validate_selector(
                    "module!int (*array_bound_return<2,1>()) [((2)>(1))]"),
                "array-bound comparison signature remains valid");
    expect_true(peak_target_resolver_validate_selector(
                    "module!decltype(values[((2)>(1))]) selector_array_value()"),
                "decltype bracket expression signature remains valid");
    expect_true(peak_target_resolver_validate_selector(
                    "module!decltype(Box{((2)>(1))}) selector_brace_value()"),
                "decltype brace expression signature remains valid");
    expect_true(peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N>=0),int>>()") &&
                    peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N >=0),int>>()") &&
                    peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N>= 0),int>>()") &&
                    peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N >= 0),int>>()"),
                "greater-equal relation whitespace does not affect validation");
    expect_true(!peak_target_resolver_validate_selector(
                    "module!f<std::enable_if_t<(N=>M),int>>()"),
                "reversed greater-equal is invalid");
    expect_true(peak_target_resolver_validate_selector(
                    "module!foo(std::vector<int>)"),
                "template in a parameter list remains valid");
    expect_true(!peak_target_resolver_validate_selector(
                    "module!foo(std::vector<int)"),
                "unclosed parameter template is invalid");
    expect_true(!peak_target_resolver_validate_selector("module!foo(int>)"),
                "extra parameter template close is invalid");
    expect_true(!peak_target_resolver_validate_selector(
                    "module!f<foo(FixedString<3)>()"),
                "unclosed template in a call frame is invalid");
    expect_true(!peak_target_resolver_validate_selector("module!foo([)]"),
                "mismatched bracket closer is invalid");
    expect_true(!peak_target_resolver_validate_selector("module!foo([x)"),
                "mismatched nested closer is invalid");
    expect_true(!peak_target_resolver_validate_selector("module!foo([x"),
                "unclosed bracket is invalid");
    expect_true(!peak_target_resolver_validate_selector("module!foo({x"),
                "unclosed brace is invalid");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::Widget::operator[](int)"),
                "operator bracket remains valid");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::operator<< <T>(T)"),
                "template operator shift-left keeps its template opener");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::operator >> <T>(T)"),
                "spaced template operator shift-right keeps its template opener");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::operator+<T>(T)"),
                "template operator plus keeps its adjacent template opener");
    expect_true(peak_target_resolver_validate_selector(
                    "module!ns::operator<<<T>(T)"),
                "template operator shift-left keeps its adjacent template opener");
    expect_true(!peak_target_resolver_validate_selector("module!symbol!extra"),
                "two top-level module separators are invalid");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "Derived::target(int)",
                    "non-virtual thunk to Derived::target(int)"),
                "ordinary selector excludes non-virtual thunk");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "Derived::target(int)",
                    "virtual thunk to Derived::target(int)"),
                "ordinary selector excludes virtual thunk");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "Derived::target(int)",
                    "covariant return thunk to Derived::target(int)"),
                "ordinary selector excludes covariant thunk");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "Derived::target(int)",
                    "transaction clone for Derived::target(int)"),
                "ordinary selector excludes transaction clone");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "Derived::target(int)",
                    "non-transaction clone for Derived::target(int)"),
                "ordinary selector excludes non-transaction clone");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "function_pointer_return<int>(int)",
                    "int (*function_pointer_return<int>(int))(int) [clone .constprop.0]"),
                "name-only selector excludes GCC IPA clone");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "int (*(*nested_pointer_return<int>(int))())(int)",
                    "int (*(*nested_pointer_return<int>(int))())(int) [clone .isra.0]"),
                "return-prefixed selector excludes GCC IPA clone");
    expect_true(peak_target_resolver_full_signature_matches(
                    "non-virtual thunk to Derived::target(int)",
                    "non-virtual thunk to Derived::target(int)"),
                "full artificial signature remains exact-matchable");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "const", "int peak_test::Widget::func(int) const"),
                "qualifier-only selector does not match a full signature suffix");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "(int) const", "int peak_test::Widget::func(int) const"),
                "parameter-only selector does not match a full signature suffix");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "<int>(peak_test::TemplateOperatorValue const&, int)",
                    "int peak_test::operator<< <int>(peak_test::TemplateOperatorValue const&, int)"),
                "operator template fragment does not match a full signature suffix");
    expect_true(peak_target_resolver_full_signature_matches(
                    "peak_test::operator<< <int>(peak_test::TemplateOperatorValue const&, int)",
                    "int peak_test::operator<< <int>(peak_test::TemplateOperatorValue const&, int)"),
                "complete return-prefixed operator signature matches");
    expect_true(peak_target_resolver_full_signature_matches(
                    "multiword_return<int>(int)",
                    "unsigned long multiword_return<int>(int)"),
                "name-only multiword return signature matches");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "long multiword_return<int>(int)",
                    "unsigned long multiword_return<int>(int)"),
                "partial multiword return prefix does not match");
    expect_true(peak_target_resolver_full_signature_matches(
                    "const_ref_return<int>()", "int const& const_ref_return<int>()"),
                "name-only const-reference return signature matches");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "const& const_ref_return<int>()", "int const& const_ref_return<int>()"),
                "partial const-reference prefix does not match");
    expect_true(peak_target_resolver_full_signature_matches(
                    "function_pointer_return<int>(int)",
                    "int (*function_pointer_return<int>(int))(int)"),
                "name-only function-pointer return signature matches");
    expect_true(peak_target_resolver_full_signature_matches(
                    "noexcept_pointer_return<2,1>()",
                    "int (*noexcept_pointer_return<2,1>())() noexcept"),
                "name-only noexcept-pointer return signature matches");
    expect_true(peak_target_resolver_full_signature_matches(
                    "array_return<2,1>()",
                    "int (*array_return<2,1>()) [((2)>(1))]"),
                "name-only array return signature matches");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "long function_pointer_return<int>(int)",
                    "int (*function_pointer_return<int>(int))(int)"),
                "partial function-pointer return prefix does not match");
    expect_true(peak_target_resolver_full_signature_matches(
                    "plain_operator_type_return<int>(int)",
                    "operator_type plain_operator_type_return<int>(int)"),
                "return type containing operator does not alter the name boundary");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "type plain_operator_type_return<int>(int)",
                    "operator_type plain_operator_type_return<int>(int)"),
                "partial operator-like return type does not match");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "CallbackType()",
                    "decltype (new CallbackType()) allocation_outer<CallbackType>(CallbackType)"),
                "callable return-type fragment is not the exported declarator");
    expect_true(peak_target_resolver_full_signature_matches(
                    "allocation_outer<CallbackType>(CallbackType)",
                    "decltype (new CallbackType()) allocation_outer<CallbackType>(CallbackType)"),
                "allocation outer name-only signature matches");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "Q::f(int)", "int Q::f(int) const"),
                "missing const qualifier does not match");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "Q::f(int)", "int Q::f(int) &"),
                "missing lvalue-reference qualifier does not match");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "Q::f(int)", "int Q::f(int) noexcept"),
                "missing noexcept qualifier does not match");
    expect_true(peak_target_resolver_full_signature_matches(
                    "Q::f(int) const", "int Q::f(int) const"),
                "complete const-qualified signature matches");
    expect_true(peak_target_resolver_full_signature_matches(
                    "Q::f(int) noexcept", "int Q::f(int) noexcept"),
                "complete noexcept-qualified signature matches when demangled");
    expect_true(peak_target_resolver_full_signature_matches(
                    "less_return<int,int>(int,int)",
                    "decltype ({parm#1}<{parm#2}) less_return<int,int>(int,int)"),
                "return expression relation does not hide the exported declarator");
    expect_true(peak_target_resolver_full_signature_matches(
                    "shift_return<int>(int)",
                    "decltype ({parm#1}<<(1)) shift_return<int>(int)"),
                "return expression shift does not hide the exported declarator");
    expect_true(peak_target_resolver_full_signature_matches(
                    "nested_pointer_return<int>(int)",
                    "int (*(*nested_pointer_return<int>(int))())(int)"),
                "nested pointer-return declarator remains eligible");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "new(unsigned long)", "void* peak_test::OperatorSurface::operator new(unsigned long)"),
                "operator new suffix is not an independent function id");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "delete(void*)", "void peak_test::OperatorSurface::operator delete(void*)"),
                "operator delete suffix is not an independent function id");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "bool() const", "peak_test::OperatorSurface::operator bool() const"),
                "conversion operator suffix is not an independent function id");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "long() const", "peak_test::OperatorSurface::operator unsigned long() const"),
                "multiword conversion operator suffix is not an independent function id");
    expect_true(peak_target_resolver_full_signature_matches(
                    "peak_test::operator\"\" _peak_literal(unsigned long long)",
                    "unsigned long long peak_test::operator\"\" _peak_literal(unsigned long long)"),
                "full user-defined literal selector matches");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "_peak_literal(unsigned long long)",
                    "unsigned long long peak_test::operator\"\" _peak_literal(unsigned long long)"),
                "literal suffix is not an independent function id");
    expect_true(peak_target_resolver_full_signature_matches(
                    "conversion_pointer_return<peak_test::ConversionPointerSource>(peak_test::ConversionPointerSource)",
                    "decltype (&peak_test::ConversionPointerSource::operator int) conversion_pointer_return<peak_test::ConversionPointerSource>(peak_test::ConversionPointerSource)"),
                "operator in a completed return-expression frame does not hide the declarator");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "int() const",
                    "decltype (&peak_test::ConversionPointerSource::operator int) conversion_pointer_return<peak_test::ConversionPointerSource>(peak_test::ConversionPointerSource)"),
                "return-expression conversion suffix is not an independent function id");
    expect_true(peak_target_resolver_full_signature_matches(
                    "probe::concrete_angle_plus_return<int>(int)",
                    "probe::Holder<&probe::operator+> probe::concrete_angle_plus_return<int>(int)"),
                "operator in a return template does not hide the declarator");
    expect_true(!peak_target_resolver_full_signature_matches(
                    "operator+(probe::OperatorValue)",
                    "probe::Holder<&probe::operator+> probe::concrete_angle_plus_return<int>(int)"),
                "return-template operator suffix is not an independent function id");
    expect_true(resolve("module!symbol+0xffffffffffffffffffffffffffff", &resolution) ==
                    PEAK_TARGET_RESOLVE_INVALID,
                "overflowing offset is invalid");
    peak_target_resolution_clear(&resolution);

    g_free(strong_selector);
    g_free(weak_selector);
    g_free(offset_selector);
    g_free(mangled_selector);
    g_free(module_b_selector);
    g_free(module_a_selector);
    gum_deinit_embedded();
    if (failures != 0) {
        return 1;
    }
    puts("target_resolver_test_ok");
    return 0;
}
