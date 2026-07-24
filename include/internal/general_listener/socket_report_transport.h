#ifndef PEAK_SOCKET_REPORT_TRANSPORT_H
#define PEAK_SOCKET_REPORT_TRANSPORT_H

/**
 * @file socket_report_transport.h
 * @brief Aggregate immutable final-report snapshots through PEAK wire-v11.
 */

#include "internal/general_listener/report_snapshot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Sources from which socket aggregation may obtain the process rank. */
typedef enum {
    /** Use active MPI first, then fall back to launcher metadata. */
    PEAK_SOCKET_REPORT_RANK_MPI_OR_ENV = 0,
    /** Use launcher metadata only; this mode never calls MPI. */
    PEAK_SOCKET_REPORT_RANK_ENV_ONLY,
    /** Require launcher metadata and never assume an unlabelled singleton. */
    PEAK_SOCKET_REPORT_RANK_ENV_REQUIRED,
} PeakSocketReportRankSource;

/** Outcome of the blocking first phase of socket report aggregation. */
typedef enum {
    /** Aggregation failed; the caller may choose rank-local output. */
    PEAK_SOCKET_REPORT_FAILED = 0,
    /** This is a single-process report; an owned snapshot is ready. */
    PEAK_SOCKET_REPORT_SINGLE_READY,
    /** A peer submitted its snapshot and accepted the root's final ACK. */
    PEAK_SOCKET_REPORT_PEER_RELEASED,
    /** Root gathered every peer; complete report output must precede commit. */
    PEAK_SOCKET_REPORT_ROOT_PREPARED,
} PeakSocketReportStatus;

/** Opaque root-side state retained between gather and release. */
typedef struct PeakSocketReportSession PeakSocketReportSession;

/**
 * Gathers immutable report snapshots through the established wire-v11 socket
 * protocol.
 *
 * Root advances a bounded set of nonblocking peer connections under both a
 * no-progress deadline and a rank-count-scaled absolute gather deadline.
 * Valid protocol milestones refresh only the former. Root reserves the release
 * port before accepting gather peers so outbound ephemeral ports cannot claim
 * it between phases. A peer enters the final-release wait only after receiving
 * a complete registration receipt. Root completes that peer's gather only
 * after receiving the matching receipt confirmation; if the confirmation
 * fails, the registered peer waits for root's authoritative fallback decision.
 * A partial, invalid, duplicate, or unconfirmed peer fails the whole aggregate
 * without publishing it.
 *
 * The function blocks peers until root commits or aborts the prepared report.
 * On SINGLE_READY and ROOT_PREPARED, @p aggregate_out receives an owned
 * snapshot. ROOT_PREPARED also returns an owned session through @p session_out.
 * Both output pointers are required. Other outcomes, including a missing
 * output pointer, clear every provided output to NULL. Wire-v11 does not carry
 * instrumented markers; the aggregate retains root's marker or promotes it
 * when a positive aggregate call count proves that some rank instrumented the
 * slot. Peer-only instrumented slots with zero calls are not represented.
 * Wire-v11 is supported only among ranks with the same byte order,
 * floating-point representation, and 64-bit Linux C ABI. The peer release-wait
 * budget covers the absolute gather budget plus two phase budgets for report
 * publication and confirmed release. A positive
 * `PEAK_OUTPUT_AGGREGATION_RELEASE_TIMEOUT_MS` may raise that budget, but it is
 * clamped to that computed minimum.
 */
PeakSocketReportStatus peak_socket_report_transport_begin(
    const PeakReportSnapshot* local,
    PeakSocketReportRankSource rank_source,
    PeakSocketReportSession** session_out,
    PeakReportSnapshot** aggregate_out);

/**
 * Sends the final ACK decision to every registered peer, waits for each peer's
 * confirmation, and consumes @p session.
 *
 * The caller must atomically publish the aggregate CSV and finish the text
 * report before this call. A validated decision is authoritative: a peer that
 * received ACK never downgrades to rank-local output merely because its
 * confirmation path later fails. A false result means at least one registered
 * peer did not confirm before the root deadline; no contradictory fallback
 * decision is sent after ACK release begins.
 */
bool peak_socket_report_transport_commit(PeakSocketReportSession* session);

/**
 * Sends RELEASE_FALLBACK to every registered peer and consumes @p session.
 *
 * This is for any root-side preparation or report-publication failure before
 * the final ACK sequence begins, including aggregate CSV I/O failure.
 */
void peak_socket_report_transport_abort(PeakSocketReportSession* session);

#ifdef PEAK_ENABLE_TEST_HOOKS
/** Test-only observations from the current process's latest socket gather. */
typedef struct {
    uint32_t wire_version;
    uint32_t root_payload_count;
    uint32_t root_receipt_count;
    uint32_t root_confirmation_count;
    uint32_t root_max_active;
    uint32_t root_release_target_count;
    uint32_t root_release_confirmed_count;
    uint8_t root_release_decision;
    bool peer_receipt_received;
    bool peer_confirmation_sent;
    bool peer_release_started;
    bool peer_release_decision_received;
    bool peer_release_confirmation_sent;
    uint8_t peer_release_decision;
} PeakSocketReportTestTelemetry;

/** Clears test-only socket gather observations. */
void peak_socket_report_test_telemetry_reset(void);

/** Copies test-only socket gather observations to @p telemetry_out. */
void peak_socket_report_test_telemetry_get(
    PeakSocketReportTestTelemetry* telemetry_out);

/** Computes a test-only rolling deadline from an injected clock value. */
int64_t peak_socket_report_test_progress_deadline_us(
    int64_t now_us,
    int64_t hard_deadline_us,
    int progress_timeout_ms);

/** Parses the first host represented by a Slurm node-list expression. */
int peak_general_listener_test_first_slurm_host(const char* nodelist,
                                                char* out,
                                                size_t out_size);
#endif

#endif /* PEAK_SOCKET_REPORT_TRANSPORT_H */
