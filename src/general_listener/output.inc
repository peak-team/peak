#ifdef PEAK_ENABLE_TEST_HOOKS
PEAK_API int
peak_general_listener_test_checkpoint_snapshot_lock_hold(void)
{
    return pthread_mutex_lock(&lock);
}

PEAK_API int
peak_general_listener_test_checkpoint_snapshot_lock_release(void)
{
    return pthread_mutex_unlock(&lock);
}
#endif

static gboolean
peak_general_listener_checkpoint_snapshot_lock(void)
{
    return pthread_mutex_trylock(&lock) == 0;
}

typedef struct {
    PeakGeneralListenerCheckpointShadow* shadows;
    gulong* sequences;
    int* invalid;
} PeakExecCheckpointThreadSnapshot;

#ifdef PEAK_ENABLE_TEST_HOOKS
static _Atomic int peak_checkpoint_test_busy_pause = 0;
static _Atomic int peak_checkpoint_test_busy_held = 0;
static _Atomic int peak_checkpoint_test_busy_released = 0;

PEAK_API void
peak_general_listener_test_checkpoint_busy_pause_enable(void)
{
    atomic_store_explicit(&peak_checkpoint_test_busy_released,
                          0,
                          memory_order_release);
    atomic_store_explicit(&peak_checkpoint_test_busy_held,
                          0,
                          memory_order_release);
    atomic_store_explicit(&peak_checkpoint_test_busy_pause,
                          1,
                          memory_order_release);
}

PEAK_API int
peak_general_listener_test_checkpoint_busy_is_held(void)
{
    return atomic_load_explicit(&peak_checkpoint_test_busy_held,
                                memory_order_acquire);
}

PEAK_API void
peak_general_listener_test_checkpoint_busy_release(void)
{
    atomic_store_explicit(&peak_checkpoint_test_busy_released,
                          1,
                          memory_order_release);
}

static void
peak_general_listener_test_checkpoint_busy_pause(void)
{
    if (atomic_load_explicit(&peak_checkpoint_test_busy_pause,
                             memory_order_acquire) == 0) {
        return;
    }
    atomic_store_explicit(&peak_checkpoint_test_busy_held,
                          1,
                          memory_order_release);
    while (atomic_load_explicit(&peak_checkpoint_test_busy_released,
                                memory_order_acquire) == 0) {
        sched_yield();
    }
    atomic_store_explicit(&peak_checkpoint_test_busy_pause,
                          0,
                          memory_order_release);
}
#endif

static gboolean
peak_exec_checkpoint_copy_listener(
    const PeakGeneralListener* listener,
    PeakExecCheckpointThreadSnapshot* snapshot,
    size_t thread_count)
{
#if defined(__linux__) && defined(SYS_process_vm_readv)
    if (listener->checkpoint_shadow == NULL) {
        errno = EOPNOTSUPP;
        return FALSE;
    }
    struct iovec local = { snapshot->shadows,
                           thread_count * sizeof(*snapshot->shadows) };
    struct iovec remote = { listener->checkpoint_shadow,
                            thread_count * sizeof(*listener->checkpoint_shadow) };
    size_t expected = thread_count * sizeof(*snapshot->shadows);
    for (unsigned int attempt = 0; attempt < 8; attempt++) {
        gboolean busy = FALSE;

        for (size_t index = 0; index < thread_count; index++) {
            snapshot->sequences[index] = atomic_load_explicit(
                &listener->checkpoint_shadow[index].sequence, memory_order_acquire);
            snapshot->invalid[index] = atomic_load_explicit(
                &listener->checkpoint_shadow[index].invalid, memory_order_acquire);
            if ((snapshot->sequences[index] & 1UL) != 0 ||
                snapshot->invalid[index] != 0) {
                busy = TRUE;
                break;
            }
        }
        if (busy) {
#ifdef PEAK_ENABLE_TEST_HOOKS
            peak_general_listener_test_checkpoint_busy_pause();
#endif
            continue;
        }

        long copied = peak_exec_raw_syscall6(
            SYS_process_vm_readv,
            (long)getpid(),
            (long)&local,
            1,
            (long)&remote,
            1,
            0);
        if (copied < 0 || (size_t)copied != expected) {
            return FALSE;
        }
        for (size_t index = 0; index < thread_count; index++) {
            gulong current = atomic_load_explicit(
                &listener->checkpoint_shadow[index].sequence, memory_order_acquire);
            int invalid = atomic_load_explicit(
                &listener->checkpoint_shadow[index].invalid, memory_order_acquire);
            if (current != snapshot->sequences[index] ||
                invalid != snapshot->invalid[index] || invalid != 0 ||
                (current & 1UL) != 0) {
                busy = TRUE;
                break;
            }
        }
        if (!busy) {
            return TRUE;
        }
    }
    errno = EAGAIN;
    return FALSE;
#else
    (void)listener;
    (void)snapshot;
    (void)thread_count;
    errno = ENOSYS;
    return FALSE;
#endif
}

static void
peak_exec_checkpoint_free_snapshot(PeakExecCheckpointRow* records,
                                   size_t* name_lengths,
                                   char* names,
                                   void* thread_storage)
{
    free(thread_storage);
    free(names);
    free(name_lengths);
    free(records);
}

gboolean
peak_general_listener_checkpoint_for_exec(unsigned long long checkpoint_index)
{
    size_t hook_count;
    size_t thread_count;
    size_t name_bytes = 0;
    size_t thread_storage_bytes;
    PeakExecCheckpointRow* records = NULL;
    size_t* name_lengths = NULL;
    char* names = NULL;
    void* thread_storage = NULL;
    PeakExecCheckpointThreadSnapshot threads = {0};
    if (!peak_general_listener_checkpoint_snapshot_lock()) {
        return FALSE;
    }
    hook_count = peak_hook_address_count;
    thread_count = peak_max_num_threads;
    pthread_mutex_unlock(&lock);

    if (hook_count > SIZE_MAX / sizeof(*records) ||
        hook_count > SIZE_MAX / sizeof(*name_lengths) ||
        thread_count > SIZE_MAX / sizeof(gdouble)) {
        errno = EOVERFLOW;
        return FALSE;
    }
    records = calloc(hook_count != 0 ? hook_count : 1, sizeof(*records));
    name_lengths = calloc(hook_count != 0 ? hook_count : 1,
                          sizeof(*name_lengths));
    if (records == NULL || name_lengths == NULL) {
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           NULL,
                                           NULL);
        return FALSE;
    }

    if (!peak_general_listener_checkpoint_snapshot_lock()) {
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           NULL,
                                           NULL);
        return FALSE;
    }
    for (size_t i = 0; i < hook_count; i++) {
        const char* name =
            (peak_demangled_strings != NULL &&
             peak_demangled_strings[i] != NULL) ?
                peak_demangled_strings[i] :
            (peak_hook_strings != NULL && peak_hook_strings[i] != NULL) ?
                peak_hook_strings[i] :
                "";
        size_t length = strlen(name) + 1;

        if (length > SIZE_MAX - name_bytes) {
            pthread_mutex_unlock(&lock);
            peak_exec_checkpoint_free_snapshot(records,
                                               name_lengths,
                                               NULL,
                                               NULL);
            errno = EOVERFLOW;
            return FALSE;
        }
        name_lengths[i] = length;
        name_bytes += length;
    }
    pthread_mutex_unlock(&lock);

    if (thread_count != 0 &&
        thread_count > SIZE_MAX /
            (sizeof(PeakGeneralListenerCheckpointShadow) + sizeof(gulong) +
             sizeof(int))) {
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           NULL,
                                           NULL);
        errno = EOVERFLOW;
        return FALSE;
    }
    thread_storage_bytes = thread_count *
        (sizeof(PeakGeneralListenerCheckpointShadow) + sizeof(gulong) +
         sizeof(int));
    names = malloc(name_bytes != 0 ? name_bytes : 1);
    int thread_storage_error = posix_memalign(
        &thread_storage,
        64,
        thread_storage_bytes != 0 ? thread_storage_bytes : 1);
    if (thread_storage_error != 0) {
        thread_storage = NULL;
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           names,
                                           thread_storage);
        errno = thread_storage_error;
        return FALSE;
    }
    if (names == NULL) {
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           names,
                                           thread_storage);
        return FALSE;
    }
    threads.shadows = thread_storage;
    threads.sequences = (gulong*)(threads.shadows + thread_count);
    threads.invalid = (int*)(threads.sequences + thread_count);

    if (!peak_general_listener_checkpoint_snapshot_lock()) {
        peak_exec_checkpoint_free_snapshot(records,
                                           name_lengths,
                                           names,
                                           thread_storage);
        return FALSE;
    }
    size_t name_offset = 0;
    for (size_t i = 0; i < hook_count; i++) {
        const char* name;
        char* record_name;
        size_t current_length;

        if (i >= peak_hook_address_count || hook_address == NULL ||
            array_listener == NULL || hook_address[i] == NULL ||
            array_listener[i] == NULL) {
            continue;
        }
        name =
            (peak_demangled_strings != NULL &&
             peak_demangled_strings[i] != NULL) ?
                peak_demangled_strings[i] :
            (peak_hook_strings != NULL && peak_hook_strings[i] != NULL) ?
                peak_hook_strings[i] :
                "";
        current_length = strlen(name) + 1;
        if (current_length > name_lengths[i] ||
            !peak_exec_checkpoint_copy_listener(
                PEAKGENERAL_LISTENER(array_listener[i]),
                &threads,
                thread_count)) {
            pthread_mutex_unlock(&lock);
            peak_exec_checkpoint_free_snapshot(records,
                                               name_lengths,
                                               names,
                                               thread_storage);
            return FALSE;
        }
        record_name = names + name_offset;
        memcpy(record_name, name, current_length);
        records[i].name = record_name;
        name_offset += name_lengths[i];

        for (size_t j = 0; j < thread_count; j++) {
            const PeakGeneralListenerCheckpointShadow* shadow =
                &threads.shadows[j];
            gulong calls = shadow->completed_calls;

            records[i].num_calls += calls;
            records[i].total_time += shadow->total_time;
            records[i].exclusive_time += shadow->exclusive_time;
            if (calls != 0) {
                records[i].threads_seen++;
                if (shadow->total_time > records[i].max_total_time) {
                    records[i].max_total_time = shadow->total_time;
                }
                if (shadow->total_time < records[i].min_total_time ||
                    records[i].threads_seen == 1) {
                    records[i].min_total_time = shadow->total_time;
                }
                if (shadow->max_time > records[i].max_time) {
                    records[i].max_time = shadow->max_time;
                }
                if (shadow->min_time < records[i].min_time ||
                    records[i].threads_seen == 1) {
                    records[i].min_time = shadow->min_time;
                }
            }
        }
    }
    pthread_mutex_unlock(&lock);

    gboolean written = peak_exec_checkpoint_write_rows(
        checkpoint_index,
        records,
        hook_count,
        peak_general_overhead);
    peak_exec_checkpoint_free_snapshot(records,
                                       name_lengths,
                                       names,
                                       thread_storage);
    return written;
}

static PeakReportSnapshot*
peak_general_listener_build_report_snapshot(
    const gulong* sum_num_calls,
    const gdouble* sum_total_time,
    const gdouble* max_total_time,
    const gdouble* min_total_time,
    const gdouble* sum_exclusive_time,
    const gfloat* sum_max_time,
    const gfloat* sum_min_time,
    const gulong* thread_count,
    int rank_count,
    const PeakReportOverhead* report_overhead)
{
    PeakReportSnapshot* snapshot =
        peak_report_snapshot_create(peak_hook_address_count);

    if (snapshot == NULL) {
        return NULL;
    }
    get_argv0(&snapshot->program);
    snapshot->overhead_per_call = peak_general_overhead;
    snapshot->rank_count = rank_count;
    if (report_overhead != NULL) {
        snapshot->overhead = *report_overhead;
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        const char* name =
            peak_demangled_strings != NULL &&
                    peak_demangled_strings[i] != NULL ?
                peak_demangled_strings[i] :
            peak_hook_strings != NULL && peak_hook_strings[i] != NULL ?
                peak_hook_strings[i] :
                "";

        if (!peak_report_snapshot_set_name(snapshot, i, name)) {
            peak_report_snapshot_destroy(snapshot);
            return NULL;
        }
        snapshot->instrumented[i] = hook_address[i] != NULL;
        snapshot->detached[i] = array_listener_detached != NULL ?
                                    array_listener_detached[i] : FALSE;
        snapshot->reattached[i] = array_listener_reattached != NULL ?
                                      array_listener_reattached[i] : FALSE;
        snapshot->revisited[i] = array_listener_revisited != NULL ?
                                     array_listener_revisited[i] : FALSE;
        snapshot->num_calls[i] = sum_num_calls[i];
        snapshot->total_time[i] = sum_total_time[i];
        snapshot->max_total_time[i] = max_total_time[i];
        snapshot->min_total_time[i] = min_total_time[i];
        snapshot->exclusive_time[i] = sum_exclusive_time[i];
        snapshot->max_time[i] = sum_max_time[i];
        snapshot->min_time[i] = sum_min_time[i];
        snapshot->thread_count[i] = thread_count[i];
    }
    return snapshot;
}

static void
peak_general_listener_release_report_names(void)
{
    if (peak_demangled_strings == NULL) {
        return;
    }
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        g_free(peak_demangled_strings[i]);
    }
    g_free(peak_demangled_strings);
    peak_demangled_strings = NULL;
}

static double
peak_general_listener_profile_seconds_from_calls(gulong* sum_num_calls)
{
    double profile_seconds = 0.0;

    if (sum_num_calls == NULL) {
        return 0.0;
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i] && sum_num_calls[i] != 0) {
            profile_seconds +=
                (double)sum_num_calls[i] * peak_general_overhead;
        }
    }
    double heartbeat_profile_seconds =
        peak_general_listener_report_heartbeat_overhead_seconds();
    if (peak_general_listener_positive_finite(heartbeat_profile_seconds) &&
        profile_seconds <= DBL_MAX - heartbeat_profile_seconds) {
        profile_seconds += heartbeat_profile_seconds;
    }

    return profile_seconds;
}

static void
peak_general_listener_populate_report_risk_fields(PeakReportOverhead* report)
{
    double profile_control_risk_seconds = DBL_MAX;

    if (report == NULL) {
        return;
    }
    report->local_ranks = peak_general_listener_local_mpi_ranks();
    report->control_risk_seconds =
        peak_general_listener_control_risk_seconds(report->control_seconds);
    (void)peak_general_listener_add_nonnegative_finite(
        report->profile_seconds,
        report->control_risk_seconds,
        &profile_control_risk_seconds);
    report->profile_control_risk_seconds = profile_control_risk_seconds;
}

static PeakReportOverhead
peak_general_listener_local_report_overhead(gulong* sum_num_calls)
{
    PeakReportOverhead overhead = {0};
    PeakDetachAccountingSnapshot accounting;
    double elapsed_seconds = peak_main_time;
    double control_risk_seconds = 0.0;
    double profile_control_risk_seconds = DBL_MAX;

    if (peak_general_listener_final_report_snapshot.valid) {
        overhead = (PeakReportOverhead) {
            .valid = TRUE,
            .accounting_valid =
                peak_general_listener_final_report_snapshot.accounting_valid,
            .local_ranks =
                peak_general_listener_final_report_snapshot.local_ranks,
            .stop_window_count =
                peak_general_listener_final_report_snapshot.stop_window_count,
            .failed_stop_window_count =
                peak_general_listener_final_report_snapshot.failed_stop_window_count,
            .elapsed_seconds =
                peak_general_listener_final_report_snapshot.elapsed_seconds,
            .elapsed_min_seconds =
                peak_general_listener_final_report_snapshot.elapsed_seconds,
            .elapsed_max_seconds =
                peak_general_listener_final_report_snapshot.elapsed_seconds,
            .profile_seconds =
                peak_general_listener_final_report_snapshot.profile_seconds,
            .control_seconds =
                peak_general_listener_final_report_snapshot.control_seconds,
            .management_seconds =
                peak_general_listener_final_report_snapshot.management_seconds,
            .control_risk_seconds =
                peak_general_listener_final_report_snapshot.control_risk_seconds,
            .profile_control_risk_seconds =
                peak_general_listener_final_report_snapshot.profile_control_risk_seconds,
            .profile_ratio =
                peak_general_listener_final_report_snapshot.profile_ratio,
            .control_ratio =
                peak_general_listener_final_report_snapshot.control_ratio,
            .profile_control_risk_ratio =
                peak_general_listener_final_report_snapshot.profile_control_risk_ratio,
            .control_risk_ratio =
                peak_general_listener_final_report_snapshot.control_risk_ratio,
            .ratio = peak_general_listener_final_report_snapshot.ratio,
            .management_ratio =
                peak_general_listener_final_report_snapshot.management_ratio,
        };
        return overhead;
    }

    overhead.profile_seconds =
        peak_general_listener_profile_seconds_from_calls(sum_num_calls);
    overhead.accounting_valid =
        peak_general_listener_runtime_accounting_snapshot(&accounting);
    overhead.stop_window_count =
        peak_general_listener_control_window_count_since_heartbeat(
            &accounting);
    overhead.failed_stop_window_count =
        peak_general_listener_failed_window_count_since_heartbeat(
            &accounting);
    overhead.elapsed_seconds = elapsed_seconds;
    overhead.elapsed_min_seconds = elapsed_seconds;
    overhead.elapsed_max_seconds = elapsed_seconds;
    overhead.control_seconds =
        (double)peak_general_listener_control_wall_ns_since_heartbeat(
            &accounting) / 1e9;
    overhead.management_seconds =
        peak_general_listener_management_cpu_seconds();
    control_risk_seconds =
        peak_general_listener_control_risk_seconds(overhead.control_seconds);
    (void)peak_general_listener_add_nonnegative_finite(
        overhead.profile_seconds,
        control_risk_seconds,
        &profile_control_risk_seconds);
    if (elapsed_seconds > 0.0) {
        overhead.profile_ratio =
            overhead.profile_seconds / elapsed_seconds;
        overhead.control_ratio =
            overhead.control_seconds / elapsed_seconds;
        overhead.profile_control_risk_ratio =
            profile_control_risk_seconds / elapsed_seconds;
        overhead.control_risk_ratio =
            control_risk_seconds / elapsed_seconds;
        overhead.ratio =
            (overhead.profile_seconds + overhead.control_seconds) /
            elapsed_seconds;
        overhead.management_ratio =
            overhead.management_seconds / elapsed_seconds;
    }
    overhead.valid = TRUE;
    peak_general_listener_populate_report_risk_fields(&overhead);
    return overhead;
}
