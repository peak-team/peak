# Installed Package Contract

PEAK 1.0 provides an installed shared library and command-line tools. The
exported CMake target is intentionally an artifact-only preload interface:

```cmake
find_package(PEAK 1 CONFIG REQUIRED)
add_custom_target(profile-my-application
  COMMAND ${CMAKE_COMMAND} -E env
    "LD_PRELOAD=$<TARGET_FILE:PEAK::peak>"
    "PEAK_TARGET=my_function"
    $<TARGET_FILE:my_application>
  DEPENDS my_application)
```

`PEAK::peak` identifies the installed production DSO for use through
`$<TARGET_FILE:PEAK::peak>`. Consumers must not rely on
`target_link_libraries()` to retain the DSO: toolchain `--as-needed` behavior
may remove a library for which the executable references no symbols. Linux
applications load PEAK through `LD_PRELOAD`; macOS applications use
`DYLD_INSERT_LIBRARIES`. `PEAK_PUBLIC_INTERFACE` is `PRELOAD_ONLY`.

PEAK does not currently publish a general C or C++ header API. Headers under
the source-tree `include/` directory are implementation interfaces and are not
installed. Downstream code must not include or depend on them.

The package uses `SameMajorVersion` compatibility. A 1.x installation may
satisfy requests for an equal or older 1.x version, but it does not claim
compatibility with 2.x. The DSO uses ABI soname major 1.

MPI, CUDA, and OTF2 are private implementation dependencies. Their build-time
state is reported through `PEAK_MPI_ENABLED`, `PEAK_CUDA_ENABLED`, and
`PEAK_OTF2_ENABLED`; none becomes a transitive CMake requirement of
`PEAK::peak`.

## Patched Frida Gum compatibility

The supported default build uses the hash-pinned Frida Gum 17.15.3 devkit and
the PEAK patch set shipped in the same PEAK source release. The Gum archive is
linked privately into `libpeak`; it is not part of the installed CMake
interface. A caller-provided devkit must satisfy the configure-time PEAK Gum
API and ABI checks documented in [patched-frida-gum.md](patched-frida-gum.md).
Package-manager builds must disable downloads with `PEAK_FETCH_DEPS=OFF` and
provide that compatible devkit explicitly.

The default 17.15.3 devkit archives are pinned as follows:

| Platform | SHA-256 |
| --- | --- |
| Linux x86_64 | `f827b75f432c5f90ae57c71979e90e1c93edfa3aa3ac252b0d547f3087306f01` |
| Linux Arm64 | `b7b9f914ccb2f70c0663bfa20614d4b58fa8fc5f9e0a7786d3fb1c22113b8c61` |
| macOS x86_64 | `7378f605d351a0cfd53b46c3b608f4bde383eff30b1c4c886e11006dbf08f54f` |
| macOS Arm64 | `1904bc3559e27da517289f940abd37ab9cde4cca82a78e229b386acfa5beb487` |

Optional OTF2 3.1.1 uses SHA-256
`5a4e013a51ac4ed794fe35c55b700cd720346fda7f33ec84c76b86a5fb880a6e`.
These values are part of the 1.0 source release and must match its CMake
download definitions.
