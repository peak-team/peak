static gboolean
peak_general_listener_parse_detach_count_override(gulong* count_out)
{
    const char* value = getenv("PEAK_DETACH_COUNT");

    if (value == NULL || value[0] == '\0') {
        return FALSE;
    }

    errno = 0;
    char* end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);

    if (errno == ERANGE || end == value || *end != '\0' || parsed == 0) {
        peak_log_info("[peak] ignoring invalid PEAK_DETACH_COUNT=%s\n", value);
        return FALSE;
    }

    if (count_out != NULL) {
        *count_out = (gulong)parsed;
    }
    return TRUE;
}

static unsigned int
peak_general_listener_parse_uint_env_default(const char* name,
                                             unsigned int default_value)
{
    const char* value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return default_value;
    }

    errno = 0;
    char* end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' ||
        parsed > G_MAXUINT) {
        peak_log_info("[peak] ignoring invalid %s=%s\n", name, value);
        return default_value;
    }

    return (unsigned int)parsed;
}

static gboolean
peak_general_listener_parse_positive_uint_text(const char* value,
                                               unsigned int* parsed_out)
{
    const char* cursor;
    char* end = NULL;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') {
        return FALSE;
    }
    for (cursor = value; *cursor != '\0'; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            return FALSE;
        }
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' || parsed == 0 ||
        parsed > G_MAXUINT) {
        return FALSE;
    }
    if (parsed_out != NULL) {
        *parsed_out = (unsigned int)parsed;
    }
    return TRUE;
}

static unsigned int
peak_general_listener_local_mpi_ranks(void)
{
    static const char* local_size_envs[] = {
        "MPI_LOCALNRANKS",
        "OMPI_COMM_WORLD_LOCAL_SIZE",
        "MV2_COMM_WORLD_LOCAL_SIZE",
        "PMI_LOCAL_SIZE",
    };

    for (size_t i = 0; i < G_N_ELEMENTS(local_size_envs); i++) {
        unsigned int parsed = 0;
        if (peak_general_listener_parse_positive_uint_text(
                getenv(local_size_envs[i]),
                &parsed)) {
            return parsed;
        }
    }
    return 1U;
}

static void
peak_general_listener_init_reattach_policy_once(void)
{
    unsigned int cooldown_ms =
        peak_general_listener_parse_uint_env_default(
            PEAK_REATTACH_COOLDOWN_MS_ENV,
            peak_reattach_default_cooldown_ms);
    peak_reattach_cooldown_s = (double)cooldown_ms / 1000.0;
}

static void
peak_general_listener_init_reattach_policy(void)
{
    if (g_once_init_enter(&peak_reattach_policy_initialized)) {
        peak_general_listener_init_reattach_policy_once();
        g_once_init_leave(&peak_reattach_policy_initialized, 1);
    }
}

static gboolean
peak_env_value_truthy(const char* value)
{
    return value != NULL &&
           (g_ascii_strcasecmp(value, "1") == 0 ||
            g_ascii_strcasecmp(value, "true") == 0 ||
            g_ascii_strcasecmp(value, "yes") == 0 ||
            g_ascii_strcasecmp(value, "on") == 0);
}

static gboolean
peak_env_value_falsey(const char* value)
{
    return value != NULL &&
           (g_ascii_strcasecmp(value, "0") == 0 ||
            g_ascii_strcasecmp(value, "false") == 0 ||
            g_ascii_strcasecmp(value, "off") == 0 ||
            g_ascii_strcasecmp(value, "no") == 0);
}

static void
peak_general_listener_init_attach_policy_once(void)
{
    gboolean policy_valid = FALSE;

    peak_allow_unsafe_gum_prologue =
        peak_env_value_truthy(g_getenv(PEAK_ALLOW_UNSAFE_GUM_PROLOGUE_ENV));

    peak_unsafe_gum_prologue_policy =
        peak_unsafe_gum_prologue_policy_from_env(
            g_getenv(PEAK_UNSAFE_GUM_PROLOGUE_POLICY_ENV),
            &policy_valid);
    if (!policy_valid) {
        peak_log_info("[peak] ignoring invalid %s=%s; using %s policy\n",
                   PEAK_UNSAFE_GUM_PROLOGUE_POLICY_ENV,
                   g_getenv(PEAK_UNSAFE_GUM_PROLOGUE_POLICY_ENV),
                   peak_unsafe_gum_prologue_policy_name(
                       PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT));
        peak_unsafe_gum_prologue_policy =
            PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT;
    }
}

static void
peak_general_listener_init_attach_policy(void)
{
    if (g_once_init_enter(&peak_attach_policy_initialized)) {
        peak_general_listener_init_attach_policy_once();
        g_once_init_leave(&peak_attach_policy_initialized, 1);
    }
}

gboolean
peak_general_listener_attach_target_is_supported(const char* symbol_name,
                                                 gpointer address)
{
    const char* reason = NULL;

    peak_general_listener_init_attach_policy();

    if (peak_allow_unsafe_gum_prologue) {
        return TRUE;
    }

    if (peak_gum_prologue_too_short_for_attach(address, &reason)) {
        g_printerr("[peak] skipping Gum attach for hook %s: target prologue is too small for Gum entry patch (reason=%s); target will remain unprofiled\n",
                   symbol_name != NULL ? symbol_name : "<unknown>",
                   reason != NULL ? reason : "unknown");
        return FALSE;
    }

    if (peak_unsafe_gum_prologue_check(address,
                                       peak_unsafe_gum_prologue_policy,
                                       &reason)) {
        g_printerr("[peak] skipping Gum attach for hook %s: target prologue is not safe for Gum relocation (reason=%s, policy=%s); set %s=1 to override\n",
                   symbol_name != NULL ? symbol_name : "<unknown>",
                   reason != NULL ? reason : "unknown",
                   peak_unsafe_gum_prologue_policy_name(
                       peak_unsafe_gum_prologue_policy),
                   PEAK_ALLOW_UNSAFE_GUM_PROLOGUE_ENV);
        return FALSE;
    }

    return TRUE;
}

gboolean
peak_general_listener_support_attach_target_is_supported(const char* symbol_name,
                                                         gpointer address)
{
    peak_general_listener_init_attach_policy();

    (void)symbol_name;
    (void)address;
    return TRUE;
}

static gboolean
peak_general_listener_startup_attach_can_skip_stop(void)
{
    DIR* dir = opendir("/proc/self/task");
    struct dirent* entry;
    unsigned int task_count = 0;

    if (dir == NULL) {
        return FALSE;
    }

    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;

        if (name[0] == '.') {
            continue;
        }
        task_count++;
        if (task_count > 1) {
            closedir(dir);
            return FALSE;
        }
    }

    closedir(dir);
    return task_count == 1;
}

gboolean
peak_general_listener_needs_dynamic_attach(void)
{
    return peak_dynamic_attach_needed;
}

static gboolean
peak_socket_reduce_fallback_enabled(void)
{
    const char* value =
        getenv(PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK_ENV);

    if (value == NULL || value[0] == '\0') {
        return TRUE;
    }
    if (peak_env_value_falsey(value)) {
        return FALSE;
    }
    if (peak_env_value_truthy(value)) {
        return TRUE;
    }

    peak_log_info("[peak] Ignoring invalid %s=%s; using fallback-to-local behavior\n",
               PEAK_OUTPUT_AGGREGATION_SOCKET_FALLBACK_ENV,
               value);
    return TRUE;
}

static void
peak_general_listener_mark_mpi_reducer_failed_closed(void)
{
    atomic_store_explicit(&peak_general_mpi_reducer_failed_closed,
                          TRUE,
                          memory_order_release);
}

gboolean
peak_general_listener_mpi_reducer_failed_closed(void)
{
    return atomic_load_explicit(&peak_general_mpi_reducer_failed_closed,
                                memory_order_acquire);
}

static long
peak_parse_long_env(const char* name)
{
    const char* value = getenv(name);
    char* end = NULL;
    long parsed;

    if (value == NULL || value[0] == '\0') {
        return -1;
    }

    parsed = strtol(value, &end, 10);
    if (end == value || parsed < 0) {
        return -1;
    }

    return parsed;
}

static long
peak_mpi_env_size(void)
{
    static const char* names[] = {
        "PMI_SIZE",
        "PMIX_SIZE",
        "OMPI_COMM_WORLD_SIZE",
        "SLURM_NTASKS",
        NULL
    };

    for (const char** name = names; *name != NULL; name++) {
        long value = peak_parse_long_env(*name);
        if (value > 0) {
            return value;
        }
    }

    return -1;
}

static long
peak_mpi_env_rank(void)
{
    static const char* names[] = {
        "PMI_RANK",
        "PMIX_RANK",
        "OMPI_COMM_WORLD_RANK",
        "MV2_COMM_WORLD_RANK",
        "SLURM_PROCID",
        NULL
    };

    for (const char** name = names; *name != NULL; name++) {
        long value = peak_parse_long_env(*name);
        if (value >= 0) {
            return value;
        }
    }

    return -1;
}

static gboolean
peak_general_listener_should_print_text(gboolean rank_local_mpi_output)
{
    const char* value = getenv(PEAK_TEXT_OUTPUT_ENV);

    if (value != NULL && value[0] != '\0') {
        return peak_env_value_truthy(value);
    }

    return !rank_local_mpi_output;
}

typedef struct {
    size_t hook_id;
    GumInvocationListener* listener;
    PeakDetachOperation operation;
    PeakHookState stable_state;
    unsigned int retry_count;
    PeakDetachStatus last_retry_status;
    double pending_age_s;
} PeakGeneralBatchCandidate;

typedef enum {
    PEAK_GENERAL_SHUTDOWN_FAILURE_NONE = 0,
    PEAK_GENERAL_SHUTDOWN_FAILURE_PREPARE,
    PEAK_GENERAL_SHUTDOWN_FAILURE_SNAPSHOT_UNSAFE,
    PEAK_GENERAL_SHUTDOWN_FAILURE_PAUSE_FAILED
} PeakGeneralShutdownFailureBucket;

typedef struct {
    PeakGeneralShutdownFailureBucket bucket;
    PeakDetachStatus status;
} PeakGeneralShutdownFailure;

static double peak_general_controller_pending_age_for_trace_unlocked(size_t hook_id,
                                                                     double now);

static unsigned int
peak_general_controller_shutdown_drain_ms(void)
{
#ifdef PEAK_ENABLE_TEST_HOOKS
    const char* value = g_getenv("PEAK_TEST_CONTROLLER_SHUTDOWN_DRAIN_MS");

    if (value != NULL && value[0] != '\0') {
        char* end = NULL;
        unsigned long parsed = strtoul(value, &end, 10);

        if (end != value && *end == '\0' && parsed <= G_MAXUINT) {
            return (unsigned int)parsed;
        }
    }
#endif

    return peak_controller_shutdown_drain_ms;
}

static void
peak_general_shutdown_failure_note(PeakGeneralShutdownFailure* failure,
                                   PeakGeneralShutdownFailureBucket bucket,
                                   PeakDetachStatus status)
{
    if (failure == NULL) {
        return;
    }

    failure->bucket = bucket;
    failure->status = status;
}

static const char*
peak_general_shutdown_failure_bucket_string(
    const PeakGeneralShutdownFailure* failure)
{
    if (failure == NULL) {
        return "unknown";
    }

    switch (failure->bucket) {
        case PEAK_GENERAL_SHUTDOWN_FAILURE_PREPARE:
            return "prepare-exhausted";
        case PEAK_GENERAL_SHUTDOWN_FAILURE_SNAPSHOT_UNSAFE:
            return "snapshot-unsafe";
        case PEAK_GENERAL_SHUTDOWN_FAILURE_PAUSE_FAILED:
            return "pause-failed";
        case PEAK_GENERAL_SHUTDOWN_FAILURE_NONE:
        default:
            return "unknown";
    }
}

void peak_general_listener_controller_lock(void)
{
    pthread_mutex_lock(&lock);
}

void peak_general_listener_controller_unlock(void)
{
    pthread_mutex_unlock(&lock);
}

void peak_general_listener_controller_wake(void)
{
    pthread_mutex_lock(&general_controller_wake_mutex);
    pthread_cond_signal(&general_controller_wake_cond);
    pthread_mutex_unlock(&general_controller_wake_mutex);
}
