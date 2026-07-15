/*
 * Compiler-proof entry accounting for the dlopen replacement.
 *
 * Strict teardown guards the first 256 bytes of peak_dlopen while restoring
 * the real loader entry.  Keep ownership publication in assembler so compiler
 * entry instrumentation (sanitizer coverage, profiling, split stack, and
 * similar prologues) cannot call outside that guard before the active count is
 * visible.  The tail branch preserves the original call/return ABI while the
 * C body owns the matching decrement on every exit.
 */
#if defined(__x86_64__)
    .text
    .p2align 4
    .globl peak_dlopen
    .hidden peak_dlopen
    .type peak_dlopen, @function
    .hidden peak_dlopen_active_replacement_count
    .hidden peak_dlopen_body
peak_dlopen:
#if defined(__CET__) && (__CET__ & 1)
    endbr64
#endif
    lock addl $1, peak_dlopen_active_replacement_count(%rip)
    jmp peak_dlopen_body
    .size peak_dlopen, .-peak_dlopen
#elif defined(__aarch64__)
    .text
    .p2align 4
    .globl peak_dlopen
    .hidden peak_dlopen
    .type peak_dlopen, %function
    .hidden peak_dlopen_active_replacement_count
    .hidden peak_dlopen_body
peak_dlopen:
#if defined(__ARM_FEATURE_BTI_DEFAULT)
    bti c
#endif
    adrp x9, peak_dlopen_active_replacement_count
    add x9, x9, :lo12:peak_dlopen_active_replacement_count
1:
    ldaxr w10, [x9]
    add w10, w10, #1
    stlxr w11, w10, [x9]
    cbnz w11, 1b
    b peak_dlopen_body
    .size peak_dlopen, .-peak_dlopen
#else
#error "PEAK dlopen entry stub requires x86_64 or AArch64"
#endif

    .section .note.GNU-stack,"",%progbits
