#include <stddef.h>
#include <stdlib.h>

static volatile size_t implementation_call_count;

#if defined(__ELF__) && (defined(__x86_64__) || defined(__amd64__))
__attribute__((visibility("hidden"), noinline, used))
void* peak_tail_jump_malloc_impl(size_t size)
{
    void* pointer = malloc(size);
    implementation_call_count++;
    return pointer;
}

__attribute__((visibility("hidden"), noinline, used))
void peak_tail_jump_free_impl(void* pointer)
{
    free(pointer);
    implementation_call_count++;
}

/* Cover Frontera's rel32 tail jump and the common RIP-indirect variant. */
__asm__(
    ".pushsection .text\n"
    ".p2align 4\n"
    ".globl fftw_malloc\n"
    ".type fftw_malloc,@function\n"
    "fftw_malloc:\n"
    "jmp peak_tail_jump_malloc_impl\n"
    ".size fftw_malloc, .-fftw_malloc\n"
    ".p2align 4\n"
    ".globl fftw_free\n"
    ".type fftw_free,@function\n"
    "fftw_free:\n"
    "jmp *peak_tail_jump_free_target(%rip)\n"
    ".size fftw_free, .-fftw_free\n"
    ".pushsection .data\n"
    ".p2align 3\n"
    ".local peak_tail_jump_free_target\n"
    ".type peak_tail_jump_free_target,@object\n"
    ".size peak_tail_jump_free_target, 8\n"
    "peak_tail_jump_free_target:\n"
    ".quad peak_tail_jump_free_impl\n"
    ".popsection\n"
    ".popsection\n");
#else
__attribute__((visibility("default"), noinline))
void* fftw_malloc(size_t size)
{
    return malloc(size);
}

__attribute__((visibility("default"), noinline))
void fftw_free(void* pointer)
{
    free(pointer);
}

#define peak_tail_jump_malloc_impl fftw_malloc
#define peak_tail_jump_free_impl fftw_free
#endif

__attribute__((visibility("default"), noinline))
void peak_tail_jump_call_implementations_directly(void)
{
    void* pointer = peak_tail_jump_malloc_impl(64);
    peak_tail_jump_free_impl(pointer);
}
