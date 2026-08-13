#if !defined(__APPLE__) || \
    !(defined(__arm64__) || defined(__aarch64__))
#error "This test target requires macOS Arm64"
#endif

    .text
    .p2align 2
    .globl _peak_macos_branch_target
_peak_macos_branch_target:
    b _peak_macos_smoke_target
