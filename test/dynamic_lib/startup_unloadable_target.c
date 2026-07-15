__attribute__((visibility("default"), noinline))
void peak_startup_unloadable_target(void)
{
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
#endif
}

__attribute__((visibility("default"), noinline))
void peak_startup_unloadable_target_second(void)
{
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
#endif
}
