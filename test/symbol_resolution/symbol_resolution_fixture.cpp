#ifndef PEAK_FIXTURE_VALUE
#define PEAK_FIXTURE_VALUE 0
#endif

#include <cstddef>
#include <new>

extern "C" int peak_symbol_legacy_helper(int value);

template <typename T>
__attribute__((used))
void selector_decltype_syntax_template()
{
}

template <int N, int M>
__attribute__((used))
void selector_decltype_syntax_proof()
{
    selector_decltype_syntax_template<decltype(N > M)>();
    selector_decltype_syntax_template<decltype(N < M)>();
}

template void selector_decltype_syntax_proof<2, 1>();

struct SelectorSyntaxBox {
    int value;
};

int selector_syntax_values[2];

template <int N, int M>
__attribute__((used))
void selector_bracket_brace_syntax_proof()
{
    using ArrayValue = decltype(selector_syntax_values[((N) > (M))]);
    using BraceValue = decltype(SelectorSyntaxBox{((N) > (M))});

    static_assert(sizeof(ArrayValue) == sizeof(int));
    static_assert(sizeof(BraceValue) == sizeof(SelectorSyntaxBox));
}

template void selector_bracket_brace_syntax_proof<2, 1>();

template <int N, int M>
__attribute__((noinline, used, visibility("default")))
int (*array_bound_return()) [((N) > (M))]
{
    static int result[((N) > (M))];

    return &result;
}

template int (*array_bound_return<2, 1>()) [((2) > (1))];

template <typename T>
__attribute__((noinline, used, visibility("default")))
unsigned long multiword_return(int value)
{
    return static_cast<unsigned long>(peak_symbol_legacy_helper(value));
}

template unsigned long multiword_return<int>(int);

template <typename T>
__attribute__((noinline, used, visibility("default")))
const T& const_ref_return()
{
    static const T value{};

    return value;
}

template const int& const_ref_return<int>();

__attribute__((noinline, used, visibility("default")))
int function_pointer_return_target(int value)
{
    return peak_symbol_legacy_helper(value);
}

template <typename T>
__attribute__((noinline, used, visibility("default")))
int (*function_pointer_return(T value))(int)
{
    (void)value;
    return function_pointer_return_target;
}

template int (*function_pointer_return<int>(int))(int);

/* Keep a GCC IPA clone-shaped exported name in the test DSO. This avoids
 * depending on an optimizer deciding to emit a clone for the regression. */
int (*function_pointer_return_clone_fixture(int))(int)
    __asm__("_Z23function_pointer_returnIiEPFiiET_.clone_for_test.0");

int (*function_pointer_return_clone_fixture(int value))(int)
{
    (void)value;
    return function_pointer_return_target;
}

__attribute__((noinline, used, visibility("default")))
int noexcept_pointer_return_target() noexcept
{
    return PEAK_FIXTURE_VALUE;
}

template <int N, int M>
__attribute__((noinline, used, visibility("default")))
auto noexcept_pointer_return() -> int (*)() noexcept
{
    static_assert(N > M);
    return noexcept_pointer_return_target;
}

template auto noexcept_pointer_return<2, 1>() -> int (*)() noexcept;

template <int N, int M>
__attribute__((noinline, used, visibility("default")))
auto array_return() -> int (*)[((N) > (M))]
{
    static int result[((N) > (M))];

    return &result;
}

template auto array_return<2, 1>() -> int (*)[((2) > (1))];

struct operator_type {
    int value;
};

template <typename T>
__attribute__((noinline, used, visibility("default")))
operator_type plain_operator_type_return(T value)
{
    return {static_cast<int>(value)};
}

template operator_type plain_operator_type_return<int>(int);

struct CallbackType {
};

template <typename T>
__attribute__((noinline, used, visibility("default")))
decltype(new T()) allocation_outer(T)
{
    return new T();
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunevaluated-expression"
#endif
template decltype(new CallbackType()) allocation_outer<CallbackType>(CallbackType);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

template <typename T, typename U>
auto less_return(T first, U second) -> decltype(first < second)
{
    return first < second;
}

template bool less_return<int, int>(int, int);

template <typename T>
auto shift_return(T first) -> decltype(first << 1)
{
    return first << 1;
}

template int shift_return<int>(int);

__attribute__((noinline, used, visibility("default")))
int nested_pointer_return_target(int value)
{
    return peak_symbol_legacy_helper(value);
}

__attribute__((noinline, used, visibility("default")))
int (*nested_pointer_return_middle())(int)
{
    return nested_pointer_return_target;
}

template <typename T>
__attribute__((noinline, used, visibility("default")))
int (*(*nested_pointer_return(T))())(int)
{
    return nested_pointer_return_middle;
}

template int (*(*nested_pointer_return<int>(int))())(int);

namespace peak_test {
struct QualifierOnly {
    __attribute__((noinline, used, visibility("default")))
    int const_method(int value) const
    {
        return peak_symbol_legacy_helper(value + PEAK_FIXTURE_VALUE);
    }

    __attribute__((noinline, used, visibility("default")))
    int lref_method(int value) &
    {
        return peak_symbol_legacy_helper(value + PEAK_FIXTURE_VALUE);
    }

    __attribute__((noinline, used, visibility("default")))
    int noexcept_method(int value) noexcept
    {
        return peak_symbol_legacy_helper(value + PEAK_FIXTURE_VALUE);
    }
};

struct OperatorSurface {
    static __attribute__((noinline, used, visibility("default")))
    void* operator new(std::size_t size)
    {
        return ::operator new(size);
    }

    static __attribute__((noinline, used, visibility("default")))
    void operator delete(void* pointer) noexcept
    {
        ::operator delete(pointer);
    }

    __attribute__((noinline, used, visibility("default")))
    operator bool() const
    {
        return peak_symbol_legacy_helper(PEAK_FIXTURE_VALUE) != -1;
    }

    __attribute__((noinline, used, visibility("default")))
    operator unsigned long() const
    {
        return static_cast<unsigned long>(peak_symbol_legacy_helper(PEAK_FIXTURE_VALUE));
    }
};

__attribute__((noinline, used, visibility("default")))
unsigned long long operator""_peak_literal(unsigned long long value)
{
    return static_cast<unsigned long long>(
        peak_symbol_legacy_helper(static_cast<int>(value) + PEAK_FIXTURE_VALUE));
}

struct ConversionPointerSource {
    __attribute__((noinline, used, visibility("default")))
    operator int() const
    {
        return peak_symbol_legacy_helper(PEAK_FIXTURE_VALUE);
    }
};
}

template <typename T>
__attribute__((noinline, used, visibility("default")))
auto conversion_pointer_return(T) -> decltype(&T::operator int)
{
    return &T::operator int;
}

template auto conversion_pointer_return<peak_test::ConversionPointerSource>(
    peak_test::ConversionPointerSource) -> decltype(&peak_test::ConversionPointerSource::operator int);

namespace probe {
struct OperatorValue {
};

__attribute__((noinline, used, visibility("default")))
int operator+(OperatorValue)
{
    return peak_symbol_legacy_helper(PEAK_FIXTURE_VALUE);
}

template <auto Function>
struct Holder {
};

template <typename T>
__attribute__((noinline, used, visibility("default")))
Holder<&operator+> concrete_angle_plus_return(T)
{
    return {};
}

template Holder<&operator+> concrete_angle_plus_return<int>(int);
}

struct ThunkBaseA {
    virtual ~ThunkBaseA() = default;
    virtual int anchor();
};

struct ThunkBaseB {
    virtual ~ThunkBaseB() = default;
    virtual int target(int value);
};

struct Derived : ThunkBaseA, ThunkBaseB {
    __attribute__((noinline, used, visibility("default")))
    int target(int value) override;
};

int ThunkBaseA::anchor()
{
    return 0;
}

int ThunkBaseB::target(int value)
{
    return value;
}

int Derived::target(int value)
{
    return peak_symbol_legacy_helper(value + PEAK_FIXTURE_VALUE);
}

namespace peak_test {
struct TemplateOperatorValue {
};

template <typename T>
__attribute__((noinline, used, visibility("default")))
int operator<<(const TemplateOperatorValue&, T value)
{
    return peak_symbol_legacy_helper(static_cast<int>(value) + PEAK_FIXTURE_VALUE);
}

template int operator<< <int>(const TemplateOperatorValue&, int);

template <typename T>
__attribute__((noinline, used, visibility("default")))
int operator>>(const TemplateOperatorValue&, T value)
{
    return peak_symbol_legacy_helper(static_cast<int>(value) + PEAK_FIXTURE_VALUE);
}

template int operator>> <int>(const TemplateOperatorValue&, int);

template <typename T>
__attribute__((noinline, used, visibility("default")))
int operator+(const TemplateOperatorValue&, T value)
{
    return peak_symbol_legacy_helper(static_cast<int>(value) + PEAK_FIXTURE_VALUE);
}

template int operator+<int>(const TemplateOperatorValue&, int);

struct Widget {
    static int func(int value, double scale);
    static int func(double value);
    int operator()(int value) const;
    int operator[](int value) const;
    bool operator!() const;
    bool operator!=(const Widget& other) const;
};

__attribute__((noinline, used, visibility("default")))
int Widget::func(int value, double scale)
{
    return value + static_cast<int>(scale) + PEAK_FIXTURE_VALUE;
}

__attribute__((noinline, used, visibility("default")))
int Widget::func(double value)
{
    return static_cast<int>(value) + PEAK_FIXTURE_VALUE;
}

__attribute__((noinline, used, visibility("default")))
int Widget::operator()(int value) const
{
    return value + PEAK_FIXTURE_VALUE;
}

__attribute__((noinline, used, visibility("default")))
int Widget::operator[](int value) const
{
    return peak_symbol_legacy_helper(value + PEAK_FIXTURE_VALUE);
}

__attribute__((noinline, used, visibility("default")))
bool Widget::operator!() const
{
    return peak_symbol_legacy_helper(PEAK_FIXTURE_VALUE) == -1;
}

__attribute__((noinline, used, visibility("default")))
bool Widget::operator!=(const Widget& other) const
{
    return peak_symbol_legacy_helper(this != &other) != 0;
}

struct NamespacedReturn {
    int value;
};

template <typename T>
__attribute__((noinline, used, visibility("default")))
T template_func(T value)
{
    return value;
}

template int template_func<int>(int);

template <typename T>
__attribute__((noinline, used, visibility("default")))
NamespacedReturn namespaced_template(T value)
{
    return {static_cast<int>(value)};
}

template NamespacedReturn namespaced_template<int>(int);

__attribute__((noinline, used, visibility("default")))
int collision()
{
    return PEAK_FIXTURE_VALUE;
}

__attribute__((noinline, used, visibility("default")))
int legacy_short_unique(int value)
{
    int first = peak_symbol_legacy_helper(value + PEAK_FIXTURE_VALUE);
    int second = peak_symbol_legacy_helper(first + value);

    return second + PEAK_FIXTURE_VALUE;
}
}

extern "C" __attribute__((noinline, used, visibility("default")))
int collision(void)
{
    return PEAK_FIXTURE_VALUE;
}

extern "C" __attribute__((visibility("default")))
int peak_symbol_strong_alias(void)
{
    return PEAK_FIXTURE_VALUE;
}

#if defined(__ELF__)
extern "C" int peak_symbol_weak_alias(void)
    __attribute__((weak, alias("peak_symbol_strong_alias"), visibility("default")));
#else
extern "C" __attribute__((visibility("default")))
int peak_symbol_weak_alias(void)
{
    return peak_symbol_strong_alias();
}
#endif

extern "C" __attribute__((visibility("default")))
int peak_symbol_fixture_invoke(void)
{
    Derived derived;

    return peak_test::Widget::func(1, 2.0) +
           peak_test::template_func<int>(3) +
           peak_test::Widget{}(4) +
           peak_test::namespaced_template<int>(5).value +
           peak_test::legacy_short_unique(6) +
           (peak_test::TemplateOperatorValue{} << 7) +
           (peak_test::TemplateOperatorValue{} >> 8) +
           (peak_test::TemplateOperatorValue{} + 9) +
           peak_test::Widget{}[7] +
           static_cast<int>(multiword_return<int>(8)) +
           const_ref_return<int>() +
           function_pointer_return<int>(9)(10) +
           noexcept_pointer_return<2, 1>()() +
           (*array_return<2, 1>())[0] +
           plain_operator_type_return<int>(11).value +
           (delete allocation_outer<CallbackType>(CallbackType{}), 0) +
           less_return<int, int>(12, 13) +
           shift_return<int>(14) +
           nested_pointer_return<int>(15)()(16) +
           peak_test::QualifierOnly{}.const_method(12) +
           ([] { peak_test::QualifierOnly value; return value.lref_method(13); })() +
           peak_test::QualifierOnly{}.noexcept_method(14) +
           (delete new peak_test::OperatorSurface, 0) +
           static_cast<bool>(peak_test::OperatorSurface{}) +
           static_cast<int>(static_cast<unsigned long>(peak_test::OperatorSurface{})) +
           static_cast<int>(peak_test::operator""_peak_literal(17)) +
           (peak_test::ConversionPointerSource{}.*
                conversion_pointer_return<peak_test::ConversionPointerSource>(
                    peak_test::ConversionPointerSource{}))() +
           sizeof(probe::concrete_angle_plus_return<int>(18)) +
           (!peak_test::Widget{}) +
           (peak_test::Widget{} != peak_test::Widget{}) +
           static_cast<ThunkBaseB*>(&derived)->target(7);
}
