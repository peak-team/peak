#ifndef PEAK_SYSCALL_INTERCEPTOR_POLICY_H
#define PEAK_SYSCALL_INTERCEPTOR_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>

#define PEAK_CLOSE_PATCH_HAZARD_PREFIX_BYTES 16U
#define PEAK_CLOSE_PATCH_HAZARD_SCAN_BYTES 32U

static inline int
peak_syscall_interceptor_has_inline_close_syscall_bytes(const uint8_t* code,
                                                        size_t code_size)
{
#if defined(__linux__) && defined(SYS_close) && \
    (defined(__x86_64__) || defined(__amd64__))
    const size_t max_start =
        code_size < PEAK_CLOSE_PATCH_HAZARD_PREFIX_BYTES
            ? code_size
            : PEAK_CLOSE_PATCH_HAZARD_PREFIX_BYTES;

    if (code == NULL) {
        return 0;
    }

    for (size_t offset = 0; offset < max_start; offset++) {
        uint32_t syscall_number = 0;

        if (offset + 7 > code_size ||
            code[offset] != 0xb8 ||
            code[offset + 5] != 0x0f ||
            code[offset + 6] != 0x05) {
            continue;
        }

        memcpy(&syscall_number, &code[offset + 1], sizeof(syscall_number));
        if (syscall_number == (uint32_t)SYS_close) {
            return 1;
        }
    }

    return 0;
#else
    (void)code;
    (void)code_size;
    return 0;
#endif
}

#endif
