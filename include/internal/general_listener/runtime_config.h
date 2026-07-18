#ifndef PEAK_RUNTIME_CONFIG_H
#define PEAK_RUNTIME_CONFIG_H

/**
 * @file runtime_config.h
 * @brief Parse general-listener environment settings without lifecycle state.
 */

#include <stdbool.h>

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

/** Applies the text-output override and rank-local output default. */
bool peak_general_listener_should_print_text(bool rank_local_mpi_output);

#endif /* PEAK_RUNTIME_CONFIG_H */
