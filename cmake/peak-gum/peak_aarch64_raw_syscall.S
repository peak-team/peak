#if defined(__aarch64__)
    .text
    .p2align 2
    .global peak_aarch64_raw_syscall6_raw
    .hidden peak_aarch64_raw_syscall6_raw
    .type peak_aarch64_raw_syscall6_raw, %function
peak_aarch64_raw_syscall6_raw:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    mov x5, x6
    svc #0
    ret
    .size peak_aarch64_raw_syscall6_raw, .-peak_aarch64_raw_syscall6_raw

    .section .note.GNU-stack,"",%progbits
#endif
