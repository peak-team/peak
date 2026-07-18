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

gboolean
peak_general_listener_needs_dynamic_attach(void)
{
    return peak_dynamic_attach_needed;
}

typedef struct {
    size_t hook_id;
    GumInvocationListener* listener;
    PeakDetachOperation operation;
    PeakGumTargetAttachPlan attach_plan;
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
