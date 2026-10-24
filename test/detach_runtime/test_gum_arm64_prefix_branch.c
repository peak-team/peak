#include "frida-gum.h"
#include <stdio.h>
#include <unistd.h>
extern int peak_prefix_hpsort(int);
extern int peak_prefix_linear(int);
extern int peak_prefix_branch_entry(int);
extern int peak_prefix_loop3(int);
extern int peak_prefix_early_return(int);
extern gboolean gum_arm64_relocator_can_relocate(gpointer, guint,
    GumRelocationScenario, GumRelocationPolicy, guint*, arm64_reg*);
static guint calls;
static void count(GumInvocationContext* context, gpointer data)
{
    (void)context;
    (void)data;
    calls++;
}
/* Volatile byte reads avoid NVHPC 24.7's code-address memory-intrinsic ICE. */
static void snapshot(gpointer target, guint8 before[16])
{
    const volatile guint8* code = (const volatile guint8*)target;
    guint i;
    for (i = 0; i != 16; i++) before[i] = code[i];
}
static gboolean unchanged(gpointer target, const guint8 before[16])
{
    const volatile guint8* code = (const volatile guint8*)target;
    guint i;
    for (i = 0; i != 16; i++) if (before[i] != code[i]) return FALSE;
    return TRUE;
}
static int check_range(gpointer target, guint expected)
{
    guint maximum = 0;
    arm64_reg scratch = ARM64_REG_INVALID;
    gboolean result = gum_arm64_relocator_can_relocate(target, 16,
        GUM_SCENARIO_ONLINE, GUM_RELOCATION_CHECKED, &maximum, &scratch);
    if (result != (expected == 16) || maximum != expected ||
        (expected != 0 && scratch == ARM64_REG_INVALID)) {
        fprintf(stderr, "prefix mismatch: result=%d max=%u expected=%u\n",
            result, maximum, expected);
        return 1;
    }
    return 0;
}
static int check_profile(GumInterceptor* interceptor,
    GumInvocationListener* listener, int (*target)(int), guint expected,
    int expected_zero, int expected_seven)
{
    guint length = 0;
    guint8 active[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint8 original[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint8 before[16];
    snapshot((gpointer)target, before);
    if (target(0) != expected_zero || target(7) != expected_seven) {
        fputs("unexpected fixture baseline\n", stderr);
        return 1;
    }
    calls = 0;
    if (gum_interceptor_peak_attach_exact(interceptor, (gpointer)target,
            listener, NULL) != GUM_ATTACH_OK ||
        !gum_interceptor_peak_get_function_patch(interceptor, (gpointer)target,
            listener, active, original, &length) || length != expected ||
        target(0) != expected_zero || target(7) != expected_seven || calls != 2) {
        fprintf(stderr, "prefix profile failed: length=%u expected=%u calls=%u\n",
            length, expected, calls);
        return 1;
    }
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    if (!unchanged((gpointer)target, before) ||
        target(0) != expected_zero || target(7) != expected_seven || calls != 2) {
        fputs("detach did not restore the original entry\n", stderr);
        return 1;
    }
    return 0;
}
int main(void)
{
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    alarm(10); /* Bound a regression that turns the three-iteration loop into a hang. */
    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(count, NULL, NULL, NULL);
    if (check_range((gpointer)peak_prefix_hpsort, 12) ||
        check_range((gpointer)peak_prefix_linear, 16) ||
        check_range((gpointer)peak_prefix_loop3, 4) ||
        check_range((gpointer)peak_prefix_early_return, 16) ||
        check_range((gpointer)peak_prefix_branch_entry, 4)) return 1;
    if (check_profile(interceptor, listener, peak_prefix_hpsort, 8, 7, 7) ||
        check_profile(interceptor, listener, peak_prefix_linear, 16, 7, 7) ||
        check_profile(interceptor, listener, peak_prefix_branch_entry, 4, 7, 7) ||
        check_profile(interceptor, listener, peak_prefix_loop3, 4, 3, 3) ||
        check_profile(interceptor, listener, peak_prefix_early_return, 16, 0, 8)) return 1;
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    alarm(0);
    puts("gum_arm64_prefix_branch_ok");
    return 0;
}
