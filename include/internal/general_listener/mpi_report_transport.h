#ifndef PEAK_MPI_REPORT_TRANSPORT_H
#define PEAK_MPI_REPORT_TRANSPORT_H

#include "internal/general_listener/report_snapshot.h"

#include <stdbool.h>

/** Outcome of one MPI final-report aggregation attempt. */
typedef enum {
    /** Rank zero owns the aggregate returned through the output parameter. */
    PEAK_MPI_REPORT_TRANSPORT_ROOT_READY = 0,
    /** A non-root rank completed every collective successfully. */
    PEAK_MPI_REPORT_TRANSPORT_PEER_COMPLETE,
    /** Report identities or semantic prerequisites require local output. */
    PEAK_MPI_REPORT_TRANSPORT_LOCAL_FALLBACK,
    /** MPI failed or timed out and this process must not call MPI again. */
    PEAK_MPI_REPORT_TRANSPORT_FAILED_CLOSED,
} PeakMpiReportTransportResult;

/**
 * Aggregates an immutable local report through MPI_COMM_WORLD.
 *
 * Collective order, datatypes, operations, and diagnostic labels are part of
 * this private transport contract. A collective failure or timeout poisons the
 * transport with release semantics; all later calls return fail-closed without
 * touching MPI. Duplicate names and rank-to-rank count/name mismatches are
 * semantic incompatibilities and return local fallback without poisoning MPI.
 *
 * On PEAK_MPI_REPORT_TRANSPORT_ROOT_READY, rank zero owns the snapshot stored
 * in @p root_aggregate. All other outcomes leave it NULL. The aggregate keeps
 * rank zero's owned names, program text, and instrumented markers while
 * replacing reducible metrics, transition markers, overhead, and rank count
 * with their MPI aggregate.
 */
PeakMpiReportTransportResult peak_mpi_report_transport_reduce(
    const PeakReportSnapshot* local,
    PeakReportSnapshot** root_aggregate);

/** Returns whether an MPI transport failure has poisoned this process. */
bool peak_mpi_report_transport_failed_closed(void);

/**
 * Clears the fail-closed marker before a new, proven-safe report lifecycle.
 *
 * This must never be used to retry MPI after a failed aggregation request.
 */
void peak_mpi_report_transport_reset_failed_closed(void);

#endif /* PEAK_MPI_REPORT_TRANSPORT_H */
