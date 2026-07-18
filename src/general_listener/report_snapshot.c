#include "internal/general_listener/report_snapshot.h"

#include <stdlib.h>
#include <string.h>

static char*
peak_report_snapshot_duplicate_string(const char* source)
{
    const char* value = source != NULL ? source : "";
    size_t length = strlen(value) + 1;
    char* copy = malloc(length);

    if (copy != NULL) {
        memcpy(copy, value, length);
    }
    return copy;
}

static bool
peak_report_snapshot_allocate_arrays(PeakReportSnapshot* snapshot)
{
    size_t count;

    if (snapshot == NULL) {
        return false;
    }
    count = snapshot->hook_count;
    if (count == 0) {
        return true;
    }

#define PEAK_REPORT_ALLOCATE(field)                                           \
    do {                                                                      \
        snapshot->field = calloc(count, sizeof(*snapshot->field));            \
        if (snapshot->field == NULL) {                                        \
            return false;                                                     \
        }                                                                     \
    } while (0)

    PEAK_REPORT_ALLOCATE(names);
    PEAK_REPORT_ALLOCATE(instrumented);
    PEAK_REPORT_ALLOCATE(detached);
    PEAK_REPORT_ALLOCATE(reattached);
    PEAK_REPORT_ALLOCATE(revisited);
    PEAK_REPORT_ALLOCATE(num_calls);
    PEAK_REPORT_ALLOCATE(total_time);
    PEAK_REPORT_ALLOCATE(max_total_time);
    PEAK_REPORT_ALLOCATE(min_total_time);
    PEAK_REPORT_ALLOCATE(exclusive_time);
    PEAK_REPORT_ALLOCATE(max_time);
    PEAK_REPORT_ALLOCATE(min_time);
    PEAK_REPORT_ALLOCATE(thread_count);

#undef PEAK_REPORT_ALLOCATE
    return true;
}

PeakReportSnapshot*
peak_report_snapshot_create(size_t hook_count)
{
    PeakReportSnapshot* snapshot = calloc(1, sizeof(*snapshot));

    if (snapshot == NULL) {
        return NULL;
    }
    snapshot->hook_count = hook_count;
    snapshot->rank_count = 1;
    if (!peak_report_snapshot_allocate_arrays(snapshot)) {
        peak_report_snapshot_destroy(snapshot);
        return NULL;
    }
    return snapshot;
}

bool
peak_report_snapshot_set_program(PeakReportSnapshot* snapshot,
                                 const char* program)
{
    char* copy;

    if (snapshot == NULL) {
        return false;
    }
    copy = peak_report_snapshot_duplicate_string(program);
    if (copy == NULL) {
        return false;
    }
    free(snapshot->program);
    snapshot->program = copy;
    return true;
}

bool
peak_report_snapshot_set_name(PeakReportSnapshot* snapshot,
                              size_t hook_id,
                              const char* name)
{
    char* copy;

    if (snapshot == NULL || hook_id >= snapshot->hook_count) {
        return false;
    }
    copy = peak_report_snapshot_duplicate_string(name);
    if (copy == NULL) {
        return false;
    }
    free(snapshot->names[hook_id]);
    snapshot->names[hook_id] = copy;
    return true;
}

PeakReportSnapshot*
peak_report_snapshot_clone(
    const PeakReportSnapshot* source)
{
    PeakReportSnapshot* copy;
    size_t count;

    if (source == NULL) {
        return NULL;
    }
    copy = peak_report_snapshot_create(source->hook_count);
    if (copy == NULL) {
        return NULL;
    }
    count = source->hook_count;
    if (!peak_report_snapshot_set_program(copy, source->program)) {
        peak_report_snapshot_destroy(copy);
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (!peak_report_snapshot_set_name(copy, i, source->names[i])) {
            peak_report_snapshot_destroy(copy);
            return NULL;
        }
    }

#define PEAK_REPORT_COPY(field)                                               \
    do {                                                                      \
        if (count != 0) {                                                     \
            memcpy(copy->field, source->field,                               \
                   count * sizeof(*copy->field));                             \
        }                                                                     \
    } while (0)

    PEAK_REPORT_COPY(instrumented);
    PEAK_REPORT_COPY(detached);
    PEAK_REPORT_COPY(reattached);
    PEAK_REPORT_COPY(revisited);
    PEAK_REPORT_COPY(num_calls);
    PEAK_REPORT_COPY(total_time);
    PEAK_REPORT_COPY(max_total_time);
    PEAK_REPORT_COPY(min_total_time);
    PEAK_REPORT_COPY(exclusive_time);
    PEAK_REPORT_COPY(max_time);
    PEAK_REPORT_COPY(min_time);
    PEAK_REPORT_COPY(thread_count);

#undef PEAK_REPORT_COPY
    copy->overhead_per_call = source->overhead_per_call;
    copy->rank_count = source->rank_count;
    copy->overhead = source->overhead;
    return copy;
}

void
peak_report_snapshot_prepare_for_render(PeakReportSnapshot* snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    peak_report_sanitize_times(snapshot->hook_count,
                               snapshot->total_time,
                               snapshot->exclusive_time);
}

uint64_t
peak_report_snapshot_slot_identity_hash(
    const PeakReportSnapshot* snapshot,
    size_t hook_id)
{
    const unsigned char* text;
    uint64_t hash = 1469598103934665603ULL;

    if (snapshot == NULL || hook_id >= snapshot->hook_count) {
        return 0;
    }
    text = (const unsigned char*)(snapshot->names[hook_id] != NULL ?
                                      snapshot->names[hook_id] : "");
    while (*text != '\0') {
        hash ^= (uint64_t)*text++;
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool
peak_report_snapshot_has_duplicate_names(
    const PeakReportSnapshot* snapshot)
{
    if (snapshot == NULL) {
        return false;
    }
    for (size_t i = 0; i < snapshot->hook_count; i++) {
        const char* left = snapshot->names[i];

        if (left == NULL || left[0] == '\0') {
            continue;
        }
        for (size_t j = i + 1; j < snapshot->hook_count; j++) {
            const char* right = snapshot->names[j];

            if (right != NULL && strcmp(left, right) == 0) {
                return true;
            }
        }
    }
    return false;
}

void
peak_report_snapshot_destroy(PeakReportSnapshot* snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    free(snapshot->program);
    if (snapshot->names != NULL) {
        for (size_t i = 0; i < snapshot->hook_count; i++) {
            free(snapshot->names[i]);
        }
    }
    free(snapshot->names);
    free(snapshot->instrumented);
    free(snapshot->detached);
    free(snapshot->reattached);
    free(snapshot->revisited);
    free(snapshot->num_calls);
    free(snapshot->total_time);
    free(snapshot->max_total_time);
    free(snapshot->min_total_time);
    free(snapshot->exclusive_time);
    free(snapshot->max_time);
    free(snapshot->min_time);
    free(snapshot->thread_count);
    free(snapshot);
}
