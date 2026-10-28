#ifndef PEAK_GLOBAL_BUDGET_H
#define PEAK_GLOBAL_BUDGET_H
#include <stdint.h>
#include <stdbool.h>
typedef struct { uint64_t sequence, completed_wave; double profile_seconds, control_seconds, management_seconds, elapsed_seconds, pressure, unfunded_seconds, transport_seconds; uint32_t attached_targets; } PeakGlobalBudgetSample;
typedef struct { bool active; uint64_t wave, decision_sequence, view_epoch, root_completed_wave, acknowledged_sequence; double global_spent_seconds, remaining_seconds, allowance_seconds, wave_span_seconds, granted_total_seconds, budget_seconds, reserved_seconds, control_reserve_seconds, startup_seconds, transport_seconds; } PeakGlobalBudgetView;
/* Listener stays owned by this module until stop. Call on the MPI init thread. */
int peak_global_budget_listen(unsigned short *port);
uint64_t peak_global_budget_nonce(void);
void peak_global_budget_set_nonce(uint64_t nonce);
int peak_global_budget_start(const char *host, unsigned short port, unsigned rank, unsigned size, double target, void (*wake)(void));
void peak_global_budget_publish(const PeakGlobalBudgetSample *sample);
PeakGlobalBudgetView peak_global_budget_view(void);
bool peak_global_budget_try_spend(uint64_t cost_ns);
void peak_global_budget_pressure(void);
void peak_global_budget_complete(uint64_t wave);
void peak_global_budget_startup(double seconds);
void peak_global_budget_request_credit(uint64_t cost_ns);
bool peak_global_budget_flush(unsigned timeout_ms);
void peak_global_budget_stop(void);
#endif
