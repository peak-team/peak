/* App/provider use only their ordinary C++ ABI. Run the exact pair natively
 * and preloaded; no PEAK/Gum calls can repair an exception in this oracle. */
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <thread>

extern "C" float peak_cxx_throw_inner(unsigned);
extern "C" float peak_cxx_throw_outer(unsigned);
extern "C" unsigned peak_cxx_throw_destroyed();
extern "C" unsigned peak_cxx_throw_unwound();

namespace {
thread_local unsigned app_destroyed;
struct AppGuard { ~AppGuard() noexcept { ++app_destroyed; } };

bool exercise()
{
    constexpr unsigned iterations = 24;
    unsigned expected_destroyed = 0, expected_unwound = 0, expected_app = 0;
    for (unsigned iteration = 0; iteration < iterations; ++iteration) {
        for (unsigned mode = 0; mode < 3; ++mode) {
            errno = EINTR;
            try {
                AppGuard app;
                float result = peak_cxx_throw_outer(mode);
                int observed_errno = errno;
                std::uint32_t bits;
                std::memcpy(&bits, &result, sizeof(bits));
                if (mode == 1 || observed_errno != ERANGE ||
                    (mode == 0 && bits != UINT32_C(0xc1280000)) ||
                    (mode == 2 && result != 23.0f)) return false;
            } catch (int value) {
                if (mode != 1 || value != 17 || errno != EDOM) return false;
            } catch (...) { return false; }
            expected_destroyed += 2;
            expected_unwound += mode == 1 ? 2 : (mode == 2 ? 1 : 0);
            ++expected_app;
            if (peak_cxx_throw_destroyed() != expected_destroyed ||
                peak_cxx_throw_unwound() != expected_unwound ||
                app_destroyed != expected_app) return false;
        }
    }
    return true;
}
}

int main()
{
    bool ok = exercise();
    bool worker_ok = false;
    std::thread worker([&] { worker_ok = exercise(); });
    worker.join();
    if (!ok || !worker_ok) {
        std::fputs("C++ exception/RAII/errno transparency mismatch\n", stderr);
        return 1;
    }
    std::puts("cxx_throw_transparency_ok threads=2 calls=288 propagated=48 caught_inside=48");
    return 0;
}
