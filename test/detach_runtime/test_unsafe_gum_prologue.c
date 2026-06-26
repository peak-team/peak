#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void (*unsafe_target_fn)(const uint8_t* src, uint8_t* dst);

typedef struct {
    const char* name;
    const char* symbol;
    unsafe_target_fn fn;
    size_t size;
} UnsafeCase;

#if defined(__x86_64__) || defined(__amd64__)
#define X86_COPY72_REST(counter_reg) \
    "mov %ecx, (%rsi,%rax,1)\n\t" \
    "mov 0x4(%rdi,%rax,1), %r8d\n\t" \
    "mov %r8d, 0x4(%rsi,%rax,1)\n\t" \
    "mov 0x8(%rdi,%rax,1), %r9d\n\t" \
    "mov %r9d, 0x8(%rsi,%rax,1)\n\t" \
    "mov 0xc(%rdi,%rax,1), %r10d\n\t" \
    "mov %r10d, 0xc(%rsi,%rax,1)\n\t" \
    "mov 0x10(%rdi,%rax,1), %r11d\n\t" \
    "mov %r11d, 0x10(%rsi,%rax,1)\n\t" \
    "mov 0x14(%rdi,%rax,1), %ecx\n\t" \
    "mov %ecx, 0x14(%rsi,%rax,1)\n\t" \
    "mov 0x18(%rdi,%rax,1), %r8d\n\t" \
    "mov %r8d, 0x18(%rsi,%rax,1)\n\t" \
    "mov 0x1c(%rdi,%rax,1), %r9d\n\t" \
    "mov %r9d, 0x1c(%rsi,%rax,1)\n\t" \
    "mov 0x20(%rdi,%rax,1), %r10d\n\t" \
    "mov %r10d, 0x20(%rsi,%rax,1)\n\t" \
    "mov 0x24(%rdi,%rax,1), %r11d\n\t" \
    "mov %r11d, 0x24(%rsi,%rax,1)\n\t" \
    "mov 0x28(%rdi,%rax,1), %ecx\n\t" \
    "mov %ecx, 0x28(%rsi,%rax,1)\n\t" \
    "mov 0x2c(%rdi,%rax,1), %r8d\n\t" \
    "mov %r8d, 0x2c(%rsi,%rax,1)\n\t" \
    "mov 0x30(%rdi,%rax,1), %r9d\n\t" \
    "mov %r9d, 0x30(%rsi,%rax,1)\n\t" \
    "mov 0x34(%rdi,%rax,1), %r10d\n\t" \
    "mov %r10d, 0x34(%rsi,%rax,1)\n\t" \
    "mov 0x38(%rdi,%rax,1), %r11d\n\t" \
    "mov %r11d, 0x38(%rsi,%rax,1)\n\t" \
    "mov 0x3c(%rdi,%rax,1), %ecx\n\t" \
    "mov %ecx, 0x3c(%rsi,%rax,1)\n\t" \
    "mov 0x40(%rdi,%rax,1), %r8d\n\t" \
    "mov %r8d, 0x40(%rsi,%rax,1)\n\t" \
    "mov 0x44(%rdi,%rax,1), %r9d\n\t" \
    "mov %r9d, 0x44(%rsi,%rax,1)\n\t" \
    "add $0x48, %rax\n\t" \
    "cmp $0x4, %" counter_reg "\n\t" \
    "jb 1b\n\t" \
    "ret\n\t"

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_dl_inc_target(const uint8_t* src, uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        "xor %dl, %dl\n\t"
        "xor %eax, %eax\n\t"
        "1:\n\t"
        "mov (%rdi,%rax,1), %ecx\n\t"
        "inc %dl\n\t"
        X86_COPY72_REST("dl"));
}

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_dl_add_target(const uint8_t* src, uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        "xor %dl, %dl\n\t"
        "xor %eax, %eax\n\t"
        "1:\n\t"
        "mov (%rdi,%rax,1), %ecx\n\t"
        "add $1, %dl\n\t"
        X86_COPY72_REST("dl"));
}

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_edx_inc_target(const uint8_t* src, uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        "xor %edx, %edx\n\t"
        "xor %eax, %eax\n\t"
        "1:\n\t"
        "mov (%rdi,%rax,1), %ecx\n\t"
        "inc %edx\n\t"
        X86_COPY72_REST("edx"));
}

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_edx_add_target(const uint8_t* src, uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        "xor %edx, %edx\n\t"
        "xor %eax, %eax\n\t"
        "1:\n\t"
        "mov (%rdi,%rax,1), %ecx\n\t"
        "add $1, %edx\n\t"
        X86_COPY72_REST("edx"));
}

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_dl_sub_zero_target(const uint8_t* src, uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        ".byte 0x28, 0xd2\n\t"
        "xor %eax, %eax\n\t"
        "1:\n\t"
        "mov (%rdi,%rax,1), %ecx\n\t"
        "inc %dl\n\t"
        X86_COPY72_REST("dl"));
}

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_edx_sub_zero_target(const uint8_t* src, uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        ".byte 0x29, 0xd2\n\t"
        "xor %eax, %eax\n\t"
        "1:\n\t"
        "mov (%rdi,%rax,1), %ecx\n\t"
        "inc %edx\n\t"
        X86_COPY72_REST("edx"));
}

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_dl_mov_zero_target(const uint8_t* src, uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        ".byte 0xb2, 0x00\n\t"
        "xor %eax, %eax\n\t"
        "1:\n\t"
        "mov (%rdi,%rax,1), %ecx\n\t"
        "inc %dl\n\t"
        X86_COPY72_REST("dl"));
}

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_rdx_xor_target(const uint8_t* src, uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        "xor %rdx, %rdx\n\t"
        "xor %eax, %eax\n\t"
        "1:\n\t"
        "mov (%rdi,%rax,1), %ecx\n\t"
        "inc %dl\n\t"
        X86_COPY72_REST("dl"));
}

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_edx_mov_target(const uint8_t* src, uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        "mov $0, %edx\n\t"
        "xor %eax, %eax\n\t"
        "1:\n\t"
        "mov (%rdi,%rax,1), %ecx\n\t"
        "inc %dl\n\t"
        X86_COPY72_REST("dl"));
}

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_dl_dead_target(const uint8_t* src, uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        "xor %dl, %dl\n\t"
        "xor %eax, %eax\n\t"
        "mov (%rdi,%rax,1), %ecx\n\t"
        "mov %ecx, (%rsi,%rax,1)\n\t"
        "ret\n\t");
}

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_rdx_dead_target(const uint8_t* src, uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        "xor %rdx, %rdx\n\t"
        "xor %eax, %eax\n\t"
        "mov (%rdi,%rax,1), %ecx\n\t"
        "mov %ecx, (%rsi,%rax,1)\n\t"
        "ret\n\t");
}

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_ret_in_prefix_target(const uint8_t* src, uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        "mov (%rdi), %ecx\n\t"
        "mov %ecx, (%rsi)\n\t"
        "mov 0x4(%rdi), %ecx\n\t"
        "mov %ecx, 0x4(%rsi)\n\t"
        "mov 0x8(%rdi), %ecx\n\t"
        "mov %ecx, 0x8(%rsi)\n\t"
        "mov 0xc(%rdi), %ecx\n\t"
        "mov %ecx, 0xc(%rsi)\n\t"
        "mov 0x10(%rdi), %ecx\n\t"
        "mov %ecx, 0x10(%rsi)\n\t"
        "ret\n\t");
}

__attribute__((noinline, visibility("default")))
void
peak_unsafe_gum_x86_safe_c_target(const uint8_t* src, uint8_t* dst)
{
    const volatile uint8_t* vsrc = src;
    volatile uint8_t* vdst = dst;

    for (int i = 0; i < 32; i++) {
        vdst[i] = vsrc[i];
    }
}

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_high_movabs_target(const uint8_t* src, uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        "movabs $0xcacbc2c341424344, %r8\n\t"
        "mov (%rdi), %ecx\n\t"
        "mov %ecx, (%rsi)\n\t"
        "mov 0x4(%rdi), %ecx\n\t"
        "mov %ecx, 0x4(%rsi)\n\t"
        "mov 0x8(%rdi), %ecx\n\t"
        "mov %ecx, 0x8(%rsi)\n\t"
        "mov 0xc(%rdi), %ecx\n\t"
        "mov %ecx, 0xc(%rsi)\n\t"
        "mov 0x10(%rdi), %ecx\n\t"
        "mov %ecx, 0x10(%rsi)\n\t"
        "ret\n\t");
}

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_ret_byte_immediate_target(const uint8_t* src,
                                              uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        "mov $0xcacbc2c3, %eax\n\t"
        "mov (%rdi), %ecx\n\t"
        "mov %ecx, (%rsi)\n\t"
        "mov 0x4(%rdi), %ecx\n\t"
        "mov %ecx, 0x4(%rsi)\n\t"
        "mov 0x8(%rdi), %ecx\n\t"
        "mov %ecx, 0x8(%rsi)\n\t"
        "mov 0xc(%rdi), %ecx\n\t"
        "mov %ecx, 0xc(%rsi)\n\t"
        "mov 0x10(%rdi), %ecx\n\t"
        "mov %ecx, 0x10(%rsi)\n\t"
        "mov 0x14(%rdi), %ecx\n\t"
        "mov %ecx, 0x14(%rsi)\n\t"
        "mov 0x18(%rdi), %ecx\n\t"
        "mov %ecx, 0x18(%rsi)\n\t"
        "ret\n\t");
}

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_plain_immediate_target(const uint8_t* src,
                                           uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        "mov $0x41424344, %eax\n\t"
        "mov (%rdi), %ecx\n\t"
        "mov %ecx, (%rsi)\n\t"
        "mov 0x4(%rdi), %ecx\n\t"
        "mov %ecx, 0x4(%rsi)\n\t"
        "mov 0x8(%rdi), %ecx\n\t"
        "mov %ecx, 0x8(%rsi)\n\t"
        "mov 0xc(%rdi), %ecx\n\t"
        "mov %ecx, 0xc(%rsi)\n\t"
        "mov 0x10(%rdi), %ecx\n\t"
        "mov %ecx, 0x10(%rsi)\n\t"
        "mov 0x14(%rdi), %ecx\n\t"
        "mov %ecx, 0x14(%rsi)\n\t"
        "mov 0x18(%rdi), %ecx\n\t"
        "mov %ecx, 0x18(%rsi)\n\t"
        "mov 0x1c(%rdi), %ecx\n\t"
        "mov %ecx, 0x1c(%rsi)\n\t"
        "mov 0x20(%rdi), %ecx\n\t"
        "mov %ecx, 0x20(%rsi)\n\t"
        "mov 0x24(%rdi), %ecx\n\t"
        "mov %ecx, 0x24(%rsi)\n\t"
        "mov 0x28(%rdi), %ecx\n\t"
        "mov %ecx, 0x28(%rsi)\n\t"
        "mov 0x2c(%rdi), %ecx\n\t"
        "mov %ecx, 0x2c(%rsi)\n\t"
        "mov 0x30(%rdi), %ecx\n\t"
        "mov %ecx, 0x30(%rsi)\n\t"
        "mov 0x34(%rdi), %ecx\n\t"
        "mov %ecx, 0x34(%rsi)\n\t"
        "mov 0x38(%rdi), %ecx\n\t"
        "mov %ecx, 0x38(%rsi)\n\t"
        "mov 0x3c(%rdi), %ecx\n\t"
        "mov %ecx, 0x3c(%rsi)\n\t"
        "ret\n\t");
}

__attribute__((naked, noinline, visibility("default")))
void
peak_unsafe_gum_x86_long_copy_safe_target(const uint8_t* src, uint8_t* dst)
{
    (void)src;
    (void)dst;
    __asm__ __volatile__(
        "mov (%rdi), %ecx\n\t"
        "mov %ecx, (%rsi)\n\t"
        "mov 0x4(%rdi), %ecx\n\t"
        "mov %ecx, 0x4(%rsi)\n\t"
        "mov 0x8(%rdi), %ecx\n\t"
        "mov %ecx, 0x8(%rsi)\n\t"
        "mov 0xc(%rdi), %ecx\n\t"
        "mov %ecx, 0xc(%rsi)\n\t"
        "mov 0x10(%rdi), %ecx\n\t"
        "mov %ecx, 0x10(%rsi)\n\t"
        "mov 0x14(%rdi), %ecx\n\t"
        "mov %ecx, 0x14(%rsi)\n\t"
        "mov 0x18(%rdi), %ecx\n\t"
        "mov %ecx, 0x18(%rsi)\n\t"
        "mov 0x1c(%rdi), %ecx\n\t"
        "mov %ecx, 0x1c(%rsi)\n\t"
        "mov 0x20(%rdi), %ecx\n\t"
        "mov %ecx, 0x20(%rsi)\n\t"
        "mov 0x24(%rdi), %ecx\n\t"
        "mov %ecx, 0x24(%rsi)\n\t"
        "mov 0x28(%rdi), %ecx\n\t"
        "mov %ecx, 0x28(%rsi)\n\t"
        "mov 0x2c(%rdi), %ecx\n\t"
        "mov %ecx, 0x2c(%rsi)\n\t"
        "mov 0x30(%rdi), %ecx\n\t"
        "mov %ecx, 0x30(%rsi)\n\t"
        "mov 0x34(%rdi), %ecx\n\t"
        "mov %ecx, 0x34(%rsi)\n\t"
        "mov 0x38(%rdi), %ecx\n\t"
        "mov %ecx, 0x38(%rsi)\n\t"
        "mov 0x3c(%rdi), %ecx\n\t"
        "mov %ecx, 0x3c(%rsi)\n\t"
        "ret\n\t");
}

static const UnsafeCase unsafe_cases[] = {
    { "x86_dl_inc", "peak_unsafe_gum_x86_dl_inc_target",
      peak_unsafe_gum_x86_dl_inc_target, 288 },
    { "x86_dl_add", "peak_unsafe_gum_x86_dl_add_target",
      peak_unsafe_gum_x86_dl_add_target, 288 },
    { "x86_edx_inc", "peak_unsafe_gum_x86_edx_inc_target",
      peak_unsafe_gum_x86_edx_inc_target, 288 },
    { "x86_edx_add", "peak_unsafe_gum_x86_edx_add_target",
      peak_unsafe_gum_x86_edx_add_target, 288 },
    { "x86_dl_sub_zero", "peak_unsafe_gum_x86_dl_sub_zero_target",
      peak_unsafe_gum_x86_dl_sub_zero_target, 288 },
    { "x86_edx_sub_zero", "peak_unsafe_gum_x86_edx_sub_zero_target",
      peak_unsafe_gum_x86_edx_sub_zero_target, 288 },
    { "x86_dl_mov_zero", "peak_unsafe_gum_x86_dl_mov_zero_target",
      peak_unsafe_gum_x86_dl_mov_zero_target, 288 },
    { "x86_rdx_xor", "peak_unsafe_gum_x86_rdx_xor_target",
      peak_unsafe_gum_x86_rdx_xor_target, 288 },
    { "x86_edx_mov", "peak_unsafe_gum_x86_edx_mov_target",
      peak_unsafe_gum_x86_edx_mov_target, 288 },
    { "x86_dl_dead", "peak_unsafe_gum_x86_dl_dead_target",
      peak_unsafe_gum_x86_dl_dead_target, 4 },
    { "x86_rdx_dead", "peak_unsafe_gum_x86_rdx_dead_target",
      peak_unsafe_gum_x86_rdx_dead_target, 4 },
    { "x86_ret_in_prefix", "peak_unsafe_gum_x86_ret_in_prefix_target",
      peak_unsafe_gum_x86_ret_in_prefix_target, 8 },
    { "x86_safe_c", "peak_unsafe_gum_x86_safe_c_target",
      peak_unsafe_gum_x86_safe_c_target, 32 },
    { "x86_high_movabs", "peak_unsafe_gum_x86_high_movabs_target",
      peak_unsafe_gum_x86_high_movabs_target, 20 },
    { "x86_ret_byte_immediate", "peak_unsafe_gum_x86_ret_byte_immediate_target",
      peak_unsafe_gum_x86_ret_byte_immediate_target, 28 },
    { "x86_plain_immediate", "peak_unsafe_gum_x86_plain_immediate_target",
      peak_unsafe_gum_x86_plain_immediate_target, 64 },
    { "x86_long_copy_safe", "peak_unsafe_gum_x86_long_copy_safe_target",
      peak_unsafe_gum_x86_long_copy_safe_target, 64 },
};
#elif defined(__aarch64__)
void peak_unsafe_gum_arm64_ip_counter_target(const uint8_t* src, uint8_t* dst);
void peak_unsafe_gum_arm64_ip_cursor_target(const uint8_t* src, uint8_t* dst);

static const UnsafeCase unsafe_cases[] = {
    { "arm64_ip_counter", "peak_unsafe_gum_arm64_ip_counter_target",
      peak_unsafe_gum_arm64_ip_counter_target, 256 },
    { "arm64_ip_cursor", "peak_unsafe_gum_arm64_ip_cursor_target",
      peak_unsafe_gum_arm64_ip_cursor_target, 256 },
};
#else
static const UnsafeCase unsafe_cases[] = {
};
#endif

static const size_t unsafe_case_count =
    sizeof(unsafe_cases) / sizeof(unsafe_cases[0]);

static const UnsafeCase*
find_case(const char* name)
{
    for (size_t i = 0; i < unsafe_case_count; i++) {
        if (strcmp(unsafe_cases[i].name, name) == 0 ||
            strcmp(unsafe_cases[i].symbol, name) == 0) {
            return &unsafe_cases[i];
        }
    }
    return NULL;
}

static int
run_case(const UnsafeCase* test_case)
{
    uint8_t src[288];
    uint8_t dst[288];

    for (int iter = 0; iter < 1000; iter++) {
        for (int i = 0; i < (int)sizeof(src); i++) {
            src[i] = (uint8_t)((i * 17 + iter) & 0xff);
        }
        memset(dst, 0, sizeof(dst));

        test_case->fn(src, dst);
        if (memcmp(src, dst, test_case->size) != 0) {
            for (size_t i = 0; i < test_case->size; i++) {
                if (src[i] != dst[i]) {
                    fprintf(stderr,
                            "unsafe_prologue_mismatch case=%s iter=%d byte=%lu got=%u expected=%u\n",
                            test_case->name,
                            iter,
                            (unsigned long)i,
                            (unsigned int)dst[i],
                            (unsigned int)src[i]);
                    break;
                }
            }
            return 2;
        }
    }

    printf("unsafe_gum_prologue_guard_ok:%s\n", test_case->name);
    return 0;
}

int
main(int argc, char** argv)
{
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < unsafe_case_count; i++) {
            printf("%s %s\n", unsafe_cases[i].name, unsafe_cases[i].symbol);
        }
        return 0;
    }

    if (argc == 3 && strcmp(argv[1], "--case") == 0) {
        const UnsafeCase* test_case = find_case(argv[2]);
        if (test_case == NULL) {
            fprintf(stderr, "unsupported unsafe prologue case: %s\n", argv[2]);
            return 77;
        }
        return run_case(test_case);
    }

    if (argc == 1) {
        for (size_t i = 0; i < unsafe_case_count; i++) {
            int status = run_case(&unsafe_cases[i]);
            if (status != 0) {
                return status;
            }
        }
        return 0;
    }

    fprintf(stderr, "usage: %s [--list | --case NAME]\n", argv[0]);
    return 2;
}
