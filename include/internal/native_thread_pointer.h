#ifndef PEAK_NATIVE_THREAD_POINTER_H
#define PEAK_NATIVE_THREAD_POINTER_H

#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__) || defined(__aarch64__))
#include <stddef.h>
#include <stdint.h>

static inline void * peak_native_thread_pointer(void)
{
#if defined(__aarch64__)
    void * value;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
    return value;
#else
    return __builtin_thread_pointer();
#endif
}

static inline ptrdiff_t peak_native_tls_offset(const void * address)
{
    /* TLS relocations must first load a GPR, rather than a vector memory operand. */
    __asm__ volatile("" : "+r"(address));
    return (ptrdiff_t)((uintptr_t)address - (uintptr_t)peak_native_thread_pointer());
}
#endif

#endif /* PEAK_NATIVE_THREAD_POINTER_H */
