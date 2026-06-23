#define _GNU_SOURCE
#include "peak_jit_provider.h"
#include "peak_general_listener_internal.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PEAK_JIT_ENABLE_ENV      "PEAK_JIT_ENABLE"
#define PEAK_JIT_PROVIDER_ENV    "PEAK_JIT_PROVIDER"
#define PEAK_JIT_MAP_PATH_ENV    "PEAK_JIT_MAP_PATH"
#define PEAK_JIT_TRACE_PATH_ENV  "PEAK_JIT_TRACE_PATH"

static gboolean peak_jit_provider_enabled = FALSE;
static gboolean peak_jit_perfmap_enabled = FALSE;
static char* peak_jit_perfmap_path = NULL;
static off_t peak_jit_perfmap_offset = 0;

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

static const char*
peak_jit_trace_path(void)
{
    const char* path = g_getenv(PEAK_JIT_TRACE_PATH_ENV);
    return path != NULL && path[0] != '\0' ? path : NULL;
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

    fprintf(fp,
            "%.9f,%s,%s,%s,0x%" PRIxPTR ",%zu,%s\n",
            peak_second(),
            event != NULL ? event : "<unknown>",
            provider != NULL ? provider : "<unknown>",
            name != NULL ? name : "<unknown>",
            address,
            size,
            result != NULL ? result : "<unknown>");
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

static void
peak_jit_provider_drain_perfmap(void)
{
    FILE* fp;
    char line[4096];

    if (peak_jit_perfmap_path == NULL) {
        return;
    }

    fp = fopen(peak_jit_perfmap_path, "r");
    if (fp == NULL) {
        return;
    }

    if (fseeko(fp, 0, SEEK_END) == 0) {
        off_t end = ftello(fp);
        if (end >= 0 && peak_jit_perfmap_offset > end) {
            peak_jit_perfmap_offset = 0;
        }
    }
    if (fseeko(fp, peak_jit_perfmap_offset, SEEK_SET) != 0) {
        peak_jit_perfmap_offset = 0;
        (void)fseeko(fp, 0, SEEK_SET);
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        uintptr_t address = 0;
        size_t size = 0;
        char* name = NULL;

        if (!peak_jit_parse_perfmap_line(line, &address, &size, &name)) {
            continue;
        }

        if (!peak_jit_range_is_executable(address, size)) {
            peak_jit_trace("perfmap-record",
                           "perfmap",
                           name,
                           address,
                           size,
                           "not-executable");
            continue;
        }

        gboolean attached =
            peak_general_listener_dynamic_attach_symbol(name,
                                                        (gpointer)address,
                                                        size,
                                                        "perfmap");
        peak_jit_trace("perfmap-record",
                       "perfmap",
                       name,
                       address,
                       size,
                       attached ? "attached" : "not-matched");
    }

    {
        off_t next_offset = ftello(fp);
        if (next_offset >= 0) {
            peak_jit_perfmap_offset = next_offset;
        }
    }

    fclose(fp);
}

void
peak_jit_provider_enable(void)
{
    const char* enable = g_getenv(PEAK_JIT_ENABLE_ENV);
    const char* providers = g_getenv(PEAK_JIT_PROVIDER_ENV);
    const char* map_path = g_getenv(PEAK_JIT_MAP_PATH_ENV);

    peak_jit_provider_disable();

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
    g_free(peak_jit_perfmap_path);
    peak_jit_perfmap_path = NULL;
}

void
peak_jit_provider_drain_pending(void)
{
    if (!peak_jit_provider_enabled) {
        return;
    }

    if (peak_jit_perfmap_enabled) {
        peak_jit_provider_drain_perfmap();
    }
}

gboolean
peak_jit_provider_is_enabled(void)
{
    return peak_jit_provider_enabled;
}
