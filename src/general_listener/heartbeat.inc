
typedef struct {
    size_t index;
    double ratio;  // detach pressure, or reattach historical seconds
    double rate;   // detach contribution key, or oldest-detach ordering key
    double lifetime;
} OverheadEntry;

// ratio descending (global detach)
static int compare_ratio_de(const void* a, const void* b) {
    const OverheadEntry* x = (const OverheadEntry*)a;
    const OverheadEntry* y = (const OverheadEntry*)b;

    if (x->ratio < y->ratio) return 1;
    if (x->ratio > y->ratio) return -1;

    // tie-break: faster-growing overhead first
    if (x->rate < y->rate) return 1;
    if (x->rate > y->rate) return -1;
    if (x->index < y->index) return -1;
    if (x->index > y->index) return 1;
    return 0;
}

// rate ascending (global reattach)
static int compare_rate_inc(const void* a, const void* b) {
    const OverheadEntry* x = (const OverheadEntry*)a;
    const OverheadEntry* y = (const OverheadEntry*)b;

    if (x->rate < y->rate) return -1;
    if (x->rate > y->rate) return 1;

    // tie-break: smaller historical projection first
    if (x->ratio < y->ratio) return -1;
    if (x->ratio > y->ratio) return 1;
    if (x->index < y->index) return -1;
    if (x->index > y->index) return 1;
    return 0;
}

static inline double clipd(double x, double lo, double hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static inline gulong
peak_general_listener_num_calls_load(const gulong* slot)
{
    return __atomic_load_n(slot, __ATOMIC_RELAXED);
}

static inline gulong
peak_general_listener_num_calls_increment(gulong* slot)
{
    return __atomic_add_fetch(slot, 1, __ATOMIC_RELAXED);
}

static double
peak_general_listener_profile_seconds_for_calls_unlocked(size_t hook_id,
                                                         gulong calls)
{
    double profile_seconds = (double)calls * peak_general_overhead;

    if (heartbeat_overhead != NULL && hook_id < peak_hook_address_count) {
        profile_seconds += heartbeat_overhead[hook_id];
    }
    return profile_seconds;
}

static double
peak_general_listener_profile_seconds_at_boundary(void)
{
    double profile_seconds = 0.0;

    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address == NULL ||
            array_listener == NULL ||
            !hook_address[i] ||
            array_listener[i] == NULL) {
            continue;
        }

        PeakGeneralListener* pg_listener =
            PEAKGENERAL_LISTENER(array_listener[i]);
        gulong total_calls = 0;
        for (size_t j = 0; j < peak_max_num_threads; j++) {
            total_calls +=
                peak_general_listener_num_calls_load(
                    &pg_listener->num_calls[j]);
        }
        if (total_calls != 0 &&
            profile_seconds <=
                DBL_MAX - ((double)total_calls * peak_general_overhead)) {
            profile_seconds += (double)total_calls * peak_general_overhead;
        }
    }
    pthread_mutex_unlock(&lock);

    double heartbeat_profile_seconds =
        peak_general_listener_report_heartbeat_overhead_seconds();
    if (peak_general_listener_positive_finite(heartbeat_profile_seconds) &&
        profile_seconds <= DBL_MAX - heartbeat_profile_seconds) {
        profile_seconds += heartbeat_profile_seconds;
    }

    return profile_seconds;
}

void
peak_general_listener_freeze_final_report_snapshot(void)
{
    PeakDetachAccountingSnapshot accounting;
    PeakFinalReportSnapshot snapshot = {0};
    double control_risk_seconds;
    double profile_control_risk_seconds = DBL_MAX;

    snapshot.accounting_valid =
        peak_general_listener_runtime_accounting_snapshot(&accounting);
    snapshot.valid = TRUE;
    snapshot.local_ranks = peak_general_listener_local_mpi_ranks();
    snapshot.stop_window_count =
        peak_general_listener_control_window_count_since_heartbeat(
            &accounting);
    snapshot.failed_stop_window_count =
        peak_general_listener_failed_window_count_since_heartbeat(
            &accounting);
    snapshot.elapsed_seconds = peak_main_time;
    snapshot.profile_seconds =
        peak_general_listener_profile_seconds_at_boundary();
    snapshot.control_seconds =
        (double)peak_general_listener_control_wall_ns_since_heartbeat(
            &accounting) / 1e9;
    snapshot.management_seconds =
        peak_general_listener_management_cpu_seconds();
    control_risk_seconds =
        peak_general_listener_control_risk_seconds(snapshot.control_seconds);
    (void)peak_general_listener_add_nonnegative_finite(
        snapshot.profile_seconds,
        control_risk_seconds,
        &profile_control_risk_seconds);
    snapshot.control_risk_seconds = control_risk_seconds;
    snapshot.profile_control_risk_seconds = profile_control_risk_seconds;
    if (snapshot.elapsed_seconds > 0.0) {
        snapshot.profile_ratio =
            snapshot.profile_seconds / snapshot.elapsed_seconds;
        snapshot.control_ratio =
            snapshot.control_seconds / snapshot.elapsed_seconds;
        snapshot.profile_control_risk_ratio =
            profile_control_risk_seconds / snapshot.elapsed_seconds;
        snapshot.control_risk_ratio =
            control_risk_seconds / snapshot.elapsed_seconds;
        snapshot.ratio =
            (snapshot.profile_seconds + snapshot.control_seconds) /
            snapshot.elapsed_seconds;
        snapshot.management_ratio =
            snapshot.management_seconds / snapshot.elapsed_seconds;
    }

    peak_general_listener_final_report_snapshot = snapshot;
}

static void
peak_general_listener_cache_hook_sample_unlocked(size_t hook_id,
                                                 gulong calls,
                                                 double profile_seconds)
{
    if (hook_id >= peak_hook_address_count ||
        peak_hook_cached_sample_calls == NULL ||
        peak_hook_cached_sample_profile_seconds == NULL ||
        peak_hook_cached_sample_valid == NULL) {
        return;
    }

    peak_hook_cached_sample_calls[hook_id] = calls;
    peak_hook_cached_sample_profile_seconds[hook_id] = profile_seconds;
    peak_hook_cached_sample_valid[hook_id] = TRUE;
}

static gboolean
peak_general_listener_try_hook_sample_cache_unlocked(
    size_t hook_id,
    gulong* calls_out,
    double* profile_seconds_out)
{
    if (hook_id >= peak_hook_address_count ||
        peak_hook_cached_sample_calls == NULL ||
        peak_hook_cached_sample_profile_seconds == NULL ||
        peak_hook_cached_sample_valid == NULL ||
        !peak_hook_cached_sample_valid[hook_id]) {
        return FALSE;
    }

    if (calls_out != NULL) {
        *calls_out = peak_hook_cached_sample_calls[hook_id];
    }
    if (profile_seconds_out != NULL) {
        *profile_seconds_out =
            peak_hook_cached_sample_profile_seconds[hook_id];
    }
    return TRUE;
}

static gboolean
peak_general_listener_refresh_hook_sample_cache_unlocked(
    size_t hook_id,
    gulong* calls_out,
    double* profile_seconds_out)
{
    if (hook_id >= peak_hook_address_count ||
        hook_address == NULL ||
        array_listener == NULL ||
        hook_address[hook_id] == NULL ||
        array_listener[hook_id] == NULL) {
        return FALSE;
    }

    PeakGeneralListener* pg_listener =
        PEAKGENERAL_LISTENER(array_listener[hook_id]);
    gulong total_num_calls = 0;
    for (size_t j = 0; j < peak_max_num_threads; j++) {
        total_num_calls += peak_general_listener_num_calls_load(
            &pg_listener->num_calls[j]);
    }

    double profile_seconds =
        peak_general_listener_profile_seconds_for_calls_unlocked(
            hook_id,
            total_num_calls);
    peak_general_listener_cache_hook_sample_unlocked(hook_id,
                                                     total_num_calls,
                                                     profile_seconds);
    if (calls_out != NULL) {
        *calls_out = total_num_calls;
    }
    if (profile_seconds_out != NULL) {
        *profile_seconds_out = profile_seconds;
    }
    return TRUE;
}

static gboolean
peak_general_listener_heartbeat_sample_can_reuse_cache_unlocked(
    size_t hook_id)
{
    if (hook_id >= peak_hook_address_count || peak_hook_states == NULL) {
        return FALSE;
    }

    switch (peak_hook_states[hook_id]) {
        case PEAK_HOOK_DETACHED:
        case PEAK_HOOK_REATTACH_REQUESTED:
            return TRUE;
        case PEAK_HOOK_REATTACHING:
            /*
             * The controller has entered the physical reattach path. Keep
             * scanning so the first attached sample is seeded from live slots.
             */
            return FALSE;
        default:
            return FALSE;
    }
}

static gboolean
peak_heartbeat_wait_us(unsigned int sleep_us)
{
    pthread_mutex_lock(&heartbeat_mutex);
    if (!atomic_load(&heartbeat_running)) {
        pthread_mutex_unlock(&heartbeat_mutex);
        return FALSE;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += sleep_us / 1000000U;
    ts.tv_nsec += (long)(sleep_us % 1000000U) * 1000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    (void)pthread_cond_timedwait(&heartbeat_cond, &heartbeat_mutex, &ts);
    gboolean keep_running = atomic_load(&heartbeat_running);
    pthread_mutex_unlock(&heartbeat_mutex);
    return keep_running;
}

void* peak_heartbeat_monitor(void* arg) {
    PeakHeartbeatArgs* heartbeat_args = (PeakHeartbeatArgs*)arg;
    unsigned int heartbeat_time = heartbeat_args->heartbeat_time;   // base sleep (us)
    unsigned int check_interval = heartbeat_args->check_interval;
    unsigned int hb_min_us = heartbeat_args->hb_min_us;
    unsigned int hb_max_us = heartbeat_args->hb_max_us;
    double hb_k_err = heartbeat_args->hb_k_err;
    double hb_k_rate = heartbeat_args->hb_k_rate;
    double hb_ema_a = heartbeat_args->hb_ema_a;
    unsigned int heartbeat_counter = 0;
    const unsigned int local_mpi_ranks =
        peak_general_listener_local_mpi_ranks();

    gum_interceptor_ignore_current_thread(interceptor);

    OverheadEntry* entries = g_new0(OverheadEntry, peak_hook_address_count);

    gulong* calls_snapshot = g_new0(gulong, peak_hook_address_count);
    double* profile_seconds_snapshot = g_new0(double, peak_hook_address_count);
    double* lifetime_snapshot = g_new0(double, peak_hook_address_count);
    double* ratio_snapshot = g_new0(double, peak_hook_address_count);
    double* rate_snapshot = g_new0(double, peak_hook_address_count);
    gulong* previous_calls = g_new0(gulong, peak_hook_address_count);
    double* previous_time = g_new0(double, peak_hook_address_count);
    gboolean* recent_baseline_ready = g_new0(gboolean, peak_hook_address_count);

    double now0 = peak_second();
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        calls_snapshot[i] = 0;
        profile_seconds_snapshot[i] = 0.0;
        lifetime_snapshot[i] = 0.0;
        ratio_snapshot[i] = 0.0;
        rate_snapshot[i] = 0.0;
        previous_calls[i] = 0;
        previous_time[i] = now0;
        recent_baseline_ready[i] = TRUE;
    }

    // ------------------------------
    // Global dynamics state
    // ------------------------------
    double prev_global_overhead = 0.0;
    double prev_global_time     = now0;
    double ema_global_rate      = 0.0;
    size_t heartbeat_capacity = peak_hook_address_count;
    double min_detach_observation_time =
        (double)MAX(MAX(heartbeat_time, hb_min_us),
                    PEAK_HEARTBEAT_MIN_OBSERVATION_US) / 1e6;

    if (heartbeat_time > 0) {
        unsigned int initial_sleep_us =
            (unsigned int)clipd((double)heartbeat_time,
                                (double)hb_min_us,
                                (double)hb_max_us);
        if (!peak_heartbeat_wait_us(initial_sleep_us)) {
            goto cleanup;
        }
    }

    while (atomic_load(&heartbeat_running)) {
        unsigned long long management_cpu_checkpoint_ns = 0;
        gboolean management_cpu_checkpoint_valid =
            peak_general_listener_thread_cpu_ns(
                &management_cpu_checkpoint_ns);
        gboolean wake_controller = FALSE;
        heartbeat_counter++;
        double now = peak_second();

        pthread_mutex_lock(&lock);
        size_t current_hook_count = peak_hook_address_count;
        pthread_mutex_unlock(&lock);
        if (current_hook_count > heartbeat_capacity) {
            size_t old_capacity = heartbeat_capacity;
            entries = g_renew(OverheadEntry, entries, current_hook_count);
            calls_snapshot = g_renew(gulong, calls_snapshot, current_hook_count);
            profile_seconds_snapshot =
                g_renew(double, profile_seconds_snapshot, current_hook_count);
            lifetime_snapshot =
                g_renew(double, lifetime_snapshot, current_hook_count);
            ratio_snapshot =
                g_renew(double, ratio_snapshot, current_hook_count);
            rate_snapshot = g_renew(double, rate_snapshot, current_hook_count);
            previous_calls = g_renew(gulong, previous_calls, current_hook_count);
            previous_time = g_renew(double, previous_time, current_hook_count);
            recent_baseline_ready =
                g_renew(gboolean, recent_baseline_ready, current_hook_count);
            for (size_t i = old_capacity; i < current_hook_count; i++) {
                calls_snapshot[i] = 0;
                profile_seconds_snapshot[i] = 0.0;
                lifetime_snapshot[i] = 0.0;
                ratio_snapshot[i] = 0.0;
                rate_snapshot[i] = 0.0;
                previous_calls[i] = 0;
                previous_time[i] = now;
                recent_baseline_ready[i] = FALSE;
            }
            heartbeat_capacity = current_hook_count;
        }

        double total_execution_time = now - peak_main_time;
        if (total_execution_time <= 0.0) total_execution_time = 1e-12;
        double heartbeat_observation_horizon_seconds =
            (double)hb_max_us / 1e6;
        gboolean detach_observation_ready =
            total_execution_time >= min_detach_observation_time;

        double profile_spent_seconds = 0.0;
        double control_spent_seconds = 0.0;
        double spent_ratio = 0.0;
        double attached_recent_sum = 0.0;
        double attached_lifetime_sum = 0.0;
        double attached_pressure = 0.0;
        double profile_global_overhead = 0.0;
        double projected_pending_reattach_seconds = 0.0;
        uint64_t control_pause_wall_ns = 0;
        PeakDetachAccountingSnapshot detach_accounting;

        pthread_mutex_lock(&lock);
        for (size_t i = 0; i < heartbeat_capacity; i++) {
            if (!(hook_address[i] && array_listener[i])) {
                calls_snapshot[i] = 0;
                profile_seconds_snapshot[i] = 0.0;
                lifetime_snapshot[i] = 0.0;
                ratio_snapshot[i] = 0.0;
                rate_snapshot[i] = 0.0;
                previous_calls[i] = 0;
                previous_time[i] = now;
                recent_baseline_ready[i] = FALSE;
                if (peak_hook_cached_sample_valid != NULL &&
                    i < peak_hook_address_count) {
                    peak_hook_cached_sample_valid[i] = FALSE;
                }
                continue;
            }

            gulong total_num_calls = 0;
            double hook_profile_spent_seconds = 0.0;
            gboolean reuse_cached_sample =
                peak_general_listener_heartbeat_sample_can_reuse_cache_unlocked(
                    i);
            gboolean have_sample =
                reuse_cached_sample &&
                peak_general_listener_try_hook_sample_cache_unlocked(
                    i,
                    &total_num_calls,
                    &hook_profile_spent_seconds);
            if (!have_sample) {
                have_sample =
                    peak_general_listener_refresh_hook_sample_cache_unlocked(
                        i,
                        &total_num_calls,
                        &hook_profile_spent_seconds);
            }
            if (!have_sample) {
                calls_snapshot[i] = 0;
                profile_seconds_snapshot[i] = 0.0;
                lifetime_snapshot[i] = 0.0;
                ratio_snapshot[i] = 0.0;
                rate_snapshot[i] = 0.0;
                previous_calls[i] = 0;
                previous_time[i] = now;
                recent_baseline_ready[i] = FALSE;
                continue;
            }

            calls_snapshot[i] = total_num_calls;
            profile_seconds_snapshot[i] = hook_profile_spent_seconds;
            double ratio = hook_profile_spent_seconds / total_execution_time;
            lifetime_snapshot[i] = ratio;

            // g_printerr ("ratio %ld: %ld\n", i, total_num_calls);

            double recent_rate = 0.0;
            if (!reuse_cached_sample && recent_baseline_ready[i]) {
                double dt = now - previous_time[i];
                if (dt <= 1e-12) dt = 1e-12;
                gulong previous_num_calls = previous_calls[i];
                gulong recent_calls =
                    total_num_calls >= previous_num_calls
                        ? total_num_calls - previous_num_calls
                        : total_num_calls;
                recent_rate =
                    ((double)recent_calls * peak_general_overhead) / dt;
            }
            rate_snapshot[i] = recent_rate;
            ratio_snapshot[i] = ratio;

            previous_calls[i] = total_num_calls;
            previous_time[i] = now;
            recent_baseline_ready[i] = TRUE;
            profile_spent_seconds += hook_profile_spent_seconds;

            if (peak_general_listener_hook_is_attached_for_policy_unlocked(i)) {
                attached_recent_sum += recent_rate;
                attached_lifetime_sum += ratio;
            }
        }
        pthread_mutex_unlock(&lock);

        peak_general_listener_runtime_accounting_snapshot(&detach_accounting);
        control_pause_wall_ns =
            peak_general_listener_control_wall_ns_since_heartbeat(
                &detach_accounting);
        control_spent_seconds = (double)control_pause_wall_ns / 1e9;
        peak_general_listener_management_cpu_checkpoint(
            &management_cpu_checkpoint_ns,
            &management_cpu_checkpoint_valid);
        spent_ratio =
            (profile_spent_seconds + control_spent_seconds) /
            total_execution_time;
        attached_pressure = MAX(attached_recent_sum, attached_lifetime_sum);
        profile_global_overhead = attached_pressure;
        double projected_attached_recent_sum = attached_recent_sum;
        double projected_attached_lifetime_sum = attached_lifetime_sum;

        // g_printerr ("profile_global_overhead %.3e spent_ratio %.3e\n", profile_global_overhead, spent_ratio);

        // ------------------------------------------------------------
        // 1) Per-target DETACH
        // ------------------------------------------------------------
        if (detach_observation_ready && enable_per_target_heartbeat) {
            pthread_mutex_lock(&lock);
            for (size_t i = 0; i < heartbeat_capacity; i++) {
                if (!(hook_address[i] && array_listener[i])) continue;
                if (!peak_general_listener_hook_is_attached_for_policy_unlocked(
                        i)) {
                    continue;
                }

                if (calls_snapshot[i] >= PEAK_GLOBAL_DETACH_MIN_CALLS &&
                    ratio_snapshot[i] > target_profile_ratio) {
                    gboolean accepted =
                        peak_general_listener_request_detach_with_context_unlocked(
                            i,
                            PEAK_HOOK_REQUEST_SOURCE_PER_TARGET_HEARTBEAT,
                            calls_snapshot[i],
                            lifetime_snapshot[i],
                            profile_global_overhead,
                            total_execution_time,
                            rate_snapshot[i]);
                    if (accepted) {
                        wake_controller = TRUE;
                        projected_attached_recent_sum -= rate_snapshot[i];
                        if (projected_attached_recent_sum < 0.0) {
                            projected_attached_recent_sum = 0.0;
                        }
                        projected_attached_lifetime_sum -= ratio_snapshot[i];
                        if (projected_attached_lifetime_sum < 0.0) {
                            projected_attached_lifetime_sum = 0.0;
                        }
                    }
                }
            }
             pthread_mutex_unlock(&lock);
        }

        // ------------------------------------------------------------
        // 2) Global DETACH
        // ------------------------------------------------------------
        if (detach_observation_ready && enable_global_heartbeat) {
            double projected_global_overhead =
                MAX(projected_attached_recent_sum,
                    projected_attached_lifetime_sum);
            if (projected_global_overhead >
                global_target_ratio * peak_global_detach_factor) {
                size_t n_attached = 0;
                pthread_mutex_lock(&lock);
                for (size_t i = 0; i < heartbeat_capacity; i++) {
                    if (!(hook_address[i] && array_listener[i])) continue;
                    if (!peak_general_listener_hook_is_attached_for_policy_unlocked(
                            i)) {
                        continue;
                    }
                    if (calls_snapshot[i] < PEAK_GLOBAL_DETACH_MIN_CALLS) {
                        continue;
                    }
                    double contribution =
                        MAX(rate_snapshot[i], ratio_snapshot[i]);
                    if (contribution <= 0.0) continue;
                    entries[n_attached].index = i;
                    entries[n_attached].ratio = contribution;
                    entries[n_attached].rate = rate_snapshot[i];
                    entries[n_attached].lifetime = ratio_snapshot[i];
                    n_attached++;
                }

                if (n_attached > 1) {
                    qsort(entries,
                          n_attached,
                          sizeof(OverheadEntry),
                          compare_ratio_de);
                }

                double reduced_recent_sum = projected_attached_recent_sum;
                double reduced_lifetime_sum = projected_attached_lifetime_sum;
                double reduced_global_overhead = projected_global_overhead;
                for (size_t k = 0; k < n_attached; k++) {
                    size_t idx = entries[k].index;

                    if (reduced_global_overhead <= global_target_ratio) {
                        break;
                    }
                    if (entries[k].ratio <= 0.0) {
                        break;
                    }

                    if (!(hook_address[idx] && array_listener[idx])) continue;
                    if (!peak_general_listener_hook_is_attached_for_policy_unlocked(
                            idx)) {
                        continue;
                    }
                    gboolean accepted =
                        peak_general_listener_request_detach_with_context_unlocked(
                            idx,
                            PEAK_HOOK_REQUEST_SOURCE_GLOBAL_HEARTBEAT,
                            calls_snapshot[idx],
                            lifetime_snapshot[idx],
                            profile_global_overhead,
                            total_execution_time,
                            rate_snapshot[idx]);
                    if (accepted) {
                        wake_controller = TRUE;
                        reduced_recent_sum -= entries[k].rate;
                        if (reduced_recent_sum < 0.0) {
                            reduced_recent_sum = 0.0;
                        }
                        reduced_lifetime_sum -= entries[k].lifetime;
                        if (reduced_lifetime_sum < 0.0) {
                            reduced_lifetime_sum = 0.0;
                        }
                        reduced_global_overhead =
                            MAX(reduced_recent_sum, reduced_lifetime_sum);
                    }
                }
                projected_attached_recent_sum = reduced_recent_sum;
                projected_attached_lifetime_sum = reduced_lifetime_sum;
                pthread_mutex_unlock(&lock);
            }
        }

        // ------------------------------------------------------------
        // 3) Reattach
        // ------------------------------------------------------------
        if (detach_observation_ready &&
            enable_reattach &&
            check_interval != 0 &&
            (heartbeat_counter % check_interval) == 0) {
            double reattach_gate_ratio = 0.0;
            double reattach_spent_seconds = 0.0;
            double reattach_risk_spent_ratio = 0.0;
            double headroom_seconds = 0.0;
            PeakDetachAccountingSnapshot reattach_accounting_before;
            PeakDetachAccountingSnapshot reattach_accounting_after;
            gboolean reattach_accounting_before_valid =
                peak_general_listener_runtime_accounting_snapshot(
                    &reattach_accounting_before);
            double last_stop_seconds =
                peak_detach_controller_last_stop_window_us() / 1e6;
            double predicted_batch_stop_seconds = DBL_MAX;
            gboolean reattach_accounting_after_valid =
                peak_general_listener_runtime_accounting_snapshot(
                    &reattach_accounting_after);
            gboolean reattach_accounting_coherent =
                reattach_accounting_before_valid &&
                reattach_accounting_after_valid &&
                reattach_accounting_before.completed_stop_window_count <
                    (UINT64_MAX - 1) &&
                reattach_accounting_before.failed_stop_window_count <
                    (UINT64_MAX - 1) &&
                reattach_accounting_before.stop_window_wall_ns <
                    (UINT64_MAX - 1) &&
                reattach_accounting_after.completed_stop_window_count <
                    (UINT64_MAX - 1) &&
                reattach_accounting_after.failed_stop_window_count <
                    (UINT64_MAX - 1) &&
                reattach_accounting_after.stop_window_wall_ns <
                    (UINT64_MAX - 1) &&
                reattach_accounting_before.completed_stop_window_count ==
                    reattach_accounting_after.completed_stop_window_count &&
                reattach_accounting_before.failed_stop_window_count ==
                    reattach_accounting_after.failed_stop_window_count &&
                reattach_accounting_before.stop_window_wall_ns ==
                    reattach_accounting_after.stop_window_wall_ns;
            double reattach_control_spent_seconds =
                (double)peak_general_listener_control_wall_ns_since_heartbeat(
                    &reattach_accounting_after) / 1e9;
            double reattach_control_risk_seconds = DBL_MAX;
            gboolean reattach_budget_ok =
                reattach_accounting_coherent &&
                (enable_per_target_heartbeat || enable_global_heartbeat) &&
                peak_general_listener_positive_finite(total_execution_time) &&
                peak_general_listener_nonnegative_finite(global_target_ratio) &&
                peak_general_listener_nonnegative_finite(
                    peak_global_reattach_factor) &&
                peak_general_listener_nonnegative_finite(
                    profile_spent_seconds) &&
                peak_general_listener_nonnegative_finite(
                    reattach_control_spent_seconds) &&
                peak_general_listener_nonnegative_finite(
                    last_stop_seconds) &&
                peak_general_listener_multiply_nonnegative_finite(
                    reattach_control_spent_seconds,
                    (double)local_mpi_ranks,
                    &reattach_control_risk_seconds) &&
                peak_general_listener_multiply_nonnegative_finite(
                    last_stop_seconds,
                    (double)local_mpi_ranks,
                    &predicted_batch_stop_seconds) &&
                peak_general_listener_add_nonnegative_finite(
                    profile_spent_seconds,
                    reattach_control_risk_seconds,
                    &reattach_spent_seconds) &&
                peak_general_listener_multiply_nonnegative_finite(
                    peak_global_reattach_factor,
                    global_target_ratio,
                    &reattach_gate_ratio) &&
                peak_general_listener_multiply_nonnegative_finite(
                    global_target_ratio,
                    total_execution_time,
                    &headroom_seconds) &&
                peak_general_listener_subtract_nonnegative_finite(
                    &headroom_seconds,
                    profile_spent_seconds) &&
                peak_general_listener_subtract_nonnegative_finite(
                    &headroom_seconds,
                    reattach_control_risk_seconds);
            if (reattach_budget_ok) {
                reattach_risk_spent_ratio =
                    reattach_spent_seconds / total_execution_time;
                reattach_budget_ok =
                    peak_general_listener_nonnegative_finite(
                        reattach_risk_spent_ratio) &&
                    reattach_risk_spent_ratio <= reattach_gate_ratio;
            }

            if (reattach_budget_ok) {
                size_t pending_mutation_count = 0;
                size_t detached_cnt = 0;
                size_t admitted_count = 0;

                projected_pending_reattach_seconds = 0.0;
                pthread_mutex_lock(&lock);
                for (size_t i = 0; i < heartbeat_capacity; i++) {
                    double projected_seconds;
                    double projected_ratio;
                    gboolean per_target_eligible;
                    gboolean global_eligible;

                    if (!(hook_address[i] && array_listener[i])) continue;
                    if (peak_hook_states == NULL) continue;
                    if (peak_hook_states[i] == PEAK_HOOK_DETACH_REQUESTED ||
                        peak_hook_states[i] == PEAK_HOOK_DETACHING ||
                        peak_hook_states[i] == PEAK_HOOK_REATTACH_REQUESTED ||
                        peak_hook_states[i] == PEAK_HOOK_REATTACHING) {
                        pending_mutation_count++;
                    }
                    if (peak_hook_states[i] == PEAK_HOOK_REATTACH_REQUESTED ||
                        peak_hook_states[i] == PEAK_HOOK_REATTACHING) {
                        double projected_seconds =
                            peak_general_listener_projected_detach_profile_seconds_unlocked(
                                i,
                                profile_seconds_snapshot[i]);
                        double historical_rate =
                            peak_general_listener_historical_profile_rate(
                                projected_seconds,
                                total_execution_time);
                        double pending_rate = historical_rate;
                        if (peak_hook_pending_request_rate != NULL &&
                            peak_general_listener_positive_finite(
                                peak_hook_pending_request_rate[i])) {
                            pending_rate = peak_hook_pending_request_rate[i];
                        }
                        double pending_lease_seconds =
                            peak_general_listener_next_observation_lease_seconds(
                                pending_rate,
                                heartbeat_observation_horizon_seconds);
                        if (pending_lease_seconds == DBL_MAX ||
                            !peak_general_listener_add_nonnegative_finite(
                                projected_pending_reattach_seconds,
                                pending_lease_seconds,
                                &projected_pending_reattach_seconds)) {
                            reattach_budget_ok = FALSE;
                        }
                    }
                    if (!peak_detached[i]) continue;
                    if (peak_need_detach[i]) continue;
                    if (peak_hook_states[i] != PEAK_HOOK_DETACHED) {
                        continue;
                    }
                    if (!peak_general_listener_reattach_cooldown_ready_unlocked(
                            i,
                            now)) {
                        continue;
                    }

                    projected_seconds =
                        peak_general_listener_projected_detach_profile_seconds_unlocked(
                            i,
                            profile_seconds_snapshot[i]);
                    projected_ratio =
                        projected_seconds / total_execution_time;
                    per_target_eligible =
                        enable_per_target_heartbeat &&
                        projected_ratio <= target_profile_ratio;
                    global_eligible = enable_global_heartbeat;
                    if (!per_target_eligible && !global_eligible) {
                        continue;
                    }

                    entries[detached_cnt].index = i;
                    entries[detached_cnt].ratio = projected_seconds;
                    entries[detached_cnt].rate =
                        peak_hook_last_detach_time != NULL
                            ? peak_hook_last_detach_time[i]
                            : 0.0;
                    entries[detached_cnt].lifetime = projected_ratio;
                    detached_cnt++;
                }

                size_t pending_batch_windows =
                    (pending_mutation_count +
                     PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES - 1) /
                    PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES;
                double pending_stop_seconds = 0.0;
                reattach_budget_ok =
                    reattach_budget_ok &&
                    projected_pending_reattach_seconds != DBL_MAX &&
                    peak_general_listener_subtract_nonnegative_finite(
                        &headroom_seconds,
                        projected_pending_reattach_seconds) &&
                    peak_general_listener_multiply_nonnegative_finite(
                        (double)pending_batch_windows,
                        predicted_batch_stop_seconds,
                        &pending_stop_seconds) &&
                    peak_general_listener_subtract_nonnegative_finite(
                        &headroom_seconds,
                        pending_stop_seconds) &&
                    headroom_seconds > 0.0;

                if (reattach_budget_ok && detached_cnt > 1) {
                    qsort(entries,
                          detached_cnt,
                          sizeof(OverheadEntry),
                          compare_rate_inc);
                }

                for (size_t k = 0;
                     reattach_budget_ok && headroom_seconds > 0.0 &&
                         k < detached_cnt;
                     k++) {
                    size_t i = entries[k].index;
                    double projected_seconds;
                    double projected_ratio;
                    double request_rate;
                    double incremental_lease_seconds;
                    gboolean per_target_eligible;
                    gboolean global_eligible;
                    PeakHookRequestSource source;
                    gboolean still_detached =
                        hook_address[i] && array_listener[i] &&
                        peak_detached[i] && !peak_need_detach[i] &&
                        peak_hook_states != NULL &&
                        peak_hook_states[i] == PEAK_HOOK_DETACHED;
                    if (!still_detached) continue;
                    if (!peak_general_listener_reattach_cooldown_ready_unlocked(
                            i,
                            now)) {
                        continue;
                    }

                    projected_seconds = entries[k].ratio;
                    projected_ratio = entries[k].lifetime;
                    per_target_eligible =
                        enable_per_target_heartbeat &&
                        projected_ratio <= target_profile_ratio;
                    global_eligible = enable_global_heartbeat;
                    if (!per_target_eligible && !global_eligible) {
                        continue;
                    }
                    source = per_target_eligible
                                 ? PEAK_HOOK_REQUEST_SOURCE_PER_TARGET_HEARTBEAT
                                 : PEAK_HOOK_REQUEST_SOURCE_GLOBAL_HEARTBEAT;
                    request_rate =
                        peak_general_listener_historical_profile_rate(
                            projected_seconds,
                            total_execution_time);
                    incremental_lease_seconds =
                        peak_general_listener_next_observation_lease_seconds(
                            request_rate,
                            heartbeat_observation_horizon_seconds);

                    size_t before_count =
                        pending_mutation_count + admitted_count;
                    size_t after_count = before_count + 1;
                    size_t before_windows =
                        (before_count +
                         PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES - 1) /
                        PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES;
                    size_t after_windows =
                        (after_count +
                         PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES - 1) /
                        PEAK_GENERAL_CONTROLLER_MAX_BATCH_CANDIDATES;
                    double extra_stop_seconds = DBL_MAX;
                    (void)peak_general_listener_multiply_nonnegative_finite(
                        (double)(after_windows - before_windows),
                        predicted_batch_stop_seconds,
                        &extra_stop_seconds);
                    double candidate_seconds = DBL_MAX;
                    double remaining_headroom_seconds = headroom_seconds;
                    if (incremental_lease_seconds != DBL_MAX) {
                        (void)peak_general_listener_add_nonnegative_finite(
                            incremental_lease_seconds,
                            extra_stop_seconds,
                            &candidate_seconds);
                    }

                    if (candidate_seconds == DBL_MAX ||
                        !peak_general_listener_subtract_nonnegative_finite(
                            &remaining_headroom_seconds,
                            candidate_seconds)) {
                        continue;
                    }

                    gboolean accepted;
                    double request_ratio =
                        projected_seconds / total_execution_time;
                    accepted =
                        peak_general_listener_request_reattach_with_context_unlocked(
                            i,
                            source,
                            calls_snapshot[i],
                            request_ratio,
                            spent_ratio,
                            total_execution_time,
                            request_rate);
                    if (!accepted) {
                        continue;
                    }

                    wake_controller = TRUE;
                    headroom_seconds = remaining_headroom_seconds;
                    admitted_count++;
                }
                pthread_mutex_unlock(&lock);
            }
        }

        if (wake_controller) {
            peak_general_listener_controller_wake();
        }

        // ------------------------------------------------------------
        // Adaptive heartbeat sleep
        // ------------------------------------------------------------
        double gdt = now - prev_global_time;
        if (gdt <= 1e-12) gdt = 1e-12;

        double global_rate =
            (profile_global_overhead - prev_global_overhead) / gdt;
        ema_global_rate = hb_ema_a * global_rate + (1.0 - hb_ema_a) * ema_global_rate;

        prev_global_overhead = profile_global_overhead;
        prev_global_time     = now;

        // error: how much we exceed global target (normalized)
        double err = (global_target_ratio > 0.0)
                         ? (profile_global_overhead / global_target_ratio - 1.0)
                         : 0.0;
        if (err < 0.0) err = 0.0;

        // // only care positive growth (shrinking shouldn't speed up)
        // double pos_rate = (ema_global_rate > 0.0) ? ema_global_rate : 0.0;

        // scale factor: faster when err/rate bigger
        double scale = 1.0 / (1.0 + hb_k_err * err + hb_k_rate * ema_global_rate);

        long long sleep_us = (long long)(clipd((double)heartbeat_time * scale,
                                       (double)hb_min_us,
                                       (double)hb_max_us) + 0.5);

        peak_general_listener_management_cpu_checkpoint(
            &management_cpu_checkpoint_ns,
            &management_cpu_checkpoint_valid);
        if (!peak_heartbeat_wait_us((unsigned int)sleep_us)) {
            break;
        }
    }

cleanup:
    pthread_mutex_lock(&lock);
    peak_general_listener_snapshot_heartbeat_overhead_seconds_unlocked();
    pthread_mutex_unlock(&lock);
    g_free(previous_time);
    g_free(previous_calls);
    g_free(recent_baseline_ready);
    g_free(rate_snapshot);
    g_free(ratio_snapshot);
    g_free(lifetime_snapshot);
    g_free(profile_seconds_snapshot);
    g_free(calls_snapshot);
    g_free(entries);

    gum_interceptor_unignore_current_thread(interceptor);
    return NULL;
}
