#define _GNU_SOURCE
#include "internal/jit_provider.h"
#include "internal/general_listener_internal.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PEAK_JIT_ENABLE_ENV      "PEAK_JIT_ENABLE"
#define PEAK_JIT_PROVIDER_ENV    "PEAK_JIT_PROVIDER"
#define PEAK_JIT_MAP_PATH_ENV    "PEAK_JIT_MAP_PATH"
#define PEAK_JIT_TRACE_PATH_ENV  "PEAK_JIT_TRACE_PATH"
#define PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS_ENV \
    "PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS"
#define PEAK_JIT_DRAIN_RECORD_BUDGET_ENV "PEAK_JIT_DRAIN_RECORD_BUDGET"
#define PEAK_JIT_DEFAULT_NOT_EXEC_RETRY_TIMEOUT_MS 1000UL
#define PEAK_JIT_DEFAULT_DRAIN_RECORD_BUDGET 1024UL
#ifdef PEAK_ENABLE_TEST_HOOKS
#define PEAK_JIT_TEST_ATTACH_SEQUENCE_ENV "PEAK_JIT_TEST_ATTACH_SEQUENCE"
#endif

static gboolean peak_jit_provider_enabled = FALSE;
static gboolean peak_jit_perfmap_enabled = FALSE;
static char* peak_jit_perfmap_path = NULL;
static off_t peak_jit_perfmap_offset = 0;
static gboolean peak_jit_perfmap_identity_known = FALSE;
static dev_t peak_jit_perfmap_dev = 0;
static ino_t peak_jit_perfmap_ino = 0;
static off_t peak_jit_perfmap_last_size = 0;
static GPtrArray* peak_jit_pending_records = NULL;
#ifdef PEAK_ENABLE_TEST_HOOKS
static unsigned int peak_jit_test_attach_sequence_index = 0;
#endif

typedef enum {
    PEAK_JIT_PENDING_NOT_EXECUTABLE = 0,
    PEAK_JIT_PENDING_ATTACH_RETRY
} PeakJitPendingKind;

typedef struct {
    uintptr_t address;
    size_t size;
    char* name;
    double not_exec_started_at;
} PeakJitPendingRecord;

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
peak_jit_parse_ulong_env(const char* name, unsigned long fallback)
{
    const char* value = g_getenv(name);
    char* end = NULL;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') {
        return fallback;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return fallback;
    }
    return parsed;
}

static const char*
peak_jit_trace_path(void)
{
    const char* path = g_getenv(PEAK_JIT_TRACE_PATH_ENV);
    return path != NULL && path[0] != '\0' ? path : NULL;
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
    return peak_jit_parse_ulong_env(PEAK_JIT_NOT_EXEC_RETRY_TIMEOUT_MS_ENV,
                                    PEAK_JIT_DEFAULT_NOT_EXEC_RETRY_TIMEOUT_MS);
}

static unsigned long
peak_jit_drain_record_budget(void)
{
    unsigned long budget =
        peak_jit_parse_ulong_env(PEAK_JIT_DRAIN_RECORD_BUDGET_ENV,
                                 PEAK_JIT_DEFAULT_DRAIN_RECORD_BUDGET);

    return budget == 0 ? PEAK_JIT_DEFAULT_DRAIN_RECORD_BUDGET : budget;
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

static GPtrArray*
peak_jit_pending_records_ensure(void)
{
    if (peak_jit_pending_records == NULL) {
        peak_jit_pending_records =
            g_ptr_array_new_with_free_func(peak_jit_pending_record_free);
    }

    return peak_jit_pending_records;
}

static void
peak_jit_pending_records_clear(void)
{
    if (peak_jit_pending_records != NULL) {
        g_ptr_array_set_size(peak_jit_pending_records, 0);
    }
}

static PeakJitPendingRecord*
peak_jit_pending_record_find(uintptr_t address, size_t size, const char* name)
{
    if (peak_jit_pending_records == NULL || name == NULL) {
        return NULL;
    }

    for (guint i = 0; i < peak_jit_pending_records->len; i++) {
        PeakJitPendingRecord* record =
            g_ptr_array_index(peak_jit_pending_records, i);

        if (record->address == address &&
            record->size == size &&
            strcmp(record->name, name) == 0) {
            return record;
        }
    }

    return NULL;
}

static void
peak_jit_pending_record_add(PeakJitPendingKind kind,
                            uintptr_t address,
                            size_t size,
                            const char* name)
{
    PeakJitPendingRecord* record;

    if (name == NULL || name[0] == '\0') {
        return;
    }

    record = peak_jit_pending_record_find(address, size, name);
    if (record != NULL) {
        if (kind == PEAK_JIT_PENDING_NOT_EXECUTABLE &&
            record->not_exec_started_at <= 0.0) {
            record->not_exec_started_at = peak_second();
        }
        return;
    }

    record = g_new0(PeakJitPendingRecord, 1);
    record->address = address;
    record->size = size;
    record->name = g_strdup(name);
    record->not_exec_started_at =
        kind == PEAK_JIT_PENDING_NOT_EXECUTABLE ? peak_second() : 0.0;

    g_ptr_array_add(peak_jit_pending_records_ensure(), record);
}

static void
peak_jit_pending_record_remove_index(guint index)
{
    if (peak_jit_pending_records != NULL &&
        index < peak_jit_pending_records->len) {
        g_ptr_array_remove_index(peak_jit_pending_records, index);
    }
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
    const char* sequence = g_getenv(PEAK_JIT_TEST_ATTACH_SEQUENCE_ENV);
    char** parts;
    char* token;
    gboolean forced = TRUE;

    if (sequence == NULL || sequence[0] == '\0') {
        return FALSE;
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

static PeakDynamicAttachResult
peak_jit_attach_perfmap_symbol(const char* name, uintptr_t address, size_t size)
{
    PeakDynamicAttachResult result;

#ifdef PEAK_ENABLE_TEST_HOOKS
    if (peak_jit_test_forced_attach_result(&result)) {
        return result;
    }
#endif

    return peak_general_listener_dynamic_attach_symbol(name,
                                                       (gpointer)address,
                                                       size,
                                                       "perfmap");
}

static const char*
peak_jit_attach_result_string(PeakDynamicAttachResult result)
{
    switch (result) {
        case PEAK_DYNAMIC_ATTACH_ATTACHED:
            return "attached";
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

static gboolean
peak_jit_pending_not_exec_timed_out(PeakJitPendingRecord* record,
                                    gboolean force_not_exec_timeout)
{
    double now;
    unsigned long timeout_ms;

    if (force_not_exec_timeout) {
        return TRUE;
    }

    if (record->not_exec_started_at <= 0.0) {
        record->not_exec_started_at = peak_second();
        return FALSE;
    }

    now = peak_second();
    timeout_ms = peak_jit_not_exec_retry_timeout_ms();
    return (now - record->not_exec_started_at) * 1000.0 >=
           (double)timeout_ms;
}

static gboolean
peak_jit_provider_retry_pending_records(gboolean force_not_exec_timeout,
                                        unsigned long* budget)
{
    gboolean pending = FALSE;

    if (peak_jit_pending_records == NULL ||
        peak_jit_pending_records->len == 0) {
        return FALSE;
    }

    for (guint i = 0; i < peak_jit_pending_records->len;) {
        PeakJitPendingRecord* record =
            g_ptr_array_index(peak_jit_pending_records, i);
        PeakDynamicAttachResult attach_result;

        if (budget != NULL) {
            if (*budget == 0) {
                pending = TRUE;
                break;
            }
            (*budget)--;
        }

        if (!peak_jit_range_is_executable(record->address, record->size)) {
            gboolean timed_out =
                peak_jit_pending_not_exec_timed_out(record,
                                                    force_not_exec_timeout);

            peak_jit_trace("perfmap-record",
                           "perfmap",
                           record->name,
                           record->address,
                           record->size,
                           timed_out ? "not-executable-timeout" :
                                       "not-executable-retry");
            if (timed_out) {
                peak_jit_pending_record_remove_index(i);
                continue;
            }

            pending = TRUE;
            i++;
            continue;
        }

        attach_result = peak_jit_attach_perfmap_symbol(record->name,
                                                       record->address,
                                                       record->size);
        peak_jit_trace("perfmap-record",
                       "perfmap",
                       record->name,
                       record->address,
                       record->size,
                       peak_jit_attach_result_string(attach_result));
        if (attach_result == PEAK_DYNAMIC_ATTACH_RETRY) {
            pending = TRUE;
            i++;
            continue;
        }

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
    struct stat st;

    pending |= peak_jit_provider_retry_pending_records(force_not_exec_timeout,
                                                       &budget);
    if (budget == 0) {
        return TRUE;
    }

    if (peak_jit_perfmap_path == NULL) {
        return pending;
    }

    fp = fopen(peak_jit_perfmap_path, "r");
    if (fp == NULL) {
        return pending;
    }

    if (fstat(fileno(fp), &st) == 0) {
        if (peak_jit_perfmap_identity_known &&
            (st.st_dev != peak_jit_perfmap_dev ||
             st.st_ino != peak_jit_perfmap_ino)) {
            peak_jit_perfmap_offset = 0;
            peak_jit_pending_records_clear();
        }
        peak_jit_perfmap_identity_known = TRUE;
        peak_jit_perfmap_dev = st.st_dev;
        peak_jit_perfmap_ino = st.st_ino;
        if (peak_jit_perfmap_offset > st.st_size ||
            st.st_size < peak_jit_perfmap_last_size) {
            peak_jit_perfmap_offset = 0;
            peak_jit_pending_records_clear();
        }
        peak_jit_perfmap_last_size = st.st_size;
    }

    if (fseeko(fp, 0, SEEK_END) == 0) {
        off_t end = ftello(fp);
        if (end >= 0 && peak_jit_perfmap_offset > end) {
            peak_jit_perfmap_offset = 0;
            peak_jit_pending_records_clear();
        }
    }
    if (fseeko(fp, peak_jit_perfmap_offset, SEEK_SET) != 0) {
        peak_jit_perfmap_offset = 0;
        peak_jit_pending_records_clear();
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
            peak_jit_trace("perfmap-record",
                           "perfmap",
                           name,
                           address,
                           size,
                           matches_target && force_not_exec_timeout ?
                               "not-executable-timeout" :
                               matches_target ? "not-executable-retry" :
                                                "not-executable");
            if (matches_target && !force_not_exec_timeout) {
                peak_jit_pending_record_add(PEAK_JIT_PENDING_NOT_EXECUTABLE,
                                            address,
                                            size,
                                            name);
                pending = TRUE;
            }
            if (next_offset >= 0) {
                committed_offset = next_offset;
            }
            continue;
        }

        attach_result = peak_jit_attach_perfmap_symbol(name, address, size);
        peak_jit_trace("perfmap-record",
                       "perfmap",
                       name,
                       address,
                       size,
                       peak_jit_attach_result_string(attach_result));
        if (attach_result == PEAK_DYNAMIC_ATTACH_RETRY) {
            peak_jit_pending_record_add(PEAK_JIT_PENDING_ATTACH_RETRY,
                                        address,
                                        size,
                                        name);
            pending = TRUE;
        }
        if (next_offset >= 0) {
            committed_offset = next_offset;
        }
    }

    peak_jit_perfmap_offset = committed_offset;
    fclose(fp);
    return pending;
}

void
peak_jit_provider_enable(void)
{
    const char* enable = g_getenv(PEAK_JIT_ENABLE_ENV);
    const char* providers = g_getenv(PEAK_JIT_PROVIDER_ENV);
    const char* map_path = g_getenv(PEAK_JIT_MAP_PATH_ENV);

    peak_jit_provider_disable();
#ifdef PEAK_ENABLE_TEST_HOOKS
    peak_jit_test_attach_sequence_index = 0;
#endif

    if (!peak_jit_env_truthy(enable)) {
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
    peak_jit_pending_records_clear();

    peak_jit_trace("provider-enable",
                   "perfmap",
                   peak_jit_perfmap_path,
                   0,
                   0,
                   "enabled");
    peak_general_listener_controller_wake();
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
    return peak_jit_provider_drain_pending_with_mode(TRUE);
}
