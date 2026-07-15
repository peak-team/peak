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

gboolean peak_general_listener_print(PeakOutputAggregationMode aggregation_mode)
{
    gboolean used_mpi_aggregation = FALSE;

    atomic_store_explicit(&peak_general_mpi_reducer_failed_closed,
                          FALSE,
                          memory_order_release);

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
            PeakGeneralListener* pg_listener = PEAKGENERAL_LISTENER(array_listener[i]);
            for (size_t j = 0; j < peak_max_num_threads; j++) {
                size_t index = j;
                gulong calls =
                    peak_general_listener_num_calls_load(
                        &pg_listener->num_calls[index]);
                sum_num_calls[i] += calls;
                sum_total_time[i] += pg_listener->total_time[index];
                sum_exclusive_time[i] += pg_listener->exclusive_time[index];
                if (calls != 0) {
                    thread_count[i]++;
                    if (pg_listener->total_time[index] > max_total_time[i])
                        max_total_time[i] = pg_listener->total_time[index];
                    if (pg_listener->total_time[index] < min_total_time[i] || thread_count[i] == 1)
                        min_total_time[i] = pg_listener->total_time[index];
                    if (pg_listener->max_time[index] > sum_max_time[i])
                        sum_max_time[i] = pg_listener->max_time[index];
                    if (pg_listener->min_time[index] < sum_min_time[i] || thread_count[i] == 1)
                        sum_min_time[i] = pg_listener->min_time[index];
                }
            }
            if (thread_count[i] == 0)
                thread_count[i] = 1;
        }
    }
    peak_general_listener_refresh_revisited_markers(sum_num_calls);
#ifdef HAVE_MPI
    if (aggregation_mode == PEAK_OUTPUT_AGGREGATION_MPI) {
        used_mpi_aggregation =
            peak_general_listener_reduce_result(sum_num_calls,
                                                sum_total_time,
                                                max_total_time,
                                                min_total_time,
                                                sum_exclusive_time,
                                                sum_max_time,
                                                sum_min_time,
                                                thread_count);
    } else if (aggregation_mode == PEAK_OUTPUT_AGGREGATION_SOCKET) {
        if (!peak_general_listener_socket_reduce_result(sum_num_calls,
                                                       sum_total_time,
                                                       max_total_time,
                                                       min_total_time,
                                                       sum_exclusive_time,
                                                       sum_max_time,
                                                       sum_min_time,
                                                       thread_count) &&
            peak_socket_reduce_fallback_enabled()) {
            g_printerr("[peak] Socket aggregation failed; falling back to rank-local output\n");
            PeakReportOverhead local_report =
                peak_general_listener_local_report_overhead(sum_num_calls);
            peak_general_listener_print_result(sum_num_calls,
                                              sum_total_time,
                                              max_total_time,
                                              min_total_time,
                                              sum_exclusive_time,
                                              sum_max_time,
                                              sum_min_time,
                                              thread_count,
                                              1,
                                              &local_report);
        }
    } else {
        PeakReportOverhead local_report =
            peak_general_listener_local_report_overhead(sum_num_calls);
        peak_general_listener_print_result(sum_num_calls,
                                           sum_total_time,
                                           max_total_time,
                                           min_total_time,
                                           sum_exclusive_time,
                                           sum_max_time,
                                           sum_min_time,
                                           thread_count,
                                           1,
                                           &local_report);
    }
#else
    PeakReportOverhead local_report =
        peak_general_listener_local_report_overhead(sum_num_calls);
    peak_general_listener_print_result(sum_num_calls,
                                       sum_total_time,
                                       max_total_time,
                                       min_total_time,
                                       sum_exclusive_time,
                                       sum_max_time,
                                       sum_min_time,
                                       thread_count,
                                       1,
                                       &local_report);
#endif
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
    g_free(peak_hook_dynamic_provider_discoverable);
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
    peak_hook_dynamic_provider_discoverable = NULL;
    array_listener = NULL;

    return TRUE;
}
