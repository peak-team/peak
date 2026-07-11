#ifndef __PEAK_GENERAL_LISTENER_INTERNAL_H
#define __PEAK_GENERAL_LISTENER_INTERNAL_H

#include "general_listener.h"

typedef enum {
    PEAK_DYNAMIC_ATTACH_ATTACHED = 0,
    PEAK_DYNAMIC_ATTACH_NO_MATCH,
    PEAK_DYNAMIC_ATTACH_RETRY,
    PEAK_DYNAMIC_ATTACH_FAILED
} PeakDynamicAttachResult;

PeakDynamicAttachResult peak_general_listener_dynamic_attach_symbol(
    const char* symbol_name,
    gpointer symbol_address,
    gsize symbol_size,
    const char* provider_name);

gboolean peak_general_listener_dynamic_symbol_matches_any_target(
    const char* symbol_name,
    const char* provider_name);

#endif /* __PEAK_GENERAL_LISTENER_INTERNAL_H */
