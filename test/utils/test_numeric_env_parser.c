#define _POSIX_C_SOURCE 200809L

#include "utils/env_parser.h"

#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char* const numeric_names[] = {
    "PEAK_COST",
    "PEAK_HEARTBEAT_INTERVAL",
    "PEAK_HIBERNATION_CYCLE",
    "PEAK_OVERHEAD_RATIO",
    "PEAK_GLOBAL_OVERHEAD_RATIO",
    "PEAK_GLOBAL_DETACH_FACTOR",
    "PEAK_GLOBAL_REATTACH_FACTOR",
    "PEAK_PAUSE_TIMEOUT",
    "PEAK_SIG_CONT_TIMEOUT",
    "PEAK_HB_MIN_US",
    "PEAK_HB_MAX_US",
    "PEAK_HB_K_ERR",
    "PEAK_HB_K_RATE",
    "PEAK_HB_EMA_A",
};

static void
clear_numeric_environment(void)
{
    for (size_t index = 0; index < sizeof(numeric_names) / sizeof(numeric_names[0]);
         index++) {
        unsetenv(numeric_names[index]);
    }
}

static int
check_production_unsigned_schemas(void)
{
    static const struct {
        const char* name;
        unsigned long long minimum;
        unsigned long long maximum;
        const char* valid;
        const char* invalid;
    } cases[] = {
        { "PEAK_MAX_NUM_THREADS", 0, ULONG_MAX, "4097", "18446744073709551616" },
        { "PEAK_MEMLOG_CHUNK_EVENTS", 1, SIZE_MAX, "1024", "0" },
        { "PEAK_CUDA_EVENT_POOL_CAPACITY", 1, 65536, "65536", "65537" },
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        PeakEnvUnsignedSchema schema = {
            cases[index].name, "events", 0, cases[index].minimum,
            cases[index].maximum, cases[index].minimum == 0, NULL, false,
        };
        unsigned long long parsed;

        setenv(schema.name, cases[index].valid, 1);
        if (!peak_parse_env_unsigned_checked(&schema, &parsed)) {
            fprintf(stderr, "valid production schema rejected: %s=%s\n",
                    schema.name, cases[index].valid);
            return 0;
        }
        setenv(schema.name, cases[index].invalid, 1);
        if (peak_parse_env_unsigned_checked(&schema, &parsed)) {
            fprintf(stderr, "invalid production schema accepted: %s=%s\n",
                    schema.name, cases[index].invalid);
            return 0;
        }
        unsetenv(schema.name);
    }
    return 1;
}

static int
check_unsigned_parser(void)
{
    PeakEnvWarningState warning_emitted = {0};
    PeakEnvUnsignedSchema schema = {
        "PEAK_TEST_UINT", "values", 7, 0, UINT_MAX, true,
        &warning_emitted, false,
    };
    static const struct {
        const char* value;
        unsigned long long expected;
    } cases[] = {
        { "", 7 }, { "0", 0 }, { "42", 42 }, { "42junk", 7 },
        { "-1", 7 }, { " -1", 7 }, { "-0", 7 }, { "+42", 42 },
        { " 42", 42 },
    };

    unsetenv(schema.name);
    if (peak_parse_env_unsigned(&schema) != 7) {
        return 0;
    }
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        setenv(schema.name, cases[index].value, 1);
        if (peak_parse_env_unsigned(&schema) != cases[index].expected) {
            fprintf(stderr, "unsigned parser mismatch: %s\n", cases[index].value);
            return 0;
        }
    }
    unsetenv(schema.name);
    return 1;
}

int
main(int argc, char** argv)
{
    if (argc == 2 && strcmp(argv[1], "warning") == 0) {
        clear_numeric_environment();
        setenv("PEAK_HB_MIN_US", "600000", 1);
        setenv("PEAK_HB_MAX_US", "junk", 1);
        if (peak_parse_runtime_numeric_config().heartbeat_max_us != 600000U) {
            return 1;
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "unset-warning") == 0) {
        clear_numeric_environment();
        setenv("PEAK_HB_MIN_US", "600000", 1);
        if (peak_parse_runtime_numeric_config().heartbeat_max_us != 600000U ||
            peak_parse_runtime_numeric_config().heartbeat_max_us != 600000U) {
            return 1;
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "repeated-warning") == 0) {
        clear_numeric_environment();
        setenv("PEAK_COST", "nan", 1);
        (void)peak_parse_runtime_numeric_config();
        (void)peak_parse_runtime_numeric_config();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "cuda-warning") == 0) {
        PeakEnvWarningState warning_emitted = {0};
        PeakEnvUnsignedSchema schema = {
            "PEAK_CUDA_EVENT_POOL_CAPACITY", "events", 256, 1, 65536,
            false, &warning_emitted, false,
        };

        setenv(schema.name, "", 1);
        if (peak_parse_env_unsigned(&schema) != 256 ||
            peak_parse_env_unsigned(&schema) != 256) {
            return 1;
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "cuda-unset") == 0) {
        PeakEnvWarningState warning_emitted = {0};
        PeakEnvUnsignedSchema schema = {
            "PEAK_CUDA_EVENT_POOL_CAPACITY", "events", 256, 1, 65536,
            false, &warning_emitted, false,
        };

        unsetenv(schema.name);
        return peak_parse_env_unsigned(&schema) == 256 ? 0 : 1;
    }

    static const struct {
        const char* name;
        const char* value;
    } invalid[] = {
        { "PEAK_COST", "" },
        { "PEAK_COST", "1seconds" },
        { "PEAK_COST", "nan" },
        { "PEAK_COST", "inf" },
        { "PEAK_COST", "-1" },
        { "PEAK_COST", "1e999" },
        { "PEAK_COST", "1e-999" },
        { "PEAK_HEARTBEAT_INTERVAL", "0.0000001" },
        { "PEAK_HIBERNATION_CYCLE", "-1" },
        { "PEAK_HIBERNATION_CYCLE", "4294967296" },
        { "PEAK_OVERHEAD_RATIO", "-0.1" },
        { "PEAK_GLOBAL_DETACH_FACTOR", "0.5" },
        { "PEAK_GLOBAL_REATTACH_FACTOR", "0" },
        { "PEAK_PAUSE_TIMEOUT", "-0.01" },
        { "PEAK_PAUSE_TIMEOUT", "18446744073.709551616" },
        { "PEAK_PAUSE_TIMEOUT", "18446744073.709553" },
        { "PEAK_HB_MIN_US", "0" },
        { "PEAK_HB_MAX_US", "0" },
        { "PEAK_HB_K_ERR", "-1" },
        { "PEAK_HB_K_RATE", "nan" },
        { "PEAK_HB_EMA_A", "0" },
        { "PEAK_HB_EMA_A", "1.1" },
    };

    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]);
         index++) {
        PeakRuntimeNumericConfig config;

        clear_numeric_environment();
        setenv(invalid[index].name, invalid[index].value, 1);
        config = peak_parse_runtime_numeric_config();
        if (config.detach_cost != 0.0f ||
            config.heartbeat_interval_us != 100000U ||
            config.hibernation_cycle != 50U ||
            config.target_overhead_ratio != 0.1f ||
            config.global_overhead_ratio != 0.1f ||
            config.global_detach_factor != 1.2f ||
            config.global_reattach_factor != 0.85f ||
            config.pause_timeout_ns != 10000000ULL ||
            config.sig_cont_timeout_ns != 10000000ULL ||
            config.heartbeat_min_us != 10000U ||
            config.heartbeat_max_us != 500000U ||
            config.heartbeat_error_gain != 3.0 ||
            config.heartbeat_rate_gain != 0.8 ||
            config.heartbeat_ema_alpha != 0.3) {
            fprintf(stderr, "invalid numeric value accepted: %s=%s\n",
                    invalid[index].name, invalid[index].value);
            return 1;
        }
    }

    clear_numeric_environment();
    setenv("PEAK_PAUSE_TIMEOUT", "18446744000", 1);
    if (peak_parse_runtime_numeric_config().pause_timeout_ns !=
        18446744000000000000ULL) {
        fputs("safe nanosecond boundary was rejected\n", stderr);
        return 1;
    }

    clear_numeric_environment();
    setenv("PEAK_HEARTBEAT_INTERVAL", "4294.967296", 1);
    if (peak_parse_runtime_numeric_config().heartbeat_interval_us != 100000U) {
        fputs("unrepresentable microsecond boundary was accepted\n", stderr);
        return 1;
    }

    clear_numeric_environment();
    setenv("PEAK_HB_MIN_US", "600000", 1);
    if (peak_parse_runtime_numeric_config().heartbeat_max_us != 600000U) {
        fputs("unset heartbeat maximum did not follow the minimum\n", stderr);
        return 1;
    }

    clear_numeric_environment();
    setenv("PEAK_HB_MIN_US", "20000", 1);
    setenv("PEAK_HB_MAX_US", "10000", 1);
    if (peak_parse_runtime_numeric_config().heartbeat_max_us != 20000U) {
        fputs("heartbeat bound conflict did not use the minimum\n", stderr);
        return 1;
    }

    clear_numeric_environment();
    setenv("PEAK_GLOBAL_DETACH_FACTOR", "1", 1);
    setenv("PEAK_GLOBAL_REATTACH_FACTOR", "1", 1);
    if (peak_parse_runtime_numeric_config().global_reattach_factor != 0.85f) {
        fputs("hysteresis conflict did not use the reattach fallback\n", stderr);
        return 1;
    }

    clear_numeric_environment();
    setenv("PEAK_HEARTBEAT_INTERVAL", "0", 1);
    setenv("PEAK_HIBERNATION_CYCLE", "0", 1);
    setenv("PEAK_OVERHEAD_RATIO", "0", 1);
    setenv("PEAK_GLOBAL_OVERHEAD_RATIO", "0", 1);
    setenv("PEAK_PAUSE_TIMEOUT", "0", 1);
    if (peak_parse_runtime_numeric_config().heartbeat_interval_us != 0U ||
        peak_parse_runtime_numeric_config().hibernation_cycle != 0U) {
        fputs("documented zero disable value rejected\n", stderr);
        return 1;
    }

    clear_numeric_environment();
    if (!check_production_unsigned_schemas() || !check_unsigned_parser()) {
        return 1;
    }
    puts("numeric_env_parser_test_ok");
    return 0;
}
