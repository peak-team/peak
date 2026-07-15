
static gboolean
peak_general_controller_status_is_retryable(PeakDetachStatus status)
{
    switch (status) {
        case PEAK_DETACH_STATUS_TIMEOUT:
        case PEAK_DETACH_STATUS_CLASSIFY_FAILED:
        case PEAK_DETACH_STATUS_ERROR:
            return TRUE;
        case PEAK_DETACH_STATUS_SAFE:
        case PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED:
        case PEAK_DETACH_STATUS_DISABLED:
        case PEAK_DETACH_STATUS_UNSUPPORTED:
        case PEAK_DETACH_STATUS_MISSING_GUM_API:
        case PEAK_DETACH_STATUS_PERMISSION_DENIED:
        default:
            return FALSE;
    }
}

static const char*
peak_hook_request_source_string(PeakHookRequestSource source)
{
    switch (source) {
        case PEAK_HOOK_REQUEST_SOURCE_API:
            return "api";
        case PEAK_HOOK_REQUEST_SOURCE_DETACH_COUNT:
            return "detach-count";
        case PEAK_HOOK_REQUEST_SOURCE_PER_TARGET_HEARTBEAT:
            return "per-target-heartbeat";
        case PEAK_HOOK_REQUEST_SOURCE_GLOBAL_HEARTBEAT:
            return "global-heartbeat";
        case PEAK_HOOK_REQUEST_SOURCE_NONE:
        default:
            return "none";
    }
}

static void
peak_general_controller_clear_pending_request_context_unlocked(size_t hook_id)
{
    if (hook_id >= peak_hook_address_count) {
        return;
    }

    if (peak_hook_pending_request_source != NULL) {
        peak_hook_pending_request_source[hook_id] =
            PEAK_HOOK_REQUEST_SOURCE_NONE;
    }
    if (peak_hook_pending_request_calls != NULL) {
        peak_hook_pending_request_calls[hook_id] = 0;
    }
    if (peak_hook_pending_request_ratio != NULL) {
        peak_hook_pending_request_ratio[hook_id] = 0.0;
    }
    if (peak_hook_pending_request_global_overhead != NULL) {
        peak_hook_pending_request_global_overhead[hook_id] = 0.0;
    }
    if (peak_hook_pending_request_total_time != NULL) {
        peak_hook_pending_request_total_time[hook_id] = 0.0;
    }
    if (peak_hook_pending_request_rate != NULL) {
        peak_hook_pending_request_rate[hook_id] = 0.0;
    }
}

static void
peak_general_controller_set_pending_request_context_unlocked(
    size_t hook_id,
    PeakHookRequestSource source,
    gulong calls,
    double ratio,
    double global_overhead,
    double total_time,
    double rate)
{
    if (hook_id >= peak_hook_address_count) {
        return;
    }

    if (peak_hook_pending_request_source != NULL) {
        peak_hook_pending_request_source[hook_id] = source;
    }
    if (peak_hook_pending_request_calls != NULL) {
        peak_hook_pending_request_calls[hook_id] = calls;
    }
    if (peak_hook_pending_request_ratio != NULL) {
        peak_hook_pending_request_ratio[hook_id] = ratio;
    }
    if (peak_hook_pending_request_global_overhead != NULL) {
        peak_hook_pending_request_global_overhead[hook_id] = global_overhead;
    }
    if (peak_hook_pending_request_total_time != NULL) {
        peak_hook_pending_request_total_time[hook_id] = total_time;
    }
    if (peak_hook_pending_request_rate != NULL) {
        peak_hook_pending_request_rate[hook_id] = rate;
    }
}

static gboolean
peak_general_listener_positive_finite(double value)
{
    return value > 0.0 && value == value && value <= DBL_MAX;
}

static gboolean
peak_general_listener_nonnegative_finite(double value)
{
    return value >= 0.0 && value == value && value <= DBL_MAX;
}

static gboolean
peak_general_listener_add_nonnegative_finite(double lhs,
                                             double rhs,
                                             double* out)
{
    if (!peak_general_listener_nonnegative_finite(lhs) ||
        !peak_general_listener_nonnegative_finite(rhs) ||
        lhs > DBL_MAX - rhs) {
        return FALSE;
    }

    if (out != NULL) {
        *out = lhs + rhs;
    }
    return TRUE;
}

static uint64_t
peak_general_listener_add_uint64_saturated(uint64_t lhs, uint64_t rhs)
{
    const uint64_t max_published = UINT64_MAX - 1;

    if (lhs >= max_published || rhs > max_published - lhs) {
        return max_published;
    }
    return lhs + rhs;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
uint64_t
peak_general_listener_test_add_uint64_saturated(uint64_t lhs, uint64_t rhs)
{
    return peak_general_listener_add_uint64_saturated(lhs, rhs);
}
#endif

static gboolean
peak_general_listener_subtract_nonnegative_finite(double* value,
                                                  double amount)
{
    double result;

    if (value == NULL ||
        !peak_general_listener_nonnegative_finite(*value) ||
        !peak_general_listener_nonnegative_finite(amount) ||
        amount > *value) {
        return FALSE;
    }

    result = *value - amount;
    if (!peak_general_listener_nonnegative_finite(result)) {
        return FALSE;
    }
    *value = result;
    return TRUE;
}

static gboolean
peak_general_listener_multiply_nonnegative_finite(double lhs,
                                                  double rhs,
                                                  double* out)
{
    double result;

    if (!peak_general_listener_nonnegative_finite(lhs) ||
        !peak_general_listener_nonnegative_finite(rhs) ||
        (rhs > 0.0 && lhs > DBL_MAX / rhs)) {
        return FALSE;
    }

    result = lhs * rhs;
    if (!peak_general_listener_nonnegative_finite(result)) {
        return FALSE;
    }
    if (out != NULL) {
        *out = result;
    }
    return TRUE;
}

static double
peak_general_listener_control_risk_seconds(double raw_control_seconds)
{
    double risk_control_seconds = DBL_MAX;

    if (!peak_general_listener_multiply_nonnegative_finite(
            raw_control_seconds,
            (double)peak_general_listener_local_mpi_ranks(),
            &risk_control_seconds)) {
        return DBL_MAX;
    }
    return risk_control_seconds;
}

static gboolean
peak_general_listener_multiply_positive_finite(double lhs,
                                               double rhs,
                                               double* out)
{
    double result;

    if (!peak_general_listener_positive_finite(lhs) ||
        !peak_general_listener_positive_finite(rhs) ||
        lhs > DBL_MAX / rhs) {
        return FALSE;
    }

    result = lhs * rhs;
    if (!peak_general_listener_positive_finite(result)) {
        return FALSE;
    }
    if (out != NULL) {
        *out = result;
    }
    return TRUE;
}

static double
peak_general_listener_profile_seconds_floor(void)
{
    if (peak_general_listener_positive_finite(peak_general_overhead)) {
        return peak_general_overhead;
    }
    return 1e-12;
}

static double
peak_general_listener_saved_detach_profile_seconds_unlocked(
    size_t hook_id,
    double fallback_seconds)
{
    if (hook_id < peak_hook_address_count &&
        peak_hook_detach_profile_seconds != NULL &&
        peak_general_listener_positive_finite(
            peak_hook_detach_profile_seconds[hook_id])) {
        return peak_hook_detach_profile_seconds[hook_id];
    }
    if (peak_general_listener_positive_finite(fallback_seconds)) {
        return fallback_seconds;
    }
    return peak_general_listener_profile_seconds_floor();
}

static double
peak_general_listener_projected_detach_profile_seconds_unlocked(
    size_t hook_id,
    double current_profile_seconds)
{
    double saved_seconds = 0.0;
    gboolean have_saved = FALSE;
    gboolean have_current =
        peak_general_listener_positive_finite(current_profile_seconds);

    if (hook_id < peak_hook_address_count &&
        peak_hook_detach_profile_seconds != NULL &&
        peak_general_listener_positive_finite(
            peak_hook_detach_profile_seconds[hook_id])) {
        saved_seconds = peak_hook_detach_profile_seconds[hook_id];
        have_saved = TRUE;
    }

    if (have_saved && have_current) {
        return saved_seconds > current_profile_seconds
                   ? saved_seconds
                   : current_profile_seconds;
    }
    if (have_saved) {
        return saved_seconds;
    }
    if (have_current) {
        return current_profile_seconds;
    }
    return peak_general_listener_profile_seconds_floor();
}

static double
peak_general_listener_historical_profile_rate(double projected_seconds,
                                              double elapsed_seconds)
{
    double rate;

    if (!peak_general_listener_positive_finite(projected_seconds) ||
        !peak_general_listener_positive_finite(elapsed_seconds) ||
        projected_seconds == DBL_MAX) {
        return DBL_MAX;
    }

    rate = projected_seconds / elapsed_seconds;
    return peak_general_listener_positive_finite(rate) ? rate : DBL_MAX;
}

static double
peak_general_listener_next_observation_lease_seconds(
    double request_rate,
    double observation_horizon_seconds)
{
    double lease_seconds = 0.0;

    if (!peak_general_listener_positive_finite(request_rate) ||
        !peak_general_listener_positive_finite(observation_horizon_seconds) ||
        request_rate == DBL_MAX) {
        return DBL_MAX;
    }

    if (!peak_general_listener_multiply_positive_finite(
            request_rate,
            observation_horizon_seconds,
            &lease_seconds)) {
        return DBL_MAX;
    }

    return lease_seconds;
}

static gboolean
peak_general_listener_hook_is_attached_for_policy_unlocked(size_t hook_id)
{
    if (hook_id >= peak_hook_address_count) {
        return FALSE;
    }

    if (peak_hook_states != NULL) {
        return peak_hook_states[hook_id] == PEAK_HOOK_ATTACHED;
    }
    return peak_detached == NULL || !peak_detached[hook_id];
}

static double
peak_general_listener_heartbeat_overhead_seconds_unlocked(void)
{
    double seconds = 0.0;

    if (heartbeat_overhead == NULL) {
        return 0.0;
    }

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address != NULL &&
            array_listener != NULL &&
            hook_address[i] &&
            array_listener[i] &&
            peak_general_listener_positive_finite(heartbeat_overhead[i]) &&
            seconds <= DBL_MAX - heartbeat_overhead[i]) {
            seconds += heartbeat_overhead[i];
        }
    }

    return seconds;
}

static void
peak_general_listener_snapshot_heartbeat_overhead_seconds_unlocked(void)
{
    peak_general_listener_final_heartbeat_overhead_seconds =
        peak_general_listener_heartbeat_overhead_seconds_unlocked();
}

static double
peak_general_listener_report_heartbeat_overhead_seconds(void)
{
    double seconds;

    pthread_mutex_lock(&lock);
    seconds = heartbeat_overhead != NULL
                  ? peak_general_listener_heartbeat_overhead_seconds_unlocked()
                  : peak_general_listener_final_heartbeat_overhead_seconds;
    pthread_mutex_unlock(&lock);
    return seconds;
}

static void
peak_general_listener_note_detach_profile_seconds_unlocked(size_t hook_id)
{
    double profile_seconds = 0.0;
    double fallback_seconds = 0.0;
    double detached_profile_seconds = 0.0;
    gboolean have_detached_sample = FALSE;

    if (hook_id >= peak_hook_address_count ||
        peak_hook_detach_profile_seconds == NULL) {
        return;
    }

    have_detached_sample =
        peak_general_listener_refresh_hook_sample_cache_unlocked(
            hook_id,
            NULL,
            &detached_profile_seconds);
    if (have_detached_sample) {
        fallback_seconds = detached_profile_seconds;
    }

    if (peak_hook_pending_request_ratio != NULL &&
        peak_hook_pending_request_total_time != NULL) {
        (void)peak_general_listener_multiply_positive_finite(
            peak_hook_pending_request_ratio[hook_id],
            peak_hook_pending_request_total_time[hook_id],
            &profile_seconds);
    }

    if (!have_detached_sample &&
        peak_hook_pending_request_calls != NULL &&
        peak_general_listener_positive_finite(peak_general_overhead) &&
        (double)peak_hook_pending_request_calls[hook_id] <=
            DBL_MAX / peak_general_overhead) {
        fallback_seconds =
            (double)peak_hook_pending_request_calls[hook_id] *
            peak_general_overhead;
    }
    if (!have_detached_sample &&
        heartbeat_overhead != NULL &&
        peak_general_listener_positive_finite(heartbeat_overhead[hook_id]) &&
        fallback_seconds <= DBL_MAX - heartbeat_overhead[hook_id]) {
        fallback_seconds += heartbeat_overhead[hook_id];
    }
    if (!peak_general_listener_positive_finite(fallback_seconds) &&
        peak_general_listener_positive_finite(
            peak_hook_detach_profile_seconds[hook_id])) {
        fallback_seconds = peak_hook_detach_profile_seconds[hook_id];
    }

    peak_hook_detach_profile_seconds[hook_id] =
        peak_general_listener_positive_finite(profile_seconds)
            ? profile_seconds
            : peak_general_listener_saved_detach_profile_seconds_unlocked(
                  hook_id,
                  fallback_seconds);
}

static void
peak_general_controller_reset_retry_unlocked(size_t hook_id)
{
    if (hook_id >= peak_hook_address_count ||
        peak_hook_next_retry_time == NULL ||
        peak_hook_retry_count == NULL) {
        return;
    }

    peak_hook_next_retry_time[hook_id] = 0.0;
    peak_hook_retry_count[hook_id] = 0;
    if (peak_hook_last_retry_status != NULL) {
        peak_hook_last_retry_status[hook_id] = PEAK_DETACH_STATUS_SAFE;
    }
    if (peak_hook_pending_observed_time != NULL) {
        peak_hook_pending_observed_time[hook_id] = 0.0;
    }
    peak_general_controller_clear_pending_request_context_unlocked(hook_id);
}

static void
peak_general_controller_mark_pending_started_unlocked(size_t hook_id,
                                                      double now)
{
    if (peak_hook_pending_observed_time == NULL ||
        hook_id >= peak_hook_address_count ||
        peak_hook_pending_observed_time[hook_id] > 0.0) {
        return;
    }

    peak_hook_pending_observed_time[hook_id] = now;
}

static void
peak_general_listener_note_detach_success_unlocked(size_t hook_id,
                                                   double now)
{
    if (peak_hook_last_detach_time == NULL || hook_id >= peak_hook_address_count) {
        return;
    }

    peak_hook_last_detach_time[hook_id] = now;
}

static void
peak_general_listener_note_reattach_success_unlocked(size_t hook_id)
{
    if (hook_id >= peak_hook_address_count ||
        peak_hook_reattach_request_calls == NULL ||
        peak_hook_reattach_request_calls_valid == NULL ||
        peak_hook_pending_request_calls == NULL) {
        return;
    }

    if (peak_hook_reattach_request_calls_valid[hook_id] &&
        array_listener_revisited != NULL &&
        peak_hook_pending_request_calls[hook_id] >
            peak_hook_reattach_request_calls[hook_id]) {
        array_listener_revisited[hook_id] = TRUE;
    }
    peak_hook_reattach_request_calls[hook_id] =
        peak_hook_pending_request_calls[hook_id];
    peak_hook_reattach_request_calls_valid[hook_id] = TRUE;
}

static gboolean
peak_general_listener_reattach_cooldown_ready_unlocked(size_t hook_id,
                                                       double now)
{
    peak_general_listener_init_reattach_policy();

    if (peak_reattach_cooldown_s <= 0.0 ||
        peak_hook_last_detach_time == NULL ||
        hook_id >= peak_hook_address_count ||
        peak_hook_last_detach_time[hook_id] <= 0.0) {
        return TRUE;
    }

    return now - peak_hook_last_detach_time[hook_id] >=
           peak_reattach_cooldown_s;
}

static void
peak_general_controller_note_retry_unlocked(size_t hook_id,
                                            PeakDetachStatus status)
{
    if (hook_id >= peak_hook_address_count ||
        peak_hook_next_retry_time == NULL ||
        peak_hook_retry_count == NULL ||
        !peak_general_controller_status_is_retryable(status)) {
        return;
    }

    unsigned int retry_count = peak_hook_retry_count[hook_id];
    unsigned int shift = retry_count < 6 ? retry_count : 6;
    double delay = peak_controller_retry_base_delay * (double)(1U << shift);

    if (delay > peak_controller_retry_max_delay) {
        delay = peak_controller_retry_max_delay;
    }
    if (retry_count < G_MAXUINT) {
        peak_hook_retry_count[hook_id] = retry_count + 1;
    }
    peak_hook_next_retry_time[hook_id] = peak_second() + delay;
    if (peak_hook_last_retry_status != NULL) {
        peak_hook_last_retry_status[hook_id] = status;
    }
}

static gboolean
peak_general_controller_retry_budget_exceeded_unlocked(size_t hook_id,
                                                       double now)
{
    double pending_age_s;
    unsigned int retry_count;

    if (hook_id >= peak_hook_address_count) {
        return FALSE;
    }

    peak_general_controller_init_retry_limits();

    retry_count = peak_hook_retry_count != NULL ? peak_hook_retry_count[hook_id] : 0;
    pending_age_s = peak_general_controller_pending_age_for_trace_unlocked(hook_id, now);

    if (peak_controller_max_retry_count > 0 &&
        retry_count >= peak_controller_max_retry_count) {
        return TRUE;
    }

    return peak_controller_max_pending_age_s > 0.0 &&
           pending_age_s >= peak_controller_max_pending_age_s;
}

static gboolean
peak_general_controller_retry_ready_unlocked(size_t hook_id, double now)
{
    if (hook_id >= peak_hook_address_count ||
        peak_hook_next_retry_time == NULL) {
        return TRUE;
    }

    return peak_hook_next_retry_time[hook_id] <= 0.0 ||
           peak_hook_next_retry_time[hook_id] <= now;
}

static void peak_general_controller_set_state_unlocked(size_t hook_id, PeakHookState state)
{
    if (peak_hook_states == NULL || hook_id >= peak_hook_address_count) {
        return;
    }

    peak_hook_states[hook_id] = state;
    peak_general_listener_publish_legacy_flags_unlocked(hook_id);
}

static gboolean
peak_general_controller_mpi_finalize_requested(void)
{
    return atomic_load_explicit(&peak_general_mpi_finalize_requested,
                                memory_order_acquire);
}

void peak_general_listener_controller_mark_attached_unlocked(size_t hook_id)
{
    peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_ATTACHED);
}

static gboolean
peak_general_listener_request_detach_with_context_unlocked(
    size_t hook_id,
    PeakHookRequestSource source,
    gulong calls,
    double ratio,
    double global_overhead,
    double total_time,
    double rate)
{
    if (peak_general_controller_mpi_finalize_requested()) {
        return FALSE;
    }

    if (!peak_general_hook_is_published_unlocked(hook_id)) {
        return FALSE;
    }

    switch (peak_hook_states[hook_id]) {
        case PEAK_HOOK_ATTACHED:
            peak_general_controller_reset_retry_unlocked(hook_id);
            peak_general_controller_mark_pending_started_unlocked(hook_id,
                                                                  peak_second());
            peak_general_controller_set_pending_request_context_unlocked(
                hook_id,
                source,
                calls,
                ratio,
                global_overhead,
                total_time,
                rate);
            peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_DETACH_REQUESTED);
            return TRUE;
        case PEAK_HOOK_DETACH_REQUESTED:
        case PEAK_HOOK_DETACHING:
            return TRUE;
        default:
            return FALSE;
    }
}

static gboolean
peak_general_listener_request_detach_unlocked(size_t hook_id)
{
    return peak_general_listener_request_detach_with_context_unlocked(
        hook_id,
        PEAK_HOOK_REQUEST_SOURCE_API,
        0,
        0.0,
        0.0,
        0.0,
        0.0);
}

static gboolean
peak_general_listener_request_reattach_with_context_unlocked(
    size_t hook_id,
    PeakHookRequestSource source,
    gulong calls,
    double ratio,
    double global_overhead,
    double total_time,
    double rate)
{
    if (peak_general_controller_mpi_finalize_requested()) {
        return FALSE;
    }

    if (!peak_general_hook_is_published_unlocked(hook_id)) {
        return FALSE;
    }

    switch (peak_hook_states[hook_id]) {
        case PEAK_HOOK_DETACHED:
            if (array_listener_gum_detached != NULL &&
                array_listener_gum_detach_flushed != NULL &&
                array_listener_gum_detached[hook_id] &&
                !array_listener_gum_detach_flushed[hook_id]) {
                return FALSE;
            }
            peak_general_controller_reset_retry_unlocked(hook_id);
            peak_general_controller_mark_pending_started_unlocked(hook_id,
                                                                  peak_second());
            peak_general_controller_set_pending_request_context_unlocked(
                hook_id,
                source,
                calls,
                ratio,
                global_overhead,
                total_time,
                rate);
            peak_general_controller_set_state_unlocked(hook_id, PEAK_HOOK_REATTACH_REQUESTED);
            return TRUE;
        case PEAK_HOOK_REATTACH_REQUESTED:
        case PEAK_HOOK_REATTACHING:
            return TRUE;
        default:
            return FALSE;
    }
}

static gboolean
peak_general_listener_request_reattach_unlocked(size_t hook_id)
{
    gulong calls = 0;

    if (hook_id < peak_hook_address_count &&
        peak_hook_states != NULL &&
        peak_hook_states[hook_id] == PEAK_HOOK_DETACHED &&
        !peak_general_listener_refresh_hook_sample_cache_unlocked(
            hook_id,
            &calls,
            NULL) &&
        !peak_general_listener_try_hook_sample_cache_unlocked(
            hook_id,
            &calls,
            NULL)) {
        return FALSE;
    }

    return peak_general_listener_request_reattach_with_context_unlocked(
        hook_id,
        PEAK_HOOK_REQUEST_SOURCE_API,
        calls,
        0.0,
        0.0,
        0.0,
        0.0);
}

gboolean peak_general_listener_request_detach(size_t hook_id)
{
    gboolean accepted;

    pthread_mutex_lock(&lock);
    accepted = peak_general_listener_request_detach_unlocked(hook_id);
    pthread_mutex_unlock(&lock);

    if (accepted) {
        peak_general_listener_controller_wake();
    }

    return accepted;
}

gboolean peak_general_listener_request_reattach(size_t hook_id)
{
    gboolean accepted;

    pthread_mutex_lock(&lock);
    accepted = peak_general_listener_request_reattach_unlocked(hook_id);
    pthread_mutex_unlock(&lock);

    if (accepted) {
        peak_general_listener_controller_wake();
    }

    return accepted;
}

PeakHookState peak_general_listener_hook_state(size_t hook_id)
{
    PeakHookState state = PEAK_HOOK_UNRESOLVED;

    pthread_mutex_lock(&lock);
    if (peak_hook_states != NULL && hook_id < peak_hook_address_count) {
        state = peak_hook_states[hook_id];
    }
    pthread_mutex_unlock(&lock);

    return state;
}

gboolean
peak_general_listener_controller_is_current_thread(void)
{
    return atomic_load(&general_controller_owner_known) &&
           pthread_equal(pthread_self(), general_controller_owner_thread);
}

static gboolean
peak_general_listener_provider_is_perfmap(const char* provider_name)
{
    return provider_name != NULL &&
           (g_ascii_strcasecmp(provider_name, "perfmap") == 0 ||
            g_ascii_strcasecmp(provider_name, "perf-map") == 0);
}

static gboolean
peak_general_listener_v8_optimized_symbol_matches_target(const char* target_name,
                                                         const char* symbol_name)
{
    const char* cursor = NULL;
    size_t target_len;
    size_t logical_len = 0;

    if (target_name == NULL || symbol_name == NULL) {
        return FALSE;
    }

    if (g_str_has_prefix(symbol_name, "JS:")) {
        cursor = symbol_name + 3;
    } else if (g_str_has_prefix(symbol_name, "LazyCompile:")) {
        cursor = symbol_name + 12;
    } else {
        return FALSE;
    }

    if (*cursor != '*') {
        return FALSE;
    }
    cursor++;
    if (*cursor == '\0') {
        return FALSE;
    }

    while (cursor[logical_len] != '\0' &&
           !g_ascii_isspace((guchar)cursor[logical_len])) {
        logical_len++;
    }

    target_len = strlen(target_name);
    return target_len == logical_len &&
           strncmp(target_name, cursor, logical_len) == 0;
}

static gboolean
peak_general_listener_dynamic_symbol_matches_target(const char* target_name,
                                                    const char* symbol_name,
                                                    const char* provider_name)
{
    if (target_name == NULL || symbol_name == NULL) {
        return FALSE;
    }

    if (strcmp(target_name, symbol_name) == 0) {
        return TRUE;
    }

    if (!peak_general_listener_provider_is_perfmap(provider_name)) {
        return FALSE;
    }

    return peak_general_listener_v8_optimized_symbol_matches_target(target_name,
                                                                    symbol_name);
}

gboolean
peak_general_listener_dynamic_symbol_matches_any_target(const char* symbol_name,
                                                        const char* provider_name)
{
    gboolean matched = FALSE;

    if (symbol_name == NULL || symbol_name[0] == '\0') {
        return FALSE;
    }

    pthread_mutex_lock(&lock);
    if (peak_hook_strings != NULL) {
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (peak_hook_strings[i] != NULL &&
                peak_general_listener_dynamic_symbol_matches_target(
                    peak_hook_strings[i],
                    symbol_name,
                    provider_name)) {
                matched = TRUE;
                break;
            }
        }
    }
    pthread_mutex_unlock(&lock);

    return matched;
}

GPtrArray*
peak_general_listener_snapshot_dynamic_target_names(void)
{
    GPtrArray* target_names;
    GHashTable* seen_names;

    target_names = g_ptr_array_new_with_free_func(g_free);
    seen_names = g_hash_table_new(g_str_hash, str_equal_function_general);

    pthread_mutex_lock(&lock);
    if (peak_hook_strings != NULL) {
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            const char* target_name = peak_hook_strings[i];

            if (target_name == NULL || target_name[0] == '\0' ||
                g_hash_table_contains(seen_names, target_name)) {
                continue;
            }
            g_hash_table_add(seen_names, (gpointer)target_name);
            g_ptr_array_add(target_names, g_strdup(target_name));
        }
    }
    pthread_mutex_unlock(&lock);
    g_hash_table_destroy(seen_names);

    return target_names;
}

gboolean
peak_general_listener_dynamic_symbol_address_needs_attach(
    const char* symbol_name,
    gpointer symbol_address,
    const char* provider_name)
{
    gboolean matched_target = FALSE;
    gboolean duplicate_address = FALSE;

    if (symbol_name == NULL || symbol_name[0] == '\0' ||
        symbol_address == NULL) {
        return FALSE;
    }

    pthread_mutex_lock(&lock);
    if (peak_hook_strings != NULL && hook_address != NULL) {
        for (size_t hook_id = 0;
             hook_id < peak_hook_address_count;
             hook_id++) {
            if (!peak_general_listener_dynamic_symbol_matches_target(
                    peak_hook_strings[hook_id],
                    symbol_name,
                    provider_name)) {
                continue;
            }
            matched_target = TRUE;
            if (hook_address[hook_id] == symbol_address) {
                duplicate_address = TRUE;
                break;
            }
        }
    }
    pthread_mutex_unlock(&lock);

    return matched_target && !duplicate_address;
}

static gboolean
peak_general_listener_expand_dynamic_hook_tables_unlocked(
    const char* target_name,
    size_t* hook_id_out)
{
    size_t old_count = peak_hook_address_count;
    size_t new_count = old_count + 1;
    char* target_copy = NULL;
    char** new_hook_strings;

    if (target_name == NULL || target_name[0] == '\0') {
        return FALSE;
    }

    target_copy = strdup(target_name);
    if (target_copy == NULL) {
        return FALSE;
    }

    new_hook_strings = realloc(peak_hook_strings,
                               sizeof(char*) * new_count);
    if (new_hook_strings == NULL) {
        free(target_copy);
        return FALSE;
    }
    peak_hook_strings = new_hook_strings;

    array_listener = g_renew(GumInvocationListener*, array_listener, new_count);
    array_listener_detached = g_renew(gboolean, array_listener_detached, new_count);
    array_listener_reattached = g_renew(gboolean, array_listener_reattached, new_count);
    array_listener_revisited =
        g_renew(gboolean, array_listener_revisited, new_count);
    array_listener_gum_detached =
        g_renew(gboolean, array_listener_gum_detached, new_count);
    array_listener_gum_detach_flushed =
        g_renew(gboolean, array_listener_gum_detach_flushed, new_count);
    peak_hook_last_detach_time =
        g_renew(double, peak_hook_last_detach_time, new_count);
    peak_hook_states = g_renew(PeakHookState, peak_hook_states, new_count);
    peak_hook_next_retry_time = g_renew(double, peak_hook_next_retry_time, new_count);
    peak_hook_pending_observed_time =
        g_renew(double, peak_hook_pending_observed_time, new_count);
    peak_hook_pending_request_source =
        g_renew(PeakHookRequestSource,
                peak_hook_pending_request_source,
                new_count);
    peak_hook_pending_request_calls =
        g_renew(gulong, peak_hook_pending_request_calls, new_count);
    peak_hook_pending_request_ratio =
        g_renew(double, peak_hook_pending_request_ratio, new_count);
    peak_hook_pending_request_global_overhead =
        g_renew(double, peak_hook_pending_request_global_overhead, new_count);
    peak_hook_pending_request_total_time =
        g_renew(double, peak_hook_pending_request_total_time, new_count);
    peak_hook_pending_request_rate =
        g_renew(double, peak_hook_pending_request_rate, new_count);
    peak_hook_detach_profile_seconds =
        g_renew(double, peak_hook_detach_profile_seconds, new_count);
    peak_hook_reattach_request_calls =
        g_renew(gulong, peak_hook_reattach_request_calls, new_count);
    peak_hook_reattach_request_calls_valid =
        g_renew(gboolean,
                peak_hook_reattach_request_calls_valid,
                new_count);
    peak_hook_cached_sample_calls =
        g_renew(gulong, peak_hook_cached_sample_calls, new_count);
    peak_hook_cached_sample_profile_seconds =
        g_renew(double, peak_hook_cached_sample_profile_seconds, new_count);
    peak_hook_cached_sample_valid =
        g_renew(gboolean, peak_hook_cached_sample_valid, new_count);
    peak_hook_retry_count =
        g_renew(unsigned int, peak_hook_retry_count, new_count);
    peak_hook_last_retry_status =
        g_renew(PeakDetachStatus, peak_hook_last_retry_status, new_count);
    hook_address = g_renew(gpointer, hook_address, new_count);
    peak_demangled_strings = g_renew(char*, peak_demangled_strings, new_count);
    peak_need_detach = g_renew(gboolean, peak_need_detach, new_count);
    peak_detached = g_renew(gboolean, peak_detached, new_count);
    if (heartbeat_overhead != NULL) {
        heartbeat_overhead = g_renew(gdouble, heartbeat_overhead, new_count);
    }

    peak_hook_strings[old_count] = target_copy;
    array_listener[old_count] = NULL;
    array_listener_detached[old_count] = FALSE;
    array_listener_reattached[old_count] = FALSE;
    array_listener_revisited[old_count] = FALSE;
    array_listener_gum_detached[old_count] = FALSE;
    array_listener_gum_detach_flushed[old_count] = TRUE;
    peak_hook_last_detach_time[old_count] = 0.0;
    peak_hook_states[old_count] = PEAK_HOOK_UNRESOLVED;
    peak_hook_next_retry_time[old_count] = 0.0;
    peak_hook_pending_observed_time[old_count] = 0.0;
    peak_hook_pending_request_source[old_count] =
        PEAK_HOOK_REQUEST_SOURCE_NONE;
    peak_hook_pending_request_calls[old_count] = 0;
    peak_hook_pending_request_ratio[old_count] = 0.0;
    peak_hook_pending_request_global_overhead[old_count] = 0.0;
    peak_hook_pending_request_total_time[old_count] = 0.0;
    peak_hook_pending_request_rate[old_count] = 0.0;
    peak_hook_detach_profile_seconds[old_count] = 0.0;
    peak_hook_reattach_request_calls[old_count] = 0;
    peak_hook_reattach_request_calls_valid[old_count] = FALSE;
    peak_hook_cached_sample_calls[old_count] = 0;
    peak_hook_cached_sample_profile_seconds[old_count] = 0.0;
    peak_hook_cached_sample_valid[old_count] = FALSE;
    peak_hook_retry_count[old_count] = 0;
    peak_hook_last_retry_status[old_count] = PEAK_DETACH_STATUS_SAFE;
    hook_address[old_count] = NULL;
    peak_demangled_strings[old_count] = NULL;
    peak_need_detach[old_count] = FALSE;
    peak_detached[old_count] = FALSE;
    if (heartbeat_overhead != NULL) {
        heartbeat_overhead[old_count] = 0.0;
    }

    peak_hook_address_count = new_count;
    if (hook_id_out != NULL) {
        *hook_id_out = old_count;
    }
    return TRUE;
}

PeakDynamicAttachResult
peak_general_listener_dynamic_attach_symbol(const char* symbol_name,
                                            gpointer symbol_address,
                                            gsize symbol_size,
                                            const char* provider_name)
{
    (void)symbol_size;

    if (symbol_name == NULL || symbol_name[0] == '\0' ||
        symbol_address == NULL || peak_hook_address_count == 0) {
        return PEAK_DYNAMIC_ATTACH_NO_MATCH;
    }

    if (!peak_general_listener_controller_is_current_thread()) {
        g_printerr("[peak] refusing dynamic attach outside the general listener controller thread\n");
        return PEAK_DYNAMIC_ATTACH_FAILED;
    }

    PeakDynamicAttachResult result = PEAK_DYNAMIC_ATTACH_NO_MATCH;

    pthread_mutex_lock(&lock);
    if (peak_hook_strings == NULL ||
        hook_address == NULL ||
        array_listener == NULL ||
        peak_hook_states == NULL) {
        pthread_mutex_unlock(&lock);
        return PEAK_DYNAMIC_ATTACH_FAILED;
    }

    gboolean matched_target = FALSE;
    gboolean duplicate_address = FALSE;
    const char* target_for_new_generation = NULL;
    size_t selected_hook_id = (size_t)-1;

    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (peak_hook_strings[i] == NULL) {
            continue;
        }
        if (!peak_general_listener_dynamic_symbol_matches_target(
                peak_hook_strings[i],
                symbol_name,
                provider_name)) {
            continue;
        }

        matched_target = TRUE;
        if (target_for_new_generation == NULL) {
            target_for_new_generation = peak_hook_strings[i];
        }

        if (hook_address[i] == symbol_address) {
            duplicate_address = TRUE;
            result = PEAK_DYNAMIC_ATTACH_NO_MATCH;
            break;
        }

        if (selected_hook_id == (size_t)-1 &&
            hook_address[i] == NULL &&
            array_listener[i] == NULL &&
            peak_hook_states[i] == PEAK_HOOK_UNRESOLVED) {
            selected_hook_id = i;
        }
    }

    if (result == PEAK_DYNAMIC_ATTACH_NO_MATCH &&
        matched_target &&
        !duplicate_address &&
        selected_hook_id == (size_t)-1) {
        if (!peak_general_listener_expand_dynamic_hook_tables_unlocked(
                target_for_new_generation,
                &selected_hook_id)) {
            pthread_mutex_unlock(&lock);
            return PEAK_DYNAMIC_ATTACH_FAILED;
        }
    }

    if (!duplicate_address && selected_hook_id != (size_t)-1) {
        size_t i = selected_hook_id;
        if (!peak_general_listener_attach_target_is_supported(symbol_name,
                                                              symbol_address)) {
            g_printerr("[peak] skipping dynamic attach for hook %lu (%s) from %s: unsafe Gum prologue\n",
                       (unsigned long)i,
                       symbol_name,
                       provider_name != NULL ? provider_name : "<unknown>");
            pthread_mutex_unlock(&lock);
            return PEAK_DYNAMIC_ATTACH_FAILED;
        }
        GumInvocationListener* new_listener =
            g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);
        PEAKGENERAL_LISTENER(new_listener)->hook_id = i;

        PeakDetachRequest mutation_request = {
            .hook_id = i,
            .symbol_name = peak_hook_strings[i],
            .function_address = symbol_address,
            .interceptor = interceptor,
            .listener = new_listener,
            .operation = PEAK_DETACH_OPERATION_ATTACH
        };
        PeakDetachStatus detach_status = PEAK_DETACH_STATUS_ERROR;
        gboolean prepare_ok;

#ifdef PEAK_ENABLE_TEST_HOOKS
        if (peak_env_truthy_general(
                g_getenv("PEAK_TEST_DYNAMIC_ATTACH_PREPARE_ERROR"))) {
            prepare_ok = FALSE;
            detach_status = PEAK_DETACH_STATUS_ERROR;
        } else if (peak_env_truthy_general(
                       g_getenv("PEAK_TEST_DYNAMIC_ATTACH_PREPARE_TIMEOUT"))) {
            prepare_ok = FALSE;
            detach_status = PEAK_DETACH_STATUS_TIMEOUT;
        } else
#endif
        {
            prepare_ok = peak_detach_controller_prepare_hook_mutation(
                &mutation_request,
                &detach_status);
        }
        if (!prepare_ok) {
            result =
                dlopen_interceptor_dynamic_attach_prepare_is_retryable(
                    detach_status) ?
                         PEAK_DYNAMIC_ATTACH_RETRY :
                         PEAK_DYNAMIC_ATTACH_FAILED;
            peak_log_debug("[peak] %s JIT attach for hook %lu (%s) from %s: %s\n",
                       result == PEAK_DYNAMIC_ATTACH_RETRY ? "retrying" : "skipping",
                       (unsigned long)i,
                       symbol_name,
                       provider_name != NULL ? provider_name : "<unknown>",
                       peak_detach_controller_status_string(detach_status));
            peak_general_listener_free(PEAKGENERAL_LISTENER(new_listener));
            g_object_unref(new_listener);
            pthread_mutex_unlock(&lock);
            return result;
        }

        gum_interceptor_begin_transaction(interceptor);
        GumAttachReturn attach_status =
            peak_general_listener_attach_exact(interceptor,
                                               symbol_address,
                                               new_listener);
        gum_interceptor_end_transaction(interceptor);

        if (!peak_detach_controller_finish_hook_mutation(&mutation_request,
                                                         &detach_status)) {
            peak_detach_controller_abort_after_failed_finish("JIT attach finish",
                                                            detach_status);
        }

        if (attach_status == GUM_ATTACH_OK) {
            hook_address[i] = symbol_address;
            g_free(peak_demangled_strings[i]);
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
            array_listener[i] = new_listener;
            array_listener_gum_detached[i] = FALSE;
            array_listener_gum_detach_flushed[i] = TRUE;
            peak_general_controller_reset_retry_unlocked(i);
            peak_general_controller_set_state_unlocked(i, PEAK_HOOK_ATTACHED);
            result = PEAK_DYNAMIC_ATTACH_ATTACHED;
        } else {
            g_printerr("[peak] Gum dynamic attach failed for hook %lu (%s) from %s, status=%d\n",
                       (unsigned long)i,
                       symbol_name,
                       provider_name != NULL ? provider_name : "<unknown>",
                       attach_status);
            peak_general_listener_free(PEAKGENERAL_LISTENER(new_listener));
            g_object_unref(new_listener);
            result = PEAK_DYNAMIC_ATTACH_FAILED;
        }
    }

    pthread_mutex_unlock(&lock);
    return result;
}
