#ifndef __PEAK_EXEC_INTERCEPTOR_INTERNAL_H
#define __PEAK_EXEC_INTERCEPTOR_INTERNAL_H

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_EXEC_INTERNAL __attribute__((visibility("hidden")))
#else
#define PEAK_EXEC_INTERNAL
#endif

PEAK_EXEC_INTERNAL int peak_exec_checkpoint_enabled_at_startup(void);

#endif /* __PEAK_EXEC_INTERCEPTOR_INTERNAL_H */
