#ifndef PEAK_MPI_CAUSAL_RUNTIME_H
#define PEAK_MPI_CAUSAL_RUNTIME_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HAVE_MPI
#include "internal/mpi_policy_adapter.h"

/* The callback thread is the only counter writer. Retirement never clears a
 * slot: the existing pthread registry owns safe generation reuse. Price
 * segments change only during quiescent startup; callbacks and snapshot
 * readers must not run concurrently with price publication. */
typedef struct {
  _Atomic uint64_t callback_count;
  _Atomic double observer_seconds;
  uint64_t segment_start;
  double sealed_profile_seconds;
  unsigned char padding[64 - sizeof(_Atomic uint64_t) -
                        sizeof(_Atomic double) - sizeof(uint64_t) -
                        sizeof(double)];
} PeakMpiCausalCostSlot;

extern PeakMpiCausalCostSlot *peak_mpi_causal_cost_slots;
extern size_t peak_mpi_causal_cost_capacity;
extern _Atomic double peak_mpi_causal_profile_price;

static inline void peak_mpi_causal_profile_call(size_t slot) {
  PeakMpiCausalCostSlot *slots = peak_mpi_causal_cost_slots;
  /* Both listener entry paths have already validated the mapped slot against
   * the same fixed startup capacity used to allocate this slab. */
  if (slots != NULL) {
    uint64_t calls = atomic_load_explicit(&slots[slot].callback_count,
                                          memory_order_relaxed);
    atomic_store_explicit(&slots[slot].callback_count, calls + 1,
                          memory_order_relaxed);
  }
}

int peak_mpi_causal_runtime_requested(void);
int peak_mpi_causal_runtime_needs_service(void);
int peak_mpi_causal_runtime_queue_pending(void);
int peak_mpi_causal_runtime_service_available(void);
int peak_mpi_causal_runtime_prepare(size_t slots, double q,
                                    double heartbeat_max_seconds,
                                    double ema_alpha);
void peak_mpi_causal_runtime_note_slot(size_t slot);
void peak_mpi_causal_runtime_retire_slot(size_t slot, uint64_t generation);
/* Startup-only: no callback writers or cost snapshot readers may run while
 * old price segments are sealed. Already accumulated costs are not revalued. */
void peak_mpi_causal_runtime_publish_price(double seconds);
void peak_mpi_causal_runtime_configure_control(int heartbeat_enabled,
                                               int hook_controller_enabled);
void peak_mpi_causal_runtime_worker_start(void);
void peak_mpi_causal_runtime_activation_begin(void);
void peak_mpi_causal_runtime_activation_end(void);
void peak_mpi_causal_runtime_mpi_initialized(void);
void peak_mpi_causal_runtime_finalize(void);
int peak_mpi_causal_runtime_tick(double now, double elapsed,
                                  double control_seconds,
                                  double management_seconds,
                                  uint64_t control_generation);
int peak_mpi_causal_runtime_decision(double now, double elapsed,
                                     double existing_local_headroom,
                                     PeakMpiPolicyDecision *decision);
void peak_mpi_causal_runtime_cost_snapshot(double *L, double *P);
/* One component read supplies both raw diagnostics and authoritative cuts. */
typedef struct {
  double L, P, A, producer_observer, startup, control, heartbeat;
  uint64_t control_generation;
  PeakMpiCostCut cut;
  int app_cost_known;
} PeakMpiCausalCostSnapshot;
void peak_mpi_causal_runtime_cost_now(PeakMpiCausalCostSnapshot *);
void peak_mpi_causal_runtime_cost_components(double *L, double *P, double *A);
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

#endif /* PEAK_MPI_CAUSAL_RUNTIME_H */
