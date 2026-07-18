static void
peak_general_listener_refresh_revisited_markers(
    const gulong* local_final_calls)
{
    if (array_listener_revisited == NULL) {
        return;
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (!array_listener_revisited[i] &&
            local_final_calls != NULL &&
            array_listener_reattached != NULL &&
            array_listener_reattached[i] &&
            peak_hook_reattach_request_calls != NULL &&
            peak_hook_reattach_request_calls_valid != NULL &&
            peak_hook_reattach_request_calls_valid[i] &&
            local_final_calls[i] > peak_hook_reattach_request_calls[i]) {
            array_listener_revisited[i] = TRUE;
        }
    }
}

#ifdef HAVE_MPI
typedef struct {
    gboolean* previous_detached;
    gboolean* previous_reattached;
    gboolean* previous_revisited;
    gboolean* installed_detached;
    gboolean* installed_reattached;
    gboolean* installed_revisited;
} PeakReportMarkerSwap;
#endif

static PeakReportFormatOptions
peak_general_listener_report_format_options(int rank_count)
{
    const PeakReportFormatOptions options = {
        .print_text = peak_general_listener_should_print_text(
            rank_count <= 1 && peak_general_listener_mpi_env_size() > 1),
        .truncate_names = peak_truncate_function_name,
    };

    return options;
}

static void
peak_general_listener_write_report(PeakReportSnapshot* snapshot,
                                   gboolean sanitize)
{
    PeakReportFormatOptions options;

    if (snapshot == NULL) {
        return;
    }
    if (sanitize) {
        peak_report_snapshot_prepare_for_render(snapshot);
    }
    options = peak_general_listener_report_format_options(
        snapshot->rank_count);
    (void)peak_report_formatter_write_csv(snapshot);
    peak_report_formatter_write_text(snapshot, &options);
    peak_general_listener_release_report_names();
}

#ifdef HAVE_MPI
static PeakReportMarkerSwap
peak_general_listener_begin_report_marker_swap(
    const PeakReportSnapshot* source)
{
    PeakReportMarkerSwap swap = {
        .previous_detached = array_listener_detached,
        .previous_reattached = array_listener_reattached,
        .previous_revisited = array_listener_revisited,
        .installed_detached =
            g_new0(gboolean, peak_hook_address_count),
        .installed_reattached =
            g_new0(gboolean, peak_hook_address_count),
        .installed_revisited =
            g_new0(gboolean, peak_hook_address_count),
    };

    if (source != NULL) {
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            swap.installed_detached[i] = source->detached[i] != 0;
            swap.installed_reattached[i] = source->reattached[i] != 0;
            swap.installed_revisited[i] = source->revisited[i] != 0;
        }
    }
    array_listener_detached = swap.installed_detached;
    array_listener_reattached = swap.installed_reattached;
    array_listener_revisited = swap.installed_revisited;
    return swap;
}

static void
peak_general_listener_commit_report_marker_swap(
    PeakReportMarkerSwap* swap)
{
    if (swap == NULL) {
        return;
    }
    g_free(swap->previous_detached);
    g_free(swap->previous_reattached);
    g_free(swap->previous_revisited);
    memset(swap, 0, sizeof(*swap));
}

static void
peak_general_listener_rollback_report_marker_swap(
    PeakReportMarkerSwap* swap)
{
    if (swap == NULL) {
        return;
    }
    array_listener_detached = swap->previous_detached;
    array_listener_reattached = swap->previous_reattached;
    array_listener_revisited = swap->previous_revisited;
    g_free(swap->installed_detached);
    g_free(swap->installed_reattached);
    g_free(swap->installed_revisited);
    memset(swap, 0, sizeof(*swap));
}
#endif

#ifdef HAVE_MPI
static gboolean
peak_general_listener_run_socket_report(
    PeakReportSnapshot* local,
    PeakSocketReportRankSource rank_source)
{
    PeakSocketReportSession* session = NULL;
    PeakReportSnapshot* aggregate = NULL;
    PeakSocketReportStatus status = peak_socket_report_transport_begin(
        local, rank_source, &session, &aggregate);

    if (status == PEAK_SOCKET_REPORT_SINGLE_READY) {
        peak_general_listener_write_report(aggregate, TRUE);
        peak_report_snapshot_destroy(aggregate);
        return TRUE;
    }
    if (status == PEAK_SOCKET_REPORT_PEER_RELEASED) {
        return TRUE;
    }
    if (status != PEAK_SOCKET_REPORT_ROOT_PREPARED ||
        session == NULL || aggregate == NULL) {
        peak_socket_report_transport_abort(session);
        peak_report_snapshot_destroy(aggregate);
        return FALSE;
    }

    PeakReportMarkerSwap marker_swap =
        peak_general_listener_begin_report_marker_swap(aggregate);
    PeakReportFormatOptions options =
        peak_general_listener_report_format_options(aggregate->rank_count);

    (void)peak_report_formatter_write_csv(aggregate);
    if (!peak_socket_report_transport_commit(session)) {
        (void)peak_report_formatter_remove_csv();
        peak_general_listener_rollback_report_marker_swap(&marker_swap);
        peak_report_snapshot_destroy(aggregate);
        return FALSE;
    }
    peak_report_formatter_write_text(aggregate, &options);
    peak_general_listener_release_report_names();
    peak_general_listener_commit_report_marker_swap(&marker_swap);
    peak_report_snapshot_destroy(aggregate);
    return TRUE;
}
#endif

gboolean
peak_general_listener_mpi_reducer_failed_closed(void)
{
#ifdef HAVE_MPI
    return peak_mpi_report_transport_failed_closed();
#else
    return FALSE;
#endif
}

gboolean peak_general_listener_print(PeakOutputAggregationMode aggregation_mode)
{
    gboolean used_mpi_aggregation = FALSE;
    PeakReportSnapshot* local_snapshot = NULL;

#ifdef HAVE_MPI
    peak_mpi_report_transport_reset_failed_closed();
#endif
    if (interceptor != NULL) {
        gum_interceptor_ignore_current_thread(interceptor);
    }

    gulong* sum_num_calls = g_new0(gulong, peak_hook_address_count);
    gdouble* sum_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* max_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* min_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* sum_exclusive_time = g_new0(gdouble, peak_hook_address_count);
    gfloat* sum_max_time = g_new0(gfloat, peak_hook_address_count);
    gfloat* sum_min_time = g_new0(gfloat, peak_hook_address_count);
    gulong* thread_count = g_new0(gulong, peak_hook_address_count);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i]) {
            PeakGeneralListener* pg_listener =
                PEAKGENERAL_LISTENER(array_listener[i]);
            for (size_t j = 0; j < peak_max_num_threads; j++) {
                gulong calls = peak_general_listener_num_calls_load(
                    &pg_listener->num_calls[j]);

                sum_num_calls[i] += calls;
                sum_total_time[i] += pg_listener->total_time[j];
                sum_exclusive_time[i] += pg_listener->exclusive_time[j];
                if (calls != 0) {
                    thread_count[i]++;
                    if (pg_listener->total_time[j] > max_total_time[i]) {
                        max_total_time[i] = pg_listener->total_time[j];
                    }
                    if (pg_listener->total_time[j] < min_total_time[i] ||
                        thread_count[i] == 1) {
                        min_total_time[i] = pg_listener->total_time[j];
                    }
                    if (pg_listener->max_time[j] > sum_max_time[i]) {
                        sum_max_time[i] = pg_listener->max_time[j];
                    }
                    if (pg_listener->min_time[j] < sum_min_time[i] ||
                        thread_count[i] == 1) {
                        sum_min_time[i] = pg_listener->min_time[j];
                    }
                }
            }
            if (thread_count[i] == 0) {
                min_total_time[i] = DBL_MAX;
                sum_min_time[i] = FLT_MAX;
            }
        }
    }
    peak_general_listener_refresh_revisited_markers(sum_num_calls);

    PeakReportOverhead local_report =
        peak_general_listener_local_report_overhead(sum_num_calls);
    local_snapshot = peak_general_listener_build_report_snapshot(
        sum_num_calls,
        sum_total_time,
        max_total_time,
        min_total_time,
        sum_exclusive_time,
        sum_max_time,
        sum_min_time,
        thread_count,
        1,
        &local_report);

    if (local_snapshot != NULL) {
#ifdef HAVE_MPI
        if (aggregation_mode == PEAK_OUTPUT_AGGREGATION_MPI) {
            PeakReportSnapshot* aggregate = NULL;
            PeakMpiReportTransportResult result =
                peak_mpi_report_transport_reduce(local_snapshot, &aggregate);

            if (result == PEAK_MPI_REPORT_TRANSPORT_ROOT_READY) {
                PeakReportMarkerSwap marker_swap =
                    peak_general_listener_begin_report_marker_swap(aggregate);

                peak_general_listener_commit_report_marker_swap(&marker_swap);
                peak_general_listener_write_report(aggregate, TRUE);
                peak_report_snapshot_destroy(aggregate);
                used_mpi_aggregation = TRUE;
            } else if (result == PEAK_MPI_REPORT_TRANSPORT_PEER_COMPLETE) {
                PeakReportMarkerSwap marker_swap =
                    peak_general_listener_begin_report_marker_swap(NULL);

                peak_general_listener_commit_report_marker_swap(&marker_swap);
                used_mpi_aggregation = TRUE;
            } else if (result == PEAK_MPI_REPORT_TRANSPORT_FAILED_CLOSED &&
                       peak_general_listener_socket_reduce_fallback_enabled()) {
                g_printerr("[peak] MPI reducer failed; trying PEAK-owned socket aggregation fallback without further MPI calls\n");
                if (!peak_general_listener_run_socket_report(
                        local_snapshot,
                        PEAK_SOCKET_REPORT_RANK_ENV_ONLY)) {
                    g_printerr("[peak] MPI reducer socket fallback failed; writing rank-local output\n");
                    peak_general_listener_write_report(local_snapshot, TRUE);
                }
            } else {
                peak_general_listener_write_report(local_snapshot, TRUE);
            }
        } else if (aggregation_mode == PEAK_OUTPUT_AGGREGATION_SOCKET) {
            if (!peak_general_listener_run_socket_report(
                    local_snapshot,
                    PEAK_SOCKET_REPORT_RANK_MPI_OR_ENV) &&
                peak_general_listener_socket_reduce_fallback_enabled()) {
                g_printerr("[peak] Socket aggregation failed; falling back to rank-local output\n");
                peak_general_listener_write_report(local_snapshot, TRUE);
            }
        } else {
            peak_general_listener_write_report(local_snapshot, TRUE);
        }
#else
        (void)aggregation_mode;
        peak_general_listener_write_report(local_snapshot, TRUE);
#endif
    }

    peak_report_snapshot_destroy(local_snapshot);
    g_free(sum_num_calls);
    g_free(sum_total_time);
    g_free(max_total_time);
    g_free(min_total_time);
    g_free(sum_exclusive_time);
    g_free(sum_max_time);
    g_free(sum_min_time);
    g_free(thread_count);

    if (interceptor != NULL) {
        gum_interceptor_unignore_current_thread(interceptor);
    }
    return used_mpi_aggregation;
}

gboolean peak_general_listener_dettach()
{
    if (interceptor == NULL) {
        return TRUE;
    }

    peak_general_listener_controller_stop();

    pthread_t controller_tid = pthread_self();
    pthread_t* tid_keys = g_new0(pthread_t, peak_max_num_threads);
    size_t* mapped_ids = g_new0(size_t, peak_max_num_threads);
    int* pause_session_ids = g_new0(int, peak_max_num_threads);
    int* pause_status = g_new0(int, peak_max_num_threads);

    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i]) {
            gboolean shutdown_ok = FALSE;
            unsigned int shutdown_attempts = 0;
            PeakGeneralShutdownFailure shutdown_failure = {
                .bucket = PEAK_GENERAL_SHUTDOWN_FAILURE_NONE,
                .status = PEAK_DETACH_STATUS_ERROR
            };
            double deadline = peak_second() +
                              ((double)peak_general_controller_shutdown_drain_ms() /
                               1000.0);

            do {
                gboolean snapshot_complete = FALSE;
                size_t snapshot_count =
                    pthread_listener_snapshot_threads(tid_keys,
                                                      mapped_ids,
                                                      peak_max_num_threads,
                                                      &snapshot_complete);
                for (size_t s = 0; s < snapshot_count; s++) {
                    pause_session_ids[s] = -1;
                    pause_status[s] = -1;
                }

                shutdown_attempts++;
                shutdown_ok = peak_general_controller_shutdown_hook_unlocked(
                    i,
                    controller_tid,
                    tid_keys,
                    mapped_ids,
                    pause_session_ids,
                    pause_status,
                    snapshot_count,
                    snapshot_complete,
                    &shutdown_failure);
                if (shutdown_ok) {
                    break;
                }
                if (peak_second() >= deadline) {
                    break;
                }
                usleep(1000);
            } while (TRUE);

            if (!shutdown_ok) {
                pthread_mutex_unlock(&lock);
                g_free(pause_status);
                g_free(pause_session_ids);
                g_free(mapped_ids);
                g_free(tid_keys);
                g_printerr("[peak] Gum shutdown detach was not proven safe; bucket=%s status=%s attempts=%u; leaving listener state alive\n",
                           peak_general_shutdown_failure_bucket_string(
                               &shutdown_failure),
                           peak_detach_controller_status_string(
                               shutdown_failure.status),
                           shutdown_attempts);
                return FALSE;
            }
        }
    }
    pthread_mutex_unlock(&lock);

    PeakDetachStatus helper_shutdown_status = PEAK_DETACH_STATUS_ERROR;
    if (!peak_detach_controller_shutdown_helper(&helper_shutdown_status)) {
        g_free(pause_status);
        g_free(pause_session_ids);
        g_free(mapped_ids);
        g_free(tid_keys);
        g_printerr("[peak] detach helper shutdown failed: %s; leaving listener state alive\n",
                   peak_detach_controller_status_string(helper_shutdown_status));
        return FALSE;
    }

    g_free(pause_status);
    g_free(pause_session_ids);
    g_free(mapped_ids);
    g_free(tid_keys);

    if (!peak_general_controller_flush_teardown()) {
        g_printerr("[peak] Gum detach teardown did not flush; leaving listener state alive\n");
        return FALSE;
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i] && array_listener[i] != NULL) {
            peak_general_listener_free(PEAKGENERAL_LISTENER(array_listener[i]));
            g_object_unref(array_listener[i]);
        }
    }
    g_object_unref(interceptor);
    g_free(hook_address);
    g_free(array_listener_detached);
    g_free(array_listener_reattached);
    g_free(array_listener_revisited);
    g_free(array_listener_gum_detached);
    g_free(array_listener_gum_detach_flushed);
    g_free(peak_hook_last_detach_time);
    g_free(peak_hook_states);
    g_free(peak_hook_next_retry_time);
    g_free(peak_hook_pending_observed_time);
    g_free(peak_hook_pending_request_source);
    g_free(peak_hook_pending_request_calls);
    g_free(peak_hook_pending_request_ratio);
    g_free(peak_hook_pending_request_global_overhead);
    g_free(peak_hook_pending_request_total_time);
    g_free(peak_hook_pending_request_rate);
    g_free(peak_hook_detach_profile_seconds);
    g_free(peak_hook_reattach_request_calls);
    g_free(peak_hook_reattach_request_calls_valid);
    g_free(peak_hook_cached_sample_calls);
    g_free(peak_hook_cached_sample_profile_seconds);
    g_free(peak_hook_cached_sample_valid);
    g_free(peak_hook_retry_count);
    g_free(peak_hook_last_retry_status);
    g_free(array_listener);

    interceptor = NULL;
    hook_address = NULL;
    array_listener_detached = NULL;
    array_listener_reattached = NULL;
    array_listener_revisited = NULL;
    array_listener_gum_detached = NULL;
    array_listener_gum_detach_flushed = NULL;
    peak_hook_last_detach_time = NULL;
    peak_hook_states = NULL;
    peak_hook_next_retry_time = NULL;
    peak_hook_pending_observed_time = NULL;
    atomic_store_explicit(&peak_general_listener_runtime_start_ns,
                          0,
                          memory_order_relaxed);
    atomic_store_explicit(&peak_general_listener_heartbeat_control_baseline_ns,
                          0,
                          memory_order_relaxed);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_count,
        0,
        memory_order_relaxed);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_failed_count,
        0,
        memory_order_relaxed);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_valid,
        FALSE,
        memory_order_release);
    peak_hook_pending_request_source = NULL;
    peak_hook_pending_request_calls = NULL;
    peak_hook_pending_request_ratio = NULL;
    peak_hook_pending_request_global_overhead = NULL;
    peak_hook_pending_request_total_time = NULL;
    peak_hook_pending_request_rate = NULL;
    peak_hook_detach_profile_seconds = NULL;
    peak_hook_reattach_request_calls = NULL;
    peak_hook_reattach_request_calls_valid = NULL;
    peak_hook_cached_sample_calls = NULL;
    peak_hook_cached_sample_profile_seconds = NULL;
    peak_hook_cached_sample_valid = NULL;
    peak_hook_retry_count = NULL;
    peak_hook_last_retry_status = NULL;
    array_listener = NULL;

    return TRUE;
}
