#define _GNU_SOURCE
#include "internal/jit_provider.h"
#include "internal/general_listener_internal.h"
#include "utils/env_parser.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define PEAK_JIT_ENABLE_ENV      "PEAK_JIT_ENABLE"
#define PEAK_JIT_PROVIDER_ENV    "PEAK_JIT_PROVIDER"
#define PEAK_JIT_MAP_PATH_ENV    "PEAK_JIT_MAP_PATH"
#define PEAK_JIT_TRACE_PATH_ENV  "PEAK_JIT_TRACE_PATH"
#define PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS_ENV \
    "PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS"
#define PEAK_JIT_DRAIN_RECORD_BUDGET_ENV "PEAK_JIT_DRAIN_RECORD_BUDGET"
#define PEAK_JIT_ATTACH_RETRY_TIMEOUT_MS_ENV \
    "PEAK_JIT_ATTACH_RETRY_TIMEOUT_MS"
#define PEAK_JIT_PENDING_CAPACITY_ENV "PEAK_JIT_PENDING_CAPACITY"
#define PEAK_JIT_DEFAULT_NOT_EXEC_RETRY_TIMEOUT_MS 1000UL
#define PEAK_JIT_DEFAULT_ATTACH_RETRY_TIMEOUT_MS 1000UL
#define PEAK_JIT_DEFAULT_DRAIN_RECORD_BUDGET 1024UL
#define PEAK_JIT_DEFAULT_PENDING_CAPACITY 4096UL
#define PEAK_JIT_MAX_PENDING_CAPACITY 65536UL
#ifdef PEAK_ENABLE_TEST_HOOKS
#define PEAK_JIT_TEST_ATTACH_SEQUENCE_ENV "PEAK_JIT_TEST_ATTACH_SEQUENCE"
#define PEAK_JIT_TEST_FAIL_PENDING_ALLOCATION_ENV \
    "PEAK_JIT_TEST_FAIL_PENDING_ALLOCATION"
#define PEAK_JIT_TEST_PRE_FINAL_STAT_BARRIER_ENV \
    "PEAK_JIT_TEST_PRE_FINAL_STAT_BARRIER"
#define PEAK_JIT_TEST_PRE_SOURCE_OBSERVE_BARRIER_ENV \
    "PEAK_JIT_TEST_PRE_SOURCE_OBSERVE_BARRIER"
#endif

static gboolean peak_jit_provider_enabled = FALSE;
static gboolean peak_jit_perfmap_enabled = FALSE;
static gsize peak_jit_runtime_config_initialized = 0;
static gboolean configured_jit_enabled = FALSE;
static char* configured_jit_providers = NULL;
static char* configured_jit_map_path = NULL;
static char* configured_jit_trace_path = NULL;
static unsigned long configured_jit_not_exec_retry_timeout_ms =
    PEAK_JIT_DEFAULT_NOT_EXEC_RETRY_TIMEOUT_MS;
static unsigned long configured_jit_attach_retry_timeout_ms =
    PEAK_JIT_DEFAULT_ATTACH_RETRY_TIMEOUT_MS;
static unsigned long configured_jit_drain_record_budget =
    PEAK_JIT_DEFAULT_DRAIN_RECORD_BUDGET;
static unsigned long configured_jit_pending_capacity =
    PEAK_JIT_DEFAULT_PENDING_CAPACITY;
static PeakEnvWarningState peak_jit_retry_timeout_warning_emitted;
static PeakEnvWarningState peak_jit_attach_retry_timeout_warning_emitted;
static PeakEnvWarningState peak_jit_drain_budget_warning_emitted;
static PeakEnvWarningState peak_jit_pending_capacity_warning_emitted;
static char* peak_jit_perfmap_path = NULL;
static off_t peak_jit_perfmap_offset = 0;
static gboolean peak_jit_perfmap_identity_known = FALSE;
static dev_t peak_jit_perfmap_dev = 0;
static ino_t peak_jit_perfmap_ino = 0;
static off_t peak_jit_perfmap_last_size = 0;
#ifdef PEAK_ENABLE_TEST_HOOKS
static unsigned int peak_jit_test_attach_sequence_index = 0;
static char* configured_jit_test_attach_sequence = NULL;
static unsigned long peak_jit_test_fail_pending_allocation_remaining = 0;
static gboolean peak_jit_test_final_stat_barrier_used = FALSE;
static gboolean peak_jit_test_final_stat_signal_pending = FALSE;
static gboolean peak_jit_test_source_observe_barrier_used = FALSE;
#endif

typedef enum {
    PEAK_JIT_PENDING_NOT_EXECUTABLE = 0,
    PEAK_JIT_PENDING_ATTACH_RETRY
} PeakJitPendingKind;

typedef struct {
    uint64_t provider_generation;
    uintptr_t address;
    size_t size;
    char* name;
    double created_at;
    double last_attempt_at;
    uint64_t attempt_count;
    PeakJitPendingKind pending_kind;
} PeakJitPendingRecord;

typedef enum {
    PEAK_JIT_PENDING_ADD_OK = 0,
    PEAK_JIT_PENDING_ADD_DUPLICATE,
    PEAK_JIT_PENDING_ADD_QUEUE_FULL,
    PEAK_JIT_PENDING_ADD_ALLOCATION_FAILED
} PeakJitPendingAddResult;

static PeakJitPendingRecord** peak_jit_pending_records = NULL;
static size_t peak_jit_pending_record_count = 0;
static size_t peak_jit_pending_retry_cursor = 0;
static gboolean peak_jit_single_budget_retry_turn = FALSE;
static uint64_t peak_jit_provider_generation = 0;
static _Atomic uint64_t peak_jit_pending_queue_full = 0;
static _Atomic uint64_t peak_jit_non_executable_timeout = 0;
static _Atomic uint64_t peak_jit_attach_retry_timeout = 0;
static _Atomic uint64_t peak_jit_allocation_failure = 0;
static _Atomic uint64_t peak_jit_pending_high_water = 0;

static gboolean
peak_jit_env_truthy(const char* value)
{
    return value != NULL &&
           (g_ascii_strcasecmp(value, "1") == 0 ||
            g_ascii_strcasecmp(value, "true") == 0 ||
            g_ascii_strcasecmp(value, "yes") == 0 ||
            g_ascii_strcasecmp(value, "on") == 0);
}

static gboolean
peak_jit_provider_list_contains(const char* providers, const char* name)
{
    if (providers == NULL || providers[0] == '\0') {
        return FALSE;
    }

    char** parts = g_strsplit(providers, ",", -1);
    gboolean matched = FALSE;

    for (size_t i = 0; parts[i] != NULL; i++) {
        char* token = g_strstrip(parts[i]);
        if (g_ascii_strcasecmp(token, name) == 0 ||
            (g_ascii_strcasecmp(name, "perfmap") == 0 &&
             g_ascii_strcasecmp(token, "perf-map") == 0)) {
            matched = TRUE;
            break;
        }
    }

    g_strfreev(parts);
    return matched;
}

static char*
peak_jit_default_perfmap_path(void)
{
    return g_strdup_printf("/tmp/perf-%ld.map", (long)getpid());
}

static unsigned long
peak_jit_parse_ulong_env(const char* name,
                         const char* unit,
                         unsigned long fallback,
                         unsigned long maximum,
                         bool zero_allowed,
                         PeakEnvWarningState* warning_emitted)
{
    PeakEnvUnsignedSchema schema = {
        name, unit, fallback, zero_allowed ? 0UL : 1UL, maximum,
        zero_allowed, warning_emitted, false,
    };

    return (unsigned long)peak_parse_env_unsigned(&schema);
}

static const char*
peak_jit_trace_path(void)
{
    return configured_jit_trace_path;
}

static void
peak_jit_trace_csv_field(FILE* fp, const char* value)
{
    gboolean quote = FALSE;

    if (value == NULL) {
        value = "<unknown>";
    }

    for (const char* p = value; *p != '\0'; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            quote = TRUE;
            break;
        }
    }

    if (!quote) {
        fputs(value, fp);
        return;
    }

    fputc('"', fp);
    for (const char* p = value; *p != '\0'; p++) {
        if (*p == '"') {
            fputc('"', fp);
        }
        fputc(*p, fp);
    }
    fputc('"', fp);
}

static void
peak_jit_trace(const char* event,
               const char* provider,
               const char* name,
               uintptr_t address,
               size_t size,
               const char* result)
{
    const char* path = peak_jit_trace_path();

    if (path == NULL) {
        return;
    }

    FILE* fp = fopen(path, "a");
    if (fp == NULL) {
        return;
    }

    fprintf(fp, "%.9f,", peak_second());
    peak_jit_trace_csv_field(fp, event);
    fputc(',', fp);
    peak_jit_trace_csv_field(fp, provider);
    fputc(',', fp);
    peak_jit_trace_csv_field(fp, name);
    fprintf(fp, ",0x%" PRIxPTR ",%zu,", address, size);
    peak_jit_trace_csv_field(fp, result);
    fprintf(fp, ",%" PRIu64, peak_jit_provider_generation);
    fputc('\n', fp);
    fclose(fp);
}

static char*
peak_jit_trim_record_name(char* name)
{
    char* start;
    char* end;

    if (name == NULL) {
        return NULL;
    }

    start = name;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';

    return start;
}

static gboolean
peak_jit_parse_perfmap_line(char* line,
                            uintptr_t* address_out,
                            size_t* size_out,
                            char** name_out)
{
    char* cursor = line;
    char* end = NULL;
    unsigned long long parsed_address;
    unsigned long long parsed_size;
    char* name;

    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor == '\0' || *cursor == '#') {
        return FALSE;
    }

    errno = 0;
    parsed_address = strtoull(cursor, &end, 16);
    if (errno != 0 || end == cursor) {
        return FALSE;
    }

    cursor = end;
    if (*cursor == '\0' || !isspace((unsigned char)*cursor)) {
        return FALSE;
    }
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }

    errno = 0;
    parsed_size = strtoull(cursor, &end, 16);
    if (errno != 0 || end == cursor || parsed_size == 0 ||
        parsed_size > (unsigned long long)SIZE_MAX ||
        parsed_address > (unsigned long long)UINTPTR_MAX) {
        return FALSE;
    }

    if (*end == '\0' || !isspace((unsigned char)*end)) {
        return FALSE;
    }
    name = peak_jit_trim_record_name(end);
    if (name == NULL || name[0] == '\0') {
        return FALSE;
    }

    uintptr_t address = (uintptr_t)parsed_address;
    size_t size = (size_t)parsed_size;

    if (address + (uintptr_t)size < address) {
        return FALSE;
    }

    if (address_out != NULL) {
        *address_out = address;
    }
    if (size_out != NULL) {
        *size_out = size;
    }
    if (name_out != NULL) {
        *name_out = name;
    }

    return TRUE;
}

static gboolean
peak_jit_line_is_complete(const char* line)
{
    size_t length;

    if (line == NULL) {
        return FALSE;
    }

    length = strlen(line);
    return length > 0 && line[length - 1] == '\n';
}

static gboolean
peak_jit_range_is_executable(uintptr_t address, size_t size)
{
    FILE* maps = fopen("/proc/self/maps", "r");
    char line[512];
    uintptr_t end;

    if (maps == NULL || size == 0) {
        if (maps != NULL) {
            fclose(maps);
        }
        return FALSE;
    }

    end = address + (uintptr_t)size;
    if (end < address) {
        fclose(maps);
        return FALSE;
    }

    while (fgets(line, sizeof(line), maps) != NULL) {
        unsigned long long map_start;
        unsigned long long map_end;
        char perms[5] = { 0 };

        if (sscanf(line, "%llx-%llx %4s", &map_start, &map_end, perms) != 3) {
            continue;
        }

        if (address >= (uintptr_t)map_start &&
            end <= (uintptr_t)map_end &&
            strchr(perms, 'x') != NULL) {
            fclose(maps);
            return TRUE;
        }
    }

    fclose(maps);
    return FALSE;
}

static unsigned long
peak_jit_not_exec_retry_timeout_ms(void)
{
    return configured_jit_not_exec_retry_timeout_ms;
}

static unsigned long
peak_jit_drain_record_budget(void)
{
    return configured_jit_drain_record_budget;
}

static unsigned long
peak_jit_attach_retry_timeout_ms(void)
{
    return configured_jit_attach_retry_timeout_ms;
}

static void
peak_jit_provider_reset_diagnostics(void)
{
    atomic_store_explicit(&peak_jit_pending_queue_full,
                          0,
                          memory_order_relaxed);
    atomic_store_explicit(&peak_jit_non_executable_timeout,
                          0,
                          memory_order_relaxed);
    atomic_store_explicit(&peak_jit_attach_retry_timeout,
                          0,
                          memory_order_relaxed);
    atomic_store_explicit(&peak_jit_allocation_failure,
                          0,
                          memory_order_relaxed);
    atomic_store_explicit(&peak_jit_pending_high_water,
                          0,
                          memory_order_relaxed);
}

static void
peak_jit_pending_record_free(gpointer data)
{
    PeakJitPendingRecord* record = data;

    if (record == NULL) {
        return;
    }

    g_free(record->name);
    g_free(record);
}

static gboolean
peak_jit_pending_records_ensure(void)
{
    if (peak_jit_pending_records == NULL) {
#ifdef PEAK_ENABLE_TEST_HOOKS
        if (peak_jit_test_fail_pending_allocation_remaining > 0) {
            peak_jit_test_fail_pending_allocation_remaining--;
            atomic_fetch_add_explicit(&peak_jit_allocation_failure,
                                      1,
                                      memory_order_relaxed);
            return FALSE;
        }
#endif
        peak_jit_pending_records =
            g_try_new0(PeakJitPendingRecord*, configured_jit_pending_capacity);
        if (peak_jit_pending_records == NULL) {
            atomic_fetch_add_explicit(&peak_jit_allocation_failure,
                                      1,
                                      memory_order_relaxed);
            return FALSE;
        }
    }

    return TRUE;
}

static void
peak_jit_pending_records_clear(void)
{
    for (size_t i = 0; i < peak_jit_pending_record_count; i++) {
        peak_jit_pending_record_free(peak_jit_pending_records[i]);
        peak_jit_pending_records[i] = NULL;
    }
    peak_jit_pending_record_count = 0;
    peak_jit_pending_retry_cursor = 0;
    peak_jit_single_budget_retry_turn = FALSE;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
static void
peak_jit_test_wait_before_source_observation(void)
{
    const char* path;
    FILE* marker;

    if (peak_jit_test_source_observe_barrier_used ||
        peak_jit_pending_record_count == 0) {
        return;
    }
    path = g_getenv(PEAK_JIT_TEST_PRE_SOURCE_OBSERVE_BARRIER_ENV);
    if (path == NULL || path[0] == '\0') {
        return;
    }
    peak_jit_test_source_observe_barrier_used = TRUE;
    marker = fopen(path, "w");
    if (marker == NULL) {
        return;
    }
    fclose(marker);
    for (unsigned int i = 0; i < 10000 && access(path, F_OK) == 0; i++) {
        usleep(100);
    }
}

static void
peak_jit_test_wait_before_final_stat(void)
{
    const char* path;
    FILE* marker;

    if (peak_jit_test_final_stat_barrier_used) {
        return;
    }
    path = g_getenv(PEAK_JIT_TEST_PRE_FINAL_STAT_BARRIER_ENV);
    if (path == NULL || path[0] == '\0') {
        return;
    }
    peak_jit_test_final_stat_barrier_used = TRUE;
    marker = fopen(path, "w");
    if (marker == NULL) {
        return;
    }
    fclose(marker);
    for (unsigned int i = 0; i < 10000 && access(path, F_OK) == 0; i++) {
        usleep(100);
    }
    peak_jit_test_final_stat_signal_pending = TRUE;
}

static void
peak_jit_test_signal_after_final_stat(void)
{
    const char* path;
    char* done_path;
    FILE* marker;

    if (!peak_jit_test_final_stat_signal_pending) {
        return;
    }
    peak_jit_test_final_stat_signal_pending = FALSE;
    path = g_getenv(PEAK_JIT_TEST_PRE_FINAL_STAT_BARRIER_ENV);
    if (path == NULL || path[0] == '\0') {
        return;
    }
    done_path = g_strdup_printf("%s.done", path);
    marker = fopen(done_path, "w");
    if (marker != NULL) {
        fclose(marker);
    }
    g_free(done_path);
}
#endif

static void
peak_jit_provider_advance_generation(const char* reason)
{
    peak_jit_pending_records_clear();
    peak_jit_provider_generation++;
    if (peak_jit_provider_generation == 0) {
        peak_jit_provider_generation = 1;
    }
    peak_jit_trace("provider-generation",
                   "perfmap",
                   peak_jit_perfmap_path,
                   0,
                   0,
                   reason);
}

static PeakJitPendingRecord*
peak_jit_pending_record_find(uint64_t provider_generation,
                             uintptr_t address,
                             size_t size,
                             const char* name)
{
    if (peak_jit_pending_records == NULL || name == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < peak_jit_pending_record_count; i++) {
        PeakJitPendingRecord* record = peak_jit_pending_records[i];

        if (record->provider_generation == provider_generation &&
            record->address == address &&
            record->size == size &&
            strcmp(record->name, name) == 0) {
            return record;
        }
    }

    return NULL;
}

static PeakJitPendingAddResult
peak_jit_pending_record_add(PeakJitPendingKind kind,
                            uintptr_t address,
                            size_t size,
                            const char* name)
{
    PeakJitPendingRecord* record;

    if (name == NULL || name[0] == '\0') {
        return PEAK_JIT_PENDING_ADD_ALLOCATION_FAILED;
    }

    if (peak_jit_pending_record_count >= configured_jit_pending_capacity) {
        atomic_fetch_add_explicit(&peak_jit_pending_queue_full,
                                  1,
                                  memory_order_relaxed);
        return PEAK_JIT_PENDING_ADD_QUEUE_FULL;
    }
    record = peak_jit_pending_record_find(peak_jit_provider_generation,
                                          address,
                                          size,
                                          name);
    if (record != NULL) {
        return PEAK_JIT_PENDING_ADD_DUPLICATE;
    }
    if (!peak_jit_pending_records_ensure()) {
        return PEAK_JIT_PENDING_ADD_ALLOCATION_FAILED;
    }

    record = g_try_new0(PeakJitPendingRecord, 1);
    if (record == NULL) {
        atomic_fetch_add_explicit(&peak_jit_allocation_failure,
                                  1,
                                  memory_order_relaxed);
        return PEAK_JIT_PENDING_ADD_ALLOCATION_FAILED;
    }
    size_t name_size = strlen(name) + 1;
    record->name = g_try_malloc(name_size);
    if (record->name == NULL) {
        g_free(record);
        atomic_fetch_add_explicit(&peak_jit_allocation_failure,
                                  1,
                                  memory_order_relaxed);
        return PEAK_JIT_PENDING_ADD_ALLOCATION_FAILED;
    }
    memcpy(record->name, name, name_size);
    record->provider_generation = peak_jit_provider_generation;
    record->address = address;
    record->size = size;
    record->created_at = peak_second();
    record->last_attempt_at = record->created_at;
    record->attempt_count = 1;
    record->pending_kind = kind;

    peak_jit_pending_records[peak_jit_pending_record_count++] = record;
    uint64_t previous_high_water =
        atomic_load_explicit(&peak_jit_pending_high_water,
                             memory_order_relaxed);
    while (previous_high_water < peak_jit_pending_record_count &&
           !atomic_compare_exchange_weak_explicit(
               &peak_jit_pending_high_water,
               &previous_high_water,
               peak_jit_pending_record_count,
               memory_order_relaxed,
               memory_order_relaxed)) {
    }
    return PEAK_JIT_PENDING_ADD_OK;
}

static void
peak_jit_pending_record_remove_index(size_t index)
{
    if (peak_jit_pending_records != NULL &&
        index < peak_jit_pending_record_count) {
        peak_jit_pending_record_free(peak_jit_pending_records[index]);
        peak_jit_pending_record_count--;
        if (index < peak_jit_pending_record_count) {
            peak_jit_pending_records[index] =
                peak_jit_pending_records[peak_jit_pending_record_count];
        }
        peak_jit_pending_records[peak_jit_pending_record_count] = NULL;
        if (peak_jit_pending_record_count == 0) {
            peak_jit_pending_retry_cursor = 0;
        } else {
            peak_jit_pending_retry_cursor =
                index < peak_jit_pending_record_count ? index : 0;
        }
    }
}

static void
peak_jit_pending_records_expire(void)
{
    uint64_t non_executable = 0;
    uint64_t attach_retry = 0;

    for (size_t i = 0; i < peak_jit_pending_record_count; i++) {
        if (peak_jit_pending_records[i]->pending_kind ==
            PEAK_JIT_PENDING_ATTACH_RETRY) {
            attach_retry++;
        } else {
            non_executable++;
        }
    }
    if (non_executable != 0) {
        atomic_fetch_add_explicit(&peak_jit_non_executable_timeout,
                                  non_executable,
                                  memory_order_relaxed);
        peak_jit_trace("perfmap-record",
                       "perfmap",
                       "<pending>",
                       0,
                       (size_t)non_executable,
                       "not-executable-timeout");
    }
    if (attach_retry != 0) {
        atomic_fetch_add_explicit(&peak_jit_attach_retry_timeout,
                                  attach_retry,
                                  memory_order_relaxed);
        peak_jit_trace("perfmap-record",
                       "perfmap",
                       "<pending>",
                       0,
                       (size_t)attach_retry,
                       "attach-retry-timeout");
    }
    peak_jit_pending_records_clear();
}

static gboolean
peak_jit_consume_overlong_line(FILE* fp, off_t* next_offset_out)
{
    int ch;

    if (fp == NULL) {
        return FALSE;
    }

    while ((ch = fgetc(fp)) != EOF) {
        if (ch == '\n') {
            if (next_offset_out != NULL) {
                *next_offset_out = ftello(fp);
            }
            return TRUE;
        }
    }

    return FALSE;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
static gboolean
peak_jit_test_forced_attach_result(PeakDynamicAttachResult* result_out)
{
    const char* sequence = configured_jit_test_attach_sequence;
    char** parts;
    char* token;
    gboolean forced = TRUE;

    if (sequence == NULL || sequence[0] == '\0') {
        return FALSE;
    }
    if (g_ascii_strcasecmp(sequence, "always-retry") == 0) {
        peak_jit_test_attach_sequence_index++;
        *result_out = PEAK_DYNAMIC_ATTACH_RETRY;
        return TRUE;
    }

    parts = g_strsplit(sequence, ",", -1);
    token = NULL;
    for (unsigned int i = 0; parts[i] != NULL; i++) {
        if (i == peak_jit_test_attach_sequence_index) {
            token = g_strstrip(parts[i]);
            break;
        }
    }
    if (token == NULL || token[0] == '\0') {
        g_strfreev(parts);
        return FALSE;
    }

    peak_jit_test_attach_sequence_index++;
    if (g_ascii_strcasecmp(token, "not-matched") == 0 ||
        g_ascii_strcasecmp(token, "no-match") == 0) {
        *result_out = PEAK_DYNAMIC_ATTACH_NO_MATCH;
    } else if (g_ascii_strcasecmp(token, "retry") == 0) {
        *result_out = PEAK_DYNAMIC_ATTACH_RETRY;
    } else if (g_ascii_strcasecmp(token, "failed") == 0) {
        *result_out = PEAK_DYNAMIC_ATTACH_FAILED;
    } else if (g_ascii_strcasecmp(token, "real") == 0) {
        forced = FALSE;
    } else {
        forced = FALSE;
    }

    g_strfreev(parts);
    return forced;
}
#endif

static void
peak_jit_init_runtime_config_once(void)
{
    const char* providers = g_getenv(PEAK_JIT_PROVIDER_ENV);
    const char* map_path = g_getenv(PEAK_JIT_MAP_PATH_ENV);
    const char* trace_path = g_getenv(PEAK_JIT_TRACE_PATH_ENV);
    unsigned long budget;

    configured_jit_enabled =
        peak_jit_env_truthy(g_getenv(PEAK_JIT_ENABLE_ENV));
    if (providers != NULL && providers[0] != '\0') {
        configured_jit_providers = g_strdup(providers);
    }
    if (map_path != NULL && map_path[0] != '\0') {
        configured_jit_map_path = g_strdup(map_path);
    }
    if (trace_path != NULL && trace_path[0] != '\0') {
        configured_jit_trace_path = g_strdup(trace_path);
    }
    configured_jit_not_exec_retry_timeout_ms =
        peak_jit_parse_ulong_env(PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS_ENV,
                                 "milliseconds",
                                 PEAK_JIT_DEFAULT_NOT_EXEC_RETRY_TIMEOUT_MS,
                                 ULONG_MAX,
                                 true,
                                 &peak_jit_retry_timeout_warning_emitted);
    configured_jit_attach_retry_timeout_ms =
        peak_jit_parse_ulong_env(PEAK_JIT_ATTACH_RETRY_TIMEOUT_MS_ENV,
                                 "milliseconds",
                                 PEAK_JIT_DEFAULT_ATTACH_RETRY_TIMEOUT_MS,
                                 ULONG_MAX,
                                 true,
                                 &peak_jit_attach_retry_timeout_warning_emitted);
    budget =
        peak_jit_parse_ulong_env(PEAK_JIT_DRAIN_RECORD_BUDGET_ENV,
                                 "records",
                                 PEAK_JIT_DEFAULT_DRAIN_RECORD_BUDGET,
                                 ULONG_MAX,
                                 false,
                                 &peak_jit_drain_budget_warning_emitted);
    configured_jit_drain_record_budget = budget;
    configured_jit_pending_capacity =
        peak_jit_parse_ulong_env(PEAK_JIT_PENDING_CAPACITY_ENV,
                                 "records",
                                 PEAK_JIT_DEFAULT_PENDING_CAPACITY,
                                 PEAK_JIT_MAX_PENDING_CAPACITY,
                                 false,
                                 &peak_jit_pending_capacity_warning_emitted);
#ifdef PEAK_ENABLE_TEST_HOOKS
    const char* attach_sequence =
        g_getenv(PEAK_JIT_TEST_ATTACH_SEQUENCE_ENV);
    if (attach_sequence != NULL && attach_sequence[0] != '\0') {
        configured_jit_test_attach_sequence = g_strdup(attach_sequence);
    }
    const char* fail_pending_allocation =
        g_getenv(PEAK_JIT_TEST_FAIL_PENDING_ALLOCATION_ENV);
    if (fail_pending_allocation != NULL &&
        fail_pending_allocation[0] != '\0') {
        char* end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(fail_pending_allocation, &end, 10);
        if (errno == 0 && end != fail_pending_allocation && *end == '\0') {
            peak_jit_test_fail_pending_allocation_remaining = parsed;
        }
    }
#endif
}

static void
peak_jit_init_runtime_config(void)
{
    if (g_once_init_enter(&peak_jit_runtime_config_initialized)) {
        peak_jit_init_runtime_config_once();
        g_once_init_leave(&peak_jit_runtime_config_initialized, 1);
    }
}

static PeakDynamicAttachResult
peak_jit_attach_perfmap_symbol(const char* name, uintptr_t address, size_t size)
{
#ifdef PEAK_ENABLE_TEST_HOOKS
    PeakDynamicAttachResult result;

    if (peak_jit_test_forced_attach_result(&result)) {
        return result;
    }
#endif

    return peak_general_listener_dynamic_attach_symbol(name,
                                                       (gpointer)address,
                                                       size,
                                                       "perfmap",
                                                       peak_jit_provider_generation);
}

static const char*
peak_jit_attach_result_string(PeakDynamicAttachResult result)
{
    switch (result) {
        case PEAK_DYNAMIC_ATTACH_ATTACHED:
            return "attached";
        case PEAK_DYNAMIC_ATTACH_GENERATION_REFRESHED:
            return "generation-refreshed";
        case PEAK_DYNAMIC_ATTACH_NO_MATCH:
            return "not-matched";
        case PEAK_DYNAMIC_ATTACH_RETRY:
            return "attach-retry";
        case PEAK_DYNAMIC_ATTACH_FAILED:
            return "attach-failed";
        default:
            return "attach-unknown";
    }
}

static const char*
peak_jit_pending_add_result_string(PeakJitPendingAddResult result,
                                   const char* retry_result)
{
    switch (result) {
        case PEAK_JIT_PENDING_ADD_OK:
            return retry_result;
        case PEAK_JIT_PENDING_ADD_DUPLICATE:
            return "already-pending";
        case PEAK_JIT_PENDING_ADD_QUEUE_FULL:
            return "pending-queue-full";
        case PEAK_JIT_PENDING_ADD_ALLOCATION_FAILED:
            return "pending-allocation-failure";
        default:
            return "pending-add-unknown";
    }
}

static gboolean
peak_jit_pending_record_timed_out(const PeakJitPendingRecord* record,
                                  gboolean force_not_exec_timeout)
{
    unsigned long timeout_ms;

    if (force_not_exec_timeout) {
        return TRUE;
    }

    timeout_ms = record->pending_kind == PEAK_JIT_PENDING_NOT_EXECUTABLE
                     ? peak_jit_not_exec_retry_timeout_ms()
                     : peak_jit_attach_retry_timeout_ms();
    return (peak_second() - record->created_at) * 1000.0 >=
           (double)timeout_ms;
}

static gboolean
peak_jit_provider_retry_pending_records(gboolean force_not_exec_timeout,
                                        unsigned long* budget)
{
    gboolean pending = FALSE;
    size_t remaining;

    if (peak_jit_pending_records == NULL ||
        peak_jit_pending_record_count == 0) {
        return FALSE;
    }

    remaining = peak_jit_pending_record_count;
    while (peak_jit_pending_record_count != 0 && remaining != 0) {
        if (peak_jit_pending_retry_cursor >= peak_jit_pending_record_count) {
            peak_jit_pending_retry_cursor = 0;
        }
        size_t i = peak_jit_pending_retry_cursor;
        PeakJitPendingRecord* record = peak_jit_pending_records[i];
        PeakDynamicAttachResult attach_result;

        if (budget != NULL) {
            if (*budget == 0) {
                pending = TRUE;
                break;
            }
            (*budget)--;
        }
        remaining--;
        record->last_attempt_at = peak_second();
        record->attempt_count++;

        if (!peak_jit_range_is_executable(record->address, record->size)) {
            gboolean timed_out = peak_jit_pending_record_timed_out(
                record,
                force_not_exec_timeout);

            const char* timeout_result =
                record->pending_kind == PEAK_JIT_PENDING_ATTACH_RETRY
                    ? "attach-retry-timeout"
                    : "not-executable-timeout";
            peak_jit_trace("perfmap-record",
                           "perfmap",
                           record->name,
                           record->address,
                           record->size,
                           timed_out ? timeout_result :
                                       "not-executable-retry");
            if (timed_out) {
                _Atomic uint64_t* timeout_counter =
                    record->pending_kind == PEAK_JIT_PENDING_ATTACH_RETRY
                        ? &peak_jit_attach_retry_timeout
                        : &peak_jit_non_executable_timeout;
                atomic_fetch_add_explicit(timeout_counter,
                                          1,
                                          memory_order_relaxed);
                peak_jit_pending_record_remove_index(i);
                continue;
            }

            pending = TRUE;
            peak_jit_pending_retry_cursor =
                (i + 1) % peak_jit_pending_record_count;
            continue;
        }

        attach_result = peak_jit_attach_perfmap_symbol(record->name,
                                                       record->address,
                                                       record->size);
        if (attach_result == PEAK_DYNAMIC_ATTACH_RETRY) {
            if (record->pending_kind == PEAK_JIT_PENDING_NOT_EXECUTABLE) {
                record->pending_kind = PEAK_JIT_PENDING_ATTACH_RETRY;
                record->created_at = peak_second();
            }
            gboolean timed_out = peak_jit_pending_record_timed_out(
                record,
                force_not_exec_timeout);
            peak_jit_trace("perfmap-record",
                           "perfmap",
                           record->name,
                           record->address,
                           record->size,
                           timed_out ? "attach-retry-timeout" :
                                       "attach-retry");
            if (timed_out) {
                atomic_fetch_add_explicit(&peak_jit_attach_retry_timeout,
                                          1,
                                          memory_order_relaxed);
                peak_jit_pending_record_remove_index(i);
                continue;
            }
            pending = TRUE;
            peak_jit_pending_retry_cursor =
                (i + 1) % peak_jit_pending_record_count;
            continue;
        }

        peak_jit_trace("perfmap-record",
                       "perfmap",
                       record->name,
                       record->address,
                       record->size,
                       peak_jit_attach_result_string(attach_result));
        peak_jit_pending_record_remove_index(i);
    }

    return pending;
}

static gboolean
peak_jit_provider_drain_perfmap(gboolean force_not_exec_timeout)
{
    FILE* fp;
    char line[4096];
    off_t committed_offset;
    gboolean pending = FALSE;
    unsigned long budget = peak_jit_drain_record_budget();
    gboolean single_budget = budget == 1;
    gboolean metadata_consumed = FALSE;
    struct stat st;

    if (peak_jit_perfmap_path == NULL) {
        pending |= peak_jit_provider_retry_pending_records(
            force_not_exec_timeout,
            &budget);
        return pending || peak_jit_pending_record_count != 0;
    }

#ifdef PEAK_ENABLE_TEST_HOOKS
    peak_jit_test_wait_before_source_observation();
#endif
    fp = fopen(peak_jit_perfmap_path, "r");
    if (fp == NULL) {
        pending |= peak_jit_provider_retry_pending_records(
            force_not_exec_timeout,
            &budget);
        return pending || peak_jit_pending_record_count != 0;
    }

    if (fstat(fileno(fp), &st) == 0) {
        gboolean source_replaced =
            peak_jit_perfmap_identity_known &&
            (st.st_dev != peak_jit_perfmap_dev ||
             st.st_ino != peak_jit_perfmap_ino);
        gboolean source_truncated =
            peak_jit_perfmap_identity_known && !source_replaced &&
            (peak_jit_perfmap_offset > st.st_size ||
             st.st_size < peak_jit_perfmap_last_size);
        if (source_replaced) {
            peak_jit_perfmap_offset = 0;
            peak_jit_provider_advance_generation("source-replaced");
        } else if (source_truncated) {
            peak_jit_perfmap_offset = 0;
            peak_jit_provider_advance_generation("source-truncated");
        }
        peak_jit_perfmap_identity_known = TRUE;
        peak_jit_perfmap_dev = st.st_dev;
        peak_jit_perfmap_ino = st.st_ino;
        peak_jit_perfmap_last_size = st.st_size;
    }

    if (budget > 1) {
        unsigned long retry_budget = budget / 2;
        unsigned long initial_retry_budget = retry_budget;

        pending |= peak_jit_provider_retry_pending_records(
            force_not_exec_timeout,
            &retry_budget);
        budget -= initial_retry_budget - retry_budget;
    } else if (single_budget && peak_jit_single_budget_retry_turn &&
               peak_jit_pending_record_count != 0) {
        unsigned long initial_budget = budget;

        pending |= peak_jit_provider_retry_pending_records(
            force_not_exec_timeout,
            &budget);
        if (budget != initial_budget) {
            peak_jit_single_budget_retry_turn = FALSE;
        }
    }

    if (fseeko(fp, 0, SEEK_END) == 0) {
        off_t end = ftello(fp);
        if (end >= 0 && peak_jit_perfmap_offset > end) {
            peak_jit_perfmap_offset = 0;
            peak_jit_provider_advance_generation("source-rewound");
        }
    }
    if (fseeko(fp, peak_jit_perfmap_offset, SEEK_SET) != 0) {
        peak_jit_perfmap_offset = 0;
        peak_jit_provider_advance_generation("source-seek-reset");
        (void)fseeko(fp, 0, SEEK_SET);
    }

    committed_offset = peak_jit_perfmap_offset;
    while (fgets(line, sizeof(line), fp) != NULL) {
        uintptr_t address = 0;
        size_t size = 0;
        char* name = NULL;
        off_t next_offset = ftello(fp);
        PeakDynamicAttachResult attach_result;

        if (budget == 0) {
            pending = TRUE;
            break;
        }
        budget--;
        metadata_consumed = TRUE;

        if (!peak_jit_line_is_complete(line)) {
            if (strlen(line) == sizeof(line) - 1 &&
                peak_jit_consume_overlong_line(fp, &next_offset)) {
                peak_jit_trace("perfmap-record",
                               "perfmap",
                               "<overlong>",
                               0,
                               0,
                               "overlong-record");
                if (next_offset >= 0) {
                    committed_offset = next_offset;
                }
                continue;
            }
            peak_jit_trace("perfmap-record",
                           "perfmap",
                           "<partial>",
                           0,
                           0,
                           "partial-record");
            pending = TRUE;
            break;
        }

        if (!peak_jit_parse_perfmap_line(line, &address, &size, &name)) {
            if (next_offset >= 0) {
                committed_offset = next_offset;
            }
            continue;
        }

        if (!peak_jit_range_is_executable(address, size)) {
            gboolean matches_target =
                peak_general_listener_dynamic_symbol_matches_any_target(name,
                                                                        "perfmap");
            if (!matches_target) {
                peak_jit_trace("perfmap-record",
                               "perfmap",
                               name,
                               address,
                               size,
                               "not-executable");
            } else if (force_not_exec_timeout) {
                atomic_fetch_add_explicit(&peak_jit_non_executable_timeout,
                                          1,
                                          memory_order_relaxed);
                peak_jit_trace("perfmap-record",
                               "perfmap",
                               name,
                               address,
                               size,
                               "not-executable-timeout");
            } else {
                PeakJitPendingAddResult add_result =
                    peak_jit_pending_record_add(
                        PEAK_JIT_PENDING_NOT_EXECUTABLE,
                        address,
                        size,
                        name);
                pending |= add_result == PEAK_JIT_PENDING_ADD_OK ||
                           add_result == PEAK_JIT_PENDING_ADD_DUPLICATE;
                peak_jit_trace(
                    "perfmap-record",
                    "perfmap",
                    name,
                    address,
                    size,
                    peak_jit_pending_add_result_string(
                        add_result,
                        "not-executable-retry"));
            }
            if (next_offset >= 0) {
                committed_offset = next_offset;
            }
            continue;
        }

        attach_result = peak_jit_attach_perfmap_symbol(name, address, size);
        if (attach_result == PEAK_DYNAMIC_ATTACH_RETRY) {
            if (force_not_exec_timeout) {
                atomic_fetch_add_explicit(&peak_jit_attach_retry_timeout,
                                          1,
                                          memory_order_relaxed);
                peak_jit_trace("perfmap-record",
                               "perfmap",
                               name,
                               address,
                               size,
                               "attach-retry-timeout");
            } else {
                PeakJitPendingAddResult add_result =
                    peak_jit_pending_record_add(
                        PEAK_JIT_PENDING_ATTACH_RETRY,
                        address,
                        size,
                        name);
                pending |= add_result == PEAK_JIT_PENDING_ADD_OK ||
                           add_result == PEAK_JIT_PENDING_ADD_DUPLICATE;
                peak_jit_trace(
                    "perfmap-record",
                    "perfmap",
                    name,
                    address,
                    size,
                    peak_jit_pending_add_result_string(add_result,
                                                       "attach-retry"));
            }
        } else {
            peak_jit_trace("perfmap-record",
                           "perfmap",
                           name,
                           address,
                           size,
                           peak_jit_attach_result_string(attach_result));
        }
        if (next_offset >= 0) {
            committed_offset = next_offset;
        }
    }

    peak_jit_perfmap_offset = committed_offset;
#ifdef PEAK_ENABLE_TEST_HOOKS
    if (peak_jit_perfmap_offset > 0) {
        peak_jit_test_wait_before_final_stat();
    }
#endif
    if (fstat(fileno(fp), &st) == 0) {
        if (peak_jit_perfmap_identity_known &&
            st.st_dev == peak_jit_perfmap_dev &&
            st.st_ino == peak_jit_perfmap_ino &&
            (peak_jit_perfmap_offset > st.st_size ||
             st.st_size < peak_jit_perfmap_last_size)) {
            peak_jit_perfmap_offset = 0;
            peak_jit_provider_advance_generation(
                "source-truncated-during-drain");
        }
        peak_jit_perfmap_last_size = st.st_size;
    }
#ifdef PEAK_ENABLE_TEST_HOOKS
    peak_jit_test_signal_after_final_stat();
#endif
    fclose(fp);
    if (single_budget && metadata_consumed &&
        peak_jit_pending_record_count != 0) {
        peak_jit_single_budget_retry_turn = TRUE;
    }
    if (budget > 0) {
        unsigned long initial_budget = budget;

        pending |= peak_jit_provider_retry_pending_records(
            force_not_exec_timeout,
            &budget);
        if (single_budget && budget != initial_budget) {
            peak_jit_single_budget_retry_turn = FALSE;
        }
    }
    return pending || peak_jit_pending_record_count != 0;
}

void
peak_jit_provider_enable(void)
{
    peak_jit_init_runtime_config();
    const char* providers = configured_jit_providers;
    const char* map_path = configured_jit_map_path;

    peak_jit_provider_disable();
    peak_jit_provider_reset_diagnostics();
#ifdef PEAK_ENABLE_TEST_HOOKS
    peak_jit_test_attach_sequence_index = 0;
    peak_jit_test_final_stat_barrier_used = FALSE;
    peak_jit_test_final_stat_signal_pending = FALSE;
    peak_jit_test_source_observe_barrier_used = FALSE;
#endif

    if (!configured_jit_enabled) {
        return;
    }

    peak_jit_perfmap_enabled =
        peak_jit_provider_list_contains(providers, "perfmap");
    peak_jit_provider_enabled = peak_jit_perfmap_enabled;

    if (!peak_jit_provider_enabled) {
        peak_jit_trace("provider-enable",
                       providers != NULL ? providers : "<unset>",
                       "<none>",
                       0,
                       0,
                       "unsupported-provider");
        return;
    }

    peak_jit_perfmap_path =
        map_path != NULL && map_path[0] != '\0'
            ? g_strdup(map_path)
            : peak_jit_default_perfmap_path();
    peak_jit_perfmap_offset = 0;
    peak_jit_perfmap_identity_known = FALSE;
    peak_jit_perfmap_dev = 0;
    peak_jit_perfmap_ino = 0;
    peak_jit_perfmap_last_size = 0;
    peak_jit_provider_advance_generation("provider-enabled");

    peak_jit_trace("provider-enable",
                   "perfmap",
                   peak_jit_perfmap_path,
                   0,
                   0,
                   "enabled");
    peak_general_listener_controller_wake();
}

gboolean
peak_jit_provider_requested(void)
{
    const char* value = getenv(PEAK_JIT_ENABLE_ENV);

    /* This query runs before Gum initializes its embedded GLib runtime. */
    return value != NULL &&
           (strcmp(value, "1") == 0 ||
            strcasecmp(value, "true") == 0 ||
            strcasecmp(value, "yes") == 0 ||
            strcasecmp(value, "on") == 0);
}

gboolean
peak_jit_provider_is_active(void)
{
    return peak_jit_provider_enabled;
}

void
peak_jit_provider_get_diagnostics(PeakJitProviderDiagnostics* diagnostics)
{
    if (diagnostics == NULL) {
        return;
    }

    diagnostics->pending_queue_full =
        atomic_load_explicit(&peak_jit_pending_queue_full,
                             memory_order_relaxed);
    diagnostics->non_executable_timeout =
        atomic_load_explicit(&peak_jit_non_executable_timeout,
                             memory_order_relaxed);
    diagnostics->attach_retry_timeout =
        atomic_load_explicit(&peak_jit_attach_retry_timeout,
                             memory_order_relaxed);
    diagnostics->allocation_failure =
        atomic_load_explicit(&peak_jit_allocation_failure,
                             memory_order_relaxed);
    diagnostics->provider_generation = peak_jit_provider_generation;
    diagnostics->pending_count = peak_jit_pending_record_count;
    diagnostics->pending_high_water =
        atomic_load_explicit(&peak_jit_pending_high_water,
                             memory_order_relaxed);
}

void
peak_jit_provider_disable(void)
{
    peak_jit_provider_enabled = FALSE;
    peak_jit_perfmap_enabled = FALSE;
    peak_jit_perfmap_offset = 0;
    peak_jit_perfmap_identity_known = FALSE;
    peak_jit_perfmap_dev = 0;
    peak_jit_perfmap_ino = 0;
    peak_jit_perfmap_last_size = 0;
    peak_jit_pending_records_clear();
    g_free(peak_jit_pending_records);
    peak_jit_pending_records = NULL;
    g_free(peak_jit_perfmap_path);
    peak_jit_perfmap_path = NULL;
}

static gboolean
peak_jit_provider_drain_pending_with_mode(gboolean force_not_exec_timeout)
{
    gboolean pending = FALSE;

    if (!peak_jit_provider_enabled) {
        return FALSE;
    }

    if (peak_jit_perfmap_enabled) {
        pending |= peak_jit_provider_drain_perfmap(force_not_exec_timeout);
    }
    return pending;
}

gboolean
peak_jit_provider_drain_pending(void)
{
    return peak_jit_provider_drain_pending_with_mode(FALSE);
}

gboolean
peak_jit_provider_drain_pending_force_not_exec_timeout(void)
{
    peak_jit_pending_records_expire();
    return peak_jit_provider_drain_pending_with_mode(TRUE);
}
