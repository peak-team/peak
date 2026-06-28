#include "syscall_interceptor_policy.h"

#include <stdio.h>
#include <string.h>

static int
expect_close_stub(const char* name, const unsigned char* code, size_t size,
                  int expected)
{
    int actual = peak_syscall_interceptor_has_inline_close_syscall_bytes(
        code, size);

    if (actual != expected) {
        fprintf(stderr,
                "close policy mismatch name=%s expected=%d actual=%d\n",
                name, expected, actual);
        return 1;
    }

    return 0;
}

int
main(void)
{
#if defined(__linux__) && defined(SYS_close) && \
    (defined(__x86_64__) || defined(__amd64__))
    unsigned char public_stub[PEAK_CLOSE_PATCH_HAZARD_SCAN_BYTES];
    unsigned char embedded_stub[PEAK_CLOSE_PATCH_HAZARD_SCAN_BYTES];
    unsigned char late_stub[PEAK_CLOSE_PATCH_HAZARD_SCAN_BYTES];
    unsigned char wrong_syscall[PEAK_CLOSE_PATCH_HAZARD_SCAN_BYTES];
    const unsigned char close_stub[] = {
        0xb8,
        (unsigned char)(SYS_close & 0xff),
        (unsigned char)((SYS_close >> 8) & 0xff),
        (unsigned char)((SYS_close >> 16) & 0xff),
        (unsigned char)((SYS_close >> 24) & 0xff),
        0x0f, 0x05
    };
    const unsigned char getpid_stub[] = {
        0xb8,
        (unsigned char)(SYS_getpid & 0xff),
        (unsigned char)((SYS_getpid >> 8) & 0xff),
        (unsigned char)((SYS_getpid >> 16) & 0xff),
        (unsigned char)((SYS_getpid >> 24) & 0xff),
        0x0f, 0x05
    };

    memset(public_stub, 0x90, sizeof(public_stub));
    memset(embedded_stub, 0x90, sizeof(embedded_stub));
    memset(late_stub, 0x90, sizeof(late_stub));
    memset(wrong_syscall, 0x90, sizeof(wrong_syscall));

    memcpy(public_stub, close_stub, sizeof(close_stub));
    memcpy(embedded_stub + 15, close_stub, sizeof(close_stub));
    memcpy(late_stub + 16, close_stub, sizeof(close_stub));
    memcpy(wrong_syscall + 9, getpid_stub, sizeof(getpid_stub));

    if (expect_close_stub("public", public_stub, sizeof(public_stub), 1) ||
        expect_close_stub("embedded-prefix-end", embedded_stub,
                          sizeof(embedded_stub), 1) ||
        expect_close_stub("late-after-prefix", late_stub, sizeof(late_stub),
                          0) ||
        expect_close_stub("wrong-syscall", wrong_syscall,
                          sizeof(wrong_syscall), 0)) {
        return 1;
    }
#endif

    printf("syscall_interceptor_policy_ok\n");
    return 0;
}
