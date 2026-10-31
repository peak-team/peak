#ifndef PEAK_MPI_BUDGET_EVENTS_H
#define PEAK_MPI_BUDGET_EVENTS_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
/* Sink copies all pointed-to arrays before returning. No MPI calls, waits for
 * broker credit, or callbacks into this frontend are permitted in the sink. */
typedef enum {
    PEAK_MPI_EVENT_FRONTIER, PEAK_MPI_EVENT_POST, PEAK_MPI_EVENT_WAIT,
    PEAK_MPI_EVENT_COMPLETE, PEAK_MPI_EVENT_UNKNOWN,
    PEAK_MPI_EVENT_ENTER, PEAK_MPI_EVENT_EXIT
} PeakMpiEventKind;
typedef enum { PEAK_MPI_OP_SEND, PEAK_MPI_OP_RECV,
               PEAK_MPI_OP_COLLECTIVE, PEAK_MPI_OP_AMBIGUOUS_SEND } PeakMpiEventOp;
enum {
    PEAK_MPI_CLOSED_WORLD_PREFIX = 1U << 0,
    PEAK_MPI_ENTRY_FENCE = 1U << 1,
    PEAK_MPI_OPAQUE_LIVE = 1U << 2
};
typedef struct {
    PeakMpiEventKind kind;
    PeakMpiEventOp op;
    uint64_t seq, last_mpi_seq, comm, round, tag, request;
    int rank;
    int64_t peer;
    uint32_t context_owner;
    /* MPI_EXIT: globally match same WORLD context/round. This certifies
     * a completed cost prefix, not empty MPI transport buffers. ENTRY_FENCE
     * is set only for an eligible successful actual WORLD MPI_Barrier.
     * UNKNOWN may carry OPAQUE_LIVE: untracked live state cannot recertify;
     * the broker closes causal metadata and keeps application accounting SUM. */
    uint32_t closure_flags;
    /* Saturated sum of all four snapshot fields; adapter may separately route
     * outside cost to causal lanes and inside cost to serialized SUM. */
    uint64_t cumulative_cost_ns;
    const uint64_t *requests;
    size_t count;
} PeakMpiBudgetEvent;
/* Implemented by the listener/wire integration. Feature-off must stay cheap. */
typedef struct {
    uint64_t active_outside_ns, active_inside_ns;
    uint64_t ignored_outside_ns, ignored_inside_ns;
    uint64_t runtime_raw_ns, runtime_outside_ns, runtime_sum_ns;
} PeakMpiBudgetCostSnapshot;
typedef enum { PEAK_RUNTIME_CONTROL, PEAK_RUNTIME_MANAGEMENT,
               PEAK_RUNTIME_TRANSPORT, PEAK_RUNTIME_COMPONENTS } PeakMpiRuntimeComponent;
typedef struct {
    uint64_t mpi_sequence, lifecycle_epoch;
    unsigned component;
    bool registered, eligible;
} PeakMpiRuntimeSpan;
/* Own measured delta only. raw_after is the authoritative cumulative raw
 * component after publishing that delta; no stream lock spans the activity. */
PeakMpiRuntimeSpan peak_mpi_budget_runtime_begin(PeakMpiRuntimeComponent component);
void peak_mpi_budget_runtime_end(PeakMpiRuntimeSpan *span, uint64_t own_ns,
                                uint64_t raw_after);
void peak_mpi_budget_runtime_observe(PeakMpiRuntimeComponent component, uint64_t raw);
void peak_mpi_budget_runtime_snapshot(PeakMpiBudgetCostSnapshot *snapshot);
void peak_mpi_budget_runtime_classification_close(void);
int peak_global_budget_mpi_events_enabled(void);
/* One shared funded foreground metadata operation (outer wrapper). Failure
 * permanently closes this run's metadata gate; application MPI still runs. */
int peak_global_budget_mpi_trace_begin(void);
void peak_mpi_budget_cost_snapshot(PeakMpiBudgetCostSnapshot *snapshot);
/* Conservative non-native monotonic elapsed upper bound on added frontend
 * CPU, including local context bookkeeping. Adapter retains the raw amount
 * and adds it to the next ordered outside-cost snapshot; unseen tails remain
 * reserved. UINT64_MAX means an unusable/saturated clock and must fail closed. */
void peak_global_budget_mpi_frontend_charge(uint64_t cpu_ns);
void peak_global_budget_mpi_context_register(uint32_t owner, uint64_t serial,
                                            const int *members, size_t count);
/* Accept remaining admitted records and terminal UNKNOWN after gate closure. */
void peak_global_budget_mpi_event_submit(const PeakMpiBudgetEvent *event,
                                       const PeakMpiBudgetCostSnapshot *snapshot);
/* Called on the successful outer MPI initializer, before application resumes. */
void peak_mpi_budget_events_initialize(void);
/* Private heartbeat-only publication, sharing the MPI stream lock/sequence.
 * The caller MUST meter this entire call in actual HB management SUM CPU and
 * its adaptive budget. No MPI, foreground token, or frontend cost settlement
 * occurs here; publication without a foreground operation is ready immediately. */
void peak_mpi_budget_events_frontier(void);
/* Shared TLS guard also covers bootstrap and native MPI implementation calls. */
int peak_mpi_application_enter(void);
int peak_mpi_application_active(void);
void peak_mpi_application_leave(void);
#endif /* PEAK_MPI_BUDGET_EVENTS_H */
