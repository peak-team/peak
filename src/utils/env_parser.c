#include "env_parser.h"

#include "logging.h"

#include <errno.h>
#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>

typedef enum {
    PEAK_ENV_NUMBER_UNSIGNED,
    PEAK_ENV_NUMBER_REAL,
} PeakEnvNumberType;

typedef struct {
    const char* name;
    PeakEnvNumberType type;
    const char* unit;
    double fallback;
    double minimum;
    double maximum;
    bool zero_allowed;
} PeakEnvNumberSchema;

#define PEAK_SECONDS_PER_MICROSECOND 1000000.0
#define PEAK_SECONDS_PER_NANOSECOND 1000000000.0
/* The current production numeric schemas use fewer than 32 variable names. */
#define PEAK_ENV_WARNING_LIMIT 64U

static atomic_flag peak_env_warning_lock = ATOMIC_FLAG_INIT;
static const char* peak_env_warned_names[PEAK_ENV_WARNING_LIMIT];

static bool
peak_env_warning_first_for(const char* name)
{
    bool first = false;

    while (atomic_flag_test_and_set_explicit(&peak_env_warning_lock,
                                             memory_order_acquire)) {
    }
    for (size_t index = 0; index < PEAK_ENV_WARNING_LIMIT; index++) {
        if (peak_env_warned_names[index] == NULL) {
            peak_env_warned_names[index] = name;
            first = true;
            break;
        }
        if (strcmp(peak_env_warned_names[index], name) == 0) {
            break;
        }
    }
    if (!first && peak_env_warned_names[PEAK_ENV_WARNING_LIMIT - 1] != NULL) {
        /* Preserve visibility if a future schema set outgrows this small cache. */
        first = true;
    }
    atomic_flag_clear_explicit(&peak_env_warning_lock, memory_order_release);
    return first;
}

static const PeakEnvNumberSchema peak_detach_cost_schema = {
    "PEAK_COST", PEAK_ENV_NUMBER_REAL, "seconds", 0.0,
    0.0, FLT_MAX, true,
};
static const PeakEnvNumberSchema peak_heartbeat_interval_schema = {
    "PEAK_HEARTBEAT_INTERVAL", PEAK_ENV_NUMBER_REAL, "seconds", 0.1,
    0.0, (double)UINT_MAX / PEAK_SECONDS_PER_MICROSECOND, true,
};
static const PeakEnvNumberSchema peak_hibernation_cycle_schema = {
    "PEAK_HIBERNATION_CYCLE", PEAK_ENV_NUMBER_UNSIGNED, "cycles", 50.0,
    0.0, UINT_MAX, true,
};
static const PeakEnvNumberSchema peak_target_ratio_schema = {
    "PEAK_OVERHEAD_RATIO", PEAK_ENV_NUMBER_REAL, "ratio", 0.1,
    0.0, FLT_MAX, true,
};
static const PeakEnvNumberSchema peak_global_ratio_schema = {
    "PEAK_GLOBAL_OVERHEAD_RATIO", PEAK_ENV_NUMBER_REAL, "ratio", 0.1,
    0.0, FLT_MAX, true,
};
static const PeakEnvNumberSchema peak_detach_factor_schema = {
    "PEAK_GLOBAL_DETACH_FACTOR", PEAK_ENV_NUMBER_REAL, "multiplier", 1.2,
    1.0, FLT_MAX, false,
};
static const PeakEnvNumberSchema peak_reattach_factor_schema = {
    "PEAK_GLOBAL_REATTACH_FACTOR", PEAK_ENV_NUMBER_REAL, "multiplier", 0.85,
    0.0, 1.0, false,
};
static const PeakEnvNumberSchema peak_pause_timeout_schema = {
    "PEAK_PAUSE_TIMEOUT", PEAK_ENV_NUMBER_REAL, "seconds", 0.01,
    0.0, (double)ULLONG_MAX / PEAK_SECONDS_PER_NANOSECOND, true,
};
static const PeakEnvNumberSchema peak_sig_cont_timeout_schema = {
    "PEAK_SIG_CONT_TIMEOUT", PEAK_ENV_NUMBER_REAL, "seconds", 0.01,
    0.0, (double)ULLONG_MAX / PEAK_SECONDS_PER_NANOSECOND, true,
};
static const PeakEnvNumberSchema peak_heartbeat_min_schema = {
    "PEAK_HB_MIN_US", PEAK_ENV_NUMBER_UNSIGNED, "microseconds", 10000.0,
    1.0, UINT_MAX, false,
};
static const PeakEnvNumberSchema peak_heartbeat_max_schema = {
    "PEAK_HB_MAX_US", PEAK_ENV_NUMBER_UNSIGNED, "microseconds", 500000.0,
    1.0, UINT_MAX, false,
};
static const PeakEnvNumberSchema peak_heartbeat_error_gain_schema = {
    "PEAK_HB_K_ERR", PEAK_ENV_NUMBER_REAL, "gain", 3.0,
    0.0, DBL_MAX, true,
};
static const PeakEnvNumberSchema peak_heartbeat_rate_gain_schema = {
    "PEAK_HB_K_RATE", PEAK_ENV_NUMBER_REAL, "gain", 0.8,
    0.0, DBL_MAX, true,
};
static const PeakEnvNumberSchema peak_heartbeat_ema_alpha_schema = {
    "PEAK_HB_EMA_A", PEAK_ENV_NUMBER_REAL, "ratio", 0.3,
    0.0, 1.0, false,
};

static void
peak_env_warn_fallback(const PeakEnvNumberSchema* schema,
                       const char* value,
                       double fallback)
{
    if (!peak_env_warning_first_for(schema->name)) {
        return;
    }
    peak_log_warn("[peak] invalid %s=%s; using %.17g %s\n",
                  schema->name,
                  value,
                  fallback,
                  schema->unit);
}

static void
peak_env_warn_unsigned_fallback(const PeakEnvUnsignedSchema* schema,
                                const char* value)
{
    if (!peak_env_warning_first_for(schema->name)) {
        return;
    }
    peak_log_warn("[peak] invalid %s=%s; using %llu %s\n",
                  schema->name, value, schema->fallback, schema->unit);
}

static double
peak_env_parse_number(const PeakEnvNumberSchema* schema,
                      bool* valid_out,
                      bool warn)
{
    const char* value = getenv(schema->name);
    char* end = NULL;
    double parsed;

    if (value == NULL) {
        if (valid_out != NULL) {
            *valid_out = true;
        }
        return schema->fallback;
    }
    if (value[0] == '\0') {
        if (warn) {
            peak_env_warn_fallback(schema, value, schema->fallback);
        }
        if (valid_out != NULL) {
            *valid_out = false;
        }
        return schema->fallback;
    }

    errno = 0;
    if (schema->type == PEAK_ENV_NUMBER_UNSIGNED) {
        unsigned long long integer;
        const unsigned char* cursor = (const unsigned char*)value;

        while (isspace(*cursor)) {
            cursor++;
        }
        if (*cursor == '-') {
            if (warn) {
                peak_env_warn_fallback(schema, value, schema->fallback);
            }
            if (valid_out != NULL) {
                *valid_out = false;
            }
            return schema->fallback;
        }
        integer = strtoull(value, &end, 10);
        parsed = (double)integer;
        if (errno == ERANGE || end == value || *end != '\0' ||
            integer > (unsigned long long)schema->maximum) {
            if (warn) {
                peak_env_warn_fallback(schema, value, schema->fallback);
            }
            if (valid_out != NULL) {
                *valid_out = false;
            }
            return schema->fallback;
        }
    } else {
        parsed = strtod(value, &end);
        if (errno == ERANGE || end == value || *end != '\0' ||
            !isfinite(parsed)) {
            if (warn) {
                peak_env_warn_fallback(schema, value, schema->fallback);
            }
            if (valid_out != NULL) {
                *valid_out = false;
            }
            return schema->fallback;
        }
    }

    if (parsed < schema->minimum || parsed > schema->maximum ||
        (!schema->zero_allowed && parsed == 0.0)) {
        if (warn) {
            peak_env_warn_fallback(schema, value, schema->fallback);
        }
        if (valid_out != NULL) {
            *valid_out = false;
        }
        return schema->fallback;
    }
    if (valid_out != NULL) {
        *valid_out = true;
    }
    return parsed;
}

unsigned long long
peak_parse_env_unsigned(const PeakEnvUnsignedSchema* schema)
{
    const char* value = getenv(schema->name);
    const unsigned char* cursor;
    char* end = NULL;
    unsigned long long parsed;

    if (value == NULL) {
        return schema->fallback;
    }
    cursor = (const unsigned char*)value;
    while (isspace(*cursor)) {
        cursor++;
    }
    if (value[0] == '\0' || *cursor == '-') {
        peak_env_warn_unsigned_fallback(schema, value);
        return schema->fallback;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' ||
        parsed < schema->minimum || parsed > schema->maximum ||
        (!schema->zero_allowed && parsed == 0)) {
        peak_env_warn_unsigned_fallback(schema, value);
        return schema->fallback;
    }
    return parsed;
}

double
peak_parse_env_real(const PeakEnvRealSchema* schema)
{
    PeakEnvNumberSchema number_schema = {
        schema->name, PEAK_ENV_NUMBER_REAL, schema->unit, schema->fallback,
        schema->minimum, schema->maximum, schema->zero_allowed,
    };

    return peak_env_parse_number(&number_schema, NULL, true);
}

static unsigned int
peak_env_seconds_to_microseconds(const PeakEnvNumberSchema* schema)
{
    bool valid;
    double seconds = peak_env_parse_number(schema, &valid, true);
    double microseconds = seconds * PEAK_SECONDS_PER_MICROSECOND;

    if (valid && seconds != 0.0 && microseconds < 1.0) {
        const char* value = getenv(schema->name);
        peak_env_warn_fallback(schema, value, schema->fallback);
        microseconds = schema->fallback * PEAK_SECONDS_PER_MICROSECOND;
    }
    return (unsigned int)microseconds;
}

static unsigned long long
peak_env_seconds_to_nanoseconds(const PeakEnvNumberSchema* schema)
{
    bool valid;
    double seconds = peak_env_parse_number(schema, &valid, true);
    long double nanoseconds =
        (long double)seconds * PEAK_SECONDS_PER_NANOSECOND;

    if (valid && seconds != 0.0 &&
        (nanoseconds < 1.0L || nanoseconds > (long double)ULLONG_MAX)) {
        const char* value = getenv(schema->name);
        peak_env_warn_fallback(schema, value, schema->fallback);
        nanoseconds = (long double)schema->fallback *
                      PEAK_SECONDS_PER_NANOSECOND;
    }
    return (unsigned long long)nanoseconds;
}

PeakRuntimeNumericConfig
peak_parse_runtime_numeric_config(void)
{
    bool heartbeat_max_valid;
    PeakRuntimeNumericConfig config = {
        .detach_cost =
            (float)peak_env_parse_number(&peak_detach_cost_schema, NULL, true),
        .heartbeat_interval_us =
            peak_env_seconds_to_microseconds(&peak_heartbeat_interval_schema),
        .hibernation_cycle =
            (unsigned int)peak_env_parse_number(&peak_hibernation_cycle_schema,
                                                NULL, true),
        .target_overhead_ratio =
            (float)peak_env_parse_number(&peak_target_ratio_schema, NULL, true),
        .global_overhead_ratio =
            (float)peak_env_parse_number(&peak_global_ratio_schema, NULL, true),
        .global_detach_factor =
            (float)peak_env_parse_number(&peak_detach_factor_schema, NULL, true),
        .global_reattach_factor =
            (float)peak_env_parse_number(&peak_reattach_factor_schema, NULL, true),
        .pause_timeout_ns =
            peak_env_seconds_to_nanoseconds(&peak_pause_timeout_schema),
        .sig_cont_timeout_ns =
            peak_env_seconds_to_nanoseconds(&peak_sig_cont_timeout_schema),
        .heartbeat_min_us =
            (unsigned int)peak_env_parse_number(&peak_heartbeat_min_schema,
                                                NULL, true),
        .heartbeat_max_us =
            (unsigned int)peak_env_parse_number(&peak_heartbeat_max_schema,
                                                &heartbeat_max_valid, false),
        .heartbeat_error_gain =
            peak_env_parse_number(&peak_heartbeat_error_gain_schema, NULL, true),
        .heartbeat_rate_gain =
            peak_env_parse_number(&peak_heartbeat_rate_gain_schema, NULL, true),
        .heartbeat_ema_alpha =
            peak_env_parse_number(&peak_heartbeat_ema_alpha_schema, NULL, true),
    };

    if (config.heartbeat_max_us < config.heartbeat_min_us) {
        const char* value = getenv(peak_heartbeat_max_schema.name);

        if (value != NULL) {
            peak_env_warn_fallback(&peak_heartbeat_max_schema,
                                   value,
                                   (double)config.heartbeat_min_us);
        } else {
            if (peak_env_warning_first_for(peak_heartbeat_min_schema.name)) {
                peak_log_warn("[peak] %s=%u requires %s >= %u; using %u microseconds\n",
                              peak_heartbeat_min_schema.name,
                              config.heartbeat_min_us,
                              peak_heartbeat_max_schema.name,
                              config.heartbeat_min_us,
                              config.heartbeat_min_us);
            }
        }
        config.heartbeat_max_us = config.heartbeat_min_us;
    } else if (!heartbeat_max_valid) {
        peak_env_warn_fallback(&peak_heartbeat_max_schema,
                               getenv(peak_heartbeat_max_schema.name),
                               peak_heartbeat_max_schema.fallback);
    }
    if (config.global_reattach_factor >= config.global_detach_factor) {
        const char* value = getenv(peak_reattach_factor_schema.name);

        peak_env_warn_fallback(&peak_reattach_factor_schema,
                               value,
                               peak_reattach_factor_schema.fallback);
        config.global_reattach_factor =
            (float)peak_reattach_factor_schema.fallback;
    }
    return config;
}

bool
parse_env_to_bool(const char* env_var)
{
    const char* varvalue = getenv(env_var);

    return varvalue != NULL &&
           (strcasecmp(varvalue, "true") == 0 || strcmp(varvalue, "1") == 0);
}
