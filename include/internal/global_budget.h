#ifndef PEAK_GLOBAL_BUDGET_H
#define PEAK_GLOBAL_BUDGET_H
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
typedef struct { uint64_t sequence, completed_wave, settled_control_credit_ns; double profile_seconds, control_seconds, management_seconds, elapsed_seconds, pressure, unfunded_seconds, transport_seconds, closing_seconds; uint32_t attached_targets; bool retiring, final, finalizer_invalid; } PeakGlobalBudgetSample;
typedef struct { bool active, wave_participant, cohort_finalized, accounting_invalid, recovery_pending; uint64_t wave, decision_sequence, view_epoch, root_completed_wave, acknowledged_sequence, acknowledged_demand_generation, request_deadline_ns, local_credit_ns, returned_total_ns, recovery_target_ns, rejected_spend_count, granted_total_ns; double global_spent_seconds, remaining_seconds, allowance_seconds, wave_span_seconds, granted_total_seconds, budget_seconds, reserved_seconds, control_reserve_seconds, startup_seconds, transport_seconds, refill_rtt_seconds, consumption_rate; } PeakGlobalBudgetView;
/* Listener stays owned by this module until stop. Call on the MPI init thread. */
int peak_global_budget_listen(unsigned short *port);
uint64_t peak_global_budget_nonce(void);
void peak_global_budget_set_nonce(uint64_t nonce);
int peak_global_budget_start(const char *host, unsigned short port, unsigned rank, unsigned size, double target, void (*wake)(void));
void peak_global_budget_publish(const PeakGlobalBudgetSample *sample);
PeakGlobalBudgetView peak_global_budget_view(void);
/* Fresh coordinator status without requiring a coherent diagnostic snapshot. */
bool peak_global_budget_active(void);
bool peak_global_budget_try_spend(uint64_t cost_ns);
bool peak_global_budget_try_spend_control(uint64_t cost_ns);
uint64_t peak_global_budget_control_consumed_ns(void);
void peak_global_budget_pressure(void);
void peak_global_budget_complete(uint64_t wave);
void peak_global_budget_startup(double seconds);
/* Root original MPI_Init duration only; independent from charged bootstrap. */
void peak_global_budget_origin(double original_mpi_init_seconds);
/* Return unused credits only after safe detach/idle boundary. */
void peak_global_budget_return_idle(void);
/* Caller has suspended callbacks and drained its controller. Helper stays alive. */
void peak_global_budget_retire(void);
/* Register once after HB join, using CPU baseline recorded at fini entry. */
void peak_global_budget_finalizer(clockid_t thread_clock, uint64_t cpu_start_ns, double frozen_management);
/* FINAL is published only after the existing all-rank MPI teardown proof. */
bool peak_global_budget_final_flush(unsigned timeout_ms);
void peak_global_budget_request_credit(uint64_t cost_ns);
bool peak_global_budget_flush(unsigned timeout_ms);
void peak_global_budget_stop(void);
#endif
