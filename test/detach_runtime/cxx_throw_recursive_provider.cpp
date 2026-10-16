#include <cerrno>
#include <exception>

namespace {
thread_local unsigned destroyed;
thread_local unsigned unwound;
struct Guard {
    int count = std::uncaught_exceptions();
    ~Guard() noexcept {
        ++destroyed;
        if (std::uncaught_exceptions() > count) {
            ++unwound;
            errno = EDOM;
        } else {
            errno = ERANGE;
        }
    }
};
}

extern "C" __attribute__((visibility("default"), noinline, noclone))
float peak_cxx_throw_inner(unsigned mode)
{
    Guard guard;
    if (mode) throw 17;
    return -0x1.5p+3f;
}

extern "C" __attribute__((visibility("default"), noinline, noclone))
float peak_cxx_throw_outer(unsigned mode)
{
    Guard guard;
    try {
        return peak_cxx_throw_inner(mode);
    } catch (int value) {
        if (mode == 2 && value == 17) return 23.0f;
        throw;
    }
}

extern "C" __attribute__((visibility("default"), noinline, noclone))
float peak_cxx_throw_recursive(unsigned depth)
{
    Guard guard;
    if (depth != 0) return peak_cxx_throw_recursive(depth - 1);
    throw 29;
}

extern "C" __attribute__((visibility("default")))
unsigned peak_cxx_throw_destroyed() { return destroyed; }
extern "C" __attribute__((visibility("default")))
unsigned peak_cxx_throw_unwound() { return unwound; }
