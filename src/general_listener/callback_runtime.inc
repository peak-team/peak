void
peak_general_listener_suspend_callbacks(void)
{
    atomic_store_explicit(&peak_general_callbacks_suspended,
                          TRUE,
                          memory_order_release);
}

void
peak_general_listener_note_mpi_finalize_requested(void)
{
    atomic_store_explicit(&peak_general_mpi_finalize_requested,
                          TRUE,
                          memory_order_release);
}

static void
peak_general_listener_abandon_current_invocation(PeakInvocationData* priv,
                                                 gboolean use_pause_guard)
{
    if (priv == NULL || !priv->initialized) {
        return;
    }

    if (!peak_general_listener_pop_invocation(priv, NULL)) {
        return;
    }
    if (thread_data.level == 0) {
        void* tmp_ptr = thread_data.child_time;
        thread_data.child_time = NULL;
        if (use_pause_guard) {
            pthread_pause_disable();
        }
        g_free(tmp_ptr);
        if (use_pause_guard) {
            pthread_pause_enable();
        }
    } else if (thread_data.child_time != NULL) {
        thread_data.child_time[thread_data.level] = 0.0;
    }
}

static gboolean
peak_general_listener_pop_invocation(PeakInvocationData* priv,
                                     gdouble* child_duration_out)
{
    if (child_duration_out != NULL) {
        *child_duration_out = 0.0;
    }

    if (priv == NULL || !priv->initialized ||
        thread_data.child_time == NULL ||
        thread_data.level == 0 ||
        priv->stack_level == 0 ||
        thread_data.level < priv->stack_level) {
        if (priv != NULL) {
            priv->initialized = FALSE;
            priv->stack_level = 0;
        }
        return FALSE;
    }

    while (thread_data.level > priv->stack_level) {
        thread_data.level--;
        thread_data.child_time[thread_data.level] = 0.0;
    }

    thread_data.level--;
    if (child_duration_out != NULL) {
        *child_duration_out = thread_data.child_time[thread_data.level];
    }
    thread_data.child_time[thread_data.level] = 0.0;
    priv->initialized = FALSE;
    priv->stack_level = 0;
    return TRUE;
}

static gdouble
peak_general_listener_exclusive_duration(gdouble total_duration,
                                         gdouble child_duration)
{
    if (child_duration >= total_duration) {
        return 0.0;
    }

    return total_duration - child_duration;
}

static void
peak_general_listener_on_enter(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    if (!listener || g_object_is_floating(listener)) {
            return;
    }
    gum_interceptor_ignore_current_thread(interceptor);
    PeakInvocationData* priv = GUM_IC_GET_INVOCATION_DATA(ic, PeakInvocationData);
    priv->initialized = FALSE;
    priv->stack_level = 0;
    if (atomic_load_explicit(&peak_general_callbacks_suspended,
                             memory_order_acquire)) {
        gum_interceptor_unignore_current_thread(interceptor);
        return;
    }

    PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
    pthread_t my_tid = pthread_self();
    gboolean mapped_found = FALSE;
    size_t mapped_tid = pthread_listener_lookup_thread(my_tid, &mapped_found);
    if (!mapped_found || mapped_tid >= peak_max_num_threads) {
        mapped_tid = 0;
    }
    thread_data.self_mapped_id = mapped_tid;
    thread_data.self_mapped_known = mapped_found && mapped_tid < peak_max_num_threads;
    if (peak_detach_cost == 0 && heartbeat_time == 0 &&
        !peak_detach_count_overridden) {
        // g_print ("hook_id %lu tid %lu mapped %lu\n", hook_id, pthread_self(), mapped_tid);
        // g_print ("hook_id %lu max %lu tid %lu ncall %p \n", hook_id, peak_max_num_threads, mapped_tid, self->num_calls);
        size_t index = mapped_tid;
        if (thread_data.child_time == NULL) {
            thread_data.level = 0;
            thread_data.capacity = 16;
            thread_data.child_time = g_new(gdouble, 16);
        }
        thread_data.child_time[thread_data.level] = 0.0;
        thread_data.level++;
        if (thread_data.level == thread_data.capacity) {
            thread_data.capacity *= 2;
            thread_data.child_time = g_renew(double, thread_data.child_time, thread_data.capacity);
        }
        (void)peak_general_listener_num_calls_increment(
            &self->num_calls[index]);
        // g_printerr ("hook_id %lu time %f count %lu\n", hook_id, *current_time, self->num_calls[mapped_tid]);
    } else {
        // g_print ("hook_id %lu tid %lu mapped %lu\n", hook_id, pthread_self(), mapped_tid);
        // g_print ("hook_id %lu max %lu tid %lu ncall %p \n", hook_id, peak_max_num_threads, mapped_tid, self->num_calls);
        size_t index = mapped_tid;
        if (thread_data.child_time == NULL) {
            thread_data.level = 0;
            thread_data.capacity = 16;
            pthread_pause_disable();
            thread_data.child_time = g_new(gdouble, 16);
            pthread_pause_enable();
        }
        thread_data.child_time[thread_data.level] = 0.0;
        thread_data.level++;
        if (thread_data.level == thread_data.capacity) {
            thread_data.capacity *= 2;
            pthread_pause_disable();
            thread_data.child_time = g_renew(double, thread_data.child_time, thread_data.capacity);
            pthread_pause_enable();
        }
        gulong current_num_calls =
            peak_general_listener_num_calls_increment(
                &self->num_calls[index]);
        size_t hook_id = self->hook_id;
        gboolean detach_requested = FALSE;

        pthread_mutex_lock(&lock);
        if (current_num_calls >= peak_detach_count) {
            gulong total_num_calls = 0;
            for (size_t j = 0; j < peak_max_num_threads; j++) {
                total_num_calls += peak_general_listener_num_calls_load(
                    &self->num_calls[j]);
            }
            detach_requested =
                peak_general_listener_request_detach_with_context_unlocked(
                    hook_id,
                    PEAK_HOOK_REQUEST_SOURCE_DETACH_COUNT,
                    total_num_calls,
                    0.0,
                    0.0,
                    peak_second() - peak_main_time,
                    0.0);
        }
        pthread_mutex_unlock(&lock);
        if (detach_requested) {
            peak_general_listener_controller_wake();
        }
        // gum_interceptor_revert(interceptor, hook_address[hook_id]);
        // g_printerr ("revert hook_id %lu %p\n", hook_id, hook_address[hook_id]);

        if (check_interval != 0) pthread_pause_enable();
        else pthread_pause_disable();
    }
    priv->start_time = peak_second();
    priv->stack_level = thread_data.level;
    priv->initialized = TRUE;
    gum_interceptor_unignore_current_thread(interceptor);
}

static void
peak_general_listener_on_leave(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    double end_time = peak_second();
    gum_interceptor_ignore_current_thread(interceptor);
    if (peak_detach_cost == 0 && heartbeat_time == 0 &&
        !peak_detach_count_overridden) {
        if (!listener || g_object_is_floating(listener)) {
            thread_data.level--;
            if (thread_data.level == 0) {
                void* tmp_ptr = thread_data.child_time;
                thread_data.child_time = NULL;
                g_free(tmp_ptr);
            }
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        PeakInvocationData* priv = GUM_IC_GET_INVOCATION_DATA(ic, PeakInvocationData);
        if (!priv->initialized) {
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        if (atomic_load_explicit(&peak_general_callbacks_suspended,
                                 memory_order_acquire)) {
            peak_general_listener_abandon_current_invocation(priv, FALSE);
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
        // PeakGeneralState* state = GUM_IC_GET_FUNC_DATA(ic, PeakGeneralState*);
        // size_t hook_id = state->hook_id;
        gboolean mapped_found = FALSE;
        size_t mapped_tid = pthread_listener_lookup_thread(pthread_self(), &mapped_found);
        if (!mapped_found || mapped_tid >= peak_max_num_threads) {
            mapped_tid = 0;
        }
        end_time = end_time - priv->start_time;
        size_t index = mapped_tid;
        gdouble child_duration = 0.0;
        if (!peak_general_listener_pop_invocation(priv, &child_duration)) {
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        if (end_time > self->max_time[index])
            self->max_time[index] = end_time;
        if (end_time < self->min_time[index] ||
            peak_general_listener_num_calls_load(&self->num_calls[index]) == 1)
            self->min_time[index] = end_time;
        self->total_time[index] += end_time;
        // g_printerr ("hook_id %lu time %f endtime %f child_time %f count %lu\n", hook_id, *current_time, end_time, *child_time, self->num_calls[index]);
        if (thread_data.level > 0)
            thread_data.child_time[thread_data.level - 1] += end_time;
        self->exclusive_time[index] +=
            peak_general_listener_exclusive_duration(
                end_time,
                child_duration);
        if (thread_data.level == 0) {
            void* tmp_ptr = thread_data.child_time;
            thread_data.child_time = NULL;
            g_free(tmp_ptr);
        }
    } else {
        pthread_pause_enable();
        // g_printerr("pthread_pause_enable\n");
        if (!listener || g_object_is_floating(listener)) {
            thread_data.level--;
            if (thread_data.level == 0) {
                void* tmp_ptr = thread_data.child_time;
                thread_data.child_time = NULL;
                pthread_pause_disable();
                g_free(tmp_ptr);
                pthread_pause_enable();
            }
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        PeakInvocationData* priv = GUM_IC_GET_INVOCATION_DATA(ic, PeakInvocationData);
        if (!priv->initialized) {
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        if (atomic_load_explicit(&peak_general_callbacks_suspended,
                                 memory_order_acquire)) {
            peak_general_listener_abandon_current_invocation(priv, TRUE);
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
        // PeakGeneralState* state = GUM_IC_GET_FUNC_DATA(ic, PeakGeneralState*);
        // size_t hook_id = state->hook_id;
        gboolean mapped_found = FALSE;
        size_t mapped_tid = pthread_listener_lookup_thread(pthread_self(), &mapped_found);
        if (!mapped_found || mapped_tid >= peak_max_num_threads) {
            mapped_tid = 0;
        }
        end_time = end_time - priv->start_time;
        size_t index = mapped_tid;
        gdouble child_duration = 0.0;
        if (!peak_general_listener_pop_invocation(priv, &child_duration)) {
            gum_interceptor_unignore_current_thread(interceptor);
            return;
        }
        if (end_time > self->max_time[index])
            self->max_time[index] = end_time;
        if (end_time < self->min_time[index] ||
            peak_general_listener_num_calls_load(&self->num_calls[index]) == 1)
            self->min_time[index] = end_time;
        self->total_time[index] += end_time;
        // g_printerr ("hook_id %lu time %f endtime %f child_time %f count %lu\n", hook_id, *current_time, end_time, *child_time, self->num_calls[index]);
        if (thread_data.level > 0)
            thread_data.child_time[thread_data.level - 1] += end_time;
        self->exclusive_time[index] +=
            peak_general_listener_exclusive_duration(
                end_time,
                child_duration);
        if (thread_data.level == 0) {
            void* tmp_ptr = thread_data.child_time;
            thread_data.child_time = NULL;
            pthread_pause_disable();
            g_free(tmp_ptr);
            pthread_pause_enable();
        }
    }
    gum_interceptor_unignore_current_thread(interceptor);
}

static void
peak_general_listener_class_init(PeakGeneralListenerClass* klass)
{
    (void)PEAKGENERAL_IS_LISTENER;
    (void)glib_autoptr_cleanup_PeakGeneralListener;
}

static void
peak_general_listener_iface_init(gpointer g_iface,
                                 gpointer iface_data)
{
    GumInvocationListenerInterface* iface = g_iface;

    iface->on_enter = peak_general_listener_on_enter;
    iface->on_leave = peak_general_listener_on_leave;
}

static void
peak_general_listener_init(PeakGeneralListener* self)
{
    size_t total_count = peak_max_num_threads;
    self->num_calls = g_new0(gulong, total_count);
    self->total_time = g_new0(gdouble, total_count);
    self->exclusive_time = g_new0(gdouble, total_count);
    self->max_time = g_new0(gfloat, total_count);
    self->min_time = g_new0(gfloat, total_count);
    self->target_thread_called = g_new0(gboolean, total_count);
    // g_print ("total count %lu self->num_calls %lu\n", total_count, self->num_calls[0]);
}

void
peak_general_listener_free(PeakGeneralListener* self)
{
    g_free(self->num_calls);
    g_free(self->total_time);
    g_free(self->exclusive_time);
    g_free(self->max_time);
    g_free(self->min_time);
    g_free(self->target_thread_called);
    self->target_thread_called = NULL;
}

__attribute__((noinline)) static void peak_general_overhead_dummy_func()
{
    struct timespec ts = { 0, 1 }; // Sleep for 1 nanosecond
    nanosleep(&ts, NULL);
}
