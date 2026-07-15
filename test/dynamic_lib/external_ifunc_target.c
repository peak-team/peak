#include <string.h>

typedef void (*peak_external_ifunc_fn)(void);

extern void* peak_external_ifunc_selected;

/*
 * Deliberately resolve through a pointer selected by the main executable.
 * There is no loader lookup or relocation edge from this DSO to the DSO that
 * owns the returned implementation address, so retaining the IFUNC-defining
 * DSO cannot retain the implementation provider as a side effect.
 */
static peak_external_ifunc_fn
peak_external_ifunc_resolve(void)
{
    peak_external_ifunc_fn function = NULL;

    if (peak_external_ifunc_selected == NULL ||
        sizeof(function) != sizeof(peak_external_ifunc_selected)) {
        return NULL;
    }
    memcpy(&function,
           &peak_external_ifunc_selected,
           sizeof(function));
    return function;
}

__attribute__((visibility("default"),
               ifunc("peak_external_ifunc_resolve")))
void peak_external_ifunc_target(void);
