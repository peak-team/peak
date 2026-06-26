#include "unsafe_gum_prologue.h"

#include <stdio.h>
#include <stdint.h>

static int
expect_policy(const char* name,
              const guint8* code,
              PeakUnsafeGumProloguePolicy policy,
              gboolean expected)
{
    const char* reason = NULL;
    gboolean actual =
        peak_unsafe_gum_prologue_check((gpointer)code, policy, &reason);

    if (actual != expected) {
        fprintf(stderr,
                "policy mismatch name=%s policy=%s expected=%d actual=%d reason=%s\n",
                name,
                peak_unsafe_gum_prologue_policy_name(policy),
                expected,
                actual,
                reason != NULL ? reason : "<none>");
        return 1;
    }

    return 0;
}

static int
expect_support_policy(const char* name,
                      const guint8* code,
                      gboolean expected)
{
    const char* reason = NULL;
    gboolean actual =
        peak_unsafe_gum_support_prologue_check((gpointer)code, &reason);

    if (actual != expected) {
        fprintf(stderr,
                "support policy mismatch name=%s expected=%d actual=%d reason=%s\n",
                name,
                expected,
                actual,
                reason != NULL ? reason : "<none>");
        return 1;
    }

    return 0;
}

int
main(void)
{
#if defined(__x86_64__) || defined(__amd64__)
    static const guint8 rdx_prefix[] = {
        0x30, 0xd2,                   /* xor dl, dl */
        0x31, 0xc0,                   /* xor eax, eax */
        0x8b, 0x0c, 0x07,             /* mov (%rdi,%rax,1), %ecx */
        0xfe, 0xc2,                   /* inc %dl */
        0xc3
    };
    static const guint8 rdx_dead_prefix[] = {
        0x30, 0xd2,                   /* xor dl, dl */
        0x31, 0xc0,                   /* xor eax, eax */
        0x8b, 0x0c, 0x07,             /* mov (%rdi,%rax,1), %ecx */
        0xc3
    };
    static const guint8 rdx_zero_delta_add[] = {
        0x30, 0xd2,                   /* xor dl, dl */
        0x31, 0xc0,                   /* xor eax, eax */
        0x8b, 0x0c, 0x07,             /* mov (%rdi,%rax,1), %ecx */
        0x80, 0xc2, 0x00,             /* add $0, %dl */
        0xc3
    };
    static const guint8 rdx_zero_delta_sub[] = {
        0x31, 0xd2,                   /* xor edx, edx */
        0x31, 0xc0,                   /* xor eax, eax */
        0x8b, 0x0c, 0x07,             /* mov (%rdi,%rax,1), %ecx */
        0x83, 0xea, 0x00,             /* sub $0, %edx */
        0xc3
    };
    static const guint8 rdx_zero_delta_rex_add[] = {
        0x48, 0x31, 0xd2,             /* xor rdx, rdx */
        0x31, 0xc0,                   /* xor eax, eax */
        0x8b, 0x0c, 0x07,             /* mov (%rdi,%rax,1), %ecx */
        0x48, 0x83, 0xc2, 0x00,       /* add $0, %rdx */
        0xc3
    };
    static const guint8 high_movabs_ret_imm[] = {
        0x49, 0xb8,
        0x44, 0x43, 0x42, 0x41, 0xc3, 0xc2, 0xcb, 0xca,
        0xc3
    };
    static const guint8 high_movabs_benign[] = {
        0x49, 0xb8,
        0x48, 0x47, 0x46, 0x45, 0x44, 0x43, 0x42, 0x41,
        0xc3
    };
    static const guint8 high_movabs_after_safe[] = {
        0x8b, 0x0f,                   /* mov (%rdi), %ecx */
        0x49, 0xb8,
        0x44, 0x43, 0x42, 0x41, 0xc3, 0xc2, 0xcb, 0xca,
        0xc3
    };
    static const guint8 ret_immediate[] = {
        0xb8, 0xc3, 0xc2, 0xcb, 0xca, /* mov imm32, %eax */
        0xc3
    };
    static const guint8 ret_immediate_after_safe[] = {
        0x8b, 0x0f,                   /* mov (%rdi), %ecx */
        0xb8, 0xc3, 0xc2, 0xcb, 0xca,
        0xc3
    };
    static const guint8 plain_immediate[] = {
        0xb8, 0x44, 0x43, 0x42, 0x41,
        0xc3
    };
    static const guint8 milc_f2d_prefix[] = {
        0x32, 0xd2,                   /* xor dl, dl */
        0x33, 0xc0,                   /* xor eax, eax */
        0x8b, 0x0c, 0x38,             /* mov (%rax,%rdi,1), %ecx */
        0xfe, 0xc2,                   /* inc %dl */
        0x89, 0x0c, 0x30              /* mov %ecx, (%rax,%rsi,1) */
    };
    static const guint8 close_fastpath[] = {
        0xf3, 0x0f, 0x1e, 0xfa,       /* endbr64 */
        0x64, 0x8b, 0x04, 0x25,
        0x18, 0x00, 0x00, 0x00,       /* mov %fs:0x18, %eax */
        0x85, 0xc0,                   /* test %eax, %eax */
        0x75, 0x10,                   /* jne */
        0xb8, 0x03, 0x00, 0x00, 0x00, /* mov $3, %eax */
        0x0f, 0x05,                   /* syscall */
        0x48, 0x3d, 0x00, 0xf0,
        0xff, 0xff,                   /* cmp $-4096, %rax */
        0x77, 0x41,                   /* ja */
        0xc3                          /* ret */
    };
    static const guint8 tiny_return[] = {
        0x33, 0xc0,                   /* xor %eax, %eax */
        0xc3                          /* ret */
    };
    static const guint8 unsupported_before_return[] = {
        0x0f, 0x1f, 0x00,             /* nopl (%rax) */
        0xc3                          /* ret */
    };

    if (expect_policy("rdx-prefix-default", rdx_prefix,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT, TRUE) ||
        expect_policy("rdx-prefix-conservative", rdx_prefix,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, TRUE) ||
        expect_policy("rdx-dead-default", rdx_dead_prefix,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT, FALSE) ||
        expect_policy("rdx-dead-conservative", rdx_dead_prefix,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, FALSE) ||
        expect_policy("rdx-zero-delta-add-default", rdx_zero_delta_add,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT, FALSE) ||
        expect_policy("rdx-zero-delta-sub-default", rdx_zero_delta_sub,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT, FALSE) ||
        expect_policy("rdx-zero-delta-rex-add-default",
                      rdx_zero_delta_rex_add,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT, FALSE) ||
        expect_policy("rdx-zero-delta-add-conservative", rdx_zero_delta_add,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, FALSE) ||
        expect_policy("rdx-zero-delta-sub-conservative", rdx_zero_delta_sub,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, FALSE) ||
        expect_policy("rdx-zero-delta-rex-add-conservative",
                      rdx_zero_delta_rex_add,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, FALSE) ||
        expect_policy("high-movabs-default", high_movabs_ret_imm,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT, FALSE) ||
        expect_policy("high-movabs-conservative", high_movabs_ret_imm,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, TRUE) ||
        expect_policy("high-movabs-benign-conservative", high_movabs_benign,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, FALSE) ||
        expect_policy("high-movabs-after-safe-conservative",
                      high_movabs_after_safe,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, FALSE) ||
        expect_policy("ret-immediate-default", ret_immediate,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT, FALSE) ||
        expect_policy("ret-immediate-conservative", ret_immediate,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, TRUE) ||
        expect_policy("ret-immediate-after-safe-conservative",
                      ret_immediate_after_safe,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, FALSE) ||
        expect_policy("plain-immediate-conservative", plain_immediate,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, FALSE) ||
        expect_policy("milc-f2d-default", milc_f2d_prefix,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT, TRUE) ||
        expect_policy("milc-f2d-conservative", milc_f2d_prefix,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, TRUE) ||
        expect_policy("close-fastpath-default", close_fastpath,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT, FALSE) ||
        expect_policy("close-fastpath-conservative", close_fastpath,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, FALSE) ||
        expect_policy("tiny-return-default", tiny_return,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT, FALSE) ||
        expect_policy("tiny-return-conservative", tiny_return,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, FALSE) ||
        expect_policy("unsupported-before-return-default",
                      unsupported_before_return,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT, FALSE) ||
        expect_policy("unsupported-before-return-conservative",
                      unsupported_before_return,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, FALSE) ||
        expect_support_policy("milc-f2d-support", milc_f2d_prefix, TRUE) ||
        expect_support_policy("close-fastpath-support", close_fastpath, TRUE) ||
        expect_support_policy("tiny-return-support", tiny_return, TRUE) ||
        expect_support_policy("unsupported-before-return-support",
                              unsupported_before_return, TRUE) ||
        expect_support_policy("plain-immediate-support", plain_immediate,
                              TRUE)) {
        return 1;
    }
#elif defined(__aarch64__)
    static const guint8 x16_only[] = {
        0x10, 0x00, 0x80, 0xd2,       /* mov x16, #0 */
        0x08, 0x02, 0x70, 0xb8,       /* ldr w8, [x16], #0 */
        0xc0, 0x03, 0x5f, 0xd6
    };
    static const guint8 x16_x17[] = {
        0xf0, 0x03, 0x00, 0xaa,       /* mov x16, x0 */
        0x11, 0x00, 0x04, 0x91,       /* add x17, x0, #256 */
        0xc0, 0x03, 0x5f, 0xd6
    };
    static const guint8 add_imm_ip_bits[] = {
        0x20, 0x40, 0x04, 0x91,       /* add x0, x1, #0x110 */
        0xc0, 0x03, 0x5f, 0xd6
    };

    if (expect_policy("arm64-x16-default", x16_only,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT, FALSE) ||
        expect_policy("arm64-x16-conservative", x16_only,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, FALSE) ||
        expect_policy("arm64-both-default", x16_x17,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT, FALSE) ||
        expect_policy("arm64-both-conservative", x16_x17,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, TRUE) ||
        expect_policy("arm64-add-imm-ip-bits-conservative", add_imm_ip_bits,
                      PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE, FALSE)) {
        return 1;
    }
#endif

    printf("unsafe_gum_prologue_policy_unit_ok\n");
    return 0;
}
