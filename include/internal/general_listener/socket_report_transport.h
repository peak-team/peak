#ifndef PEAK_SOCKET_REPORT_TRANSPORT_H
#define PEAK_SOCKET_REPORT_TRANSPORT_H

#include "internal/general_listener/report_snapshot.h"

#include <stdbool.h>
#include <stddef.h>

/** Sources from which socket aggregation may obtain the process rank. */
typedef enum {
    /** Use active MPI first, then fall back to launcher metadata. */
    PEAK_SOCKET_REPORT_RANK_MPI_OR_ENV = 0,
    /** Use launcher metadata only; this mode never calls MPI. */
    PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
} PeakSocketReportRankSource;

/** Outcome of the blocking first phase of socket report aggregation. */
typedef enum {
    /** Aggregation failed; the caller may choose rank-local output. */
    PEAK_SOCKET_REPORT_FAILED = 0,
    /** This is a single-process report; an owned snapshot is ready. */
    PEAK_SOCKET_REPORT_SINGLE_READY,
    /** A peer submitted its snapshot and received the root's final ACK. */
    PEAK_SOCKET_REPORT_PEER_RELEASED,
    /** Root gathered every peer; CSV output must precede commit. */
    PEAK_SOCKET_REPORT_ROOT_PREPARED,
} PeakSocketReportStatus;

/** Opaque root-side state retained between gather and release. */
typedef struct PeakSocketReportSession PeakSocketReportSession;

/**
 * Gathers immutable report snapshots through the established wire-v9 socket
 * protocol.
 *
 * The function blocks peers until root commits or aborts the prepared report.
 * On SINGLE_READY and ROOT_PREPARED, @p aggregate_out receives an owned
 * snapshot. ROOT_PREPARED also returns an owned session through @p session_out.
 * Other outcomes leave both outputs NULL.
 */
PeakSocketReportStatus peak_socket_report_transport_begin(
    const PeakReportSnapshot* local,
    PeakSocketReportRankSource rank_source,
    PeakSocketReportSession** session_out,
    PeakReportSnapshot** aggregate_out);

/**
 * Sends the final ACK to every registered peer and consumes @p session.
 *
 * The caller must write the aggregate CSV before this call. A false result
 * preserves the wire-v9 behavior: no extra fallback release is attempted, so
 * peers that did not receive an ACK fail when their release wait expires.
 */
bool peak_socket_report_transport_commit(PeakSocketReportSession* session);

/**
 * Sends RELEASE_FALLBACK to every registered peer and consumes @p session.
 *
 * This is only for a root-side preparation failure before the established CSV
 * write and ACK sequence begins. A CSV I/O failure alone does not select this
 * path because wire-v9 still releases peers and prints aggregate text.
 */
void peak_socket_report_transport_abort(PeakSocketReportSession* session);

#ifdef PEAK_ENABLE_TEST_HOOKS
/** Parses the first host represented by a Slurm node-list expression. */
int peak_general_listener_test_first_slurm_host(const char* nodelist,
                                                char* out,
                                                size_t out_size);
#endif

#endif /* PEAK_SOCKET_REPORT_TRANSPORT_H */
