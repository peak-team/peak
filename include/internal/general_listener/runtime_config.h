#ifndef PEAK_RUNTIME_CONFIG_H
#define PEAK_RUNTIME_CONFIG_H

/**
 * @file runtime_config.h
 * @brief Parse report/listener environment settings without lifecycle state.
 */

#include <stdbool.h>

/** Compiled root concurrency used by the shared gather-budget calculation. */
#define PEAK_SOCKET_GATHER_ACTIVE_MAX 128U

/** Per-connection-wave margin added to the absolute gather deadline. */
#define PEAK_SOCKET_GATHER_WAVE_BUDGET_MS 5000U

/** Maximum adaptive margin; the configured base timeout is added afterward. */
#define PEAK_SOCKET_GATHER_ADAPTIVE_MARGIN_MAX_MS 300000U

/**
 * End-to-end timeout contract shared by socket publication and MPI teardown.
 *
 * The socket peer budget covers the scaled absolute gather cap, report
 * publication, and confirmed release. The MPI post-publication gate then
 * retains two additional base phases for rank-local fallback publication and
 * collective-arrival/progress margin.
 */
typedef struct {
    unsigned int socket_phase_timeout_ms;
    unsigned int socket_gather_wave_budget_ms;
    unsigned int socket_gather_hard_timeout_ms;
    unsigned int socket_release_timeout_ms;
    unsigned int mpi_report_release_timeout_ms;
    bool socket_gather_timeout_was_scaled;
    bool socket_release_was_raised;
    unsigned int socket_combined_release_minimum_ms;
} PeakReportTimeoutBudget;

/**
 * Resolves the shared report timeout budget with saturating arithmetic.
 *
 * The zero-argument form derives the world size from launcher metadata when
 * available. See the rank-count form for the scaling and saturation contract.
 */
PeakReportTimeoutBudget peak_general_listener_report_timeout_budget(void);

/**
 * Resolves the shared report timeout budget for @p rank_count ranks.
 *
 * The socket inactivity/phase default is 60000 ms. Socket admission uses a
 * nominal maximum of 128 ranks per wave and a nominal 5000 ms wave budget.
 * The effective width may be reduced by the root's file-descriptor limit, and
 * admission spacing is clamped to preserve both the no-progress phase and the
 * absolute gather deadline. The nominal wave budget also scales the absolute
 * deadline; its adaptive margin is capped at 300000 ms and added with
 * saturation, so an explicit phase timeout is never shortened. The peer
 * end-to-end release minimum is
 * `gather_hard + 2 * socket_phase` (capped at INT_MAX), and the
 * socket-combined MPI gate adds two further phase margins (capped at
 * UINT_MAX). The ordinary MPI/local release default remains 180000 ms.
 * Invalid or zero environment values select the corresponding default.
 */
PeakReportTimeoutBudget
peak_general_listener_report_timeout_budget_for_rank_count(
    unsigned int rank_count);

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

/**
 * Resolves a consistent launcher-provided world rank/size pair.
 *
 * This function never combines values from different launcher namespaces.
 * Complete MPI-specific namespaces must agree. A Slurm pair is used only when
 * no complete MPI-specific pair is available because launchers such as ibrun
 * may propagate batch-step Slurm metadata unchanged to every MPI process.
 * Incomplete namespaces are ignored; malformed complete pairs in the selected
 * tier fail closed, as do contradictory complete MPI-specific pairs.
 */
bool peak_general_listener_mpi_env_rank_size(long* rank_out,
                                             long* size_out);

/** Returns true when any recognized world rank/size variable is present. */
bool peak_general_listener_mpi_env_world_metadata_present(void);

/** Applies the text-output override and rank-local output default. */
bool peak_general_listener_should_print_text(bool rank_local_mpi_output);

#endif /* PEAK_RUNTIME_CONFIG_H */
