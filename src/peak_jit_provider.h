#ifndef __PEAK_JIT_PROVIDER_H
#define __PEAK_JIT_PROVIDER_H

#include "frida-gum.h"

void peak_jit_provider_enable(void);
void peak_jit_provider_disable(void);
void peak_jit_provider_drain_pending(void);
gboolean peak_jit_provider_is_enabled(void);

#endif /* __PEAK_JIT_PROVIDER_H */
