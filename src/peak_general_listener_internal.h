#ifndef __PEAK_GENERAL_LISTENER_INTERNAL_H
#define __PEAK_GENERAL_LISTENER_INTERNAL_H

#include "general_listener.h"

gboolean peak_general_listener_dynamic_attach_symbol(
    const char* symbol_name,
    gpointer symbol_address,
    gsize symbol_size,
    const char* provider_name);

#endif /* __PEAK_GENERAL_LISTENER_INTERNAL_H */
