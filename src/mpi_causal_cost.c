#define _POSIX_C_SOURCE 200112L
#include "internal/mpi_causal_cost.h"
#include "internal/mpi_causal_runtime.h"

#include <math.h>
#include <stdlib.h>

PeakMpiCausalCostSlot *peak_mpi_causal_cost_slots;
size_t peak_mpi_causal_cost_capacity;
_Atomic double peak_mpi_causal_profile_price;
static _Atomic size_t used_slots;

int peak_mpi_causal_cost_prepare(size_t slots) {
  if (peak_mpi_causal_cost_slots != NULL)
    return slots == peak_mpi_causal_cost_capacity;
  if (slots == 0 || slots > SIZE_MAX / sizeof(PeakMpiCausalCostSlot))
    return 0;
  void *storage = NULL;
  if (posix_memalign(&storage, 64, slots * sizeof(PeakMpiCausalCostSlot)))
    return 0;
  PeakMpiCausalCostSlot *allocated = storage;
  for (size_t i = 0; i < slots; ++i) {
    atomic_init(&allocated[i].profile_seconds, 0);
    atomic_init(&allocated[i].observer_seconds, 0);
  }
  if (!atomic_is_lock_free(&allocated[0].profile_seconds)) {
    free(allocated);
    return 0;
  }
  atomic_init(&used_slots, 0);
  atomic_init(&peak_mpi_causal_profile_price, 0);
  peak_mpi_causal_cost_capacity = slots;
  peak_mpi_causal_cost_slots = allocated;
  return 1;
}

void peak_mpi_causal_runtime_note_slot(size_t slot) {
  if (peak_mpi_causal_cost_slots == NULL ||
      slot >= peak_mpi_causal_cost_capacity)
    return;
  size_t wanted = slot + 1;
  size_t old = atomic_load_explicit(&used_slots, memory_order_relaxed);
  /* Only slot publication, never a per-profile-call shared RMW. */
  while (old < wanted && !atomic_compare_exchange_weak_explicit(
                             &used_slots, &old, wanted, memory_order_release,
                             memory_order_relaxed)) {
  }
}

void peak_mpi_causal_runtime_publish_price(double seconds) {
  if (peak_mpi_causal_cost_slots != NULL && isfinite(seconds) && seconds >= 0)
    atomic_store_explicit(&peak_mpi_causal_profile_price, seconds,
                          memory_order_relaxed);
  /* Each actual callback adds its then-current price. A new price changes
   * only future charges; previously paid totals are never revalued. */
}

void peak_mpi_causal_cost_observer_add(size_t slot, double seconds) {
  if (peak_mpi_causal_cost_slots == NULL ||
      slot >= peak_mpi_causal_cost_capacity || !isfinite(seconds) ||
      seconds < 0)
    return;
  double old = atomic_load_explicit(
      &peak_mpi_causal_cost_slots[slot].observer_seconds, memory_order_relaxed);
  atomic_store_explicit(&peak_mpi_causal_cost_slots[slot].observer_seconds,
                        old + seconds, memory_order_relaxed);
}

size_t peak_mpi_causal_cost_used_slots(void) {
  return atomic_load_explicit(&used_slots, memory_order_acquire);
}

int peak_mpi_causal_cost_read_prefix(size_t count, double *profile,
                                     double *observer) {
  if (profile == NULL || observer == NULL || peak_mpi_causal_cost_slots == NULL)
    return 0;
  double P = 0, O = 0;
  if (count > peak_mpi_causal_cost_capacity)
    return 0;
  for (size_t i = 0; i < count; ++i) {
    P += atomic_load_explicit(&peak_mpi_causal_cost_slots[i].profile_seconds,
                              memory_order_relaxed);
    O += atomic_load_explicit(&peak_mpi_causal_cost_slots[i].observer_seconds,
                              memory_order_relaxed);
  }
  if (!isfinite(P) || !isfinite(O))
    return 0;
  *profile = P;
  *observer = O;
  return 1;
}

int peak_mpi_causal_cost_read(double *profile, double *observer) {
  return peak_mpi_causal_cost_read_prefix(peak_mpi_causal_cost_used_slots(),
                                          profile, observer);
}
