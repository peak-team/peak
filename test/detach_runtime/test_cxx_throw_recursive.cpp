/* App/provider use only their ordinary C++ ABI. Run the exact pair natively
 * and preloaded; no PEAK/Gum calls can repair an exception in this oracle. */
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <thread>

#ifdef PEAK_CXX_THROW_LATE_PROVIDER
#include <chrono>
#include <condition_variable>
#include <dlfcn.h>
#include <mutex>
static float (*peak_cxx_throw_outer)(unsigned);
static float (*peak_cxx_throw_recursive)(unsigned);
static unsigned (*peak_cxx_throw_destroyed)();
static unsigned (*peak_cxx_throw_unwound)();
#else
extern "C" float peak_cxx_throw_inner(unsigned);
extern "C" float peak_cxx_throw_outer(unsigned);
extern "C" float peak_cxx_throw_recursive(unsigned);
extern "C" unsigned peak_cxx_throw_destroyed();
extern "C" unsigned peak_cxx_throw_unwound();
#endif

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
        errno = EINTR;
        try {
            AppGuard app;
            (void)peak_cxx_throw_recursive(3);
            return false;
        } catch (int value) {
            if (value != 29 || errno != EDOM) return false;
        } catch (...) { return false; }
        expected_destroyed += 4;
        expected_unwound += 4;
        ++expected_app;
        if (peak_cxx_throw_destroyed() != expected_destroyed ||
            peak_cxx_throw_unwound() != expected_unwound ||
            app_destroyed != expected_app) return false;
    }
    return true;
}
}

#ifndef PEAK_CXX_THROW_LATE_PROVIDER
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
    std::puts("cxx_throw_transparency_ok threads=2 calls=480 propagated=96 caught_inside=48 recursive=192");
    return 0;
}
#else
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    std::mutex mutex;
    std::condition_variable condition;
    bool waiting = false, ready = false, abort = false, worker_ok = false;
    std::thread worker([&] {
        std::unique_lock<std::mutex> lock(mutex);
        waiting = true;
        condition.notify_all();
        condition.wait(lock, [&] { return ready; });
        bool run = !abort;
        lock.unlock();
        if (run) worker_ok = exercise();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return waiting; });
    }
    /* PEAK is preloaded; only the ordinary application provider is loaded
     * here. The worker already exists before this DSO and its TLS are added. */
    void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    bool loaded = false;
    if (module != nullptr) {
        peak_cxx_throw_outer = reinterpret_cast<decltype(peak_cxx_throw_outer)>(
            dlsym(module, "peak_cxx_throw_outer"));
        peak_cxx_throw_recursive = reinterpret_cast<decltype(peak_cxx_throw_recursive)>(
            dlsym(module, "peak_cxx_throw_recursive"));
        peak_cxx_throw_destroyed = reinterpret_cast<decltype(peak_cxx_throw_destroyed)>(
            dlsym(module, "peak_cxx_throw_destroyed"));
        peak_cxx_throw_unwound = reinterpret_cast<decltype(peak_cxx_throw_unwound)>(
            dlsym(module, "peak_cxx_throw_unwound"));
        loaded = peak_cxx_throw_outer && peak_cxx_throw_recursive &&
                 peak_cxx_throw_destroyed && peak_cxx_throw_unwound;
    }
    /* Deferred target attachment does not promise observation of the first
     * call immediately after dlopen. Match the existing late-target fixture. */
    if (loaded) std::this_thread::sleep_for(std::chrono::seconds(1));
    {
        std::lock_guard<std::mutex> lock(mutex);
        abort = !loaded;
        ready = true;
    }
    condition.notify_all();
    bool ok = loaded && exercise();
    worker.join();
    bool closed = module != nullptr && dlclose(module) == 0;
    if (!ok || !worker_ok || !closed) {
        std::fputs("late-provider C++ exception/RAII/errno mismatch\n", stderr);
        return 1;
    }
    std::puts("cxx_throw_late_provider_ok threads=2 calls=480 propagated=96 caught_inside=48 recursive=192");
    return 0;
}
#endif
