#ifndef PEAK_GLOBAL_BUDGET_H
#define PEAK_GLOBAL_BUDGET_H
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
typedef struct { uint64_t sequence, completed_wave, settled_control_credit_ns, background_period_ns, settled_frontend_credit_ns; double ignored_seconds, profile_inside_seconds, frontend_seconds, profile_seconds, control_seconds, management_seconds, elapsed_seconds, pressure, unfunded_seconds, transport_seconds, closing_seconds; uint32_t attached_targets, credit_exposed_targets; bool credit_exposure_known, causal_supported, causal_fallback, retiring, final, finalizer_invalid, initial_unexposed; } PeakGlobalBudgetSample;
typedef struct { bool metadata_closed, startup_sealed, broker_final_written, active, wave_participant, cohort_finalized, accounting_invalid, recovery_pending; uint64_t background_period_ns, freshness_ns, broker_process_id, wave, decision_sequence, view_epoch, root_completed_wave, acknowledged_sequence, acknowledged_demand_generation, request_deadline_ns, local_credit_ns, returned_total_ns, recovery_target_ns, rejected_spend_count, granted_total_ns; double local_closing_reserved_seconds, causal_confirmed_seconds, causal_pending_seconds, causal_raw_seconds, causal_unrepresented_seconds, broker_cpu_seconds, global_spent_seconds, remaining_seconds, allowance_seconds, wave_span_seconds, granted_total_seconds, budget_seconds, reserved_seconds, control_reserve_seconds, startup_seconds, transport_seconds, refill_rtt_seconds, consumption_rate; } PeakGlobalBudgetView;
/* Host-only diagnostic snapshot; deliberately absent from the wire schema.
 * A complete socket write is not a per-message broker acknowledgment. */
typedef struct {
    bool written, retiring, final, finalizer_registered;
    uint64_t sample_sequence, message_sequence, profile_debit_ns, frontend_debit_ns;
    double ignored_seconds, profile_inside_seconds, frontend_seconds;
    double profile_sample_seconds, profile_billed_seconds, control_seconds;
    double management_input_seconds, management_sent_seconds, transport_seconds;
    double finalizer_baseline_seconds, finalizer_cpu_seconds;
    double finalizer_override_seconds, management_increment_seconds, total_seconds;
} PeakGlobalBudgetLocalBill;
PeakGlobalBudgetLocalBill peak_global_budget_local_bill(void);
/* Listener stays owned by this module until stop. Call on the MPI init thread. */
int peak_global_budget_listen(unsigned short *port);
uint64_t peak_global_budget_nonce(void);
void peak_global_budget_set_nonce(uint64_t nonce);
int peak_global_budget_start(const char *host, unsigned short port, unsigned rank, unsigned size, double target, void (*wake)(void));
void peak_global_budget_publish(const PeakGlobalBudgetSample *sample);
/* Controller evidence after real backend success; bounded atomics/rare wake only. */
void peak_global_budget_physical_targets(uint32_t count);
PeakGlobalBudgetView peak_global_budget_view(void);
/* Fresh coordinator status without requiring a coherent diagnostic snapshot. */
bool peak_global_budget_active(void);
/* Report ordinary heartbeat CPU; zero retains the current estimate. Returns the
 * negotiated background interval, without changing callback credit bounds. */
uint64_t peak_global_budget_heartbeat_period_ns(uint64_t cpu_ns, uint64_t configured_ns);
bool peak_global_budget_try_spend(uint64_t cost_ns);
bool peak_global_budget_try_spend_control(uint64_t cost_ns);
uint64_t peak_global_budget_control_consumed_ns(void);
void peak_global_budget_pressure(void);
void peak_global_budget_complete(uint64_t wave);
/* Rank zero publishes its final startup charge before enabling native grants. */
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
/* Replace the current total-wallet admission requirement only after the
 * caller has canceled/quiesced old mutations and published its current quote.
 * closing_ns is this rank's total closing requirement; zero cash cancels only
 * admission, never callback demand or already issued cash. */
void peak_global_budget_request_admission(uint64_t cash_ns, uint64_t closing_ns);
/* Increase only while an accepted admission/observation promise is live. */
void peak_global_budget_ensure_admission(uint64_t cash_ns, uint64_t closing_ns);
bool peak_global_budget_flush(unsigned timeout_ms);
void peak_global_budget_stop(void);
/* Native helper dispatch; not called by application callbacks. */
int peak_global_budget_broker_main(int argc, char **argv);
#endif /* PEAK_GLOBAL_BUDGET_H */
