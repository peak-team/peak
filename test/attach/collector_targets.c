__attribute__((noinline, visibility("default"))) int attach_collected_first(int value)
{
    __asm__ volatile(".rept 24; nop; .endr");
    return value + 1;
}

__attribute__((noinline, visibility("default"))) int attach_collected_second(int value)
{
    __asm__ volatile(".rept 24; nop; .endr");
    return value * 2;
}
