#include "dlopen_entry_accounting.h"

static unsigned int peak_dlopen_entry_codegen_counter;

__attribute__((noinline, used))
void
peak_dlopen_entry_codegen_probe(void)
{
    PEAK_DLOPEN_REGISTER_REPLACEMENT_ENTRY(
        &peak_dlopen_entry_codegen_counter);
}
