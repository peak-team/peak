/*
 * Return ENOMEM from a four-byte x86-64 entry. Gum cannot relocate enough
 * instructions for a near jump, so this fixture exercises the real refusal
 * for the complete allocator batch.
 */
__asm__(
    ".text\n"
    ".globl posix_memalign\n"
    ".type posix_memalign, @function\n"
    "posix_memalign:\n"
    "push $12\n"
    "pop %rax\n"
    "ret\n"
    ".size posix_memalign, .-posix_memalign\n");
