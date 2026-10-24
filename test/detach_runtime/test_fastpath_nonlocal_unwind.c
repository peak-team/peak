#define _GNU_SOURCE

#include <dlfcn.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "general_listener.h"

#include "frida-gum.h"

static _Thread_local jmp_buf unwind_target;
static _Thread_local jmp_buf recursive_unwind_target;
static _Thread_local jmp_buf escape_target;
static _Thread_local jmp_buf generic_escape_target;
static atomic_ulong target_sink;
static atomic_ulong mixed_listener_enters;
static atomic_ulong mixed_listener_leaves;
static atomic_ulong unrelated_listener_enters;
static atomic_ulong unrelated_listener_leaves;

__attribute__((noinline, noclone, used, externally_visible,
               visibility("default")))
uintptr_t
peak_fastpath_unwind_inner(uintptr_t unwind)
{
    atomic_fetch_add_explicit(&target_sink, 1, memory_order_relaxed);
    if (unwind == 1) {
        longjmp(unwind_target, 1);
    }
    if (unwind == 2) {
        longjmp(escape_target, 1);
    }
    return unwind + 1;
}

__attribute__((noinline, noclone, used, externally_visible,
               visibility("default")))
uintptr_t
peak_fastpath_unwind_outer(uintptr_t unwind)
{
    uintptr_t result = 0;

    if (setjmp(unwind_target) == 0) {
        result = peak_fastpath_unwind_inner(unwind);
    } else {
        result = 2;
    }
    atomic_fetch_add_explicit(&target_sink, result, memory_order_relaxed);
    return result;
}

__attribute__((noinline, noclone, used, externally_visible,
               visibility("default")))
uintptr_t
peak_fastpath_unwind_escape_outer(uintptr_t unwind)
{
    return peak_fastpath_unwind_inner(unwind) + 1;
}

__attribute__((noinline, noclone, used, externally_visible,
               visibility("default")))
uintptr_t
peak_fastpath_unwind_generic_escape(uintptr_t unwind)
{
    atomic_fetch_add_explicit(&target_sink, 1, memory_order_relaxed);
    if (unwind != 0) {
        longjmp(generic_escape_target, 1);
    }
    return 3;
}

__attribute__((noinline, noclone, used, externally_visible,
               visibility("default")))
uintptr_t
peak_fastpath_unwind_unrelated_bridge(void)
{
    return peak_fastpath_unwind_escape_outer(0) + 1;
}

__attribute__((noinline, noclone))
static void
escape_all_from_deeper_frame(void)
{
    uintptr_t result = peak_fastpath_unwind_escape_outer(2);
    atomic_fetch_add_explicit(&target_sink, result, memory_order_relaxed);
}

__attribute__((noinline, noclone))
static void
escape_generic_from_deeper_frame(void)
{
    uintptr_t result = peak_fastpath_unwind_generic_escape(1);
    atomic_fetch_add_explicit(&target_sink, result, memory_order_relaxed);
}

__attribute__((noinline, noclone, used, externally_visible,
               visibility("default")))
uintptr_t
peak_fastpath_unwind_recursive(uintptr_t depth, uintptr_t unwind)
{
    atomic_fetch_add_explicit(&target_sink, depth + 1, memory_order_relaxed);

    if (depth == 2 && unwind != 0 &&
        setjmp(recursive_unwind_target) != 0) {
        return 7;
    }
    if (depth == 0) {
        if (unwind != 0) {
            longjmp(recursive_unwind_target, 1);
        }
        return 1;
    }
    return peak_fastpath_unwind_recursive(depth - 1, unwind) + 1;
}

static void*
required_symbol(const char* name)
{
    void* symbol = dlsym(RTLD_DEFAULT, name);
    if (symbol == NULL) {
        fprintf(stderr, "missing symbol %s: %s\n", name, dlerror());
    }
    return symbol;
}

static void
mixed_listener_on_enter(GumInvocationContext* context, gpointer user_data)
{
    (void)context;
    (void)user_data;
    atomic_fetch_add_explicit(&mixed_listener_enters, 1,
                              memory_order_relaxed);
}

static void
mixed_listener_on_leave(GumInvocationContext* context, gpointer user_data)
{
    (void)context;
    (void)user_data;
    atomic_fetch_add_explicit(&mixed_listener_leaves, 1,
                              memory_order_relaxed);
}

static void
unrelated_listener_on_enter(GumInvocationContext* context,
                            gpointer user_data)
{
    (void)context;
    (void)user_data;
    atomic_fetch_add_explicit(&unrelated_listener_enters, 1,
                              memory_order_relaxed);
}

static void
unrelated_listener_on_leave(GumInvocationContext* context,
                            gpointer user_data)
{
    (void)context;
    (void)user_data;
    atomic_fetch_add_explicit(&unrelated_listener_leaves, 1,
                              memory_order_relaxed);
}

static GumInvocationListener*
install_mixed_listener(void)
{
    typedef GumInvocationListener* (*MakeListenerFunc)(
        GumInvocationCallback,
        GumInvocationCallback,
        gpointer,
        GDestroyNotify);
    typedef GumAttachReturn (*AttachFunc)(
        GumInterceptor*,
        gpointer,
        GumInvocationListener*,
        const GumAttachOptions*);

    GumInterceptor** interceptor =
        (GumInterceptor**)required_symbol("interceptor");
    MakeListenerFunc make_listener =
        (MakeListenerFunc)required_symbol("gum_make_call_listener");
    AttachFunc attach =
        (AttachFunc)required_symbol("gum_interceptor_attach");
    GumInvocationListener* listener;
    GumInvocationListener* unrelated_listener;

    if (interceptor == NULL || *interceptor == NULL ||
        make_listener == NULL || attach == NULL) {
        return NULL;
    }
    listener = make_listener(mixed_listener_on_enter,
                             mixed_listener_on_leave,
                             NULL,
                             NULL);
    unrelated_listener = make_listener(unrelated_listener_on_enter,
                                       unrelated_listener_on_leave,
                                       NULL,
                                       NULL);
    if (listener == NULL || unrelated_listener == NULL ||
        attach(*interceptor,
               (gpointer)peak_fastpath_unwind_inner,
               listener,
               NULL) != GUM_ATTACH_OK ||
        attach(*interceptor,
               (gpointer)peak_fastpath_unwind_generic_escape,
               listener,
               NULL) != GUM_ATTACH_OK ||
        attach(*interceptor,
               (gpointer)peak_fastpath_unwind_unrelated_bridge,
               unrelated_listener,
               NULL) != GUM_ATTACH_OK) {
        fputs("failed to install mixed generic listener\n", stderr);
        return NULL;
    }
    return listener;
}

static int
require_fast_dispatch(void)
{
    typedef gboolean (*GetDiagnosticsFunc)(
        GumInterceptor*,
        gpointer,
        GumInvocationListener*,
        GumPeakPcDiagnostics*);

    GumInterceptor** interceptor =
        (GumInterceptor**)required_symbol("interceptor");
    GumInvocationListener*** listeners =
        (GumInvocationListener***)required_symbol("array_listener");
    gpointer** addresses =
        (gpointer**)required_symbol("hook_address");
    GetDiagnosticsFunc get_diagnostics =
        (GetDiagnosticsFunc)required_symbol(
            "gum_interceptor_peak_get_pc_diagnostics");
    gpointer expected_addresses[] = {
        required_symbol("peak_fastpath_unwind_outer"),
        required_symbol("peak_fastpath_unwind_inner"),
        required_symbol("peak_fastpath_unwind_recursive"),
        required_symbol("peak_fastpath_unwind_escape_outer"),
        required_symbol("peak_fastpath_unwind_generic_escape"),
    };

    if (interceptor == NULL || listeners == NULL || addresses == NULL ||
        get_diagnostics == NULL || *interceptor == NULL ||
        *listeners == NULL || *addresses == NULL) {
        return 0;
    }

    for (size_t i = 0;
         i < sizeof(expected_addresses) / sizeof(expected_addresses[0]);
         i++) {
        GumPeakPcDiagnostics diagnostics;
        if (expected_addresses[i] == NULL ||
            (*addresses)[i] != expected_addresses[i] ||
            (*listeners)[i] == NULL ||
            !get_diagnostics(*interceptor,
                             (*addresses)[i],
                             (*listeners)[i],
                             &diagnostics) ||
            diagnostics.fast_overlay_dispatch_start == NULL ||
            diagnostics.fast_overlay_dispatch_size == 0 ||
            diagnostics.fast_listener_dispatch_start == NULL ||
            diagnostics.fast_listener_dispatch_size == 0) {
            fprintf(stderr,
                    "fastpath dispatch setup mismatch at target %zu\n",
                    i);
            return 0;
        }
    }
    return 1;
}

#if defined(__linux__) && defined(__x86_64__)
static _Thread_local jmp_buf extent_unwind_target;

__attribute__((noreturn, noinline, visibility("default")))
void
peak_extent_reattach_escape(void)
{
    longjmp(extent_unwind_target, 1);
}

extern uintptr_t peak_extent_reattach_target(uintptr_t unwind);
/*
 * The ELF and CFI extents both stop at the noreturn call.  Invalid long-mode
 * instructions outside that extent make an absent boundary fail reliably,
 * rather than accidentally finding a return in the following function.
 */
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl peak_extent_reattach_target\n"
    ".type peak_extent_reattach_target,@function\n"
    "peak_extent_reattach_target:\n"
    ".cfi_startproc\n"
    "test %rdi,%rdi\n"
    "jne 1f\n"
    "mov $3,%eax\n"
    "ret\n"
    "1: sub $8,%rsp\n"
    ".cfi_def_cfa_offset 16\n"
    "call peak_extent_reattach_escape\n"
    ".cfi_endproc\n"
    ".size peak_extent_reattach_target,.-peak_extent_reattach_target\n"
    ".rept 64\n"
    ".byte 0x06\n"
    ".endr\n");

static int
exercise_extent_unwind(void)
{
    if (setjmp(extent_unwind_target) == 0) {
        peak_extent_reattach_target(1);
        return 0;
    }
    return peak_extent_reattach_target(0) == 3;
}

static int
extent_entry_is_original(void)
{
    static const unsigned char original[] = {
        0x48, 0x85, 0xff, 0x75, 0x06, 0xb8, 0x03, 0x00,
        0x00, 0x00, 0xc3, 0x48, 0x83, 0xec, 0x08, 0xe8
    };
    const volatile unsigned char* entry =
        (const volatile unsigned char*)peak_extent_reattach_target;
    for (size_t i = 0; i < sizeof(original); i++) {
        if (entry[i] != original[i])
            return 0;
    }
    return 1;
}

static int
run_extent_gum_rebuild_check(void)
{
    typedef void (*TransactionFunc)(GumInterceptor*);
    typedef void (*DetachFunc)(GumInterceptor*, GumInvocationListener*);
    typedef gboolean (*FlushFunc)(GumInterceptor*);
    typedef gboolean (*PrepareFunc)(GumInterceptor*, gpointer,
                                   GumInvocationListener*);
    typedef GumAttachReturn (*AttachFunc)(GumInterceptor*, gpointer,
                                         GumInvocationListener*, const void*);
    typedef gboolean (*RelocateFunc)(gpointer, guint,
                                     GumRelocationScenario, guint*);
    typedef gulong (*CountFunc)(size_t);
    GumInterceptor** owner = (GumInterceptor**)required_symbol("interceptor");
    GumInvocationListener*** listeners =
        (GumInvocationListener***)required_symbol("array_listener");
    TransactionFunc begin = (TransactionFunc)required_symbol(
        "gum_interceptor_begin_transaction");
    TransactionFunc end = (TransactionFunc)required_symbol(
        "gum_interceptor_end_transaction");
    DetachFunc detach = (DetachFunc)required_symbol("gum_interceptor_detach");
    FlushFunc flush = (FlushFunc)required_symbol("gum_interceptor_flush");
    PrepareFunc prepare = (PrepareFunc)required_symbol(
        "gum_interceptor_peak_prepare_fast_detach");
    AttachFunc attach = (AttachFunc)required_symbol(
        "peak_general_listener_test_gum_attach_target");
    RelocateFunc relocate = (RelocateFunc)required_symbol(
        "gum_x86_relocator_can_relocate");
    CountFunc count = (CountFunc)required_symbol(
        "peak_general_listener_test_call_count");
    if (owner == NULL || *owner == NULL || listeners == NULL ||
        *listeners == NULL || (*listeners)[0] == NULL || begin == NULL ||
        end == NULL || detach == NULL || flush == NULL || prepare == NULL ||
        attach == NULL || relocate == NULL || count == NULL)
        return 1;
    if (peak_extent_reattach_target(0) != 3 ||
        peak_extent_reattach_target(0) != 3 || count(0) != 2 ||
        extent_entry_is_original())
        return 1;
    for (unsigned int round = 0; round < 2; round++) {
        gulong before = count(0);
        if (!prepare(*owner, (gpointer)peak_extent_reattach_target,
                     (*listeners)[0]))
            return 1;
        begin(*owner);
        detach(*owner, (*listeners)[0]);
        end(*owner);
        gboolean flushed = FALSE;
        for (unsigned int attempt = 0; attempt < 2000; attempt++) {
            if ((flushed = flush(*owner)))
                break;
            usleep(1000);
        }
        if (!flushed || !extent_entry_is_original() ||
            !exercise_extent_unwind() || count(0) != before) {
            fprintf(stderr, "direct Gum detach mismatch round=%u\n", round);
            return 1;
        }
        for (guint minimum = 5; minimum <= 16; minimum += 11) {
            guint maximum = 999;
            if (relocate((gpointer)peak_extent_reattach_target, minimum,
                         GUM_SCENARIO_ONLINE, &maximum) || maximum != 0) {
                fputs("extent target unexpectedly relocates without metadata\n",
                      stderr);
                return 1;
            }
        }
        begin(*owner);
        GumAttachReturn status = attach(*owner,
            (gpointer)peak_extent_reattach_target, (*listeners)[0], NULL);
        end(*owner);
        if (status != GUM_ATTACH_OK || extent_entry_is_original() ||
            (round == 0 ? peak_extent_reattach_target(0) != 3
                        : !exercise_extent_unwind()) ||
            count(0) != before + (round == 0 ? 1 : 2)) {
            fprintf(stderr, "cached Gum rebuild failed round=%u status=%d\n",
                    round, status);
            return 1;
        }
    }
    puts("extent_cached_gum_rebuild_ok");
    return 0;
}

static int
run_extent_reattach_check(void)
{
    typedef gboolean (*RequestFunc)(size_t);
    typedef gboolean (*DrainFunc)(unsigned int);
    typedef PeakHookState (*StateFunc)(size_t);
    typedef gulong (*CountFunc)(size_t);
    RequestFunc detach = (RequestFunc)required_symbol(
        "peak_general_listener_request_detach");
    RequestFunc reattach = (RequestFunc)required_symbol(
        "peak_general_listener_request_reattach");
    DrainFunc drain = (DrainFunc)required_symbol(
        "peak_general_listener_controller_drain");
    StateFunc state = (StateFunc)required_symbol(
        "peak_general_listener_hook_state");
    CountFunc count = (CountFunc)required_symbol(
        "peak_general_listener_test_call_count");
    const size_t hook_id = 0;

    if (detach == NULL || reattach == NULL || drain == NULL ||
        state == NULL || count == NULL)
        return 1;
    if (peak_extent_reattach_target(0) != 3 ||
        peak_extent_reattach_target(0) != 3 ||
        state(hook_id) != PEAK_HOOK_ATTACHED || count(hook_id) != 2)
        return 1;

    for (unsigned int round = 0; round < 2; round++) {
        gulong before = count(hook_id);
        if (!detach(hook_id) || !drain(2000) ||
            state(hook_id) != PEAK_HOOK_DETACHED ||
            !exercise_extent_unwind() ||
            count(hook_id) != before) {
            fprintf(stderr, "extent detach/unwind mismatch round=%u\n", round);
            return 1;
        }
        if (!reattach(hook_id) || !drain(2000) ||
            state(hook_id) != PEAK_HOOK_ATTACHED ||
            (round == 0 ? peak_extent_reattach_target(0) != 3
                        : !exercise_extent_unwind()) ||
            count(hook_id) != before + (round == 0 ? 1 : 2)) {
            fprintf(stderr, "extent reattach/unwind mismatch round=%u\n", round);
            return 1;
        }
    }
    puts("extent_reattach_unwind_ok");
    return 0;
}

#endif

int
main(int argc, char** argv)
{
#if defined(__linux__) && defined(__x86_64__)
    if (argc == 2 && strcmp(argv[1], "--extent-reattach-check") == 0)
        return run_extent_reattach_check();
    if (argc == 2 && strcmp(argv[1], "--extent-gum-rebuild-check") == 0)
        return run_extent_gum_rebuild_check();
#else
    if (argc == 2 && strcmp(argv[1], "--extent-reattach-check") == 0) {
        fputs("extent reattach fixture requires Linux x86-64\n", stderr);
        return 2;
    }
#endif
    typedef gulong (*StackLevelFunc)(void);

    /*
     * The first call initializes PEAK's per-thread state through Gum's
     * complete listener path.  The second call verifies the ordinary direct
     * path before an inner direct invocation is bypassed by longjmp.
     */
    if (peak_fastpath_unwind_outer(0) != 1 ||
        peak_fastpath_unwind_outer(0) != 1 ||
        peak_fastpath_unwind_recursive(2, 0) != 3 ||
        peak_fastpath_unwind_recursive(2, 0) != 3 ||
        peak_fastpath_unwind_escape_outer(0) != 2 ||
        peak_fastpath_unwind_escape_outer(0) != 2 ||
        peak_fastpath_unwind_generic_escape(0) != 3 ||
        peak_fastpath_unwind_generic_escape(0) != 3) {
        fputs("fastpath unwind warm-up failed\n", stderr);
        return 1;
    }
    if (!require_fast_dispatch()) {
        return 1;
    }
    StackLevelFunc stack_level =
        (StackLevelFunc)required_symbol(
            "peak_general_listener_test_current_invocation_level");
    if (stack_level == NULL) {
        return 1;
    }
    /*
     * A second listener disables direct dispatch for the inner target while
     * the outer target stays direct. Each longjmp therefore bypasses a Gum
     * generic frame on its way back to a PEAK-only direct frame.
     */
    if (install_mixed_listener() == NULL) {
        return 1;
    }

    for (unsigned int iteration = 0; iteration < 1000; iteration++) {
        if (peak_fastpath_unwind_outer(1) != 2 ||
            peak_fastpath_unwind_outer(0) != 1 ||
            peak_fastpath_unwind_recursive(2, 1) != 7 ||
            peak_fastpath_unwind_recursive(2, 0) != 3) {
            fprintf(stderr,
                    "fastpath unwind result mismatch at iteration %u\n",
                    iteration);
            return 1;
        }
    }

    /*
     * Escape both the generic inner and the direct outer. The next direct
     * entry must clean both stale PEAK entries and restore Gum's generic
     * invocation stack before starting its own frame.
     */
    if (setjmp(escape_target) == 0) {
        escape_all_from_deeper_frame();
        fputs("escape-all unwind unexpectedly returned\n", stderr);
        return 1;
    }
    if (peak_fastpath_unwind_escape_outer(0) != 2) {
        fputs("escape-all recovery direct call failed\n", stderr);
        return 1;
    }

    /*
     * Also escape a generic-only PEAK frame. Its recorded stack address and
     * Gum depth must be enough for the next direct entry to clear it without
     * relying on a stale direct frame as an anchor.
     */
    if (setjmp(generic_escape_target) == 0) {
        escape_generic_from_deeper_frame();
        fputs("generic-only unwind unexpectedly returned\n", stderr);
        return 1;
    }
    if (peak_fastpath_unwind_unrelated_bridge() != 3) {
        fputs("generic-only unrelated recovery call failed\n", stderr);
        return 1;
    }

    if (atomic_load_explicit(&target_sink, memory_order_relaxed) == 0) {
        fputs("fastpath unwind target was not executed\n", stderr);
        return 1;
    }
    if (atomic_load_explicit(&mixed_listener_enters,
                             memory_order_relaxed) != 2004 ||
        atomic_load_explicit(&mixed_listener_leaves,
                             memory_order_relaxed) != 1002) {
        fprintf(stderr,
                "mixed listener count mismatch: enters=%lu leaves=%lu\n",
                atomic_load_explicit(&mixed_listener_enters,
                                     memory_order_relaxed),
                atomic_load_explicit(&mixed_listener_leaves,
                                     memory_order_relaxed));
        return 1;
    }
    if (atomic_load_explicit(&unrelated_listener_enters,
                             memory_order_relaxed) != 1 ||
        atomic_load_explicit(&unrelated_listener_leaves,
                             memory_order_relaxed) != 1) {
        fprintf(stderr,
                "unrelated listener count mismatch: enters=%lu leaves=%lu\n",
                atomic_load_explicit(&unrelated_listener_enters,
                                     memory_order_relaxed),
                atomic_load_explicit(&unrelated_listener_leaves,
                                     memory_order_relaxed));
        return 1;
    }
    if (stack_level() != 0) {
        fprintf(stderr,
                "PEAK invocation stack was not fully reaped: level=%lu\n",
                stack_level());
        return 1;
    }

    puts("fastpath_nonlocal_unwind_ok");
    return 0;
}
