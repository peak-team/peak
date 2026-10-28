#ifndef PEAK_OSCACHECONTROL_H
#define PEAK_OSCACHECONTROL_H
#include <stddef.h>
void sys_dcache_flush(void*, size_t);
void sys_icache_invalidate(void*, size_t);
#endif /* PEAK_OSCACHECONTROL_H */
