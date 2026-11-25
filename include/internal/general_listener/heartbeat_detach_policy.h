#ifndef PEAK_HEARTBEAT_DETACH_POLICY_H
#define PEAK_HEARTBEAT_DETACH_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

typedef struct {
    size_t index;
    double ratio; /* Combined detach pressure or reattach historical seconds. */
    double rate;  /* Detach contribution or oldest-detach ordering key. */
    double lifetime;
} OverheadEntry;

static inline int
peak_heartbeat_compare_detach_contribution(double x, double y,
                                          size_t x_index, size_t y_index)
{
    if (x < y) return 1;
    if (x > y) return -1;
    return (x_index > y_index) - (x_index < y_index);
}

static inline int
compare_ratio_de(const void* a, const void* b)
{
    const OverheadEntry* x = a;
    const OverheadEntry* y = b;

    if (x->ratio < y->ratio) return 1;
    if (x->ratio > y->ratio) return -1;

    /* Prefer faster-growing overhead when combined scores tie. */
    return peak_heartbeat_compare_detach_contribution(x->rate, y->rate,
                                                      x->index, y->index);
}

static inline int
peak_heartbeat_compare_recent_detach(const void* a, const void* b)
{
    const OverheadEntry* x = a;
    const OverheadEntry* y = b;
    return peak_heartbeat_compare_detach_contribution(x->rate, y->rate,
                                                      x->index, y->index);
}

static inline int
peak_heartbeat_compare_lifetime_detach(const void* a, const void* b)
{
    const OverheadEntry* x = a;
    const OverheadEntry* y = b;
    return peak_heartbeat_compare_detach_contribution(x->lifetime, y->lifetime,
                                                      x->index, y->index);
}

static inline void
peak_heartbeat_sort_detach_entries(OverheadEntry* entries, size_t count,
                                  bool recent_exceeded, bool lifetime_exceeded)
{
    if (count > 1) {
        qsort(entries, count, sizeof(*entries),
              recent_exceeded && lifetime_exceeded ? compare_ratio_de :
              recent_exceeded ? peak_heartbeat_compare_recent_detach :
                                peak_heartbeat_compare_lifetime_detach);
    }
}

#endif /* PEAK_HEARTBEAT_DETACH_POLICY_H */
