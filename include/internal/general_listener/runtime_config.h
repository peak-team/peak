#ifndef PEAK_RUNTIME_CONFIG_H
#define PEAK_RUNTIME_CONFIG_H

/**
 * @file runtime_config.h
 * @brief Parse report/listener environment settings without lifecycle state.
 */

#include <stdbool.h>

/**
 * End-to-end timeout contract shared by socket publication and MPI teardown.
 *
 * The socket peer budget covers three socket phases: gather, report
 * publication, and confirmed release. The MPI post-publication gate then
 * retains two additional socket phases for rank-local fallback publication
 * and collective-arrival/progress margin.
 */
typedef struct {
    unsigned int socket_phase_timeout_ms;
    unsigned int socket_release_timeout_ms;
    unsigned int mpi_report_release_timeout_ms;
    bool socket_release_was_raised;
    unsigned int socket_combined_release_minimum_ms;
} PeakReportTimeoutBudget;

/**
 * Resolves the shared report timeout budget with saturating arithmetic.
 *
 * Defaults are 60000 ms per socket phase, 180000 ms peer release, 180000 ms
 * ordinary MPI/local report release, and a 300000 ms minimum for a
 * socket-combined release. The resolved socket invariants are
 * `socket_release >= 3 * socket_phase` (capped at INT_MAX) and
 * `socket_combined_release_minimum = socket_release + 2 * socket_phase`
 * (capped at UINT_MAX). The ordinary MPI/local timeout is not itself raised by
 * socket settings; callers select the socket minimum only for a path that
 * attempted socket publication. Invalid or zero values select the
 * corresponding default.
 */
PeakReportTimeoutBudget peak_general_listener_report_timeout_budget(void);

/**
 * Reads the optional detach-count override.
 *
 * @param count_out Receives the positive override when non-NULL.
 * @return true when a valid override is present; otherwise false.
 */
bool peak_general_listener_parse_detach_count_override(
    unsigned long* count_out);

/** Reads an unsigned environment value or returns @p default_value. */
unsigned int peak_general_listener_parse_uint_env_default(
    const char* name,
    unsigned int default_value);

/** Returns the first valid launcher-provided local MPI rank count, or one. */
unsigned int peak_general_listener_local_mpi_ranks(void);

/** Recognizes the listener's accepted case-insensitive true spellings. */
bool peak_general_listener_env_value_truthy(const char* value);

/** Returns whether failed socket aggregation may fall back to local output. */
bool peak_general_listener_socket_reduce_fallback_enabled(void);

/** Returns the first valid launcher-provided MPI world size, or -1. */
long peak_general_listener_mpi_env_size(void);

/** Returns the first valid launcher-provided MPI rank, or -1. */
long peak_general_listener_mpi_env_rank(void);

/**
 * Resolves a consistent launcher-provided world rank/size pair.
 *
 * Unlike the legacy independent accessors above, this function never combines
 * values from different launcher namespaces. Complete MPI-specific namespaces
 * must agree. A Slurm pair is used only when no complete MPI-specific pair is
 * available because launchers such as ibrun may propagate batch-step Slurm
 * metadata unchanged to every MPI process. Incomplete namespaces are ignored;
 * malformed complete pairs in the selected tier fail closed, as do
 * contradictory complete MPI-specific pairs.
 */
bool peak_general_listener_mpi_env_rank_size(long* rank_out,
                                             long* size_out);

/** Returns true when any recognized world rank/size variable is present. */
bool peak_general_listener_mpi_env_world_metadata_present(void);

/** Applies the text-output override and rank-local output default. */
bool peak_general_listener_should_print_text(bool rank_local_mpi_output);

#endif /* PEAK_RUNTIME_CONFIG_H */
