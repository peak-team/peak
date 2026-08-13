cmake_minimum_required(VERSION 3.13)

project(peak LANGUAGES C CXX)

include(GNUInstallDirs)
include(CheckIncludeFile)

option(PEAK_FETCH_DEPS
    "Allow CMake to download pinned dependencies when callers do not provide them"
    ON)
option(PEAK_ENABLE_OTF2 "Enable OTF2 memory-trace export" OFF)
option(PEAK_USE_SYSTEM_OTF2 "Find OTF2 in the system before fetching it" ON)
option(PEAK_ENABLE_FORTRAN_TESTS "Build tests that require a Fortran compiler" OFF)

check_include_file("sys/random.h" PEAK_HAVE_SYS_RANDOM_H)
if(PEAK_HAVE_SYS_RANDOM_H)
    add_compile_definitions(PEAK_HAVE_SYS_RANDOM_H=1)
else()
    add_compile_definitions(PEAK_HAVE_SYS_RANDOM_H=0)
endif()
message(STATUS "PEAK_HAVE_SYS_RANDOM_H = ${PEAK_HAVE_SYS_RANDOM_H}")

# Set build type to Release by default if CMAKE_BUILD_TYPE is not given
set(default_build_type "Release")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
  set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE STRING "Choose the type of build." FORCE)
endif()
message(STATUS "CMAKE_BUILD_TYPE = ${CMAKE_BUILD_TYPE}")

cmake_host_system_information(RESULT N_CPU QUERY NUMBER_OF_PHYSICAL_CORES)
message(STATUS "N_CPU =  ${N_CPU}")

# Set default flags
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(CMAKE_BUILD_TYPE MATCHES "Release")
    add_compile_options("SHELL: -Wall -O3 -ffunction-sections -fdata-sections")
    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        message(STATUS "Linux detected")
        if(NOT CMAKE_C_COMPILER_ID MATCHES "NVHPC")
            add_link_options("SHELL: -Wl,-z,noexecstack,--gc-sections")
            # A private static unwinder in a preloaded DSO cannot safely
            # participate in pthread cancellation across DSO boundaries.
            # Keep the existing static-libgcc policy for final executables,
            # while shared and module libraries use the toolchain unwinder.
            add_link_options(
                "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:-static-libgcc>")
        else()
            add_link_options("SHELL: -Wl,-z,noexecstack,--gc-sections")
            message(STATUS "Skipping -static-libgcc for NVHPC compiler")
        endif()
    elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
        message(STATUS "MacOS detected")
        add_link_options("SHELL: -Wl,-dead_strip")
    endif()
elseif(CMAKE_BUILD_TYPE MATCHES "Debug")
    add_compile_options("SHELL: -Wall -O0 -g")
endif()
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

find_package(Threads REQUIRED)
find_library(DL_LIBRARY dl)
find_library(RESOLV_LIBRARY resolv)
message(STATUS "M_LIBRARY    = ${M_LIBRARY}")
message(STATUS "RESOLV_LIBRARY   = ${RESOLV_LIBRARY}")
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    find_library(RT_LIBRARY rt)
    find_library(M_LIBRARY m)
    message(STATUS "DL_LIBRARY   = ${DL_LIBRARY}")
    message(STATUS "RT_LIBRARY   = ${RT_LIBRARY}")
endif()

# Find, validate, or download frida-gum
include(cmake/frida-gum.cmake)
configure_frida_gum(
    ${PROJECT_SOURCE_DIR}/cmake
    ${PROJECT_BINARY_DIR}/frida-gum
)
message(STATUS "FRIDA_GUM_LIBRARIES   = ${FRIDA_GUM_LIBRARIES}")
message(STATUS "FRIDA_GUM_INCLUDE_DIRS = ${FRIDA_GUM_INCLUDE_DIRS}")
message(STATUS "PEAK_GUM_PEAK_API_AVAILABLE = ${PEAK_GUM_PEAK_API_AVAILABLE}")
message(STATUS "PEAK_GUM_PEAK_PC_API_AVAILABLE = ${PEAK_GUM_PEAK_PC_API_AVAILABLE}")
message(STATUS "PEAK_GUM_PEAK_EXACT_ATTACH_API_AVAILABLE = ${PEAK_GUM_PEAK_EXACT_ATTACH_API_AVAILABLE}")
message(STATUS "PEAK_GUM_DARWIN_PATCH_API_AVAILABLE = ${PEAK_GUM_DARWIN_PATCH_API_AVAILABLE}")

include(cmake/exec-platform.cmake)
peak_exec_configure_platform_support()
peak_exec_enable_raw_syscall_language()
message(STATUS "PEAK_EXEC_RAW_SYSCALL_SUPPORTED = ${PEAK_EXEC_RAW_SYSCALL_SUPPORTED}")
message(STATUS "PEAK_DETACH_HELPER_SUPPORTED = ${PEAK_DETACH_HELPER_SUPPORTED}")

# Find or download OTF2 only when memory-trace export is requested.
if(PEAK_ENABLE_OTF2)
    if(OTF2_LIBRARIES AND OTF2_INCLUDE_DIRS)
        message(STATUS "Using caller-provided OTF2")
    elseif(OTF2_LIBRARIES OR OTF2_INCLUDE_DIRS)
        message(FATAL_ERROR
            "Set both OTF2_LIBRARIES and OTF2_INCLUDE_DIRS, or neither.")
    elseif(PEAK_USE_SYSTEM_OTF2)
        find_path(OTF2_INCLUDE_DIR NAMES otf2/otf2.h)
        find_library(OTF2_LIBRARY NAMES otf2)
        if(OTF2_INCLUDE_DIR AND OTF2_LIBRARY)
            set(OTF2_INCLUDE_DIRS "${OTF2_INCLUDE_DIR}")
            set(OTF2_LIBRARIES "${OTF2_LIBRARY}")
            get_filename_component(OTF2_LIBRARY_DIR "${OTF2_LIBRARY}" DIRECTORY)
            message(STATUS "Using system OTF2: ${OTF2_LIBRARY}")
        endif()
    endif()

    if(NOT OTF2_LIBRARIES)
        if(NOT PEAK_FETCH_DEPS)
            message(FATAL_ERROR
                "PEAK_ENABLE_OTF2=ON requires system OTF2, caller-provided "
                "OTF2_LIBRARIES/OTF2_INCLUDE_DIRS, or PEAK_FETCH_DEPS=ON.")
        endif()
        message(STATUS "Fetching OTF2")
        include(cmake/otf2.cmake)
        fetch_otf2(
            ${PROJECT_SOURCE_DIR}/cmake
            ${PROJECT_BINARY_DIR}/otf2
        )
    endif()
    message(STATUS "OTF2_LIBRARIES   = ${OTF2_LIBRARIES}")
    message(STATUS "OTF2_INCLUDE_DIRS = ${OTF2_INCLUDE_DIRS}")
    message(STATUS "OTF2_LIBRARY_DIR = ${OTF2_LIBRARY_DIR}")
endif()

option(PEAK_ENABLE_MPI "Enable MPI support" ON)
if(PEAK_ENABLE_MPI)
    # reference: https://cliutils.gitlab.io/modern-cmake/chapters/packages/MPI.html
    find_package(MPI)
    if(MPI_FOUND)
        message(STATUS "Run: ${MPIEXEC} ${MPIEXEC_NUMPROC_FLAG} ${MPIEXEC_MAX_NUMPROCS} ${MPIEXEC_PREFLAGS} EXECUTABLE ${MPIEXEC_POSTFLAGS} ARGS")
        add_definitions(-DHAVE_MPI)
        include(CheckCSourceCompiles)
        set(CMAKE_REQUIRED_INCLUDES ${MPI_C_INCLUDE_DIRS})
        set(CMAKE_REQUIRED_LIBRARIES ${MPI_C_LIBRARIES})
        check_c_source_compiles(
            "#include <mpi.h>\nint main(void) { MPI_Datatype value = MPI_UINT64_T; return value == MPI_DATATYPE_NULL; }"
            PEAK_HAVE_MPI_UINT64_T)
        unset(CMAKE_REQUIRED_INCLUDES)
        unset(CMAKE_REQUIRED_LIBRARIES)
        if(PEAK_HAVE_MPI_UINT64_T)
            add_definitions(-DPEAK_HAVE_MPI_UINT64_T=1)
        else()
            message(STATUS "MPI_UINT64_T unavailable; using an exact-width C integer fallback")
        endif()
    endif()
else()
    set(MPI_FOUND FALSE)
    set(MPI_C_FOUND FALSE)
    set(MPI_CXX_FOUND FALSE)
    set(MPI_Fortran_FOUND FALSE)
    message(STATUS "MPI support disabled by PEAK_ENABLE_MPI=OFF")
endif()

# Find CUDA
# Define the option to enable CUDA profiling and CUDA tests, use -DBUILD_CUDA_PROFILE=OFF to disable them.
option(BUILD_CUDA_PROFILE "Build CUDA kernel profiling feature" ON)
if (BUILD_CUDA_PROFILE)
    if (CMAKE_VERSION VERSION_LESS 3.17)
        find_package(CUDA)
    else()
        find_package(CUDAToolkit)
    endif()
    if(CUDAToolkit_FOUND)
        if(CUDAToolkit_VERSION VERSION_LESS "11.2")
            message(WARNING "CUDA version must be >= 11.2. Found ${CUDAToolkit_VERSION} - Skipping CUDA-related features.")
        else()
            enable_language(CUDA)
            message(STATUS "CUDA ToolKit found: ${CUDAToolkit_VERSION}")
            add_definitions(-DHAVE_CUDA)
            include_directories(${CUDAToolkit_INCLUDE_DIRS})
        endif()
    elseif(CUDA_FOUND)
        if(CUDA_VERSION VERSION_LESS "11.2")
            message(WARNING "CUDA version must be >= 11.2. Found ${CUDA_VERSION} - Skipping CUDA-related features.")
        else()
            enable_language(CUDA)
            message(STATUS "CUDA found: ${CUDA_VERSION}")
            add_definitions(-DHAVE_CUDA)
            include_directories(${CUDA_INCLUDE_DIRS})
        endif()
    else()
        message(WARNING "CUDA not found. Skipping CUDA-related features.")
    endif()
endif()

# Define the option to enable tests, use -DBUILD_TESTS=ON to enable tests.
# BUILD_TESTING is also accepted for compatibility with standard CTest builds.
option(BUILD_TESTS "Build tests" OFF)
if(BUILD_TESTS)
    set(BUILD_TESTING ON CACHE BOOL "Build tests" FORCE)
else()
    option(BUILD_TESTING "Build tests" OFF)
endif()
option(PEAK_BUILD_BENCHMARKS "Build PEAK benchmark/stress targets" OFF)
option(PEAK_ENABLE_TSAN_TESTS "Enable ThreadSanitizer registry test flags" OFF)
if(PEAK_ENABLE_FORTRAN_TESTS)
    include(CheckLanguage)
    check_language(Fortran)
    if(NOT CMAKE_Fortran_COMPILER)
        message(FATAL_ERROR
            "PEAK_ENABLE_FORTRAN_TESTS=ON requires a Fortran compiler.")
    endif()
    enable_language(Fortran)
endif()
if(BUILD_TESTING OR PEAK_BUILD_BENCHMARKS)
    enable_testing()
endif()

add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
if(PEAK_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

include(CMakePackageConfigHelpers)
set(PEAK_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/PEAK")
install(EXPORT PEAKTargets
    FILE PEAKTargets.cmake
    NAMESPACE PEAK::
    DESTINATION ${PEAK_CMAKE_INSTALL_DIR})
configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/PEAKConfig.cmake.in
    ${PROJECT_BINARY_DIR}/PEAKConfig.cmake
    INSTALL_DESTINATION ${PEAK_CMAKE_INSTALL_DIR})
install(FILES
    ${PROJECT_BINARY_DIR}/PEAKConfig.cmake
    DESTINATION ${PEAK_CMAKE_INSTALL_DIR})
