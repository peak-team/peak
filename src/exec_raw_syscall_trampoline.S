#if defined(__x86_64__)
    .text
    .globl syscall
    .type syscall, @function
    .hidden peak_signal_policy_syscall_dispatch
syscall:
    jmp peak_signal_policy_syscall_dispatch
    .size syscall, .-syscall
#elif defined(__aarch64__)
    .text
    .globl syscall
    .type syscall, %function
    .hidden peak_signal_policy_syscall_dispatch
syscall:
    b peak_signal_policy_syscall_dispatch
    .size syscall, .-syscall
#endif
