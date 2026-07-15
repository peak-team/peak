static gboolean str_equal_function_general(gconstpointer a, gconstpointer b) {
    return g_strcmp0((const gchar *)a, (const gchar *)b) == 0;
}

static gboolean
peak_env_truthy_general(const char* value)
{
    return value != NULL &&
           (g_ascii_strcasecmp(value, "1") == 0 ||
            g_ascii_strcasecmp(value, "true") == 0 ||
            g_ascii_strcasecmp(value, "yes") == 0 ||
            g_ascii_strcasecmp(value, "on") == 0);
}

static unsigned int
peak_general_controller_parse_uint_env(const char* name,
                                       unsigned int default_value)
{
    const char* value = g_getenv(name);
    char* end = NULL;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') {
        return default_value;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > G_MAXUINT) {
        peak_log_info("[peak] ignoring invalid %s=%s\n", name, value);
        return default_value;
    }

    return (unsigned int)parsed;
}

static void
peak_general_controller_init_retry_limits_once(void)
{
    unsigned int max_pending_age_ms =
        peak_general_controller_parse_uint_env(
            PEAK_CONTROLLER_MAX_PENDING_AGE_MS_ENV,
            (unsigned int)(peak_controller_default_max_pending_age_s * 1000.0));

    peak_controller_max_pending_age_s =
        max_pending_age_ms == 0 ? 0.0 : (double)max_pending_age_ms / 1000.0;
    peak_controller_max_retry_count =
        peak_general_controller_parse_uint_env(
            PEAK_CONTROLLER_MAX_RETRY_COUNT_ENV,
            peak_controller_default_max_retry_count);
}

static void
peak_general_controller_init_retry_limits(void)
{
    if (g_once_init_enter(&peak_controller_retry_limits_initialized)) {
        peak_general_controller_init_retry_limits_once();
        g_once_init_leave(&peak_controller_retry_limits_initialized, 1);
    }
}

static void
peak_general_controller_init_trace_config_once(void)
{
    const char* path = g_getenv("PEAK_DETACH_TRACE_PATH");

    if (path != NULL && path[0] != '\0') {
        peak_controller_trace_path = g_strdup(path);
        peak_controller_trace_enabled = TRUE;
    }
    peak_detach_controller_configure_trace_diagnostics(
        peak_controller_trace_enabled);
}

static void
peak_general_controller_init_trace_config(void)
{
    if (g_once_init_enter(&peak_controller_trace_config_initialized)) {
        peak_general_controller_init_trace_config_once();
        g_once_init_leave(&peak_controller_trace_config_initialized, 1);
    }
}

static gboolean
peak_general_controller_trace_enabled(void)
{
    return peak_controller_trace_enabled;
}

static unsigned long long
peak_general_listener_seconds_to_ns(double seconds)
{
    if (seconds <= 0.0) {
        return 0;
    }
    if (seconds >= (double)ULLONG_MAX / 1e9) {
        return ULLONG_MAX;
    }
    return (unsigned long long)(seconds * 1e9 + 0.5);
}

static unsigned long long
peak_general_listener_timespec_to_ns_saturating(const struct timespec* value)
{
    unsigned long long seconds;
    unsigned long long nanoseconds;

    if (value == NULL || value->tv_sec < 0 || value->tv_nsec < 0) {
        return 0;
    }
    seconds = (unsigned long long)value->tv_sec;
    nanoseconds = (unsigned long long)value->tv_nsec;
    if (seconds > ULLONG_MAX / 1000000000ULL) {
        return ULLONG_MAX;
    }
    seconds *= 1000000000ULL;
    if (nanoseconds > ULLONG_MAX - seconds) {
        return ULLONG_MAX;
    }
    return seconds + nanoseconds;
}

static gboolean
peak_general_listener_thread_cpu_ns(unsigned long long* out)
{
    struct timespec value;

    if (out == NULL || clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) {
        return FALSE;
    }
    *out = peak_general_listener_timespec_to_ns_saturating(&value);
    return TRUE;
}

static unsigned long long
peak_general_listener_monotonic_delta_ns(unsigned long long current,
                                         unsigned long long previous)
{
    return current >= previous ? current - previous : 0;
}

static void
peak_general_listener_management_cpu_add_saturating(
    unsigned long long delta_ns)
{
    unsigned long long observed = atomic_load_explicit(
        &peak_general_listener_heartbeat_management_cpu_ns,
        memory_order_relaxed);

    while (delta_ns != 0 && observed != ULLONG_MAX) {
        unsigned long long updated =
            delta_ns > ULLONG_MAX - observed
                ? ULLONG_MAX
                : observed + delta_ns;
        if (atomic_compare_exchange_weak_explicit(
                &peak_general_listener_heartbeat_management_cpu_ns,
                &observed,
                updated,
                memory_order_relaxed,
                memory_order_relaxed)) {
            break;
        }
    }
}

static void
peak_general_listener_management_cpu_checkpoint(
    unsigned long long* previous_ns,
    gboolean* previous_valid)
{
    unsigned long long current_ns;

    if (previous_ns == NULL || previous_valid == NULL ||
        !peak_general_listener_thread_cpu_ns(&current_ns)) {
        return;
    }
    if (*previous_valid) {
        peak_general_listener_management_cpu_add_saturating(
            peak_general_listener_monotonic_delta_ns(current_ns,
                                                     *previous_ns));
    }
    *previous_ns = current_ns;
    *previous_valid = TRUE;
}

static double
peak_general_listener_management_cpu_seconds(void)
{
    return (double)atomic_load_explicit(
               &peak_general_listener_heartbeat_management_cpu_ns,
               memory_order_relaxed) /
           1e9;
}

static double
peak_general_listener_elapsed_at(double now)
{
    unsigned long long start_ns =
        atomic_load_explicit(&peak_general_listener_runtime_start_ns,
                             memory_order_relaxed);
    double start_time = (double)start_ns / 1e9;

    if (start_time <= 0.0) {
        start_time = peak_main_time;
    }

    if (start_time > 0.0 && now > start_time) {
        return now - start_time;
    }
    if (peak_main_time > 0.0 && peak_main_time < 3600.0) {
        return peak_main_time;
    }
    return 1e-12;
}

static uint64_t
peak_general_listener_count_since_baseline(uint64_t count,
                                           const _Atomic unsigned long long* baseline)
{
    uint64_t baseline_count;

    if (baseline == NULL) {
        return 0;
    }
    baseline_count =
        atomic_load_explicit(baseline, memory_order_relaxed);
    if (baseline_count == 0) {
        return count;
    }
    return count > baseline_count ? count - baseline_count : 0;
}

static uint64_t
peak_general_listener_control_window_count_since_heartbeat(
    const PeakDetachAccountingSnapshot* accounting)
{
    if (accounting == NULL) {
        return 0;
    }
    return peak_general_listener_count_since_baseline(
        accounting->completed_stop_window_count,
        &peak_general_listener_heartbeat_control_baseline_count);
}

static uint64_t
peak_general_listener_failed_window_count_since_heartbeat(
    const PeakDetachAccountingSnapshot* accounting)
{
    if (accounting == NULL) {
        return 0;
    }
    return peak_general_listener_count_since_baseline(
        accounting->failed_stop_window_count,
        &peak_general_listener_heartbeat_control_baseline_failed_count);
}

static uint64_t
peak_general_listener_control_wall_ns_since_heartbeat(
    const PeakDetachAccountingSnapshot* accounting)
{
    if (accounting == NULL) {
        return 0;
    }
    return peak_general_listener_count_since_baseline(
        accounting->stop_window_wall_ns,
        &peak_general_listener_heartbeat_control_baseline_ns);
}

static gboolean
peak_general_listener_accounting_snapshot_is_finite(
    const PeakDetachAccountingSnapshot* accounting)
{
    return accounting != NULL &&
           accounting->completed_stop_window_count != UINT64_MAX &&
           accounting->failed_stop_window_count != UINT64_MAX &&
           accounting->stop_window_wall_ns != UINT64_MAX;
}

static PeakDetachAccountingSnapshot
peak_general_listener_zero_accounting_snapshot(void)
{
    PeakDetachAccountingSnapshot accounting = {
        .completed_stop_window_count = 0,
        .failed_stop_window_count = 0,
        .stop_window_wall_ns = 0
    };

    return accounting;
}

static gboolean
peak_general_listener_accounting_snapshot(PeakDetachAccountingSnapshot* out)
{
    PeakDetachAccountingSnapshot candidate =
        peak_general_listener_zero_accounting_snapshot();

    if (out == NULL) {
        return FALSE;
    }

    if (!peak_detach_controller_accounting_snapshot(&candidate) ||
        !peak_general_listener_accounting_snapshot_is_finite(&candidate)) {
        *out = peak_general_listener_zero_accounting_snapshot();
        return FALSE;
    }

    *out = candidate;
    return TRUE;
}

static gboolean
peak_general_listener_runtime_accounting_snapshot(
    PeakDetachAccountingSnapshot* out)
{
    gboolean current_valid =
        peak_general_listener_accounting_snapshot(out);
    gboolean baseline_valid = atomic_load_explicit(
        &peak_general_listener_heartbeat_control_baseline_valid,
        memory_order_acquire);

    return current_valid && baseline_valid;
}

static void
peak_general_listener_reset_final_report_snapshot(void)
{
    memset(&peak_general_listener_final_report_snapshot,
           0,
           sizeof(peak_general_listener_final_report_snapshot));
}

void
peak_general_listener_note_runtime_start(double start_time)
{
    PeakDetachAccountingSnapshot accounting_baseline =
        peak_general_listener_zero_accounting_snapshot();
    gboolean baseline_valid;

    peak_general_listener_reset_final_report_snapshot();
    baseline_valid =
        peak_general_listener_accounting_snapshot(&accounting_baseline);
    atomic_store_explicit(&peak_general_listener_runtime_start_ns,
                          peak_general_listener_seconds_to_ns(start_time),
                          memory_order_relaxed);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_ns,
        accounting_baseline.stop_window_wall_ns,
        memory_order_relaxed);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_count,
        accounting_baseline.completed_stop_window_count,
        memory_order_relaxed);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_failed_count,
        accounting_baseline.failed_stop_window_count,
        memory_order_relaxed);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_control_baseline_valid,
        baseline_valid,
        memory_order_release);
    atomic_store_explicit(
        &peak_general_listener_heartbeat_management_cpu_ns,
        0,
        memory_order_relaxed);
}

static void
peak_general_controller_trace_mutation_detail(size_t hook_id,
                                              PeakDetachOperation operation,
                                              const char* result,
                                              gboolean physical,
                                              PeakDetachStatus status,
                                              unsigned int retry_count,
                                              double pending_age_s,
                                              unsigned int batch_size,
                                              double stop_window_us,
                                              unsigned int batch_id,
                                              PeakDetachStatus last_retry_status)
{
    const char* path = peak_controller_trace_path;
    const PeakDetachFailureDetail* failure_detail =
        peak_detach_controller_last_failure_detail();
    const char* failure_reason =
        failure_detail != NULL && failure_detail->reason != NULL
            ? failure_detail->reason
            : "none";
    PeakHookRequestSource request_source = PEAK_HOOK_REQUEST_SOURCE_NONE;
    gulong request_calls = 0;
    double request_ratio = 0.0;
    double request_global_overhead = 0.0;
    double request_total_time = 0.0;
    double request_rate = 0.0;
    double trace_now;
    double trace_elapsed_time;
    uint64_t accounting_wall_ns;
    double accounting_wall_s = 0.0;
    double accounting_ratio = 0.0;
    PeakDetachAccountingSnapshot detach_accounting;
    gboolean accounting_valid;
    FILE* fp;

    if (path == NULL || path[0] == '\0') {
        return;
    }

    if (hook_id < peak_hook_address_count) {
        if (peak_hook_pending_request_source != NULL) {
            request_source = peak_hook_pending_request_source[hook_id];
        }
        if (peak_hook_pending_request_calls != NULL) {
            request_calls = peak_hook_pending_request_calls[hook_id];
        }
        if (peak_hook_pending_request_ratio != NULL) {
            request_ratio = peak_hook_pending_request_ratio[hook_id];
        }
        if (peak_hook_pending_request_global_overhead != NULL) {
            request_global_overhead =
                peak_hook_pending_request_global_overhead[hook_id];
        }
        if (peak_hook_pending_request_total_time != NULL) {
            request_total_time = peak_hook_pending_request_total_time[hook_id];
        }
        if (peak_hook_pending_request_rate != NULL) {
            request_rate = peak_hook_pending_request_rate[hook_id];
        }
    }

    trace_now = peak_second();
    trace_elapsed_time = peak_general_listener_elapsed_at(trace_now);
    if (trace_elapsed_time <= 0.0) {
        trace_elapsed_time = 1e-12;
    }
    accounting_valid =
        peak_general_listener_runtime_accounting_snapshot(&detach_accounting);
    accounting_wall_ns =
        peak_general_listener_control_wall_ns_since_heartbeat(
            &detach_accounting);
    accounting_wall_s = (double)accounting_wall_ns / 1e9;
    accounting_ratio = accounting_wall_s / trace_elapsed_time;

    pthread_mutex_lock(&detach_trace_mutex);
    fp = fopen(path, "a");
    if (fp != NULL) {
        fprintf(fp,
                "%.9f,%lu,%s,%s,%s,%d,%s,%u,%.9f,%u,%.3f,%u,%s,%s,%ld,0x%llx,0x%llx,%s,%lu,%.12f,%.9f,%.9f,%.9f,%llu,%.9f,%.9f,%d,%llu\n",
                trace_now,
                (unsigned long)hook_id,
                hook_id < peak_hook_address_count && peak_hook_strings != NULL &&
                        peak_hook_strings[hook_id] != NULL
                    ? peak_hook_strings[hook_id]
                    : "<unknown>",
                peak_detach_controller_operation_string(operation),
                result != NULL ? result : "<unknown>",
                physical ? 1 : 0,
                peak_detach_controller_status_string(status),
                retry_count,
                pending_age_s,
                batch_size,
                stop_window_us,
                batch_id,
                peak_detach_controller_status_string(last_retry_status),
                failure_reason,
                failure_detail != NULL ? failure_detail->tid : 0,
                (unsigned long long)(failure_detail != NULL ? failure_detail->pc : 0),
                (unsigned long long)(failure_detail != NULL ? failure_detail->aux : 0),
                peak_hook_request_source_string(request_source),
                request_calls,
                request_ratio,
                request_global_overhead,
                request_total_time,
                request_rate,
                (unsigned long long)
                    peak_general_listener_control_window_count_since_heartbeat(
                        &detach_accounting),
                accounting_wall_s,
                accounting_ratio,
                accounting_valid ? 1 : 0,
                (unsigned long long)
                    peak_general_listener_failed_window_count_since_heartbeat(
                        &detach_accounting));
        fclose(fp);
    }
    pthread_mutex_unlock(&detach_trace_mutex);
}

static void
peak_general_controller_trace_mutation(size_t hook_id,
                                       PeakDetachOperation operation,
                                       const char* result,
                                       gboolean physical,
                                       PeakDetachStatus status)
{
    if (!peak_general_controller_trace_enabled()) {
        return;
    }

    peak_general_controller_trace_mutation_detail(hook_id,
                                                  operation,
                                                  result,
                                                  physical,
                                                  status,
                                                  hook_id < peak_hook_address_count &&
                                                          peak_hook_retry_count != NULL
                                                      ? peak_hook_retry_count[hook_id]
                                                      : 0,
                                                  0.0,
                                                  1,
                                                  strcmp(result, "prepare-failed") == 0
                                                      ? 0.0
                                                      : peak_detach_controller_last_stop_window_us(),
                                                  0,
                                                  hook_id < peak_hook_address_count &&
                                                          peak_hook_last_retry_status != NULL
                                                      ? peak_hook_last_retry_status[hook_id]
                                                      : PEAK_DETACH_STATUS_SAFE);
}

static unsigned int
peak_general_controller_allocate_batch_id_unlocked(void)
{
    unsigned int batch_id = peak_general_controller_next_batch_id++;

    if (batch_id == 0) {
        batch_id = peak_general_controller_next_batch_id++;
    }
    if (peak_general_controller_next_batch_id == 0) {
        peak_general_controller_next_batch_id = 1;
    }
    return batch_id;
}

static gboolean
peak_symbol_should_use_cpp_map(const char* symbol)
{
    if (peak_env_truthy_general(g_getenv("PEAK_ENABLE_CXX_SYMBOL_SCAN"))) {
        return TRUE;
    }

    return symbol != NULL &&
           (strstr(symbol, "::") != NULL ||
            strchr(symbol, '(') != NULL ||
            strchr(symbol, '<') != NULL ||
            strstr(symbol, "operator") != NULL);
}

gpointer
peak_general_listener_find_function(const char* symbol)
{
    gpointer address;

    if (symbol == NULL) {
        return NULL;
    }

    address = gum_find_function(symbol);
    if (address != NULL) {
        return address;
    }

    /*
     * Gum's ELF export walk intentionally excludes callable STT_NOTYPE (and
     * may omit IFUNC entries).  Let the platform loader resolve the exact
     * globally visible symbol so startup targets use the same callable entry
     * semantics as runtime provider-scoped resolution.
     */
    dlerror();
    address = dlsym(RTLD_DEFAULT, symbol);
    if (dlerror() != NULL) {
        return NULL;
    }
    return address;
}
