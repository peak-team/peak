#ifndef PEAK_DLOPEN_ENTRY_ACCOUNTING_H
#define PEAK_DLOPEN_ENTRY_ACCOUNTING_H

/*
 * Strict dlopen teardown blocks PCs in the beginning of peak_dlopen until the
 * replacement body has published itself in the active-entry counter.  GCC and
 * Clang may implement an otherwise lock-free AArch64 atomic RMW by calling an
 * out-of-line __aarch64_* helper.  A stopped thread in that helper would be
 * outside the blocked PC range without having incremented the counter.
 *
 * Keep the AArch64 registration in the caller with an architecture-baseline
 * acquire/release exclusive loop.  Other supported targets retain the direct
 * C11 lock-free atomic operation.
 */
#if defined(__aarch64__)
#define PEAK_DLOPEN_REGISTER_REPLACEMENT_ENTRY(counter_address)              \
    do {                                                                     \
        unsigned int peak_entry_value;                                       \
        unsigned int peak_entry_status;                                      \
        __asm__ __volatile__(                                                \
            "1:\n\t"                                                       \
            "ldaxr %w0, [%2]\n\t"                                          \
            "add %w0, %w0, #1\n\t"                                         \
            "stlxr %w1, %w0, [%2]\n\t"                                      \
            "cbnz %w1, 1b\n\t"                                             \
            : "=&r"(peak_entry_value), "=&r"(peak_entry_status)             \
            : "r"(counter_address)                                          \
            : "memory");                                                    \
    } while (0)
#else
#define PEAK_DLOPEN_REGISTER_REPLACEMENT_ENTRY(counter_address)              \
    atomic_fetch_add_explicit((counter_address), 1, memory_order_acq_rel)
#endif

#endif
