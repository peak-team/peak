#include "internal/general_listener/hook_coverage.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
  PeakHookCoverageSample totals;
  double attached_since;
  int attached;
} Hook;
struct PeakHookCoverage {
  Hook *hooks;
  size_t capacity;
  double start;
  int started;
};
PeakHookCoverage *peak_hook_coverage_create(size_t capacity) {
  PeakHookCoverage *c = calloc(1, sizeof(*c));
  if (!c)
    return NULL;
  if (!peak_hook_coverage_grow(c, capacity)) {
    free(c);
    return NULL;
  }
  return c;
}
void peak_hook_coverage_destroy(PeakHookCoverage *c) {
  if (c) {
    free(c->hooks);
    free(c);
  }
}
int peak_hook_coverage_grow(PeakHookCoverage *c, size_t capacity) {
  if (!c || capacity > SIZE_MAX / sizeof(Hook))
    return 0;
  if (capacity <= c->capacity)
    return 1;
  Hook *p = realloc(c->hooks, capacity * sizeof(*p));
  if (!p)
    return 0;
  memset(p + c->capacity, 0, (capacity - c->capacity) * sizeof(*p));
  c->hooks = p;
  c->capacity = capacity;
  return 1;
}
int peak_hook_coverage_start(PeakHookCoverage *c, double now) {
  if (!c || c->started || !isfinite(now) || now < 0)
    return 0;
  c->started = 1;
  c->start = now;
  for (size_t i = 0; i < c->capacity; i++) {
    c->hooks[i].totals.attached_wall_seconds = 0;
    if (c->hooks[i].attached && c->hooks[i].attached_since < now)
      c->hooks[i].attached_since = now;
  }
  return 1;
}
int peak_hook_coverage_attach(PeakHookCoverage *c, size_t id, double now,
                              int reattach) {
  if (!c || id >= c->capacity || !isfinite(now) || now < 0)
    return 0;
  Hook *h = &c->hooks[id];
  if (h->attached)
    return 0;
  h->attached = 1;
  h->attached_since = c->started && now < c->start ? c->start : now;
  h->totals.attach_successes++;
  if (reattach)
    h->totals.reattach_successes++;
  return 1;
}
int peak_hook_coverage_detach(PeakHookCoverage *c, size_t id, double now) {
  if (!c || id >= c->capacity || !isfinite(now) || now < 0)
    return 0;
  Hook *h = &c->hooks[id];
  if (!h->attached)
    return 0;
  if (c->started && now > h->attached_since)
    h->totals.attached_wall_seconds += now - h->attached_since;
  h->attached = 0;
  h->totals.detach_successes++;
  return 1;
}
int peak_hook_coverage_snapshot(const PeakHookCoverage *c, size_t id,
                                double now, PeakHookCoverageSample *out) {
  if (!c || !out || id >= c->capacity || !isfinite(now) || now < 0 ||
      !c->started)
    return 0;
  const Hook *h = &c->hooks[id];
  *out = h->totals;
  if (h->attached && now > h->attached_since)
    out->attached_wall_seconds += now - h->attached_since;
  return 1;
}
