#ifndef PEAK_ENV_PARSER_H
#define PEAK_ENV_PARSER_H

/**
 * @file env_parser.h
 * @brief Validated numeric environment configuration for PEAK.
 */

#include <stdbool.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float detach_cost;
    unsigned int heartbeat_interval_us;
    unsigned int hibernation_cycle;
    float target_overhead_ratio;
    float global_overhead_ratio;
    float global_detach_factor;
    float global_reattach_factor;
    unsigned long long pause_timeout_ns;
    unsigned long long sig_cont_timeout_ns;
    unsigned int heartbeat_min_us;
    unsigned int heartbeat_max_us;
    double heartbeat_error_gain;
    double heartbeat_rate_gain;
    double heartbeat_ema_alpha;
} PeakRuntimeNumericConfig;

typedef struct {
    const char* name;
    const char* unit;
    unsigned long long fallback;
    unsigned long long minimum;
    unsigned long long maximum;
    bool zero_allowed;
} PeakEnvUnsignedSchema;

typedef struct {
    const char* name;
    const char* unit;
    double fallback;
    double minimum;
    double maximum;
    bool zero_allowed;
} PeakEnvRealSchema;

/** Parses a decimal unsigned value according to @p schema. */
unsigned long long peak_parse_env_unsigned(const PeakEnvUnsignedSchema* schema);

/** Parses a finite real value according to @p schema. */
double peak_parse_env_real(const PeakEnvRealSchema* schema);

/**
 * Parses PEAK's numeric heartbeat and detach configuration.
 *
 * Each variable is parsed at its widest representation before being checked
 * against its schema and narrowed. Invalid present values emit one warning and
 * use the stated schema fallback. A conflicting heartbeat maximum falls back
 * to the accepted minimum, and a conflicting reattach hysteresis factor falls
 * back to its default.
 */
PeakRuntimeNumericConfig peak_parse_runtime_numeric_config(void);

/** Parses a boolean environment variable. */
bool parse_env_to_bool(const char* env_var);

#ifdef __cplusplus
}
#endif

#endif /* PEAK_ENV_PARSER_H */
