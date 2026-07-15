
static gboolean peak_general_controller_pause_called_threads(
    size_t hook_id,
    pthread_t controller_tid,
    pthread_t* tid_keys,
    size_t* mapped_ids,
    int* pause_session_ids,
    int* pause_status,
    size_t snapshot_count)
{
    gboolean all_paused = TRUE;

    for (size_t s = 0; s < snapshot_count; s++) {
        pthread_t peak_tid_key = tid_keys[s];
        size_t cur_mapped_tid = mapped_ids[s];

        if (cur_mapped_tid < peak_max_num_threads &&
            peak_tid_key != controller_tid) {
            pause_status[s] =
                pthread_pause_mapped(peak_tid_key, cur_mapped_tid, &pause_session_ids[s]);
            if (pause_status[s] != 0) {
                all_paused = FALSE;
            }
        }
    }

    return all_paused;
}

static void peak_general_controller_resume_called_threads(
    size_t hook_id,
    pthread_t controller_tid,
    pthread_t* tid_keys,
    size_t* mapped_ids,
    int* pause_session_ids,
    int* pause_status,
    size_t snapshot_count)
{
    for (size_t s = 0; s < snapshot_count; s++) {
        pthread_t peak_tid_key = tid_keys[s];
        size_t cur_mapped_tid = mapped_ids[s];

        if (cur_mapped_tid < peak_max_num_threads &&
            peak_tid_key != controller_tid &&
            (pause_status[s] == 0 || pause_status[s] == 1) &&
            pause_session_ids[s] >= 0) {
            pthread_unpause(peak_tid_key, pause_session_ids[s]);
        }
    }
}

static gboolean
peak_general_controller_snapshot_is_safe(size_t hook_id,
                                         const size_t* mapped_ids,
                                         size_t snapshot_count,
                                         gboolean snapshot_complete)
{
    static gboolean warned_incomplete_snapshot = FALSE;
    static gboolean warned_over_capacity_thread = FALSE;

    if (!snapshot_complete) {
        if (!warned_incomplete_snapshot) {
            warned_incomplete_snapshot = TRUE;
            g_printerr("[peak] skipping physical detach/reattach because tracked thread snapshot exceeded PEAK thread capacity\n");
        }
        return FALSE;
    }

    for (size_t i = 0; i < snapshot_count; i++) {
        if (mapped_ids[i] >= peak_max_num_threads) {
            if (!warned_over_capacity_thread) {
                warned_over_capacity_thread = TRUE;
                g_printerr("[peak] skipping physical detach/reattach because tracked thread id %lu exceeds PEAK thread capacity %lu\n",
                           (unsigned long)mapped_ids[i],
                           (unsigned long)peak_max_num_threads);
            }
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean
peak_general_controller_prepare_hook_mutation(size_t hook_id,
                                              GumInvocationListener* listener,
                                              PeakDetachOperation operation,
                                              PeakDetachStatus* status_out)
{
    PeakDetachRequest request = {
        .hook_id = hook_id,
        .symbol_name = hook_id < peak_hook_address_count ? peak_hook_strings[hook_id] : NULL,
        .function_address = hook_id < peak_hook_address_count ? hook_address[hook_id] : NULL,
        .interceptor = interceptor,
        .listener = listener,
        .operation = operation
    };
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    gboolean prepared = peak_detach_controller_prepare_hook_mutation(&request, &status);

    if (status_out != NULL) {
        *status_out = status;
    }

    if (!prepared) {
        peak_general_controller_trace_mutation(hook_id,
                                               operation,
                                               "prepare-failed",
                                               FALSE,
                                               status);
    }

    return prepared;
}

static gboolean
peak_general_controller_finish_hook_mutation(size_t hook_id,
                                             GumInvocationListener* listener,
                                             PeakDetachOperation operation,
                                             PeakDetachStatus* status_out)
{
    PeakDetachRequest request = {
        .hook_id = hook_id,
        .symbol_name = hook_id < peak_hook_address_count ? peak_hook_strings[hook_id] : NULL,
        .function_address = hook_id < peak_hook_address_count ? hook_address[hook_id] : NULL,
        .interceptor = interceptor,
        .listener = listener,
        .operation = operation
    };

    return peak_detach_controller_finish_hook_mutation(&request, status_out);
}

static gboolean
peak_general_controller_handle_prepare_failure_unlocked(size_t hook_id,
                                                        PeakHookState stable_state,
                                                        PeakDetachStatus status)
{
    if (peak_general_controller_status_is_retryable(status)) {
        PeakDetachStatus last_retry_status =
            peak_hook_last_retry_status != NULL && hook_id < peak_hook_address_count
                ? peak_hook_last_retry_status[hook_id]
                : PEAK_DETACH_STATUS_SAFE;

        peak_general_controller_note_retry_unlocked(hook_id, status);
        if (!peak_general_controller_retry_budget_exceeded_unlocked(
                hook_id,
                peak_second())) {
            return TRUE;
        }

        g_printerr("[peak] Abandoning %s for hook %lu (%s) after %u retries and %.3fs pending age; leaving hook %s\n",
                   stable_state == PEAK_HOOK_ATTACHED ? "detach" : "reattach",
                   (unsigned long)hook_id,
                   hook_id < peak_hook_address_count &&
                           peak_hook_strings != NULL &&
                           peak_hook_strings[hook_id] != NULL
                       ? peak_hook_strings[hook_id]
                       : "<unknown>",
                   hook_id < peak_hook_address_count && peak_hook_retry_count != NULL
                       ? peak_hook_retry_count[hook_id]
                       : 0,
                   peak_general_controller_pending_age_for_trace_unlocked(
                       hook_id,
                       peak_second()),
                   stable_state == PEAK_HOOK_ATTACHED ? "attached" : "detached");
        peak_general_controller_trace_mutation_detail(
            hook_id,
            stable_state == PEAK_HOOK_ATTACHED ? PEAK_DETACH_OPERATION_DETACH
                                               : PEAK_DETACH_OPERATION_REATTACH,
            "retry-abandoned",
            FALSE,
            status,
            hook_id < peak_hook_address_count && peak_hook_retry_count != NULL
                ? peak_hook_retry_count[hook_id]
                : 0,
            peak_general_controller_pending_age_for_trace_unlocked(
                hook_id,
                peak_second()),
            1,
            0.0,
            0,
            last_retry_status);
        peak_general_controller_reset_retry_unlocked(hook_id);
        peak_general_controller_set_state_unlocked(hook_id, stable_state);
        return FALSE;
    }

    peak_general_controller_reset_retry_unlocked(hook_id);
    peak_general_controller_set_state_unlocked(hook_id, stable_state);
    return FALSE;
}

static gboolean peak_general_controller_detach_if_requested_unlocked(
    size_t hook_id,
    GumInvocationListener* listener,
    pthread_t controller_tid,
    pthread_t* tid_keys,
    size_t* mapped_ids,
    int* pause_session_ids,
    int* pause_status,
    size_t snapshot_count,
    gboolean snapshot_complete)
{
    if (!peak_general_hook_is_published_unlocked(hook_id) ||
        listener != array_listener[hook_id] ||
        peak_hook_states[hook_id] != PEAK_HOOK_DETACH_REQUESTED) {
        return FALSE;
    }

    {
        PeakDetachStatus prepare_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_prepare_hook_mutation(hook_id,
                                                           listener,
                                                           PEAK_DETACH_OPERATION_DETACH,
                                                           &prepare_status)) {
            peak_general_controller_handle_prepare_failure_unlocked(hook_id,
                                                                    PEAK_HOOK_ATTACHED,
                                                                    prepare_status);
            return FALSE;
        }
    }

    gboolean helper_holds_threads = peak_detach_controller_threads_are_held();
    gboolean helper_applied_physical_patch =
        peak_detach_controller_current_mutation_uses_physical_patch();

    if (!helper_holds_threads &&
        !peak_general_controller_snapshot_is_safe(hook_id,
                                                  mapped_ids,
                                                  snapshot_count,
                                                  snapshot_complete)) {
        PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                          listener,
                                                          PEAK_DETACH_OPERATION_DETACH,
                                                          &finish_status)) {
            peak_detach_controller_abort_after_failed_finish("detach snapshot abort",
                                                            finish_status);
        }
        peak_general_controller_trace_mutation(hook_id,
                                               PEAK_DETACH_OPERATION_DETACH,
                                               "snapshot-unsafe",
                                               helper_applied_physical_patch,
                                               finish_status);
        peak_general_controller_reset_retry_unlocked(hook_id);
        peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_ATTACHED);
        return FALSE;
    }

    peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACHING);
    if (!helper_holds_threads &&
        !peak_general_controller_pause_called_threads(hook_id,
                                                      controller_tid,
                                                      tid_keys,
                                                      mapped_ids,
                                                      pause_session_ids,
                                                      pause_status,
                                                      snapshot_count)) {
        PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                          listener,
                                                          PEAK_DETACH_OPERATION_DETACH,
                                                          &finish_status)) {
            peak_detach_controller_abort_after_failed_finish("detach pause abort",
                                                            finish_status);
        }
        peak_general_controller_trace_mutation(hook_id,
                                               PEAK_DETACH_OPERATION_DETACH,
                                               "pause-failed",
                                               helper_applied_physical_patch,
                                               finish_status);
        peak_general_controller_resume_called_threads(hook_id,
                                                      controller_tid,
                                                      tid_keys,
                                                      mapped_ids,
                                                      pause_session_ids,
                                                      pause_status,
                                                      snapshot_count);
        peak_general_controller_reset_retry_unlocked(hook_id);
        peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_ATTACHED);
        return FALSE;
    }

    if (!helper_applied_physical_patch) {
        gum_interceptor_begin_transaction(interceptor);
        gum_interceptor_detach(interceptor, listener);
        gum_interceptor_end_transaction(interceptor);
        array_listener_gum_detached[hook_id] = TRUE;
        array_listener_gum_detach_flushed[hook_id] =
            peak_general_controller_flush_teardown();
        if (!array_listener_gum_detach_flushed[hook_id]) {
            g_printerr("[peak] Gum detach for hook %lu (%s) did not flush; reattach disabled for this hook\n",
                       (unsigned long)hook_id,
                       peak_hook_strings[hook_id] != NULL ?
                           peak_hook_strings[hook_id] : "<unknown>");
        }
    }
    PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
    if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                      listener,
                                                      PEAK_DETACH_OPERATION_DETACH,
                                                      &finish_status)) {
        peak_detach_controller_abort_after_failed_finish("detach finish",
                                                        finish_status);
    }

    if (!helper_holds_threads) {
        peak_general_controller_resume_called_threads(hook_id,
                                                      controller_tid,
                                                      tid_keys,
                                                      mapped_ids,
                                                      pause_session_ids,
                                                      pause_status,
                                                      snapshot_count);
    }

    array_listener_detached[hook_id] = TRUE;
    peak_general_listener_note_detach_success_unlocked(hook_id,
                                                       peak_second());
    peak_general_listener_note_detach_profile_seconds_unlocked(hook_id);
    peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACHED);
    if (peak_general_controller_trace_enabled()) {
        unsigned int retry_count =
            peak_hook_retry_count != NULL ? peak_hook_retry_count[hook_id] : 0;
        PeakDetachStatus last_retry_status =
            peak_hook_last_retry_status != NULL ? peak_hook_last_retry_status[hook_id]
                                                : PEAK_DETACH_STATUS_SAFE;

        peak_general_controller_trace_mutation_detail(
            hook_id,
            PEAK_DETACH_OPERATION_DETACH,
            "success",
            helper_applied_physical_patch,
            finish_status,
            retry_count,
            peak_general_controller_pending_age_for_trace_unlocked(hook_id,
                                                                   peak_second()),
            1,
            peak_detach_controller_last_stop_window_us(),
            0,
            last_retry_status);
    }
    peak_general_controller_reset_retry_unlocked(hook_id);
    return TRUE;
}

static gboolean peak_general_controller_reattach_if_requested_unlocked(
    size_t hook_id,
    pthread_t controller_tid,
    pthread_t* tid_keys,
    size_t* mapped_ids,
    int* pause_session_ids,
    int* pause_status,
    size_t snapshot_count,
    gboolean snapshot_complete)
{
    if (!peak_general_hook_is_published_unlocked(hook_id) ||
        peak_hook_states[hook_id] != PEAK_HOOK_REATTACH_REQUESTED) {
        return FALSE;
    }

    {
        PeakDetachStatus prepare_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_prepare_hook_mutation(hook_id,
                                                           array_listener[hook_id],
                                                           PEAK_DETACH_OPERATION_REATTACH,
                                                           &prepare_status)) {
            peak_general_controller_handle_prepare_failure_unlocked(hook_id,
                                                                    PEAK_HOOK_DETACHED,
                                                                    prepare_status);
            return FALSE;
        }
    }

    gboolean helper_holds_threads = peak_detach_controller_threads_are_held();
    gboolean helper_applied_physical_patch =
        peak_detach_controller_current_mutation_uses_physical_patch();

    if (!helper_holds_threads &&
        !peak_general_controller_snapshot_is_safe(hook_id,
                                                  mapped_ids,
                                                  snapshot_count,
                                                  snapshot_complete)) {
        PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                          array_listener[hook_id],
                                                          PEAK_DETACH_OPERATION_REATTACH,
                                                          &finish_status)) {
            peak_detach_controller_abort_after_failed_finish("reattach snapshot abort",
                                                            finish_status);
        }
        peak_general_controller_trace_mutation(hook_id,
                                               PEAK_DETACH_OPERATION_REATTACH,
                                               "snapshot-unsafe",
                                               helper_applied_physical_patch,
                                               finish_status);
        peak_general_controller_reset_retry_unlocked(hook_id);
        peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACHED);
        return FALSE;
    }

    peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_REATTACHING);
    if (!helper_holds_threads &&
        !peak_general_controller_pause_called_threads(hook_id,
                                                      controller_tid,
                                                      tid_keys,
                                                      mapped_ids,
                                                      pause_session_ids,
                                                      pause_status,
                                                      snapshot_count)) {
        PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                          array_listener[hook_id],
                                                          PEAK_DETACH_OPERATION_REATTACH,
                                                          &finish_status)) {
            peak_detach_controller_abort_after_failed_finish("reattach pause abort",
                                                            finish_status);
        }
        peak_general_controller_trace_mutation(hook_id,
                                               PEAK_DETACH_OPERATION_REATTACH,
                                               "pause-failed",
                                               helper_applied_physical_patch,
                                               finish_status);
        peak_general_controller_resume_called_threads(hook_id,
                                                      controller_tid,
                                                      tid_keys,
                                                      mapped_ids,
                                                      pause_session_ids,
                                                      pause_status,
                                                      snapshot_count);
        peak_general_controller_reset_retry_unlocked(hook_id);
        peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACHED);
        return FALSE;
    }

    GumAttachReturn attach_status = GUM_ATTACH_OK;
    if (!helper_applied_physical_patch) {
        gum_interceptor_begin_transaction(interceptor);
        attach_status =
            peak_general_listener_attach_exact(interceptor,
                                               hook_address[hook_id],
                                               array_listener[hook_id]);
        gum_interceptor_end_transaction(interceptor);
    }
    PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
    if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                      array_listener[hook_id],
                                                      PEAK_DETACH_OPERATION_REATTACH,
                                                      &finish_status)) {
        peak_detach_controller_abort_after_failed_finish("reattach finish",
                                                        finish_status);
    }

    if (!helper_holds_threads) {
        peak_general_controller_resume_called_threads(hook_id,
                                                      controller_tid,
                                                      tid_keys,
                                                      mapped_ids,
                                                      pause_session_ids,
                                                      pause_status,
                                                      snapshot_count);
    }

    if (attach_status != GUM_ATTACH_OK) {
        g_printerr("[peak] Gum reattach failed for hook %lu (%s), status=%d\n",
                   (unsigned long)hook_id,
                   peak_hook_strings[hook_id] != NULL ? peak_hook_strings[hook_id] : "<unknown>",
                   attach_status);
        peak_general_controller_trace_mutation(hook_id,
                                               PEAK_DETACH_OPERATION_REATTACH,
                                               "gum-failed",
                                               helper_applied_physical_patch,
                                               finish_status);
        peak_general_controller_reset_retry_unlocked(hook_id);
        peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACHED);
        return FALSE;
    }

    array_listener_gum_detached[hook_id] = FALSE;
    array_listener_gum_detach_flushed[hook_id] = TRUE;
    array_listener_reattached[hook_id] = TRUE;
    peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_ATTACHED);
    peak_general_listener_note_reattach_success_unlocked(hook_id);
    if (peak_general_controller_trace_enabled()) {
        unsigned int retry_count =
            peak_hook_retry_count != NULL ? peak_hook_retry_count[hook_id] : 0;
        PeakDetachStatus last_retry_status =
            peak_hook_last_retry_status != NULL ? peak_hook_last_retry_status[hook_id]
                                                : PEAK_DETACH_STATUS_SAFE;

        peak_general_controller_trace_mutation_detail(
            hook_id,
            PEAK_DETACH_OPERATION_REATTACH,
            "success",
            helper_applied_physical_patch,
            finish_status,
            retry_count,
            peak_general_controller_pending_age_for_trace_unlocked(hook_id,
                                                                   peak_second()),
            1,
            peak_detach_controller_last_stop_window_us(),
            0,
            last_retry_status);
    }
    peak_general_controller_reset_retry_unlocked(hook_id);
    return TRUE;
}

static gboolean peak_general_controller_shutdown_hook_unlocked(size_t hook_id,
                                                               pthread_t controller_tid,
                                                               pthread_t* tid_keys,
                                                               size_t* mapped_ids,
                                                               int* pause_session_ids,
                                                               int* pause_status,
                                                               size_t snapshot_count,
                                                               gboolean snapshot_complete,
                                                               PeakGeneralShutdownFailure* failure)
{
    if (!peak_general_hook_is_published_unlocked(hook_id)) {
        peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_SHUTDOWN);
        return TRUE;
    }

    if ((peak_hook_states[hook_id] == PEAK_HOOK_DETACHED ||
         peak_hook_states[hook_id] == PEAK_HOOK_REATTACH_REQUESTED) &&
        !array_listener_gum_detached[hook_id]) {
        PeakDetachStatus prepare_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_prepare_hook_mutation(hook_id,
                                                           array_listener[hook_id],
                                                           PEAK_DETACH_OPERATION_SHUTDOWN,
                                                           &prepare_status)) {
            peak_general_shutdown_failure_note(
                failure,
                PEAK_GENERAL_SHUTDOWN_FAILURE_PREPARE,
                prepare_status);
            return FALSE;
        }
        gboolean helper_applied_physical_patch =
            peak_detach_controller_current_mutation_uses_physical_patch();
        if (!peak_detach_controller_threads_are_held() &&
            !peak_general_controller_snapshot_is_safe(hook_id,
                                                      mapped_ids,
                                                      snapshot_count,
                                                      snapshot_complete)) {
            PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
            if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                              array_listener[hook_id],
                                                              PEAK_DETACH_OPERATION_SHUTDOWN,
                                                              &finish_status)) {
                peak_detach_controller_abort_after_failed_finish("detached shutdown snapshot abort",
                                                                finish_status);
            }
            peak_general_shutdown_failure_note(
                failure,
                PEAK_GENERAL_SHUTDOWN_FAILURE_SNAPSHOT_UNSAFE,
                PEAK_DETACH_STATUS_CLASSIFY_FAILED);
            peak_general_controller_trace_mutation(hook_id,
                                                   PEAK_DETACH_OPERATION_SHUTDOWN,
                                                   "snapshot-unsafe",
                                                   helper_applied_physical_patch,
                                                   finish_status);
            return FALSE;
        }
        PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
        gum_interceptor_begin_transaction(interceptor);
        gum_interceptor_detach(interceptor, array_listener[hook_id]);
        gum_interceptor_end_transaction(interceptor);
        array_listener_gum_detached[hook_id] = TRUE;
        array_listener_gum_detach_flushed[hook_id] = FALSE;
        if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                          array_listener[hook_id],
                                                          PEAK_DETACH_OPERATION_SHUTDOWN,
                                                          &finish_status)) {
            peak_detach_controller_abort_after_failed_finish("detached shutdown finish",
                                                            finish_status);
        }
        peak_general_controller_trace_mutation(hook_id,
                                               PEAK_DETACH_OPERATION_SHUTDOWN,
                                               "success",
                                               helper_applied_physical_patch,
                                               finish_status);
    } else if (peak_hook_states[hook_id] != PEAK_HOOK_DETACHED &&
               peak_hook_states[hook_id] != PEAK_HOOK_REATTACH_REQUESTED &&
               peak_hook_states[hook_id] != PEAK_HOOK_SHUTDOWN) {
        PeakDetachStatus prepare_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_prepare_hook_mutation(hook_id,
                                                           array_listener[hook_id],
                                                           PEAK_DETACH_OPERATION_SHUTDOWN,
                                                           &prepare_status)) {
            peak_general_shutdown_failure_note(
                failure,
                PEAK_GENERAL_SHUTDOWN_FAILURE_PREPARE,
                prepare_status);
            return FALSE;
        }
        gboolean helper_holds_threads = peak_detach_controller_threads_are_held();
        gboolean helper_applied_physical_patch =
            peak_detach_controller_current_mutation_uses_physical_patch();

        if (!helper_holds_threads &&
            !peak_general_controller_snapshot_is_safe(hook_id,
                                                      mapped_ids,
                                                      snapshot_count,
                                                      snapshot_complete)) {
            PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
            if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                              array_listener[hook_id],
                                                              PEAK_DETACH_OPERATION_SHUTDOWN,
                                                              &finish_status)) {
                peak_detach_controller_abort_after_failed_finish("shutdown snapshot abort",
                                                                finish_status);
            }
            peak_general_shutdown_failure_note(
                failure,
                PEAK_GENERAL_SHUTDOWN_FAILURE_SNAPSHOT_UNSAFE,
                PEAK_DETACH_STATUS_CLASSIFY_FAILED);
            peak_general_controller_trace_mutation(hook_id,
                                                   PEAK_DETACH_OPERATION_SHUTDOWN,
                                                   "snapshot-unsafe",
                                                   helper_applied_physical_patch,
                                                   finish_status);
            return FALSE;
        }

        peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACHING);
        if (!helper_holds_threads &&
            !peak_general_controller_pause_called_threads(hook_id,
                                                          controller_tid,
                                                          tid_keys,
                                                          mapped_ids,
                                                          pause_session_ids,
                                                          pause_status,
                                                          snapshot_count)) {
            PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
            if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                              array_listener[hook_id],
                                                              PEAK_DETACH_OPERATION_SHUTDOWN,
                                                              &finish_status)) {
                peak_detach_controller_abort_after_failed_finish("shutdown pause abort",
                                                                finish_status);
            }
            peak_general_shutdown_failure_note(
                failure,
                PEAK_GENERAL_SHUTDOWN_FAILURE_PAUSE_FAILED,
                PEAK_DETACH_STATUS_ERROR);
            peak_general_controller_trace_mutation(hook_id,
                                                   PEAK_DETACH_OPERATION_SHUTDOWN,
                                                   "pause-failed",
                                                   helper_applied_physical_patch,
                                                   finish_status);
            peak_general_controller_resume_called_threads(hook_id,
                                                          controller_tid,
                                                          tid_keys,
                                                          mapped_ids,
                                                          pause_session_ids,
                                                          pause_status,
                                                          snapshot_count);
            return FALSE;
        }

        if (!array_listener_gum_detached[hook_id]) {
            gum_interceptor_begin_transaction(interceptor);
            gum_interceptor_detach(interceptor, array_listener[hook_id]);
            gum_interceptor_end_transaction(interceptor);
            array_listener_gum_detached[hook_id] = TRUE;
            array_listener_gum_detach_flushed[hook_id] = FALSE;
        }

        PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;
        if (!peak_general_controller_finish_hook_mutation(hook_id,
                                                          array_listener[hook_id],
                                                          PEAK_DETACH_OPERATION_SHUTDOWN,
                                                          &finish_status)) {
            peak_detach_controller_abort_after_failed_finish("shutdown finish",
                                                            finish_status);
        }
        peak_general_controller_trace_mutation(hook_id,
                                               PEAK_DETACH_OPERATION_SHUTDOWN,
                                               "success",
                                               helper_applied_physical_patch,
                                               finish_status);
        if (!helper_holds_threads) {
            peak_general_controller_resume_called_threads(hook_id,
                                                          controller_tid,
                                                          tid_keys,
                                                          mapped_ids,
                                                          pause_session_ids,
                                                          pause_status,
                                                          snapshot_count);
        }
    }

    peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_SHUTDOWN);
    peak_general_controller_reset_retry_unlocked(hook_id);
    return TRUE;
}

static gboolean peak_general_controller_flush_teardown(void)
{
    const unsigned int max_attempts = 100;

    for (unsigned int attempt = 0; attempt < max_attempts; attempt++) {
        if (gum_interceptor_flush(interceptor)) {
            return TRUE;
        }
        usleep(1000);
    }

    return gum_interceptor_flush(interceptor);
}

static gboolean
peak_general_controller_has_pending_unlocked(void)
{
    if (peak_hook_states == NULL) {
        return FALSE;
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (peak_hook_states[i] == PEAK_HOOK_DETACH_REQUESTED ||
            peak_hook_states[i] == PEAK_HOOK_REATTACH_REQUESTED) {
            return TRUE;
        }
    }

    return FALSE;
}

static void
peak_general_controller_observe_pending_for_trace_unlocked(size_t hook_id,
                                                           double now)
{
    if (peak_hook_pending_observed_time == NULL ||
        hook_id >= peak_hook_address_count) {
        return;
    }

    peak_general_controller_mark_pending_started_unlocked(hook_id, now);
}

static double
peak_general_controller_pending_age_for_trace_unlocked(size_t hook_id,
                                                       double now)
{
    if (peak_hook_pending_observed_time == NULL ||
        hook_id >= peak_hook_address_count ||
        peak_hook_pending_observed_time[hook_id] <= 0.0) {
        return 0.0;
    }

    return now - peak_hook_pending_observed_time[hook_id];
}

static size_t
peak_general_controller_collect_batch_unlocked(
    double now,
    PeakGeneralBatchCandidate* candidates,
    PeakDetachRequest* requests,
    size_t max_candidates)
{
    size_t count = 0;
    size_t total = peak_hook_address_count;
    size_t start = total > 0 ? peak_general_controller_batch_cursor % total : 0;
    size_t last_selected = start;
    gboolean selected_any = FALSE;

    for (size_t offset = 0; offset < total && count < max_candidates; offset++) {
        size_t i = (start + offset) % total;
        PeakDetachOperation operation;
        PeakHookState stable_state;

        if (peak_hook_states == NULL ||
            (peak_hook_states[i] != PEAK_HOOK_DETACH_REQUESTED &&
             peak_hook_states[i] != PEAK_HOOK_REATTACH_REQUESTED)) {
            continue;
        }
        if (!peak_general_controller_retry_ready_unlocked(i, now)) {
            continue;
        }
        if (!peak_general_hook_is_published_unlocked(i)) {
            continue;
        }

        if (peak_hook_states[i] == PEAK_HOOK_DETACH_REQUESTED) {
            operation = PEAK_DETACH_OPERATION_DETACH;
            stable_state = PEAK_HOOK_ATTACHED;
        } else {
            if (array_listener_gum_detached != NULL &&
                array_listener_gum_detach_flushed != NULL &&
                array_listener_gum_detached[i] &&
                !array_listener_gum_detach_flushed[i]) {
                continue;
            }
            operation = PEAK_DETACH_OPERATION_REATTACH;
            stable_state = PEAK_HOOK_DETACHED;
        }

        peak_general_controller_observe_pending_for_trace_unlocked(i, now);

        candidates[count] = (PeakGeneralBatchCandidate){
            .hook_id = i,
            .listener = array_listener[i],
            .operation = operation,
            .stable_state = stable_state,
            .retry_count = peak_hook_retry_count != NULL ? peak_hook_retry_count[i] : 0,
            .last_retry_status = peak_hook_last_retry_status != NULL
                                      ? peak_hook_last_retry_status[i]
                                      : PEAK_DETACH_STATUS_SAFE,
            .pending_age_s =
                peak_general_controller_pending_age_for_trace_unlocked(i, now)
        };
        requests[count] = (PeakDetachRequest){
            .hook_id = i,
            .symbol_name = peak_hook_strings != NULL ? peak_hook_strings[i] : NULL,
            .function_address = hook_address[i],
            .interceptor = interceptor,
            .listener = array_listener[i],
            .operation = operation
        };
        last_selected = i;
        selected_any = TRUE;
        count++;
    }

    if (selected_any && total > 0) {
        peak_general_controller_batch_cursor = (last_selected + 1) % total;
    }

    return count;
}

static gboolean
peak_general_controller_process_pending_batch_unlocked(void)
{
    PeakGeneralBatchCandidate candidates[PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES];
    PeakDetachRequest requests[PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES];
    PeakDetachBatchResult results[PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES];
    size_t prepared_count = 0;
    double now = peak_second();
    double stop_window_us = 0.0;
    unsigned int batch_id = 0;
    PeakDetachStatus batch_status = PEAK_DETACH_STATUS_ERROR;
    gboolean mutation_ok[PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES];
    GumAttachReturn attach_status[PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES];
    size_t candidate_count =
        peak_general_controller_collect_batch_unlocked(
            now,
            candidates,
            requests,
            PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES);

    if (candidate_count == 0) {
        return FALSE;
    }
    batch_id = peak_general_controller_allocate_batch_id_unlocked();
    for (size_t i = 0; i < candidate_count; i++) {
        mutation_ok[i] = TRUE;
        attach_status[i] = GUM_ATTACH_OK;
    }

    (void)peak_detach_controller_prepare_hook_mutation_batch(requests,
                                                             candidate_count,
                                                             results,
                                                             &prepared_count,
                                                             &batch_status);
    for (size_t i = 0; i < candidate_count; i++) {
        size_t hook_id = candidates[i].hook_id;

        if (!results[i].prepared) {
            continue;
        }

        if (candidates[i].operation == PEAK_DETACH_OPERATION_DETACH) {
            peak_general_controller_set_state_unlocked(hook_id,
                                                       PEAK_HOOK_DETACHING);
            if (!results[i].uses_physical_patch) {
                gum_interceptor_begin_transaction(interceptor);
                gum_interceptor_detach(interceptor, candidates[i].listener);
                gum_interceptor_end_transaction(interceptor);
                array_listener_gum_detached[hook_id] = TRUE;
                array_listener_gum_detach_flushed[hook_id] =
                    peak_general_controller_flush_teardown();
                if (!array_listener_gum_detach_flushed[hook_id]) {
                    g_printerr("[peak] Gum detach for hook %lu (%s) did not flush; reattach disabled for this hook\n",
                               (unsigned long)hook_id,
                               peak_hook_strings[hook_id] != NULL ?
                                   peak_hook_strings[hook_id] : "<unknown>");
                }
            }
        } else {
            peak_general_controller_set_state_unlocked(hook_id,
                                                       PEAK_HOOK_REATTACHING);
            if (!results[i].uses_physical_patch) {
                gum_interceptor_begin_transaction(interceptor);
                attach_status[i] =
                    peak_general_listener_attach_exact(interceptor,
                                                       hook_address[hook_id],
                                                       array_listener[hook_id]);
                gum_interceptor_end_transaction(interceptor);
                mutation_ok[i] = attach_status[i] == GUM_ATTACH_OK;
            }
        }
    }

    if (prepared_count > 0) {
        PeakDetachStatus finish_status = PEAK_DETACH_STATUS_ERROR;

        if (!peak_detach_controller_finish_hook_mutation_batch(&finish_status)) {
            peak_detach_controller_abort_after_failed_finish("batch finish",
                                                            finish_status);
        }
        stop_window_us = peak_detach_controller_last_stop_window_us();

        for (size_t i = 0; i < candidate_count; i++) {
            if (!results[i].prepared) {
                continue;
            }
            if (candidates[i].operation == PEAK_DETACH_OPERATION_DETACH) {
                array_listener_detached[candidates[i].hook_id] = TRUE;
                peak_general_listener_note_detach_success_unlocked(
                    candidates[i].hook_id,
                    peak_second());
                peak_general_listener_note_detach_profile_seconds_unlocked(
                    candidates[i].hook_id);
                peak_general_controller_set_state_unlocked(candidates[i].hook_id,
                                                           PEAK_HOOK_DETACHED);
                peak_general_controller_trace_mutation_detail(
                    candidates[i].hook_id,
                    candidates[i].operation,
                    "success",
                    results[i].uses_physical_patch,
                    finish_status,
                    candidates[i].retry_count,
                    candidates[i].pending_age_s,
                    (unsigned int)candidate_count,
                    stop_window_us,
                    batch_id,
                    candidates[i].last_retry_status);
                peak_general_controller_reset_retry_unlocked(candidates[i].hook_id);
                continue;
            }

            if (!mutation_ok[i]) {
                g_printerr("[peak] Gum reattach failed for hook %lu (%s), status=%d\n",
                           (unsigned long)candidates[i].hook_id,
                           peak_hook_strings[candidates[i].hook_id] != NULL ?
                               peak_hook_strings[candidates[i].hook_id] : "<unknown>",
                           attach_status[i]);
                peak_general_controller_trace_mutation_detail(
                    candidates[i].hook_id,
                    candidates[i].operation,
                    "gum-failed",
                    results[i].uses_physical_patch,
                    finish_status,
                    candidates[i].retry_count,
                    candidates[i].pending_age_s,
                    (unsigned int)candidate_count,
                    stop_window_us,
                    batch_id,
                    candidates[i].last_retry_status);
                peak_general_controller_reset_retry_unlocked(candidates[i].hook_id);
                peak_general_controller_set_state_unlocked(candidates[i].hook_id,
                                                           PEAK_HOOK_DETACHED);
                continue;
            }

            array_listener_gum_detached[candidates[i].hook_id] = FALSE;
            array_listener_gum_detach_flushed[candidates[i].hook_id] = TRUE;
            array_listener_reattached[candidates[i].hook_id] = TRUE;
            peak_general_controller_set_state_unlocked(candidates[i].hook_id,
                                                       PEAK_HOOK_ATTACHED);
            peak_general_listener_note_reattach_success_unlocked(
                candidates[i].hook_id);
            peak_general_controller_trace_mutation_detail(
                candidates[i].hook_id,
                candidates[i].operation,
                "success",
                results[i].uses_physical_patch,
                finish_status,
                candidates[i].retry_count,
                candidates[i].pending_age_s,
                (unsigned int)candidate_count,
                stop_window_us,
                batch_id,
                candidates[i].last_retry_status);
            peak_general_controller_reset_retry_unlocked(candidates[i].hook_id);
        }
    }

    for (size_t i = 0; i < candidate_count; i++) {
        size_t hook_id = candidates[i].hook_id;

        if (results[i].prepared) {
            continue;
        }

        peak_general_controller_handle_prepare_failure_unlocked(
            hook_id,
            candidates[i].stable_state,
            results[i].status);
        peak_general_controller_trace_mutation_detail(
            hook_id,
            candidates[i].operation,
            "prepare-failed",
            FALSE,
            results[i].status,
            peak_hook_retry_count != NULL ? peak_hook_retry_count[hook_id] : 0,
            candidates[i].pending_age_s,
            (unsigned int)candidate_count,
            stop_window_us,
            batch_id,
            peak_hook_last_retry_status != NULL
                ? peak_hook_last_retry_status[hook_id]
                : PEAK_DETACH_STATUS_SAFE);
    }

    return prepared_count > 0;
}

static gboolean
peak_general_controller_process_pending_unlocked(pthread_t controller_tid,
                                                 pthread_t* tid_keys,
                                                 size_t* mapped_ids,
                                                 int* pause_session_ids,
                                                 int* pause_status)
{
    gboolean did_work = FALSE;
    double now = peak_second();

    if (peak_general_controller_mpi_finalize_requested()) {
        return FALSE;
    }

    if (peak_detach_controller_strict_batch_supported()) {
        (void)controller_tid;
        (void)tid_keys;
        (void)mapped_ids;
        (void)pause_session_ids;
        (void)pause_status;
        return peak_general_controller_process_pending_batch_unlocked();
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        gboolean snapshot_complete = FALSE;
        size_t snapshot_count;

        if (peak_hook_states == NULL ||
            (peak_hook_states[i] != PEAK_HOOK_DETACH_REQUESTED &&
             peak_hook_states[i] != PEAK_HOOK_REATTACH_REQUESTED)) {
            continue;
        }
        if (!peak_general_controller_retry_ready_unlocked(i, now)) {
            continue;
        }

        snapshot_count = pthread_listener_snapshot_threads(tid_keys,
                                                           mapped_ids,
                                                           peak_max_num_threads,
                                                           &snapshot_complete);
        for (size_t s = 0; s < snapshot_count; s++) {
            pause_session_ids[s] = -1;
            pause_status[s] = -1;
        }

        if (peak_hook_states[i] == PEAK_HOOK_DETACH_REQUESTED) {
            did_work |= peak_general_controller_detach_if_requested_unlocked(i,
                                                                            array_listener[i],
                                                                            controller_tid,
                                                                            tid_keys,
                                                                            mapped_ids,
                                                                            pause_session_ids,
                                                                            pause_status,
                                                                            snapshot_count,
                                                                            snapshot_complete);
        } else if (peak_hook_states[i] == PEAK_HOOK_REATTACH_REQUESTED) {
            did_work |= peak_general_controller_reattach_if_requested_unlocked(i,
                                                                              controller_tid,
                                                                              tid_keys,
                                                                              mapped_ids,
                                                                              pause_session_ids,
                                                                              pause_status,
                                                                              snapshot_count,
                                                                              snapshot_complete);
        }
    }

    return did_work;
}

gboolean
peak_general_listener_controller_drain(unsigned int timeout_ms)
{
    if (interceptor == NULL ||
        peak_hook_states == NULL ||
        peak_hook_address_count == 0) {
        return TRUE;
    }

    if (peak_general_controller_mpi_finalize_requested()) {
        return TRUE;
    }

    pthread_t controller_tid = pthread_self();
    pthread_t* tid_keys = g_new0(pthread_t, peak_max_num_threads);
    size_t* mapped_ids = g_new0(size_t, peak_max_num_threads);
    int* pause_session_ids = g_new0(int, peak_max_num_threads);
    int* pause_status = g_new0(int, peak_max_num_threads);
    double deadline = peak_second() + ((double)timeout_ms / 1000.0);
    gboolean drained = FALSE;

    gum_interceptor_ignore_current_thread(interceptor);

    for (;;) {
        gboolean pending;

        pthread_mutex_lock(&lock);
        (void)peak_general_controller_process_pending_unlocked(controller_tid,
                                                               tid_keys,
                                                               mapped_ids,
                                                               pause_session_ids,
                                                               pause_status);
        pending = peak_general_controller_has_pending_unlocked();
        pthread_mutex_unlock(&lock);

        if (!pending) {
            drained = TRUE;
            break;
        }
        if (peak_second() >= deadline) {
            break;
        }

        peak_general_listener_controller_wake();
        usleep(1000);
    }

    g_free(pause_status);
    g_free(pause_session_ids);
    g_free(mapped_ids);
    g_free(tid_keys);

    gum_interceptor_unignore_current_thread(interceptor);

    return drained;
}

static void*
peak_general_controller_thread_main(void* arg)
{
    (void)arg;

    pthread_t controller_tid = pthread_self();
    pthread_t* tid_keys = g_new0(pthread_t, peak_max_num_threads);
    size_t* mapped_ids = g_new0(size_t, peak_max_num_threads);
    int* pause_session_ids = g_new0(int, peak_max_num_threads);
    int* pause_status = g_new0(int, peak_max_num_threads);

    general_controller_owner_thread = pthread_self();
    atomic_store(&general_controller_owner_known, TRUE);

    gum_interceptor_ignore_current_thread(interceptor);

    for (;;) {
        gboolean should_run;

        pthread_mutex_lock(&general_controller_wake_mutex);
        should_run = general_controller_running;
        pthread_mutex_unlock(&general_controller_wake_mutex);
        if (!should_run) {
            double deadline =
                peak_second() +
                ((double)peak_general_controller_shutdown_drain_ms() / 1000.0);
            while (peak_jit_provider_drain_pending()) {
                if (peak_second() >= deadline) {
                    (void)peak_jit_provider_drain_pending_force_not_exec_timeout();
                    break;
                }
                usleep(1000);
            }
            break;
        }

        pthread_mutex_lock(&lock);
        (void)peak_general_controller_process_pending_unlocked(controller_tid,
                                                               tid_keys,
                                                               mapped_ids,
                                                               pause_session_ids,
                                                               pause_status);
        pthread_mutex_unlock(&lock);

        dlopen_interceptor_drain_dynamic_attach_queue();
        peak_jit_provider_drain_pending();

        pthread_mutex_lock(&general_controller_wake_mutex);
        if (general_controller_running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            (void)pthread_cond_timedwait(&general_controller_wake_cond,
                                         &general_controller_wake_mutex,
                                         &ts);
        }
        pthread_mutex_unlock(&general_controller_wake_mutex);
    }

    gum_interceptor_unignore_current_thread(interceptor);
    atomic_store(&general_controller_owner_known, FALSE);

    g_free(pause_status);
    g_free(pause_session_ids);
    g_free(mapped_ids);
    g_free(tid_keys);

    return NULL;
}

static void
peak_general_controller_start(void)
{
    pthread_mutex_lock(&general_controller_wake_mutex);
    if (!general_controller_thread_started) {
        general_controller_running = TRUE;
        if (pthread_create(&general_controller_thread,
                           NULL,
                           peak_general_controller_thread_main,
                           NULL) == 0) {
            general_controller_thread_started = TRUE;
        } else {
            general_controller_running = FALSE;
            g_printerr("[peak] failed to start general detach controller thread\n");
        }
    }
    pthread_mutex_unlock(&general_controller_wake_mutex);
}

void
peak_general_listener_controller_stop(void)
{
    gboolean should_join;
    pthread_t thread;
    unsigned int shutdown_drain_ms =
        peak_general_controller_shutdown_drain_ms();

    if (!peak_general_listener_controller_drain(shutdown_drain_ms)) {
        g_printerr("[peak] timed out draining pending target hook detach/reattach requests before controller shutdown\n");
    }

    pthread_mutex_lock(&general_controller_wake_mutex);
    should_join = general_controller_thread_started;
    if (should_join) {
        general_controller_running = FALSE;
        thread = general_controller_thread;
        pthread_cond_broadcast(&general_controller_wake_cond);
    }
    pthread_mutex_unlock(&general_controller_wake_mutex);

    if (should_join) {
        pthread_join(thread, NULL);
        pthread_mutex_lock(&general_controller_wake_mutex);
        general_controller_thread_started = FALSE;
        pthread_mutex_unlock(&general_controller_wake_mutex);
    }
}
