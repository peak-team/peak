/* Use the existing genuine Gum target and ordinary send wrapper. This fixture
 * seeds no owned receipt or slot: every owned frame comes from a real STOP. */
#define main peak_existing_controller_fixture_main
#define __wrap_peak_signal_policy_send_thread_signal fe_existing_send_wrapper
#include "test_detach_controller.c"
#undef __wrap_peak_signal_policy_send_thread_signal
#undef main

#if defined(__linux__) && defined(__x86_64__) && defined(PEAK_HAVE_GUM_PEAK_PC_API)
static const char* fe_mode;
static atomic_int fe_worker_tid, fe_spin_flag = 1;
static int fe_worker_send_count, fe_injection_count, fe_effect_count, fe_restore_count;
static int fe_tracking;
static guint fe_length;
static guint8 fe_active[GUM_PEAK_MAX_PROLOGUE_SIZE];
extern const unsigned char peak_fe_spin_begin[], peak_fe_spin_end[];
extern gboolean peak_detach_controller_test_signal_effect(
    long, int, uintptr_t, size_t, uintptr_t*, gboolean*);
extern gboolean peak_detach_controller_test_signal_pc_published(long);
extern size_t peak_detach_controller_test_physical_record_snapshot(
    size_t, unsigned char*, size_t);

__attribute__((naked, noinline, noclone)) static void
fe_spin(atomic_int* flag __attribute__((unused)))
{
    asm volatile(".global peak_fe_spin_begin\n"
                 "peak_fe_spin_begin: cmpb $0,(%rdi)\n"
                 "jne peak_fe_spin_begin\n"
                 ".global peak_fe_spin_end\n"
                 "peak_fe_spin_end: ret\n");
}

static void*
fe_worker(void* unused)
{
    (void)unused;
    atomic_store(&fe_worker_tid, (int)syscall(SYS_gettid));
    fe_spin(&fe_spin_flag);
    return NULL;
}

/* An actual SI_QUEUE packet with no matching owned receipt is delivered to
 * the controller itself. Never inject ownership credit or call the handler. */
static void
fe_contaminate(void)
{
    int before = peak_signal_policy_unexpected_delivery_count();
    int rc = __real_peak_signal_policy_send_thread_signal(
        (pid_t)syscall(SYS_gettid),
        peak_detach_controller_test_signal_backend_signum(), 0);
    if (rc != 0 || peak_signal_policy_unexpected_delivery_count() != before + 1) {
        fprintf(stderr, "FAIL: actual unowned contamination rc=%d before=%d after=%d\n",
                rc, before, peak_signal_policy_unexpected_delivery_count());
        _exit(1);
    }
    fe_injection_count++;
    fprintf(stderr, "forward-fixture actual_unowned_packet=1 injections=%d\n",
            fe_injection_count);
}

int
__wrap_peak_signal_policy_send_thread_signal(pid_t tid, int signo,
                                            unsigned long cookie)
{
    int rc = fe_existing_send_wrapper(tid, signo, cookie);
    int saved = errno;
    if (fe_tracking && tid == atomic_load(&fe_worker_tid) && rc == 0) {
        fe_worker_send_count++;
        fprintf(stderr, "forward-fixture actual_owned_send=1 tid=%d S=%d cookie=0x%lx rc=0\n",
                (int)tid, signo, cookie);
        if (strcmp(fe_mode, "recoverable") == 0 && fe_injection_count == 0)
            fe_contaminate();
    }
    errno = saved;
    return rc;
}

void
peak_test_before_signal_forward_effect(void)
{
    if (strcmp(fe_mode, "before") == 0) {
        fprintf(stderr, "forward-fixture before_effect_boundary=1\n");
        fe_contaminate();
    }
}

void
peak_test_after_signal_forward_effect(int action)
{
    fe_effect_count++;
    int observed = 0;
    if (action == PEAK_DETACH_HELPER_INSTRUCTION_SET_PC)
        observed = peak_detach_controller_test_signal_pc_published(
            atomic_load(&fe_worker_tid));
    else if (action == PEAK_DETACH_HELPER_INSTRUCTION_SINGLE_STEP_OUT_OF_RANGE)
        observed = *peak_fe_spin_end == 0xcc;
    else if (action == PEAK_DETACH_HELPER_INSTRUCTION_WRITE_MEMORY)
        observed = fe_length > 0 && memcmp(fe_active,
            (void*)strict_helper_target, fe_length) != 0;
    if (!observed) {
        fprintf(stderr, "FAIL: actual forward effect missing action=%d\n", action);
        _exit(1);
    }
    fprintf(stderr, "forward-fixture action=%d actual_effect_observed=1\n", action);
    if (strcmp(fe_mode, "after") == 0 || strcmp(fe_mode, "pc-fatal") == 0 ||
        strcmp(fe_mode, "temp-fatal") == 0)
        fe_contaminate();
    else if (action == PEAK_DETACH_HELPER_INSTRUCTION_SINGLE_STEP_OUT_OF_RANGE)
        atomic_store(&fe_spin_flag, 0);
}

int
peak_test_after_signal_memory_write(uint64_t address, size_t size)
{
    if (strcmp(fe_mode, "temp-error") == 0 &&
        address == (uint64_t)(uintptr_t)peak_fe_spin_end) {
        if (size != 1 || *peak_fe_spin_end != 0xcc) {
            fprintf(stderr, "FAIL: injected error must follow actual trap write\n");
            _exit(1);
        }
        fprintf(stderr, "forward-fixture actual_trap_before_RX_failure=1\n");
        return EACCES;
    }
    return 0;
}

void
peak_test_after_signal_temp_restore(void)
{
    if (*peak_fe_spin_end != 0xc3) {
        fprintf(stderr, "FAIL: actual temporary trap not restored\n");
        _exit(1);
    }
    fe_restore_count++;
    fprintf(stderr, "forward-fixture actual_exit_byte_restored=1\n");
}

static int
fe_run(int batch)
{
    pthread_t worker;
    int started = 0;
    gboolean prepared = FALSE;
    int custom = strncmp(fe_mode, "pc-", 3) == 0 ||
        strncmp(fe_mode, "temp-", 5) == 0;
    guint8 original[GUM_PEAK_MAX_PROLOGUE_SIZE], after[GUM_PEAK_MAX_PROLOGUE_SIZE];
    unsigned char record_before[128], record_after[128];
    GumPeakPcDiagnostics diagnostics = {0}, diagnostics_after = {0};
    gum_init_embedded();
    GumInterceptor* interceptor = gum_interceptor_obtain();
    GumInvocationListener* listener = gum_make_call_listener(preflight_on_enter, NULL, NULL, NULL);
    memcpy(original, (void*)strict_helper_target, sizeof(original));
    check_int("forward fixture real Gum attach", gum_interceptor_attach(
        interceptor, (gpointer)strict_helper_target, listener, NULL), GUM_ATTACH_OK);
    gum_interceptor_flush(interceptor);
    check_true("forward fixture diagnostics", gum_interceptor_peak_get_pc_diagnostics(
        interceptor, (gpointer)strict_helper_target, listener, &diagnostics));
    fe_length = diagnostics.overwritten_prologue_len;
    if (failures || fe_length == 0 || fe_length > sizeof(fe_active)) {
        failures++;
        goto cleanup;
    }
    memcpy(fe_active, (void*)strict_helper_target, fe_length);
    check_true("forward fixture initially hooked", memcmp(fe_active, original, fe_length) != 0);
    peak_detach_controller_note_thread_creation_gate_installed(TRUE);
    check_true("forward fixture reserved signal", peak_detach_controller_test_signal_backend_signum() > 0);
    if (pthread_create(&worker, NULL, fe_worker, NULL) != 0) { failures++; goto cleanup; }
    started = 1;
    double deadline = preflight_now() + 2.0;
    while (atomic_load(&fe_worker_tid) == 0 && preflight_now() < deadline) usleep(1000);
    check_true("forward worker ready", atomic_load(&fe_worker_tid) > 0);
    usleep(1000);
    if (failures) goto cleanup;
    PeakDetachRequest request = {
        .hook_id = 133, .symbol_name = "strict_helper_target",
        .function_address = (gpointer)strict_helper_target, .interceptor = interceptor,
        .listener = listener,
        /* ATTACH grants metadata mutation permission with no evacuation write.
         * DETACH drives the real WRITE_MEMORY path for before/after tests. */
        .operation = custom || strcmp(fe_mode, "permission") == 0
            ? PEAK_DETACH_OPERATION_ATTACH : PEAK_DETACH_OPERATION_DETACH
    };
    size_t record_size = peak_detach_controller_test_physical_record_snapshot(
        request.hook_id, record_before, sizeof(record_before));
    check_true("forward fixture record snapshot", record_size > 0);
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    PeakDetachBatchResult result = {0};
    size_t count = SIZE_MAX;
    fe_tracking = 1;
    prepared = batch ? peak_detach_controller_prepare_hook_mutation_batch(
        &request, 1, &result, &count, &status)
        : peak_detach_controller_prepare_hook_mutation(&request, &status);
    fe_tracking = 0;
    fprintf(stderr, "forward-fixture mode=%s batch=%d prepared=%d status=%s actual_owned_sends=%d effects=%d\n",
            fe_mode, batch, prepared, peak_detach_controller_status_string(status),
            fe_worker_send_count, fe_effect_count);
    check_true("forward fixture normal owned receipt-backed send", fe_worker_send_count == 1);
    if (strcmp(fe_mode, "recoverable") == 0) {
        check_true("pre-effect contamination refuses permission", !prepared &&
            status == PEAK_DETACH_STATUS_UNSUPPORTED && fe_injection_count == 1 && fe_effect_count == 0);
        if (batch) check_true("pre-effect batch flags cleared", count == 0 &&
            !result.prepared && !result.uses_physical_patch);
        memcpy(after, (void*)strict_helper_target, fe_length);
        check_true("refused prepare preserves actual Gum bytes", memcmp(fe_active, after, fe_length) == 0);
        check_true("refused prepare preserves Gum diagnostics", gum_interceptor_peak_get_pc_diagnostics(
            interceptor, (gpointer)strict_helper_target, listener, &diagnostics_after) &&
            memcmp(&diagnostics, &diagnostics_after, sizeof(diagnostics)) == 0);
        check_true("refused prepare preserves controller physical record",
            peak_detach_controller_test_physical_record_snapshot(request.hook_id,
                record_after, sizeof(record_after)) == record_size &&
            memcmp(record_before, record_after, record_size) == 0);
        int calls = atomic_load(&preflight_callback_count);
        strict_helper_target();
        check_int("refused prepare preserves actual listener invocation",
            atomic_load(&preflight_callback_count) - calls, 1);
        check_true("refused prepare releases held peers and creation gate",
            !peak_detach_controller_threads_are_held() &&
            !peak_detach_controller_current_mutation_uses_physical_patch() &&
            peak_detach_controller_test_thread_creation_gate_epoch() == 0);
    } else {
        check_true("forward fixture prepared mutation", prepared &&
            status == PEAK_DETACH_STATUS_SAFE);
        if (!prepared) goto cleanup;
        if (strcmp(fe_mode, "permission") == 0) {
            check_int("permission-only transaction had no forward code effect", fe_effect_count, 0);
            fprintf(stderr, "forward-fixture permission_granted_without_code_effect=1\n");
            fe_contaminate();
        } else if (custom) {
            int action = strncmp(fe_mode, "pc-", 3) == 0
                ? PEAK_DETACH_HELPER_INSTRUCTION_SET_PC
                : PEAK_DETACH_HELPER_INSTRUCTION_SINGLE_STEP_OUT_OF_RANGE;
            uintptr_t pc = 0;
            gboolean published = FALSE;
            gboolean ok = peak_detach_controller_test_signal_effect(
                atomic_load(&fe_worker_tid), action, (uintptr_t)peak_fe_spin_begin,
                (size_t)(peak_fe_spin_end - peak_fe_spin_begin), &pc, &published);
            check_true("actual owned-worker effect completed", ok && pc >= (uintptr_t)peak_fe_spin_begin &&
                pc < (uintptr_t)peak_fe_spin_end && fe_effect_count == 1);
            if (action == PEAK_DETACH_HELPER_INSTRUCTION_SET_PC)
                check_true("actual owned-worker identity PC published", published);
            else check_int("temporary trap restoration observed", fe_restore_count, 1);
        }
        check_true("forward fixture finish", peak_detach_controller_finish_hook_mutation(&request, &status));
        prepared = FALSE;
        if (strcmp(fe_mode, "pc-positive") != 0 && strcmp(fe_mode, "temp-positive") != 0) {
            fprintf(stderr, "FAIL: expected fatal guard returned mode=%s\n", fe_mode);
            failures++;
        }
    }
cleanup:
    /* If an expected fatal path returns, release safely for fixture cleanup;
     * its explicit FAIL remains visible to the fatal harness. */
    if (prepared) peak_detach_controller_finish_hook_mutation(NULL, NULL);
    atomic_store(&fe_spin_flag, 0);
    if (started) pthread_join(worker, NULL);
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_flush(interceptor);
    g_object_unref(listener); g_object_unref(interceptor); gum_deinit_embedded();
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
#endif

int main(int argc, char** argv)
{
#if defined(__linux__) && defined(__x86_64__) && defined(PEAK_HAVE_GUM_PEAK_PC_API)
    if (argc != 2) return EXIT_FAILURE;
    fe_mode = argv[1];
    return fe_run(g_getenv("PEAK_TEST_FORWARD_BATCH") != NULL);
#else
    (void)argc; (void)argv;
    return 77;
#endif
}
