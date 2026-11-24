#ifndef PEAK_ATTACH_RUNTIME_CONFIG_H
#define PEAK_ATTACH_RUNTIME_CONFIG_H

/* The attached runtime force-includes this header. Include stdlib before
 * redirecting getenv, so its public declaration remains intact. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static inline int
peak_attach_config_is_enabled(const char *value)
{
    return value != NULL &&
           (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0);
}

static inline const char *
peak_attach_exec_propagation_error(const char *chain, const char *checkpoint)
{
    if (peak_attach_config_is_enabled(chain) ||
        peak_attach_config_is_enabled(checkpoint))
        return "late attach cannot propagate PEAK across exec: PEAK_EXEC_CHAIN "
               "and PEAK_EXEC_CHECKPOINT require startup interposition";
    return NULL;
}

#if defined(PEAK_ATTACH_RUNTIME)
/* Import Gum's GLib symbol remapping before overriding g_getenv. */
#include "frida-gum.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif
char *peak_attach_getenv(const char *name);
const char *peak_attach_glib_getenv(const char *name);
int peak_attach_runtime_prepare(void);
int peak_attach_runtime_start(char **message);
int peak_attach_runtime_attempt_is_terminal(void);
/* Only the serialized agent preflight may reset, before terminal boundary. */
void peak_attach_config_reset_preflight(void);
#ifdef __cplusplus
}
#endif

#if defined(PEAK_ATTACH_RUNTIME)
#define getenv peak_attach_getenv
#undef g_getenv
#define g_getenv peak_attach_glib_getenv
#endif
#endif /* PEAK_ATTACH_RUNTIME_CONFIG_H */
