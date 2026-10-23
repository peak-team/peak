#include "frida-gum.h"
#include <stdio.h>
#include <string.h>
extern int peak_prefix_branch_unsafe(int);
extern int peak_prefix_branch_safe(int);
extern int peak_prefix_branch_indirect(int);
extern int peak_prefix_branch_eret(int);
extern int peak_prefix_branch_bc(int);
extern int peak_prefix_branch_unsafe8(int);
extern int peak_prefix_branch_safe8(int);
extern int peak_prefix_branch_unsafe12(int);
extern int peak_prefix_branch_ret8(int);
extern int peak_prefix_branch_backward8(int);
extern int peak_prefix_branch_terminal8(int);
/* The public devkit amalgamation omits this exported relocator API. Keep its
 * pinned signature explicit so C compilers do not infer an unprototyped call. */
extern gboolean gum_arm64_relocator_can_relocate(gpointer, guint,
    GumRelocationScenario, GumRelocationPolicy, guint*, arm64_reg*);
extern gboolean peak_gum_arm64_can_relocate(gpointer, guint,
    GumRelocationScenario, GumRelocationPolicy, guint*, arm64_reg*)
    __attribute__((weak));
static guint calls;
static void count(GumInvocationContext* context, gpointer data)
{
    (void)context;
    (void)data;
    calls++;
}
static int check_fallback(gpointer target, guint expected_max, gboolean safe)
{
    guint old_max = 0, maximum = 0;
    arm64_reg scratch = ARM64_REG_INVALID;
    guint8 before[16];
    gboolean result;
    memcpy(before, target, sizeof(before));
    result = gum_arm64_relocator_can_relocate(target, 16,
        GUM_SCENARIO_ONLINE, GUM_RELOCATION_CHECKED, &old_max, &scratch);
    if (result || old_max != expected_max) {
        fprintf(stderr, "fallback fixture mismatch: result=%d maximum=%u expected=%u\n",
            result, old_max, expected_max);
        return 1;
    }
    scratch = ARM64_REG_INVALID;
    result = peak_gum_arm64_can_relocate(target, 16, GUM_SCENARIO_ONLINE,
        GUM_RELOCATION_CHECKED, &maximum, &scratch);
    if (result || maximum != (safe ? old_max : 0) ||
        memcmp(before, target, sizeof(before)) != 0) {
        fprintf(stderr, "fallback guard mismatch: result=%d maximum=%u safe=%d\n",
            result, maximum, safe);
        return 1;
    }
    return 0;
}
/* NVHPC 24.7 cannot compile memcpy/memcmp with a function symbol directly
 * as the data address ("alignment: unknown type"). Keep byte comparisons
 * behind an opaque pointer boundary while retaining the same assertions. */
static __attribute__((noinline)) int check_refused(GumInterceptor* interceptor,
    GumInvocationListener* listener, gpointer target)
{
    guint8 before[16];
    GumAttachReturn status;
    memcpy(before, target, sizeof(before));
    status = gum_interceptor_attach(interceptor, target, listener, NULL);
    if (status == GUM_ATTACH_OK || memcmp(before, target, sizeof(before)) != 0) {
        fprintf(stderr, "backend accepted or changed unsafe fallback: status=%d\n",
            status);
        return 1;
    }
    return 0;
}
int main(void)
{
    guint old_max = 0, maximum = 0, length = 0;
    arm64_reg scratch = ARM64_REG_INVALID;
    guint8 active[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint8 original[GUM_PEAK_MAX_PROLOGUE_SIZE];
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    GumAttachReturn status;
    gboolean have_patch;
    int result_zero, result_one;
    if (peak_gum_arm64_can_relocate == NULL) {
        puts("caller-provided Gum lacks prefix-branch check");
        return 77;
    }
    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(count, NULL, NULL, NULL);
    if (check_fallback((gpointer)peak_prefix_branch_unsafe8, 8, FALSE) ||
        check_fallback((gpointer)peak_prefix_branch_safe8, 8, TRUE) ||
        check_fallback((gpointer)peak_prefix_branch_unsafe12, 12, FALSE) ||
        check_fallback((gpointer)peak_prefix_branch_ret8, 8, TRUE) ||
        check_fallback((gpointer)peak_prefix_branch_backward8, 8, FALSE) ||
        check_fallback((gpointer)peak_prefix_branch_terminal8, 8, FALSE))
        return 1;
    if (check_refused(interceptor, listener, (gpointer)peak_prefix_branch_unsafe8) ||
        check_refused(interceptor, listener, (gpointer)peak_prefix_branch_unsafe12) ||
        check_refused(interceptor, listener, (gpointer)peak_prefix_branch_backward8) ||
        check_refused(interceptor, listener, (gpointer)peak_prefix_branch_terminal8))
        return 1;
    /* This reproduces the pinned relocator's blind spot without deliberately
     * executing overwritten pointer data in the normal regression run. */
    if (!gum_arm64_relocator_can_relocate((gpointer)peak_prefix_branch_unsafe,
            16, GUM_SCENARIO_ONLINE, GUM_RELOCATION_CHECKED, &old_max, &scratch) ||
        old_max != 16) {
        fputs("fixture no longer reproduces the pinned CFG omission\n", stderr);
        return 1;
    }
    scratch = ARM64_REG_INVALID;
    if (peak_gum_arm64_can_relocate((gpointer)peak_prefix_branch_unsafe,
            16, GUM_SCENARIO_ONLINE, GUM_RELOCATION_CHECKED, &maximum, &scratch) ||
        maximum != 0) {
        fputs("alternate-path branch into entry patch was accepted\n", stderr);
        return 1;
    }
    scratch = ARM64_REG_INVALID;
    if (peak_gum_arm64_can_relocate((gpointer)peak_prefix_branch_indirect,
            16, GUM_SCENARIO_ONLINE, GUM_RELOCATION_CHECKED, &maximum, &scratch) ||
        maximum != 0) {
        fprintf(stderr, "indirect alternate path accepted: maximum=%u\n", maximum);
        return 1;
    }
    scratch = ARM64_REG_INVALID;
    if (peak_gum_arm64_can_relocate((gpointer)peak_prefix_branch_eret,
            16, GUM_SCENARIO_ONLINE, GUM_RELOCATION_CHECKED, &maximum, &scratch) ||
        maximum != 0) {
        fprintf(stderr, "exception return alternate path accepted: maximum=%u\n", maximum);
        return 1;
    }
    scratch = ARM64_REG_INVALID;
    if (peak_gum_arm64_can_relocate((gpointer)peak_prefix_branch_bc,
            16, GUM_SCENARIO_ONLINE, GUM_RELOCATION_CHECKED, &maximum, &scratch) ||
        maximum != 0) {
        fprintf(stderr, "unsupported BC.cond prefix accepted: maximum=%u\n", maximum);
        return 1;
    }
    if (check_refused(interceptor, listener,
            (gpointer)peak_prefix_branch_unsafe))
        return 1;
    result_zero = peak_prefix_branch_unsafe(0);
    if (result_zero != 7 || calls != 0) {
        fputs("unsafe target did not fail closed without mutation\n", stderr);
        return 1;
    }
    status = gum_interceptor_attach(interceptor,
        (gpointer)peak_prefix_branch_safe, listener, NULL);
    if (status != GUM_ATTACH_OK) {
        fprintf(stderr, "backend rejected safe target: status=%d\n", status);
        return 1;
    }
    have_patch = gum_interceptor_peak_get_function_patch(interceptor,
        (gpointer)peak_prefix_branch_safe, listener, active, original, &length);
    result_zero = peak_prefix_branch_safe(0);
    result_one = peak_prefix_branch_safe(1);
    if (!have_patch || length != 16 || result_zero != 7 ||
        result_one != 7 || calls != 2) {
        fputs("safe alternate path lost ordinary 16-byte profiling\n", stderr);
        return 1;
    }
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    result_zero = peak_prefix_branch_safe(0);
    if (result_zero != 7 || calls != 2) {
        fprintf(stderr, "safe detach result=%d calls=%u\n", result_zero, calls);
        return 1;
    }
    calls = 0;
    status = gum_interceptor_attach(interceptor,
        (gpointer)peak_prefix_branch_ret8, listener, NULL);
    if (status != GUM_ATTACH_OK) {
        fprintf(stderr, "backend rejected safe 8-byte target: status=%d\n", status);
        return 1;
    }
    have_patch = gum_interceptor_peak_get_function_patch(interceptor,
        (gpointer)peak_prefix_branch_ret8, listener, active, original, &length);
    result_zero = peak_prefix_branch_ret8(7);
    if (!have_patch || length != 8 || result_zero != 7 || calls != 1) {
        fprintf(stderr, "safe fallback lost 8-byte profiling: length=%u calls=%u\n",
            length, calls);
        return 1;
    }
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    result_zero = peak_prefix_branch_ret8(7);
    if (result_zero != 7 || calls != 1) {
        fputs("safe 8-byte detach failed\n", stderr);
        return 1;
    }
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    puts("gum_arm64_prefix_branch_ok");
    return 0;
}
