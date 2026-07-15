#if defined(__x86_64__)
    .text
    .globl peak_notype_target
peak_notype_target:
    .rept 32
    nop
    .endr
    ret
    .size peak_notype_target, .-peak_notype_target
    .section .note.GNU-stack,"",@progbits
#elif defined(__aarch64__)
    .text
    .globl peak_notype_target
peak_notype_target:
    .rept 8
    nop
    .endr
    ret
    .size peak_notype_target, .-peak_notype_target
    .section .note.GNU-stack,"",%progbits
#else
#error "peak_notype_target is only supported on x86_64 and AArch64"
#endif
