__attribute__((noinline, visibility("default")))
int peak_attach_plugin_target(int value)
{
    /* Leave a relocatable prefix on both Linux architectures. */
    __asm__ volatile(".rept 20; nop; .endr");
    return value + 1;
}
