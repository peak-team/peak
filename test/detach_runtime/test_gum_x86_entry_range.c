#include "frida-gum.h"
#include <stdio.h>
#include <string.h>

extern gboolean gum_x86_relocator_can_relocate(
    gpointer address, guint min_bytes, GumRelocationScenario scenario,
    guint *maximum);
extern int peak_x86_linear(int value);
extern int peak_x86_internal_jcc(int value);
extern int peak_x86_external_jcc(int value);
extern int peak_x86_internal_mid_jcc(int value);
extern int peak_x86_prefix_read(int value);
extern int peak_x86_rip_nop(int value);
extern void peak_x86_endbr_copy(const guint8 *source, guint8 *destination);

static guint calls;

static void enter(GumInvocationContext *context, gpointer data)
{
    (void)context;
    (void)data;
    calls++;
}

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static int range(gpointer entry, gboolean expected, guint maximum)
{
    guint actual = 999;

    CHECK(gum_x86_relocator_can_relocate(
        entry, 5, GUM_SCENARIO_ONLINE, &actual) == expected);
    CHECK(actual == maximum);
    return 0;
}

static int refuse(GumInterceptor *interceptor, GumInvocationListener *listener,
                  gpointer entry)
{
    guint8 before[5];
    guint old = calls;

    memcpy(before, entry, sizeof before);
    CHECK(range(entry, FALSE, 0) == 0);
    CHECK(gum_interceptor_peak_attach_exact(
        interceptor, entry, listener, NULL) != GUM_ATTACH_OK);
    CHECK(memcmp(before, entry, sizeof before) == 0);
    CHECK(calls == old);
    return 0;
}

static int attach_case(GumInterceptor *interceptor,
                       GumInvocationListener *listener, int (*function)(int),
                       int zero, int one)
{
    guint old = calls;
    guint length;
    guint8 before[5];
    guint8 active[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint8 original[GUM_PEAK_MAX_PROLOGUE_SIZE];

    memcpy(before, function, sizeof before);
    CHECK(range(function, TRUE, sizeof before) == 0);
    CHECK(function(0) == zero && function(1) == one);
    CHECK(calls == old);
    CHECK(gum_interceptor_peak_attach_exact(
        interceptor, function, listener, NULL) == GUM_ATTACH_OK);
    CHECK(gum_interceptor_peak_get_function_patch(
        interceptor, function, listener, active, original, &length));
    CHECK(length == sizeof before);
    CHECK(memcmp(original, before, sizeof before) == 0);
    CHECK(function(0) == zero && function(1) == one);
    CHECK(calls == old + 2);
    gum_interceptor_detach(interceptor, listener);
    CHECK(memcmp(before, function, sizeof before) == 0);
    CHECK(function(0) == zero && function(1) == one);
    CHECK(calls == old + 2);
    return 0;
}

static int endbr_copy(GumInterceptor *interceptor,
                      GumInvocationListener *listener)
{
    const guint8 landing[] = {0xf3, 0x0f, 0x1e, 0xfa};
    guint8 source[16];
    guint8 destination[16];
    guint8 before[6];
    guint8 active[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint8 original[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint index;
    guint old = calls;
    guint length;

    for (index = 0; index != sizeof source; index++) {
        source[index] = index * 13 + 7;
    }
    memcpy(before, peak_x86_endbr_copy, sizeof before);
    CHECK(memcmp(before, landing, sizeof landing) == 0);
    CHECK(range(peak_x86_endbr_copy, TRUE, 6) == 0);
    memset(destination, 0, sizeof destination);
    peak_x86_endbr_copy(source, destination);
    CHECK(memcmp(source, destination, sizeof source) == 0);
    CHECK(calls == old);
    CHECK(gum_interceptor_peak_attach_exact(
        interceptor, peak_x86_endbr_copy, listener, NULL) == GUM_ATTACH_OK);
    CHECK(gum_interceptor_peak_get_function_patch(
        interceptor, peak_x86_endbr_copy, listener, active, original, &length));
    CHECK(length == sizeof before);
    CHECK(memcmp(original, before, sizeof before) == 0);
    memset(destination, 0, sizeof destination);
    peak_x86_endbr_copy(source, destination);
    CHECK(memcmp(source, destination, sizeof source) == 0);
    CHECK(calls == old + 1);
    gum_interceptor_detach(interceptor, listener);
    CHECK(memcmp(before, peak_x86_endbr_copy, sizeof before) == 0);
    memset(destination, 0, sizeof destination);
    peak_x86_endbr_copy(source, destination);
    CHECK(memcmp(source, destination, sizeof source) == 0);
    CHECK(calls == old + 1);
    return 0;
}

static int rip_nop(GumInterceptor *interceptor, GumInvocationListener *listener)
{
    guint8 before[7];
    guint8 active[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint8 original[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint old = calls;
    guint length;

    memcpy(before, peak_x86_rip_nop, sizeof before);
    CHECK(range(peak_x86_rip_nop, TRUE, sizeof before) == 0);
    CHECK(peak_x86_rip_nop(0) == 7 && calls == old);
    CHECK(gum_interceptor_peak_attach_exact(
        interceptor, peak_x86_rip_nop, listener, NULL) == GUM_ATTACH_OK);
    CHECK(gum_interceptor_peak_get_function_patch(
        interceptor, peak_x86_rip_nop, listener, active, original, &length));
    CHECK(length == sizeof before);
    CHECK(memcmp(original, before, sizeof before) == 0);
    CHECK(peak_x86_rip_nop(0) == 7 && calls == old + 1);
    gum_interceptor_detach(interceptor, listener);
    CHECK(memcmp(before, peak_x86_rip_nop, sizeof before) == 0);
    CHECK(peak_x86_rip_nop(0) == 7 && calls == old + 1);
    return 0;
}

int main(void)
{
    GumInterceptor *interceptor;
    GumInvocationListener *listener;

    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(enter, NULL, NULL, NULL);
    CHECK(attach_case(interceptor, listener, peak_x86_linear, 11, 11) == 0);
    CHECK(endbr_copy(interceptor, listener) == 0);
    CHECK(refuse(interceptor, listener, peak_x86_internal_mid_jcc) == 0);
    CHECK(attach_case(interceptor, listener, peak_x86_internal_jcc, 19, 19) == 0);
    CHECK(attach_case(interceptor, listener, peak_x86_external_jcc, 17, 13) == 0);
    CHECK(refuse(interceptor, listener, peak_x86_prefix_read) == 0);
    CHECK(rip_nop(interceptor, listener) == 0);
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    puts("gum_x86_entry_range_ok");
    return 0;
}
