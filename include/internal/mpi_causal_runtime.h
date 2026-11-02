#ifndef PEAK_MPI_CAUSAL_RUNTIME_H
#define PEAK_MPI_CAUSAL_RUNTIME_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HAVE_MPI
#include "internal/mpi_policy_adapter.h"

/* The callback thread remains the only writer of a priced slot.  Retirement
 * never clears it: the existing pthread registry owns safe generation reuse. */
typedef struct {
  _Atomic double profile_seconds;
  _Atomic double observer_seconds;
  unsigned char padding[64 - 2 * sizeof(_Atomic double)];
} PeakMpiCausalCostSlot;

extern PeakMpiCausalCostSlot *peak_mpi_causal_cost_slots;
extern size_t peak_mpi_causal_cost_capacity;
extern _Atomic double peak_mpi_causal_profile_price;

static inline void peak_mpi_causal_profile_call(size_t slot) {
  PeakMpiCausalCostSlot *slots = peak_mpi_causal_cost_slots;
  if (slots != NULL && slot < peak_mpi_causal_cost_capacity) {
    double paid = atomic_load_explicit(&slots[slot].profile_seconds,
                                       memory_order_relaxed);
    double price = atomic_load_explicit(&peak_mpi_causal_profile_price,
                                        memory_order_relaxed);
    atomic_store_explicit(&slots[slot].profile_seconds, paid + price,
                          memory_order_relaxed);
  }
}

int peak_mpi_causal_runtime_requested(void);
int peak_mpi_causal_runtime_prepare(size_t slots, double q,
                                    double heartbeat_max_seconds,
                                    double ema_alpha);
void peak_mpi_causal_runtime_note_slot(size_t slot);
void peak_mpi_causal_runtime_retire_slot(size_t slot, uint64_t generation);
void peak_mpi_causal_runtime_publish_price(double seconds);
void peak_mpi_causal_runtime_configure_control(int heartbeat_enabled,
                                               int hook_controller_enabled);
void peak_mpi_causal_runtime_activation_begin(void);
void peak_mpi_causal_runtime_activation_end(void);
void peak_mpi_causal_runtime_mpi_initialized(void);
void peak_mpi_causal_runtime_finalize(void);
void peak_mpi_causal_runtime_tick(double now, double elapsed,
                                  double control_seconds,
                                  double management_seconds,
                                  uint64_t control_generation);
int peak_mpi_causal_runtime_decision(double now, double elapsed,
                                     double existing_local_headroom,
                                     PeakMpiPolicyDecision *decision);
void peak_mpi_causal_runtime_cost_snapshot(double *L, double *P);
void peak_mpi_causal_runtime_report(void);
#else
static inline void peak_mpi_causal_profile_call(size_t slot) { (void)slot; }
static inline void peak_mpi_causal_runtime_note_slot(size_t slot) {
  (void)slot;
}
static inline void peak_mpi_causal_runtime_retire_slot(size_t slot,
                                                       uint64_t generation) {
  (void)slot;
  (void)generation;
}
static inline void peak_mpi_causal_runtime_publish_price(double price) {
  (void)price;
}
#endif

#endif
