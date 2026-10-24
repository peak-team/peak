#include "frida-gum.h"
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
extern int peak_prefix_hpsort(int);
extern int peak_prefix_linear(int);
extern int peak_prefix_requested_w9(int);
extern int peak_prefix_branch4(int);
extern int peak_prefix_known_backedge(int);
extern int peak_prefix_ret8(int);
extern int peak_prefix_branch_entry(int);
extern int peak_prefix_indirect_entry(int);
extern int peak_prefix_trap_entry(int);
extern int peak_prefix_trap4(int);
extern int peak_prefix_trap_suffix(int);
extern int peak_prefix_long_4100(int);
extern int peak_prefix_long_8200(int);
extern int peak_prefix_literal_self(int);
extern int peak_prefix_literal_cross(int);
extern int peak_prefix_literal_boundary(int);
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
static int check_requested_scratch(gpointer target, guint expected)
{
    guint maximum = 0;
    arm64_reg scratch = ARM64_REG_X9;
    gboolean result = gum_arm64_relocator_can_relocate(target, 16,
        GUM_SCENARIO_ONLINE, GUM_RELOCATION_CHECKED, &maximum, &scratch);
    if (result != (expected == 16) || maximum != expected ||
        scratch != (expected != 0 ? ARM64_REG_X9 : ARM64_REG_INVALID)) {
        fprintf(stderr, "requested scratch mismatch: result=%d max=%u scratch=%d\n",
            result, maximum, scratch);
        return 1;
    }
    return 0;
}
static int check_refused(GumInterceptor* interceptor,
    GumInvocationListener* listener, gpointer target)
{
    guint8 before[16];
    snapshot(target, before);
    if (gum_interceptor_peak_attach_exact(interceptor, target, listener, NULL) ==
            GUM_ATTACH_OK || !unchanged(target, before)) {
        fputs("unsupported entry was attached or changed\n", stderr);
        return 1;
    }
    return 0;
}
static int check_profile(GumInterceptor* interceptor,
    GumInvocationListener* listener, int (*target)(int), guint expected)
{
    guint length = 0;
    guint8 active[GUM_PEAK_MAX_PROLOGUE_SIZE];
    guint8 original[GUM_PEAK_MAX_PROLOGUE_SIZE];
    int zero = target(0), seven = target(7);
    calls = 0;
    if (gum_interceptor_peak_attach_exact(interceptor, (gpointer)target,
            listener, NULL) != GUM_ATTACH_OK ||
        !gum_interceptor_peak_get_function_patch(interceptor, (gpointer)target,
            listener, active, original, &length) || length != expected ||
        target(0) != zero || target(7) != seven || calls != 2) {
        fprintf(stderr, "prefix profile failed: length=%u expected=%u calls=%u\n",
            length, expected, calls);
        return 1;
    }
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    if (target(0) != zero || target(7) != seven || calls != 2) return 1;
    return 0;
}
static int check_mapping_end(GumInterceptor* interceptor,
    GumInvocationListener* listener, guint size)
{
    long page = sysconf(_SC_PAGESIZE);
    guint32* code;
    guint8* mapping;
    int result;
    if (page <= 0) return 1;
    mapping = mmap(NULL, (gsize)page * 2, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return 1;
    code = (guint32*)(mapping + page - size);
    if (size == 8) code[0] = 0x528000e0; /* mov w0,#7 */
    code[size / 4 - 1] = 0xd65f03c0; /* ret */
    gum_clear_cache(code, size);
    if (mprotect(mapping, page, PROT_READ | PROT_EXEC) != 0 ||
        mprotect(mapping + page, page, PROT_NONE) != 0) {
        munmap(mapping, (gsize)page * 2);
        return 1;
    }
    result = check_range((gpointer)code, size) ||
        check_profile(interceptor, listener, (int (*)(int))code, size);
    munmap(mapping, (gsize)page * 2);
    return result;
}
int main(void)
{
#if !defined(PEAK_GUM_ENTRY_RANGE_CHECKS)
    puts("caller-provided Gum lacks entry-range checks");
    return 77;
#else
    GumInterceptor* interceptor;
    GumInvocationListener* listener;
    if (gum_interceptor_peak_entry_range_revision() != 2) return 1;
    gum_init_embedded();
    interceptor = gum_interceptor_obtain();
    listener = gum_make_call_listener(count, NULL, NULL, NULL);
    if (check_range((gpointer)peak_prefix_hpsort, 8) ||
        check_range((gpointer)peak_prefix_linear, 16) ||
        check_requested_scratch((gpointer)peak_prefix_linear, 16) ||
        check_requested_scratch((gpointer)peak_prefix_hpsort, 8) ||
        check_requested_scratch((gpointer)peak_prefix_requested_w9, 0) ||
        check_range((gpointer)peak_prefix_branch4, 4) ||
        check_range((gpointer)peak_prefix_known_backedge, 4) ||
        check_range((gpointer)peak_prefix_ret8, 8) ||
        check_range((gpointer)peak_prefix_branch_entry, 0) ||
        check_range((gpointer)peak_prefix_indirect_entry, 0) ||
        check_range((gpointer)peak_prefix_trap_entry, 0) ||
        check_range((gpointer)peak_prefix_trap4, 4) ||
        check_range((gpointer)peak_prefix_trap_suffix, 16) ||
        check_range((gpointer)peak_prefix_long_4100, 16) ||
        check_range((gpointer)peak_prefix_long_8200, 16) ||
        check_range((gpointer)peak_prefix_literal_self, 0) ||
        check_range((gpointer)peak_prefix_literal_cross, 0) ||
        check_range((gpointer)peak_prefix_literal_boundary, 16)) return 1;
    if (check_refused(interceptor, listener, (gpointer)peak_prefix_branch_entry) ||
        check_refused(interceptor, listener, (gpointer)peak_prefix_indirect_entry) ||
        check_refused(interceptor, listener, (gpointer)peak_prefix_trap_entry) ||
        check_refused(interceptor, listener, (gpointer)peak_prefix_literal_self)) return 1;
    /* Execute only locally supported fixtures. In particular, the known
     * backedge loop and trap-only classifiers above are never called. */
    if (check_profile(interceptor, listener, peak_prefix_hpsort, 8) ||
        check_profile(interceptor, listener, peak_prefix_linear, 16) ||
        check_profile(interceptor, listener, peak_prefix_branch4, 4) ||
        check_profile(interceptor, listener, peak_prefix_ret8, 8) ||
        check_profile(interceptor, listener, peak_prefix_long_4100, 16) ||
        check_profile(interceptor, listener, peak_prefix_long_8200, 16) ||
        check_mapping_end(interceptor, listener, 8) ||
        check_mapping_end(interceptor, listener, 4)) return 1;
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    puts("gum_arm64_prefix_branch_ok");
    return 0;
#endif
}
