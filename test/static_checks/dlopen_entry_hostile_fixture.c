#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef void* (*peak_dlopen_fn)(const char* filename, int flags);

__attribute__((visibility("hidden")))
void* peak_dlopen(const char* filename, int flags);

_Atomic unsigned int peak_dlopen_active_replacement_count
    __attribute__((visibility("hidden"))) = 0;

__attribute__((visibility("hidden"), noinline))
void*
peak_dlopen_body(const char* filename, int flags)
{
    return filename != NULL ? (void*)(uintptr_t)(unsigned int)flags : NULL;
}

/* Keep the hidden assembly entry reachable despite --gc-sections. */
__attribute__((used, visibility("default")))
peak_dlopen_fn peak_dlopen_entry_hostile_keep = peak_dlopen;
