#include "cuda_test_common.cuh"

#include <cuda.h>
#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace {
constexpr int kDriverCycles = 12;

constexpr char kContextPtx[] = R"ptx(
.version 6.4
.target sm_52
.address_size 64

.visible .entry peak_cuda_context_driver_zero(
    .param .u64 peak_cuda_context_driver_zero_param_0)
{
    .reg .b32 %r<3>;
    .reg .b64 %rd<2>;

    ld.param.u64 %rd1, [peak_cuda_context_driver_zero_param_0];
    ld.global.u32 %r1, [%rd1];
    add.u32 %r2, %r1, 1;
    st.global.u32 [%rd1], %r2;
    ret;
}

.visible .entry peak_cuda_context_driver_one(
    .param .u64 peak_cuda_context_driver_one_param_0)
{
    .reg .b32 %r<3>;
    .reg .b64 %rd<2>;

    ld.param.u64 %rd1, [peak_cuda_context_driver_one_param_0];
    ld.global.u32 %r1, [%rd1];
    add.u32 %r2, %r1, 1;
    st.global.u32 [%rd1], %r2;
    ret;
}

.visible .entry peak_cuda_context_harvester_marker()
{
    ret;
}
)ptx";

struct ExplicitContextFixture {
    CUcontext context = nullptr;
    CUmodule module = nullptr;
    CUfunction function = nullptr;
    CUfunction marker = nullptr;
    CUstream stream = nullptr;
    CUdeviceptr value = 0;
};

bool
check_driver(CUresult result, const char* operation)
{
    if (result == CUDA_SUCCESS) {
        return true;
    }
    const char* name = nullptr;
    (void)cuGetErrorName(result, &name);
    std::fprintf(stderr, "cuda_test_error: %s failed: %s\n", operation,
                 name != nullptr ? name : "unknown CUDA Driver error");
    return false;
}

CUresult
create_context(CUcontext* context, CUdevice device)
{
#if CUDA_VERSION >= 13000
    CUctxCreateParams params = {};
    return cuCtxCreate(context, &params, CU_CTX_SCHED_AUTO, device);
#else
    return cuCtxCreate(context, CU_CTX_SCHED_AUTO, device);
#endif
}

bool
check_current_context(CUcontext expected, const char* operation)
{
    CUcontext current = nullptr;
    if (!check_driver(cuCtxGetCurrent(&current), operation)) {
        return false;
    }
    if (current != expected) {
        std::fprintf(stderr,
                     "cuda_test_error: %s changed the current context\n",
                     operation);
        return false;
    }
    return true;
}

void
destroy_unlaunched_fixture(ExplicitContextFixture* fixture)
{
    if (fixture->context == nullptr) {
        return;
    }
    CUcontext current = nullptr;
    if (cuCtxGetCurrent(&current) == CUDA_SUCCESS &&
        current == fixture->context) {
        CUcontext popped = nullptr;
        (void)cuCtxPopCurrent(&popped);
    }
    (void)cuCtxDestroy(fixture->context);
    *fixture = {};
}

bool
initialize_fixture(ExplicitContextFixture* fixture, CUdevice device,
                   const char* function_name, CUcontext initial_context)
{
    if (!check_driver(create_context(&fixture->context, device),
                      "cuCtxCreate")) {
        return false;
    }

    bool initialized =
        check_driver(cuModuleLoadData(&fixture->module, kContextPtx),
                     "cuModuleLoadData") &&
        check_driver(cuModuleGetFunction(&fixture->function, fixture->module,
                                        function_name),
                     "cuModuleGetFunction") &&
        check_driver(cuModuleGetFunction(
                         &fixture->marker, fixture->module,
                         "peak_cuda_context_harvester_marker"),
                     "marker cuModuleGetFunction") &&
        check_driver(cuStreamCreate(&fixture->stream, CU_STREAM_NON_BLOCKING),
                     "cuStreamCreate") &&
        check_driver(cuMemAlloc(&fixture->value, sizeof(unsigned int)),
                     "cuMemAlloc") &&
        check_driver(cuMemsetD32Async(fixture->value, 0, 1,
                                     fixture->stream),
                     "cuMemsetD32Async") &&
        check_driver(cuStreamSynchronize(fixture->stream),
                     "initial cuStreamSynchronize");

    CUcontext popped = nullptr;
    if (!check_driver(cuCtxPopCurrent(&popped), "setup cuCtxPopCurrent") ||
        popped != fixture->context) {
        std::fprintf(stderr,
                     "cuda_test_error: setup popped the wrong context\n");
        initialized = false;
    }
    if (!check_current_context(initial_context,
                               "setup context restoration")) {
        initialized = false;
    }
    if (!initialized) {
        destroy_unlaunched_fixture(fixture);
    }
    return initialized;
}

using ForceQueryErrorFunction = void (*)(int);
using HarvesterQueryCountFunction = unsigned long long (*)(void);
using HarvesterReadyFunction = int (*)(void);
using ActiveSlotCountFunction = unsigned long long (*)(void);

struct HarvesterSeams {
    ForceQueryErrorFunction force_query_error = nullptr;
    HarvesterQueryCountFunction query_count = nullptr;
    HarvesterReadyFunction ready = nullptr;
    ActiveSlotCountFunction active_slots = nullptr;
};

HarvesterSeams
load_harvester_seams()
{
    HarvesterSeams seams;
    seams.force_query_error = reinterpret_cast<ForceQueryErrorFunction>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_force_query_error_once"));
    seams.query_count = reinterpret_cast<HarvesterQueryCountFunction>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_harvester_query_count"));
    seams.ready = reinterpret_cast<HarvesterReadyFunction>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_harvester_ready"));
    seams.active_slots = reinterpret_cast<ActiveSlotCountFunction>(
        dlsym(RTLD_DEFAULT, "peak_cuda_test_active_slot_count"));
    return seams;
}

bool
wait_for_query_after(const HarvesterSeams& seams,
                     unsigned long long previous)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (seams.query_count() > previous) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool
wait_for_no_active_slots(const HarvesterSeams& seams)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (seams.active_slots() == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool
launch_marker(ExplicitContextFixture* fixture, CUcontext initial_context)
{
    if (!check_driver(cuCtxPushCurrent(fixture->context),
                      "marker cuCtxPushCurrent") ||
        !check_current_context(fixture->context,
                               "marker context before cuLaunchKernel")) {
        return false;
    }
    bool valid = check_driver(
                     cuLaunchKernel(fixture->marker, 1, 1, 1, 1, 1, 1, 0,
                                    fixture->stream, nullptr, nullptr),
                     "marker cuLaunchKernel") &&
                 check_current_context(
                     fixture->context,
                     "marker context after cuLaunchKernel") &&
                 check_driver(cuStreamSynchronize(fixture->stream),
                              "marker cuStreamSynchronize");
    CUcontext popped = nullptr;
    if (!check_driver(cuCtxPopCurrent(&popped), "marker cuCtxPopCurrent") ||
        popped != fixture->context ||
        !check_current_context(initial_context,
                               "marker context restoration")) {
        valid = false;
    }
    return valid;
}

bool
initialize_harvester(ExplicitContextFixture* fixture,
                     CUcontext initial_context,
                     const HarvesterSeams& seams,
                     int* warmup_attempts)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(1);
    while (seams.ready() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        if (!launch_marker(fixture, initial_context)) {
            return false;
        }
        ++*warmup_attempts;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (seams.ready() == 0) {
        return false;
    }
    unsigned long long previous = seams.query_count();
    return launch_marker(fixture, initial_context) &&
           wait_for_query_after(seams, previous) &&
           wait_for_no_active_slots(seams);
}

bool
launch_once(ExplicitContextFixture* fixture, CUcontext initial_context)
{
    if (!check_driver(cuCtxPushCurrent(fixture->context),
                      "launch cuCtxPushCurrent") ||
        !check_current_context(fixture->context,
                               "context before cuLaunchKernel")) {
        return false;
    }

    CUdeviceptr value = fixture->value;
    void* arguments[] = {&value};
    bool launched = check_driver(
        cuLaunchKernel(fixture->function, 1, 1, 1, 1, 1, 1, 0,
                       fixture->stream, arguments, nullptr),
        "cuLaunchKernel");
    if (!check_current_context(fixture->context,
                               "context after cuLaunchKernel")) {
        launched = false;
    }

    CUcontext popped = nullptr;
    if (!check_driver(cuCtxPopCurrent(&popped), "launch cuCtxPopCurrent") ||
        popped != fixture->context) {
        std::fprintf(stderr,
                     "cuda_test_error: launch popped the wrong context\n");
        launched = false;
    }
    if (!check_current_context(initial_context,
                               "launch context restoration")) {
        launched = false;
    }
    return launched;
}

bool
read_result(ExplicitContextFixture* fixture, CUcontext initial_context,
            unsigned int* result)
{
    if (!check_driver(cuCtxPushCurrent(fixture->context),
                      "result cuCtxPushCurrent")) {
        return false;
    }
    bool valid =
        check_driver(cuStreamSynchronize(fixture->stream),
                     "result cuStreamSynchronize") &&
        check_driver(cuMemcpyDtoH(result, fixture->value, sizeof(*result)),
                     "cuMemcpyDtoH");

    CUcontext popped = nullptr;
    if (!check_driver(cuCtxPopCurrent(&popped), "result cuCtxPopCurrent") ||
        popped != fixture->context) {
        std::fprintf(stderr,
                     "cuda_test_error: result read popped the wrong context\n");
        valid = false;
    }
    if (!check_current_context(initial_context,
                               "result context restoration")) {
        valid = false;
    }
    return valid;
}

#if CUDART_VERSION < 13000
__global__ void
peak_cuda_multi_device_kernel(int* value)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        atomicAdd(value, 1);
    }
}

bool
run_optional_multi_device_check(int device_count, bool* called)
{
    *called = false;
    if (device_count < 2) {
        return true;
    }

    cudaDeviceProp properties[2] = {};
    for (int device = 0; device < 2; ++device) {
        if (!peak_cuda_test_check(cudaGetDeviceProperties(&properties[device],
                                                          device),
                                  "cudaGetDeviceProperties")) {
            return false;
        }
        if (!properties[device].cooperativeMultiDeviceLaunch) {
            return true;
        }
    }

    cudaStream_t streams[2] = {};
    int* values[2] = {};
    cudaLaunchParams launches[2] = {};
    bool ready = true;
    for (int device = 0; device < 2 && ready; ++device) {
        ready = peak_cuda_test_check(cudaSetDevice(device), "cudaSetDevice") &&
                peak_cuda_test_check(
                    cudaStreamCreateWithFlags(&streams[device],
                                              cudaStreamNonBlocking),
                    "cudaStreamCreateWithFlags") &&
                peak_cuda_test_check(cudaMalloc(&values[device], sizeof(int)),
                                     "cudaMalloc") &&
                peak_cuda_test_check(cudaMemset(values[device], 0, sizeof(int)),
                                     "cudaMemset");
        if (ready) {
            launches[device].func =
                reinterpret_cast<void*>(peak_cuda_multi_device_kernel);
            launches[device].gridDim = dim3(1, 1, 1);
            launches[device].blockDim = dim3(1, 1, 1);
            launches[device].args = nullptr;
            launches[device].sharedMem = 0;
            launches[device].stream = streams[device];
        }
    }

    void* arguments[2][1] = {{&values[0]}, {&values[1]}};
    if (ready) {
        launches[0].args = arguments[0];
        launches[1].args = arguments[1];
        *called = true;
        ready = peak_cuda_test_check(
            cudaLaunchCooperativeKernelMultiDevice(launches, 2, 0),
            "cudaLaunchCooperativeKernelMultiDevice");
    }

    int host_values[2] = {};
    for (int device = 0; device < 2 && ready; ++device) {
        if (streams[device] == nullptr) {
            continue;
        }
        ready =
            peak_cuda_test_check(cudaSetDevice(device), "cudaSetDevice") &&
            peak_cuda_test_check(cudaStreamSynchronize(streams[device]),
                                 "cudaStreamSynchronize") &&
            peak_cuda_test_check(cudaMemcpy(&host_values[device],
                                            values[device], sizeof(int),
                                            cudaMemcpyDeviceToHost),
                                 "cudaMemcpy");
    }
    if (ready && (host_values[0] != 1 || host_values[1] != 1)) {
        std::fprintf(stderr,
                     "cuda_test_error: cooperative multi-device results are "
                     "%d/%d, expected 1/1\n",
                     host_values[0], host_values[1]);
        ready = false;
    }

    for (int device = 0; device < 2; ++device) {
        if (streams[device] == nullptr) {
            continue;
        }
        (void)cudaSetDevice(device);
        if (values[device] != nullptr) {
            (void)cudaFree(values[device]);
        }
        (void)cudaStreamDestroy(streams[device]);
    }
    return ready;
}
#endif
}

int
main()
{
    int device_count = 0;
    int requirement = peak_cuda_test_require_devices(1, &device_count);
    if (requirement != 0) {
        return requirement;
    }
    if (!check_driver(cuInit(0), "cuInit")) {
        return 1;
    }

    CUdevice physical_device = 0;
    CUcontext initial_context = nullptr;
    if (!check_driver(cuDeviceGet(&physical_device, 0), "cuDeviceGet") ||
        !check_driver(cuCtxGetCurrent(&initial_context),
                      "initial cuCtxGetCurrent")) {
        return 1;
    }

    ExplicitContextFixture fixtures[2];
    if (!initialize_fixture(&fixtures[0], physical_device,
                            "peak_cuda_context_driver_zero",
                            initial_context) ||
        !initialize_fixture(&fixtures[1], physical_device,
                            "peak_cuda_context_driver_one",
                            initial_context)) {
        destroy_unlaunched_fixture(&fixtures[0]);
        destroy_unlaunched_fixture(&fixtures[1]);
        return 1;
    }

    HarvesterSeams seams = load_harvester_seams();
    int seam_count = (seams.force_query_error != nullptr ? 1 : 0) +
                     (seams.query_count != nullptr ? 1 : 0) +
                     (seams.ready != nullptr ? 1 : 0) +
                     (seams.active_slots != nullptr ? 1 : 0);
    if (seam_count != 0 && seam_count != 4) {
        std::fprintf(stderr,
                     "cuda_test_error: incomplete PEAK CUDA harvester seam "
                     "set\n");
        return 1;
    }
    const bool query_error_injected = seam_count == 4;
    int initialization_warmups = 0;
    if (query_error_injected) {
        if (!initialize_harvester(&fixtures[0], initial_context, seams,
                                  &initialization_warmups)) {
            std::fprintf(stderr,
                         "cuda_test_error: CUDA harvester did not become "
                         "ready\n");
            return 1;
        }
        seams.force_query_error(1);
    }

    int context_restore_checks = 0;
    for (int cycle = 0; cycle < kDriverCycles; ++cycle) {
        for (ExplicitContextFixture& fixture : fixtures) {
            if (!launch_once(&fixture, initial_context)) {
                return 1;
            }
            ++context_restore_checks;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    unsigned int results[2] = {};
    if (!read_result(&fixtures[0], initial_context, &results[0]) ||
        !read_result(&fixtures[1], initial_context, &results[1])) {
        return 1;
    }
    if (results[0] != kDriverCycles || results[1] != kDriverCycles) {
        std::fprintf(stderr,
                     "cuda_test_error: Driver context results are %u/%u, "
                     "expected %d/%d\n",
                     results[0], results[1], kDriverCycles, kDriverCycles);
        return 1;
    }

    bool multi_device_called = false;
#if CUDART_VERSION < 13000
    if (!run_optional_multi_device_check(device_count,
                                         &multi_device_called)) {
        return 1;
    }
#endif
    const bool driver_names_available =
        dlsym(RTLD_DEFAULT, "cuFuncGetName") != nullptr;

    /*
     * PEAK's reusable timing events remain owned by these explicit contexts
     * until process-exit detach destroys the event pool. Destroying the
     * contexts here would invalidate those events first, so process teardown
     * intentionally reclaims this fixture's contexts and their resources.
     */
    std::printf("cuda_multi_context_ok physical_devices=%d "
                "explicit_contexts=2 driver_cycles=%d "
                "context_restore_checks=%d results=%u/%u "
                "function_handles_equal=%d multi_device_called=%d "
                "driver_names_available=%d query_error_injected=%d "
                "initialization_warmups=%d\n",
                device_count, kDriverCycles, context_restore_checks,
                results[0], results[1],
                fixtures[0].function == fixtures[1].function ? 1 : 0,
                multi_device_called ? 1 : 0,
                driver_names_available ? 1 : 0,
                query_error_injected ? 1 : 0,
                initialization_warmups);
    return 0;
}
