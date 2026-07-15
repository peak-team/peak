#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>

static void whereis(const char *name, void *p) {
    Dl_info info;
    if (dladdr(p, &info) && info.dli_fname) {
        // printf("[where] %-12s @ %-18p from %s\n", name, p, info.dli_fname);
    } else {
        // printf("[where] %-12s @ %-18p (dladdr: unknown)\n", name, p);
    }
}

__attribute__((visibility("default"), noinline))
void b_dynamic(void) {
    whereis("b_dynamic", (void*) &b_dynamic);
    // puts("libB: b_dynamic() says hello");
}

__attribute__((visibility("default"), noinline))
void b_tail_impl(void) {
    __asm__ __volatile__("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
}

#if defined(__x86_64__)
__asm__(
    ".text\n"
    ".globl b_tail_wrapper\n"
    ".type b_tail_wrapper, @function\n"
    "b_tail_wrapper:\n"
    "jmp b_tail_impl@PLT\n"
    ".size b_tail_wrapper, .-b_tail_wrapper\n");
#elif defined(__aarch64__)
__asm__(
    ".text\n"
    ".globl b_tail_wrapper\n"
    ".type b_tail_wrapper, %function\n"
    "b_tail_wrapper:\n"
    "b b_tail_impl\n"
    ".size b_tail_wrapper, .-b_tail_wrapper\n");
#else
__attribute__((visibility("default"), noinline))
void b_tail_wrapper(void) {
    b_tail_impl();
}
#endif
