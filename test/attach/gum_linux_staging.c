#include "frida-gum.h"
#include <assert.h>
#include <string.h>
static volatile int hits;
__attribute__((noinline)) static int target(int n) {
  asm volatile(".rept 20; nop; .endr");
  return n + 1;
}
static void enter(GumInvocationContext *c, gpointer d) {
  (void)c;
  (void)d;
  hits++;
}
int main(void) {
  int (*volatile call)(int) = target;
  gum_init_embedded();
  GumInterceptor *i = gum_interceptor_obtain();
  GumInvocationListener *l = gum_make_call_listener(enter, NULL, NULL, NULL);
  GumPeakStagedMutation *s;
  guint8 before[32], active[32], original[32];
  gpointer p;
  guint len;
  memcpy(before, target, 32);
  assert(gum_interceptor_peak_prepare_staged_mutation(i, target, l, NULL, NULL,
                                                      NULL, &s));
  assert(memcmp(before, target, 32) == 0);
  assert(call(1) == 2 && hits == 0);
  assert(gum_interceptor_peak_staged_mutation_patch(s, &p, active, original,
                                                    &len));
  assert(p == (gpointer)target && len <= 32);
  gum_interceptor_peak_finish_staged_mutation(s, FALSE);
  assert(memcmp(before, target, 32) == 0);
  assert(gum_interceptor_peak_prepare_staged_mutation(i, target, l, NULL, NULL,
                                                      NULL, &s));
  assert(gum_interceptor_peak_staged_mutation_patch(s, &p, active, original,
                                                    &len));
  gum_mprotect(p, len, GUM_PAGE_RWX);
  memcpy(p, active, len);
  __builtin___clear_cache(p, (char *)p + len);
  gum_interceptor_peak_finish_staged_mutation(s, TRUE);
  assert(call(1) == 2 && hits == 1);
  return 0;
}
