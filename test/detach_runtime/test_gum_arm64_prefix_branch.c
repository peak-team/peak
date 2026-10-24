#include "frida-gum.h"
#include <stdio.h>
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
extern int peak_prefix_branch_budget(int);
extern int peak_prefix_branch_budget4(int);
extern int peak_prefix_branch_safe4(int);
extern int peak_prefix_branch_sxtw_kill(int);
extern int peak_prefix_branch_ubfm_kill(int);
extern int peak_prefix_branch_sxtw_live(int);
extern int peak_prefix_branch_bfm_live(int);
extern int peak_prefix_branch_x16_live(int);
extern int peak_prefix_branch_bl_inside(int);
extern int peak_prefix_branch_bl_boundary(int);
extern int peak_prefix_literal_w(int);
extern int peak_prefix_literal_x(int);
extern int peak_prefix_literal_sw(int);
extern int peak_prefix_literal_s(int);
extern int peak_prefix_literal_d(int);
extern int peak_prefix_literal_q(int);
extern int peak_prefix_literal_boundary(int);
extern int peak_prefix_literal_entry(int);
extern int peak_prefix_literal_crossing(int);
extern int peak_prefix_suffix_svc(int);
extern int peak_prefix_suffix_svc_live(int);
extern int peak_order_a(int);
extern int peak_order_b(int);
extern int peak_order_back_a(int);
extern int peak_order_back_b(int);
extern int peak_order_overlap_a(int);
extern int peak_order_overlap_b(int);
extern int peak_order_interior_b(int);
extern int peak_suffix_dispatch(int (*callee)(int), int);
extern int peak_prefix_indirect_entry(int);
extern int peak_extent_returning(int);
extern const guint8 peak_extent_returning_end[];
extern int peak_extent_noreturn(int);
extern const guint8 peak_extent_noreturn_end[];
extern int peak_extent_cold(int);
extern const guint8 peak_extent_cold_end[];
/* The public devkit amalgamation omits this exported relocator API. */
extern gboolean gum_arm64_relocator_can_relocate(gpointer, guint,
    GumRelocationScenario, GumRelocationPolicy, guint*, arm64_reg*);
static guint calls;
static void count(GumInvocationContext* context, gpointer data)
{
    (void)context;
    (void)data;
    calls++;
}
/* NVHPC 24.7 cannot compile memory intrinsics on code addresses. Volatile
 * byte reads preserve the assertions without recreating those intrinsics. */
static void snapshot_entry(gpointer target, guint8 before[16])
{
    const volatile guint8* code = (const volatile guint8*)target;
    guint i;
    for (i = 0; i != 16; i++) before[i] = code[i];
}
static gboolean entry_matches(gpointer target, const guint8 before[16])
{
    const volatile guint8* code = (const volatile guint8*)target;
    guint i;
    for (i = 0; i != 16; i++) {
        if (before[i] != code[i]) return FALSE;
    }
    return TRUE;
}
static int check_range(gpointer target, guint expected_max)
{
    guint maximum = 0;
    arm64_reg scratch = ARM64_REG_INVALID;
    guint8 before[16];
    gboolean result;
    snapshot_entry(target, before);
    result = gum_arm64_relocator_can_relocate(target, 16,
        GUM_SCENARIO_ONLINE, GUM_RELOCATION_CHECKED, &maximum, &scratch);
    if (result != (expected_max == 16) || maximum != expected_max ||
        !entry_matches(target, before)) {
        fprintf(stderr, "range mismatch: result=%d maximum=%u expected=%u\n",
            result, maximum, expected_max);
        return 1;
    }
    if (expected_max != 0) {
        scratch = ARM64_REG_INVALID;
        result = gum_arm64_relocator_can_relocate(target, expected_max,
            GUM_SCENARIO_ONLINE, GUM_RELOCATION_CHECKED, &maximum, &scratch);
        if (!result || maximum < expected_max || scratch == ARM64_REG_INVALID) {
            fputs("selected range did not remain relocatable\n", stderr);
            return 1;
        }
    }
    return 0;
}
static int check_refused(GumInterceptor* interceptor,
    GumInvocationListener* listener, gpointer target)
{
    guint8 before[16];
    GumAttachReturn status;
    snapshot_entry(target, before);
    status = gum_interceptor_peak_attach_exact(interceptor, target, listener, NULL);
    if (status == GUM_ATTACH_OK || !entry_matches(target, before)) {
        fprintf(stderr, "backend accepted or changed unsafe target: status=%d\n",
            status);
        return 1;
    }
    return 0;
}
static int check_profile_with_extent(GumInterceptor* interceptor,
    GumInvocationListener* listener, int (*target)(int), guint expected_length,
    const GumMemoryRange* extent)
{
    guint length = 0;
    guint8 active[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint8 original[GUM_PEAK_MAX_PROLOGUE_SIZE];
    GumAttachReturn status;
    gboolean have_patch;
    int baseline_zero = target(0), baseline_seven = target(7);
    int result_zero, result_seven;
    if (baseline_seven != 7) {
        fputs("unexpected profile fixture baseline\n", stderr);
        return 1;
    }
    calls = 0;
    status = extent != NULL
        ? gum_interceptor_peak_attach_with_extent(interceptor, (gpointer)target,
            listener, NULL, extent, TRUE)
        : gum_interceptor_attach(interceptor, (gpointer)target, listener, NULL);
    if (status != GUM_ATTACH_OK) {
        fprintf(stderr, "backend rejected safe range %u: status=%d\n",
            expected_length, status);
        return 1;
    }
    have_patch = gum_interceptor_peak_get_function_patch(interceptor,
        (gpointer)target, listener, active, original, &length);
    result_zero = target(0);
    result_seven = target(7);
    if (!have_patch || length != expected_length ||
        result_zero != baseline_zero || result_seven != baseline_seven || calls != 2) {
        fprintf(stderr, "range profile failed: length=%u expected=%u calls=%u\n",
            length, expected_length, calls);
        return 1;
    }
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    result_zero = target(0);
    result_seven = target(7);
    if (result_zero != baseline_zero || result_seven != baseline_seven || calls != 2) {
        fputs("range detach failed\n", stderr);
        return 1;
    }
    return 0;
}
static int check_profile(GumInterceptor* interceptor,
    GumInvocationListener* listener, int (*target)(int), guint expected_length)
{
    return check_profile_with_extent(interceptor, listener, target,
        expected_length, NULL);
}
static int check_extent_cases(GumInterceptor* interceptor,
    GumInvocationListener* listener)
{
    GumMemoryRange extent;
    guint8 active[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint8 original[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint length = 0;
    extent.base_address = GUM_ADDRESS(peak_extent_returning);
    extent.size = GUM_ADDRESS(peak_extent_returning_end) - extent.base_address;
    if (check_profile_with_extent(interceptor, listener, peak_extent_returning,
            8, &extent)) return 1;
    extent.base_address = GUM_ADDRESS(peak_extent_cold);
    extent.size = GUM_ADDRESS(peak_extent_cold_end) - extent.base_address;
    if (check_profile_with_extent(interceptor, listener, peak_extent_cold,
            8, &extent)) return 1;
    extent.base_address = GUM_ADDRESS(peak_extent_noreturn);
    extent.size = GUM_ADDRESS(peak_extent_noreturn_end) - extent.base_address;
    if (check_refused(interceptor, listener, (gpointer)peak_extent_noreturn) ||
        gum_interceptor_peak_attach_with_extent(interceptor,
            (gpointer)peak_extent_noreturn, listener, NULL, &extent, TRUE) != GUM_ATTACH_OK ||
        !gum_interceptor_peak_get_function_patch(interceptor,
            (gpointer)peak_extent_noreturn, listener, active, original, &length) ||
        length != 16) return 1;
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    return check_refused(interceptor, listener, (gpointer)peak_extent_noreturn);
}
static void count_slot(GumInvocationContext* context, gpointer data)
{
    (void)context;
    (*(guint*)data)++;
}
static int check_scope_ended(gpointer target)
{
    guint8* actual;
    gsize size = 0;
    gboolean equal;
    guint8 live[16];
    snapshot_entry(target, live);
    actual = gum_interceptor_peak_read_relocation_code(target, 16, &size);
    equal = actual != NULL && size == 16;
    if (equal) {
        guint i;
        for (i = 0; i != 16; i++) {
            if (actual[i] != live[i]) equal = FALSE;
        }
    }
    g_free(actual);
    if (!equal || !gum_interceptor_peak_relocation_range_available(target, 16)) {
        fputs("relocation analysis scope leaked into ordinary reads\n", stderr);
        return 1;
    }
    return 0;
}
static int check_order(GumInterceptor* interceptor, int (*a)(int), int (*b)(int),
    guint span, guint expected_b, gboolean a_first, gboolean queued)
{
    guint a_calls = 0, b_calls = 0, length = 0;
    guint8 active[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint8 original[GUM_PEAK_MAX_PROLOGUE_SIZE];
    GumInvocationListener* la = gum_make_call_listener(count_slot, NULL, &a_calls, NULL);
    GumInvocationListener* lb = gum_make_call_listener(count_slot, NULL, &b_calls, NULL);
    GumAttachReturn ra, rb;
    int zero = a(0), seven = a(7);
    if (zero != 7 || seven != 7) return 1;
    if (queued) gum_interceptor_begin_transaction(interceptor);
    if (a_first) {
        ra = gum_interceptor_peak_attach_exact(interceptor, (gpointer)a, la, NULL);
        rb = gum_interceptor_peak_attach_exact(interceptor, (gpointer)b, lb, NULL);
    } else {
        rb = gum_interceptor_peak_attach_exact(interceptor, (gpointer)b, lb, NULL);
        ra = gum_interceptor_peak_attach_exact(interceptor, (gpointer)a, la, NULL);
    }
    if (queued) gum_interceptor_end_transaction(interceptor);
    if (ra != GUM_ATTACH_OK || rb != GUM_ATTACH_OK ||
        !gum_interceptor_peak_get_function_patch(interceptor, (gpointer)a,
            la, active, original, &length) || length != span) {
        fprintf(stderr, "hook order failed: A=%d B=%d span=%u expected=%u\n",
            ra, rb, length, span);
        return 1;
    }
    zero = a(0);
    seven = a(7);
    if (zero != 7 || seven != 7 || a_calls != 2 || b_calls != expected_b ||
        check_scope_ended((gpointer)b)) return 1;
    gum_interceptor_detach(interceptor, la);
    gum_interceptor_flush(interceptor);
    ra = gum_interceptor_peak_attach_exact(interceptor, (gpointer)a, la, NULL);
    if (ra != GUM_ATTACH_OK) return 1;
    zero = a(0);
    seven = a(7);
    if (zero != 7 || seven != 7 || a_calls != 4 || b_calls != 2 * expected_b ||
        check_scope_ended((gpointer)b)) return 1;
    gum_interceptor_detach(interceptor, la);
    gum_interceptor_detach(interceptor, lb);
    gum_interceptor_flush(interceptor);
    g_object_unref(la);
    g_object_unref(lb);
    return 0;
}
static int check_active_interior_target(GumInterceptor* interceptor,
    GumInvocationListener* listener)
{
    GumAttachReturn status = gum_interceptor_peak_attach_exact(interceptor,
        (gpointer)peak_order_a, listener, NULL);
    if (status != GUM_ATTACH_OK ||
        check_refused(interceptor, listener, (gpointer)peak_order_interior_b) ||
        check_scope_ended((gpointer)peak_order_a)) return 1;
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    return 0;
}
static int replacement_seventeen(int value)
{
    (void)value;
    return 17;
}
static int check_opaque_replacement(GumInterceptor* interceptor,
    GumInvocationListener* listener)
{
    GumReplaceReturn status = gum_interceptor_replace(interceptor,
        (gpointer)peak_order_b, (gpointer)replacement_seventeen, NULL, NULL);
    if (status != GUM_REPLACE_OK) return 1;
    calls = 0;
    if (gum_interceptor_peak_attach_exact(interceptor, (gpointer)peak_order_a,
            listener, NULL) != GUM_ATTACH_OK ||
        peak_order_a(0) != 17 || peak_order_a(7) != 7 || calls != 2 ||
        check_scope_ended((gpointer)peak_order_b)) return 1;
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    if (peak_order_a(0) != 17 || calls != 2) return 1;
    gum_interceptor_revert(interceptor, (gpointer)peak_order_b);
    gum_interceptor_flush(interceptor);
    return peak_order_a(0) != 7;
}
static int check_occupied_overlap(GumInterceptor* interceptor,
    GumInvocationListener* listener)
{
    GumAttachReturn status = gum_interceptor_peak_attach_exact(interceptor,
        (gpointer)peak_order_overlap_a, listener, NULL);
    if (status != GUM_ATTACH_OK ||
        check_refused(interceptor, listener, (gpointer)peak_order_overlap_b) ||
        check_scope_ended((gpointer)peak_order_overlap_a)) return 1;
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    return 0;
}
static int dispatch_plus_one(int value) { return value + 1; }
static int dispatch_plus_seven(int value) { return value + 7; }
static int check_indirect_dispatch(GumInterceptor* interceptor,
    GumInvocationListener* listener)
{
    calls = 0;
    if (gum_interceptor_peak_attach_exact(interceptor,
            (gpointer)peak_suffix_dispatch, listener, NULL) != GUM_ATTACH_OK)
        return 1;
    if (peak_suffix_dispatch(dispatch_plus_one, 3) != 4 ||
        peak_suffix_dispatch(dispatch_plus_seven, 3) != 10 || calls != 2)
        return 1;
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    if (peak_suffix_dispatch(dispatch_plus_one, 5) != 6 || calls != 2)
        return 1;
    return 0;
}
int main(void)
{
#if !defined(PEAK_GUM_ENTRY_RANGE_CHECKS)
    puts("caller-provided Gum lacks entry-range checks");
    return 77;
#else
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    if (gum_interceptor_peak_entry_range_revision() != 1) {
        fputs("entry-range header/archive revision mismatch\n", stderr);
        return 1;
    }
    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(count, NULL, NULL, NULL);
    if (check_range((gpointer)peak_prefix_branch_unsafe, 8) ||
        check_range((gpointer)peak_prefix_branch_safe, 16) ||
        check_range((gpointer)peak_prefix_branch_unsafe8, 4) ||
        check_range((gpointer)peak_prefix_branch_safe8, 8) ||
        check_range((gpointer)peak_prefix_branch_unsafe12, 4) ||
        check_range((gpointer)peak_prefix_branch_ret8, 8) ||
        check_range((gpointer)peak_prefix_branch_backward8, 4) ||
        check_range((gpointer)peak_prefix_branch_terminal8, 4) ||
        check_range((gpointer)peak_prefix_branch_indirect, 16) ||
        check_range((gpointer)peak_prefix_indirect_entry, 0) ||
        check_range((gpointer)peak_suffix_dispatch, 16) ||
        check_range((gpointer)peak_extent_returning, 8) ||
        check_range((gpointer)peak_extent_cold, 8) ||
        check_range((gpointer)peak_extent_noreturn, 0) ||
        check_range((gpointer)peak_prefix_branch_eret, 0) ||
        check_range((gpointer)peak_prefix_branch_bc, 0) ||
        check_range((gpointer)peak_prefix_branch_budget, 0) ||
        check_range((gpointer)peak_prefix_branch_budget4, 0) ||
        check_range((gpointer)peak_prefix_branch_safe4, 4) ||
        check_range((gpointer)peak_prefix_branch_sxtw_kill, 16) ||
        check_range((gpointer)peak_prefix_branch_ubfm_kill, 16) ||
        check_range((gpointer)peak_prefix_branch_sxtw_live, 0) ||
        check_range((gpointer)peak_prefix_branch_bfm_live, 0) ||
        check_range((gpointer)peak_prefix_branch_x16_live, 0) ||
        check_range((gpointer)peak_prefix_branch_bl_inside, 4) ||
        check_range((gpointer)peak_prefix_branch_bl_boundary, 16) ||
        check_range((gpointer)peak_prefix_literal_w, 8) ||
        check_range((gpointer)peak_prefix_literal_x, 8) ||
        check_range((gpointer)peak_prefix_literal_sw, 8) ||
        check_range((gpointer)peak_prefix_literal_s, 8) ||
        check_range((gpointer)peak_prefix_literal_d, 8) ||
        check_range((gpointer)peak_prefix_literal_q, 4) ||
        check_range((gpointer)peak_prefix_literal_boundary, 16) ||
        check_range((gpointer)peak_prefix_literal_entry, 0) ||
        check_range((gpointer)peak_prefix_literal_crossing, 0) ||
        check_range((gpointer)peak_prefix_suffix_svc, 16) ||
        check_range((gpointer)peak_prefix_suffix_svc_live, 0))
        return 1;
    if (check_refused(interceptor, listener, (gpointer)peak_prefix_indirect_entry) ||
        check_refused(interceptor, listener, (gpointer)peak_prefix_branch_eret) ||
        check_refused(interceptor, listener, (gpointer)peak_prefix_branch_bc) ||
        check_refused(interceptor, listener, (gpointer)peak_prefix_branch_x16_live))
        return 1;
    if (check_profile(interceptor, listener, peak_prefix_branch_safe, 16) ||
        check_profile(interceptor, listener, peak_prefix_branch_unsafe, 8) ||
        check_profile(interceptor, listener, peak_prefix_branch_ret8, 8) ||
        check_profile(interceptor, listener, peak_prefix_branch_safe4, 4) ||
        check_profile(interceptor, listener, peak_prefix_branch_sxtw_kill, 16) ||
        check_profile(interceptor, listener, peak_prefix_branch_ubfm_kill, 16) ||
        check_profile(interceptor, listener, peak_prefix_suffix_svc, 16))
        return 1;
    if (check_order(interceptor, peak_order_a, peak_order_b, 16, 1, FALSE, FALSE) ||
        check_order(interceptor, peak_order_a, peak_order_b, 16, 1, TRUE, FALSE) ||
        check_order(interceptor, peak_order_back_a, peak_order_back_b, 8, 1, FALSE, FALSE) ||
        check_order(interceptor, peak_order_overlap_a, peak_order_overlap_b, 8, 2, FALSE, FALSE) ||
        check_order(interceptor, peak_order_overlap_a, peak_order_overlap_b, 8, 2, FALSE, TRUE) ||
        check_active_interior_target(interceptor, listener) ||
        check_opaque_replacement(interceptor, listener) ||
        check_extent_cases(interceptor, listener) ||
        check_indirect_dispatch(interceptor, listener) ||
        check_occupied_overlap(interceptor, listener))
        return 1;
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    puts("gum_arm64_prefix_branch_ok");
    return 0;
#endif
}
