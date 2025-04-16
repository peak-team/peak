#include "general_listener.h"

#define PEAK_SIG_STOP (SIGRTMIN + 0)
#define PEAK_SIG_CONT (SIGRTMIN + 1)

static GumInterceptor* interceptor;
static GumInvocationListener** array_listener;
static gboolean* array_listener_detached;
static gboolean* array_listener_reattached;
extern GumMetalHashTable* peak_tid_mapping;
extern gboolean* peak_need_detach;
extern gboolean* peak_detached;
extern PeakHeartbeatArgs* args;
extern gdouble* heartbeat_overhead;
extern gboolean** peak_target_thread_called;
static gpointer* hook_address = NULL;
static double peak_general_overhead;
extern size_t peak_hook_address_count;
extern char** peak_hook_strings;
static char** peak_demangled_strings;
extern gulong peak_max_num_threads;
extern double peak_main_time;
extern float peak_detach_cost;
extern float target_profile_ratio;
extern unsigned int post_wait_interval;
extern unsigned long long sig_cont_wait_interval;
static gulong peak_detach_count = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

gboolean heartbeat_running = true; 

static void peak_general_listener_iface_init(gpointer g_iface, gpointer iface_data);

#define PEAKGENERAL_TYPE_LISTENER (peak_general_listener_get_type())
G_DECLARE_FINAL_TYPE(PeakGeneralListener, peak_general_listener, PEAKGENERAL, LISTENER, GObject)
G_DEFINE_TYPE_EXTENDED(PeakGeneralListener,
                       peak_general_listener,
                       G_TYPE_OBJECT,
                       0,
                       G_IMPLEMENT_INTERFACE(GUM_TYPE_INVOCATION_LISTENER,
                                             peak_general_listener_iface_init))
typedef struct _PeakGeneralThreadState {
    gulong level;
    gulong capacity;
    gdouble* child_time;
} PeakGeneralThreadState;

static __thread PeakGeneralThreadState thread_data;

sem_t pthread_pause_sem;
pthread_once_t pthread_pause_once_ctrl = PTHREAD_ONCE_INIT;

void pthread_pause_handler(int signal)
{
    //Post semaphore to confirm that signal is handled
    sem_post(&pthread_pause_sem);
    //Suspend if needed
    if (signal == PEAK_SIG_STOP) {
        sigset_t new_mask, original_mask, wait_set;

        // Block all signals except PEAK_SIG_CONT
        sigfillset(&new_mask);
        sigdelset(&new_mask, PEAK_SIG_CONT);

        // Apply the new signal mask
        pthread_sigmask(SIG_SETMASK, &new_mask, &original_mask);

        // Prepare to wait for PEAK_SIG_CONT
        sigemptyset(&wait_set);
        sigaddset(&wait_set, PEAK_SIG_CONT);

        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        // in nanoseconds
        timeout.tv_sec += sig_cont_wait_interval / 1000000000;
        timeout.tv_nsec += sig_cont_wait_interval % 1000000000;

        // ts.tv_nsec overflow
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec += 1;
            timeout.tv_nsec -= 1000000000;
        }

        // Wait for the signal with timeout
        sigtimedwait(&wait_set, NULL, &timeout);

        // Restore the original signal mask
        pthread_sigmask(SIG_SETMASK, &original_mask, NULL);

        // sigset_t sigset;
        // struct timespec timeout;
        // sigfillset(&sigset);
        // sigdelset(&sigset, PEAK_SIG_STOP);
        // sigdelset(&sigset, PEAK_SIG_CONT);
        // /*TODO: need to make this a standalone parameter*/
        // // Set the timeout 
        // timeout.tv_sec += post_wait_interval;
        // sigtimedwait(&sigset, NULL, &timeout);
        // // sigsuspend(&sigset); //Wait for next signal
    } else
        return;
}

void pthread_pause_once(void)
{
    sem_init(&pthread_pause_sem, 0, 1);

    // Prepare sigset
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, PEAK_SIG_STOP);
    sigaddset(&sigset, PEAK_SIG_CONT);   

    //Register signal handlers
    //We now use sigaction() instead of signal(), because it supports SA_RESTART
    const struct sigaction pause_sa = {
        .sa_handler = pthread_pause_handler,
        .sa_mask = sigset,
        .sa_flags = SA_RESTART,
        .sa_restorer = NULL
    };
    sigaction(PEAK_SIG_STOP, &pause_sa, NULL);
    sigaction(PEAK_SIG_CONT, &pause_sa, NULL);  
}

#define pthread_pause_init() (pthread_once(&pthread_pause_once_ctrl, &pthread_pause_once))

void pthread_pause_enable()
{
    pthread_pause_init();
    //Prepare sigset
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, PEAK_SIG_STOP);
    sigaddset(&sigset, PEAK_SIG_CONT);
    //UnBlock signals
    pthread_sigmask(SIG_UNBLOCK, &sigset, NULL);
}

void pthread_pause_disable()
{
    //This is important for when you want to do some signal unsafe stuff
    //Eg.: locking mutex, calling printf() which has internal mutex, etc...
    //After unlocking mutex, you can enable pause again.
    pthread_pause_init();

    //Make sure all signals are dispatched before we block them
    //sem_wait(&pthread_pause_sem);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    // in nanoseconds
    ts.tv_sec += post_wait_interval / 1000000000;
    ts.tv_nsec += post_wait_interval % 1000000000;

    // ts.tv_nsec overflow
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }

    if (sem_timedwait(&pthread_pause_sem, &ts) == -1 && errno == ETIMEDOUT) {
        sem_post(&pthread_pause_sem);
    }

    //Block signals
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, PEAK_SIG_STOP);
    sigaddset(&sigset, PEAK_SIG_CONT);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    sem_post(&pthread_pause_sem);
}

int pthread_pause(pthread_t thread)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    // in nanoseconds
    ts.tv_sec += post_wait_interval / 1000000000;
    ts.tv_nsec += post_wait_interval % 1000000000;

    // ts.tv_nsec overflow
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }

    if (sem_timedwait(&pthread_pause_sem, &ts) == -1 && errno == ETIMEDOUT) {
        sem_post(&pthread_pause_sem);
    }
    // sem_wait(&pthread_pause_sem);
    //If signal queue is full, we keep retrying
    while (pthread_kill(thread, PEAK_SIG_STOP) == EAGAIN)
        usleep(1000);
    //pthread_pause_yield();
    return 0;
}

int pthread_unpause(pthread_t thread)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    // in nanoseconds
    ts.tv_sec += post_wait_interval / 1000000000;
    ts.tv_nsec += post_wait_interval % 1000000000;

    // ts.tv_nsec overflow
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }

    if (sem_timedwait(&pthread_pause_sem, &ts) == -1 && errno == ETIMEDOUT) {
        sem_post(&pthread_pause_sem);
    }
    //If signal queue is full, we keep retrying
    while (pthread_kill(thread, PEAK_SIG_CONT) == EAGAIN)
        usleep(1000);
    //pthread_pause_yield();
    return 0;
}

void* peak_heartbeat_monitor(void* arg) {
    unsigned int heartbeat_time = args->heartbeat_time;
    unsigned int check_interval = args->check_interval;
    unsigned int heartbeat_counter = 0;
    double total_execution_time = 0.0;
    gum_interceptor_ignore_current_thread(interceptor);
    pthread_t my_tid = pthread_self();
    
    while (heartbeat_running) {
        total_execution_time = peak_second() - peak_main_time;
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (hook_address[i] && array_listener[i]) {
                PeakGeneralListener* pg_listener = PEAKGENERAL_LISTENER(array_listener[i]);
                gulong total_num_calls = 0;
                for (size_t j = 0; j < peak_max_num_threads; j++) {
                    size_t index = j;
                    total_num_calls += pg_listener->num_calls[index];
                }
                
                double current_profile_ratio = (total_num_calls * peak_general_overhead + heartbeat_overhead[i]) / total_execution_time;
                if (current_profile_ratio > target_profile_ratio && !peak_detached[i]) {
                    // g_printerr("detach\n");
                    peak_need_detach[i] = TRUE;
                }
                if (check_interval != 0) {
                    if ((++heartbeat_counter % check_interval) == 0) {
                    // g_printerr("total_num_calls: %d\n", total_num_calls);
                    // g_printerr("current_profile_ratio = %3e, target_profile_ratio = %3e, peak_need_detach = %d\n", current_profile_ratio, target_profile_ratio, peak_need_detach[i]);
                    // g_printerr("array_listener_detached: %d, array_listener_reattached: %d\n", array_listener_detached[i], array_listener_reattached[i]);
                        if (current_profile_ratio <= target_profile_ratio && peak_detached[i]) {
                            // g_printerr("reattach\n");
                            double start_attach_time = peak_second();
                            pthread_mutex_lock(&lock);
                            GumMetalHashTableIter peak_tid_iter;
                            pthread_t peak_tid_key;
                            gum_metal_hash_table_iter_init(&peak_tid_iter, peak_tid_mapping);
                            while (gum_metal_hash_table_iter_next(&peak_tid_iter, (void**)&peak_tid_key, NULL)) {
                                // g_print ("peak_tid_key %lu\n", peak_tid_key);
                                // my_tid = pthread_self();
                                size_t cur_mapped_tid = (size_t)(gum_metal_hash_table_lookup(peak_tid_mapping, GUINT_TO_POINTER(peak_tid_key)));
                                if (peak_tid_key != my_tid && peak_target_thread_called[i][cur_mapped_tid])
                                    pthread_pause(peak_tid_key);
                            }
                            gum_interceptor_begin_transaction(interceptor);
                            gum_interceptor_attach(interceptor,
                                                    hook_address[i],
                                                    array_listener[i],
                                                    NULL);
                            gum_interceptor_end_transaction(interceptor);
                            gum_metal_hash_table_iter_init(&peak_tid_iter, peak_tid_mapping);
                            while (gum_metal_hash_table_iter_next(&peak_tid_iter, (void**)&peak_tid_key, NULL)) {
                                // g_print ("peak_tid_key %lu\n", peak_tid_key);
                                // my_tid = pthread_self();
                                size_t cur_mapped_tid = (size_t)(gum_metal_hash_table_lookup(peak_tid_mapping, GUINT_TO_POINTER(peak_tid_key)));
                                if (peak_tid_key != my_tid && peak_target_thread_called[i][cur_mapped_tid])
                                    pthread_unpause(peak_tid_key);
                            }
                            peak_need_detach[i] = FALSE;
                            peak_detached[i] = FALSE;
                            array_listener_reattached[i] = TRUE;
                            pthread_mutex_unlock(&lock);
                            double end_attach_time = peak_second();
                            heartbeat_overhead[i] += end_attach_time - start_attach_time;
                            // g_printerr("heartbeat_overhead = %3e\n", heartbeat_overhead[i]);
                        }
                    }
                }
                
            }
        }
        usleep(heartbeat_time);
    }
    gum_interceptor_unignore_current_thread(interceptor);
    return NULL;
}

static void
peak_general_listener_on_enter(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    if (!listener || g_object_is_floating(listener)) {
            return;
    }
    gum_interceptor_ignore_current_thread(interceptor);
    PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
    pthread_t my_tid = pthread_self();
    size_t mapped_tid = (size_t)(gum_metal_hash_table_lookup(peak_tid_mapping, GUINT_TO_POINTER(my_tid)));
    if (peak_detach_count == 0) {
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
        self->num_calls[index]++;
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
        self->num_calls[index]++;
        size_t hook_id = self->hook_id;
        peak_target_thread_called[hook_id][index] = true;
        if (self->num_calls[index] >= peak_detach_count) peak_need_detach[hook_id] = true;
        if (peak_need_detach[hook_id]) {
            pthread_mutex_lock(&lock);
            array_listener_detached[hook_id] = TRUE;
            GumMetalHashTableIter peak_tid_iter;
            pthread_t peak_tid_key;
            gum_metal_hash_table_iter_init(&peak_tid_iter, peak_tid_mapping);
            while (gum_metal_hash_table_iter_next(&peak_tid_iter, (void**)&peak_tid_key, NULL)) {
                size_t cur_mapped_tid = (size_t)(gum_metal_hash_table_lookup(peak_tid_mapping, GUINT_TO_POINTER(peak_tid_key)));
                if (peak_tid_key != my_tid && peak_target_thread_called[hook_id][cur_mapped_tid])
                    pthread_pause(peak_tid_key);
                
            }
            gum_interceptor_begin_transaction(interceptor);
            gum_interceptor_detach(interceptor, listener);
            gum_interceptor_end_transaction(interceptor);
            gum_metal_hash_table_iter_init(&peak_tid_iter, peak_tid_mapping);
            while (gum_metal_hash_table_iter_next(&peak_tid_iter, (void**)&peak_tid_key, NULL)) {
                size_t cur_mapped_tid = (size_t)(gum_metal_hash_table_lookup(peak_tid_mapping, GUINT_TO_POINTER(peak_tid_key)));
                if (peak_tid_key != my_tid && peak_target_thread_called[hook_id][cur_mapped_tid])
                    pthread_unpause(peak_tid_key);
               
            }
            peak_detached[hook_id] = true;
            pthread_mutex_unlock(&lock);
            // gum_interceptor_revert(interceptor, hook_address[hook_id]);
            // g_printerr ("revert hook_id %lu %p\n", hook_id, hook_address[hook_id]);
        }

        if (check_interval != 0) pthread_pause_enable();
        else pthread_pause_disable();
    }
    double* current_time = GUM_IC_GET_INVOCATION_DATA(ic, double);
    gum_interceptor_unignore_current_thread(interceptor);
    *current_time = peak_second();
}

static void
peak_general_listener_on_leave(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    double end_time = peak_second();
    gum_interceptor_ignore_current_thread(interceptor);
    if (peak_detach_count == 0) {
        if (!listener || g_object_is_floating(listener)) {
            thread_data.level--;
            if (thread_data.level == 0) {
                void* tmp_ptr = thread_data.child_time;
                thread_data.child_time = NULL;
                g_free(tmp_ptr);
            }
            return;
        }
        PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
        // PeakGeneralState* state = GUM_IC_GET_FUNC_DATA(ic, PeakGeneralState*);
        double* current_time = GUM_IC_GET_INVOCATION_DATA(ic, double);
        // size_t hook_id = state->hook_id;
        size_t mapped_tid = (size_t)(gum_metal_hash_table_lookup(peak_tid_mapping, GUINT_TO_POINTER(pthread_self())));
        end_time = end_time - *current_time;
        size_t index = mapped_tid;
        if (end_time > self->max_time[index])
            self->max_time[index] = end_time;
        if (end_time < self->min_time[index] || self->num_calls[index] == 1)
            self->min_time[index] = end_time;
        self->total_time[index] += end_time;
        // g_printerr ("hook_id %lu time %f endtime %f child_time %f count %lu\n", hook_id, *current_time, end_time, *child_time, self->num_calls[index]);
        thread_data.level--;
        if (thread_data.level > 0)
            thread_data.child_time[thread_data.level - 1] += end_time;
        self->exclusive_time[index] += end_time - thread_data.child_time[thread_data.level];
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
            return;
        }
        PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
        // PeakGeneralState* state = GUM_IC_GET_FUNC_DATA(ic, PeakGeneralState*);
        double* current_time = GUM_IC_GET_INVOCATION_DATA(ic, double);
        // size_t hook_id = state->hook_id;
        size_t mapped_tid = (size_t)(gum_metal_hash_table_lookup(peak_tid_mapping, GUINT_TO_POINTER(pthread_self())));
        end_time = end_time - *current_time;
        size_t index = mapped_tid;
        if (end_time > self->max_time[index])
            self->max_time[index] = end_time;
        if (end_time < self->min_time[index] || self->num_calls[index] == 1)
            self->min_time[index] = end_time;
        self->total_time[index] += end_time;
        // g_printerr ("hook_id %lu time %f endtime %f child_time %f count %lu\n", hook_id, *current_time, end_time, *child_time, self->num_calls[index]);
        thread_data.level--;
        if (thread_data.level > 0)
            thread_data.child_time[thread_data.level - 1] += end_time;
        self->exclusive_time[index] += end_time - thread_data.child_time[thread_data.level];
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
    // g_print ("total count %lu self->num_calls %lu\n", total_count, self->num_calls[0]);
}

static void
peak_general_listener_free(PeakGeneralListener* self)
{
    g_free(self->num_calls);
    g_free(self->total_time);
    g_free(self->exclusive_time);
    g_free(self->max_time);
    g_free(self->min_time);
}

__attribute__((noinline)) static void peak_general_overhead_dummy_func()
{
    struct timespec ts = { 0, 1 }; // Sleep for 1 nanosecond
    nanosleep(&ts, NULL);
}

static void
peak_general_overhead_bootstrapping()
{
    GumInvocationListener* listener_bootstrapping = g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);
    PEAKGENERAL_LISTENER(listener_bootstrapping)->hook_id = 0;
    gum_interceptor_begin_transaction(interceptor);
    gum_interceptor_attach(interceptor,
                           &peak_general_overhead_dummy_func,
                           listener_bootstrapping,
                           NULL);
    gum_interceptor_end_transaction(interceptor);

    guint n_tests = 2000;
    double* time = g_new(double, n_tests * 2);
    for (guint i = 0; i < n_tests; i++) {
        time[n_tests + i] = peak_second();
        peak_general_overhead_dummy_func();
        time[n_tests + i] = peak_second() - time[n_tests + i];
    }

    // g_printerr("%10lu times  %10.3f s total  %10.3e s max  %10.3e s min \n",
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->num_calls[0],
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->total_time[0],
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->max_time[0],
    //             PEAKGENERAL_LISTENER(listener_bootstrapping)->min_time[0]);
    gum_interceptor_detach(interceptor, listener_bootstrapping);
    peak_general_listener_free(PEAKGENERAL_LISTENER(listener_bootstrapping));
    g_object_unref(listener_bootstrapping);

    for (guint i = 0; i < n_tests; i++) {
        time[i] = peak_second();
        peak_general_overhead_dummy_func();
        time[i] = peak_second() - time[i];
    }

    // g_printerr("orig %.6e time %.6e\n", orig_time, time);
    peak_general_overhead = (median_double(&time[n_tests], n_tests) - median_double(&time[0], n_tests));
    g_free(time);
}

void peak_general_listener_attach()
{
    pthread_pause_enable();
    interceptor = gum_interceptor_obtain();
    array_listener = (GumInvocationListener**)g_new0(gpointer, peak_hook_address_count);
    array_listener_detached = g_new0(gboolean, peak_hook_address_count);
    array_listener_reattached = g_new0(gboolean, peak_hook_address_count);

    hook_address = g_new0(gpointer, peak_hook_address_count);
    peak_demangled_strings = g_new0(char*, peak_hook_address_count);
    // g_printerr ("peak_hook_address_count %lu peak_max_num_threads %lu\n",  peak_hook_address_count, peak_max_num_threads);
    gum_interceptor_begin_transaction(interceptor);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        // replace certain function we are capturing already.
        if (strcmp(peak_hook_strings[i], "MPI_Finalize") == 0) {
            hook_address[i] = gum_find_function("peak_pmpi_finalize");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "close") == 0) {
            hook_address[i] = gum_find_function("peak_close");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "exit") == 0) {
            hook_address[i] = gum_find_function("peak_exit");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "main") == 0) {
            hook_address[i] = NULL;
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchKernel") == 0) {
            hook_address[i] = gum_find_function("peak_cuda_launch_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchCooperativeKernel") == 0) {
            hook_address[i] = gum_find_function("peak_cuda_launch_cooperative_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchCooperativeKernelMultiDevice") == 0) {
            hook_address[i] = gum_find_function("peak_cuda_launch_cooperative_kernel_multiple_device");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchKernelEx") == 0) {
            // C++ API template versions, also use cudaLaunchKernelExC internal
            hook_address[i] = gum_find_function("peak_cuda_launch_kernel_exc");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cudaLaunchKernelExC") == 0) {
            hook_address[i] = gum_find_function("peak_cuda_launch_kernel_exc");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchKernel") == 0) {
            hook_address[i] = gum_find_function("peak_cu_launch_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchCooperativeKernel") == 0) {
            hook_address[i] = gum_find_function("peak_cu_launch_cooperative_kernel");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchCooperativeKernelMultiDevice") == 0) {
            hook_address[i] = gum_find_function("peak_cu_launch_cooperative_kernel_multiple_device");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else if (strcmp(peak_hook_strings[i], "cuLaunchKernelEx") == 0) {
            hook_address[i] = gum_find_function("peak_cu_launch_kernel_ex");
            peak_demangled_strings[i] = g_strdup(peak_hook_strings[i]);
        } else {
            if (cxa_demangle_status(peak_hook_strings[i]) != 0) {
                // peak_hook_strings is just function name
                gchar* truncate_hook = g_strdup_printf("*%s*", peak_hook_strings[i]);
                GArray* addresses = gum_find_functions_matching(truncate_hook);
                for (gsize j = 0; j < addresses->len; j++) {
                    gpointer addr = g_array_index(addresses, gpointer, j);
                    gchar* mangled = gum_symbol_name_from_address(addr);
                    gboolean function_match = FALSE;
                    if (!mangled) continue;
                
                    char* demangled = cxa_demangle(mangled);
                    g_free(mangled);
                    if (!demangled) continue;
    
                    gchar* function_name = extract_function_name(demangled);
                    if (strcmp(peak_hook_strings[i], function_name) == 0) {
                        hook_address[i] = addr;
                        peak_demangled_strings[i] = g_strdup(demangled);
                        function_match = TRUE;
                    }
                    free(demangled);
                    free(function_name);

                    if (function_match) {
                        break;
                    }
                }
                g_array_free(addresses, TRUE);
                g_free(truncate_hook);
            } else {
                hook_address[i] = gum_find_function(peak_hook_strings[i]);
                char* demangled = cxa_demangle(peak_hook_strings[i]);
                peak_demangled_strings[i] = g_strdup(demangled);
                free(demangled);
            }
        }
        if (hook_address[i]) {
            // g_printerr ("%s address = %p\n", peak_hook_strings[i], hook_address[i]);
            array_listener[i] = g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);
            PEAKGENERAL_LISTENER(array_listener[i])->hook_id = i;
            gum_interceptor_attach(interceptor,
                                   hook_address[i],
                                   array_listener[i],
                                   NULL);
        }
    }
    gum_interceptor_end_transaction(interceptor);
    if (peak_hook_address_count) {
        peak_general_overhead_bootstrapping();
        if (peak_detach_cost > 0)
            peak_detach_count = (peak_detach_cost > peak_general_overhead) ? peak_detach_cost / peak_general_overhead : 1;
    }
}

static void
peak_general_listener_print_result(gulong* sum_num_calls,
                                   gdouble* sum_total_time,
                                   gdouble* max_total_time,
                                   gdouble* min_total_time,
                                   gdouble* sum_exclusive_time,
                                   gfloat* sum_max_time,
                                   gfloat* sum_min_time,
                                   gulong* thread_count,
                                   const int rank_count)
{
    guint max_function_width = 20;
    guint max_col_width = 10;
    guint row_width = max_function_width + max_col_width * 5 + 7;
    char* row_separator = malloc(row_width + 1);
    memset(row_separator, '-', row_width);
    row_separator[row_width] = '\0';

    char* argv_o;
    get_argv0(&argv_o);
    double total_overhead = 0.0;
    gboolean have_output = FALSE;
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i] && sum_num_calls[i] != 0) {
            total_overhead += (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                              * peak_general_overhead;
            have_output = TRUE;
        }
    }
    if (have_output) {
        g_printerr("%.*s\n", row_width, row_separator);
        g_printerr("%*s PEAK Library\n", (row_width - 12) / 2, "");
        g_printerr("%.*s\n", row_width, row_separator);
        g_printerr("Time: %f\n", peak_main_time);
        g_printerr("PEAK done with: %s\n", argv_o);
        g_printerr("Estimated overhead: %.3es per call and %.3es total\n", peak_general_overhead, total_overhead);

        g_printerr("\n%.*s function statistics (call)  %.*s\n", (row_width - 28) / 2, row_separator, (row_width - 28) / 2, row_separator);
        g_printerr(" individual call counts and time (in seconds)\n");
        g_printerr("%.*s\n", row_width, row_separator);
        g_printerr("|%*s|%*s|%*s|%*s|%*s|%*s|\n",
                   max_function_width, "function",
                   max_col_width, "count",
                   max_col_width, "per thread",
                   max_col_width, "per rank",
                   max_col_width, "max",
                   max_col_width, "min");
        g_printerr("%.*s\n", row_width, row_separator);
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (hook_address[i] && sum_num_calls[i] != 0) {
                if (!array_listener_detached[i])
                    g_printerr("|%*s|%*lu|%*lu|%*lu|%*.3e|%*.3e|\n",
                               max_function_width, truncate_string(peak_demangled_strings[i], max_function_width),
                               max_col_width, sum_num_calls[i],
                               max_col_width, sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                               max_col_width, sum_num_calls[i] / rank_count,
                               max_col_width, sum_max_time[i],
                               max_col_width, sum_min_time[i]);
                else {
                    if (!array_listener_reattached[i])
                        g_printerr("|%*s*|%*lu|%*lu|%*lu|%*.3e|%*.3e|\n",
                                max_function_width - 1, truncate_string(peak_demangled_strings[i], max_function_width-1),
                                max_col_width, sum_num_calls[i],
                                max_col_width, sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                                max_col_width, sum_num_calls[i] / rank_count,
                                max_col_width, sum_max_time[i],
                                max_col_width, sum_min_time[i]);
                    else
                        g_printerr("|%*s**|%*lu|%*lu|%*lu|%*.3e|%*.3e|\n",
                                max_function_width - 1, truncate_string(peak_demangled_strings[i], max_function_width-1),
                                max_col_width, sum_num_calls[i],
                                max_col_width, sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                                max_col_width, sum_num_calls[i] / rank_count,
                                max_col_width, sum_max_time[i],
                                max_col_width, sum_min_time[i]);
                }
                    
            }
        }
        g_printerr("%.*s\n", row_width, row_separator);

        g_printerr("\n%.*s function statistics (thread)  %.*s\n", (row_width - 30) / 2, row_separator, (row_width - 30) / 2, row_separator);
        g_printerr(" thread aggregated time (in seconds)\n");
        g_printerr("%.*s\n", row_width, row_separator);
        g_printerr("|%*s|%*s|%*s|%*s|%*s|%*s|\n",
                   max_function_width, "function",
                   max_col_width, "total",
                   max_col_width, "exclusive",
                   max_col_width, "max",
                   max_col_width, "min",
                   max_col_width, "overhead");
        g_printerr("%.*s\n", row_width, row_separator);
        for (size_t i = 0; i < peak_hook_address_count; i++) {
            if (hook_address[i] && sum_num_calls[i] != 0) {
                if (!array_listener_detached[i])
                    g_printerr("|%*s|%*.3f|%*.3f|%*.3f|%*.3f|%*.3e|\n",
                               max_function_width, truncate_string(peak_demangled_strings[i], max_function_width),
                               max_col_width, sum_total_time[i],
                               max_col_width, sum_exclusive_time[i],
                               max_col_width, max_total_time[i],
                               max_col_width, min_total_time[i],
                               max_col_width, (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                                              * peak_general_overhead);
                else {
                    if (!array_listener_reattached[i])
                        g_printerr("|%*s*|%*.3f|%*.3f|%*.3f|%*.3f|%*.3e|\n",
                                    max_function_width - 1, truncate_string(peak_demangled_strings[i], max_function_width-1),
                                    max_col_width, sum_total_time[i],
                                    max_col_width, sum_exclusive_time[i],
                                    max_col_width, max_total_time[i],
                                    max_col_width, min_total_time[i],
                                    max_col_width, (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                                                    * peak_general_overhead);
                    else
                        g_printerr("|%*s**|%*.3f|%*.3f|%*.3f|%*.3f|%*.3e|\n",
                                max_function_width - 1, truncate_string(peak_demangled_strings[i], max_function_width-1),
                                max_col_width, sum_total_time[i],
                                max_col_width, sum_exclusive_time[i],
                                max_col_width, max_total_time[i],
                                max_col_width, min_total_time[i],
                                max_col_width, (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                                                * peak_general_overhead);
                }
            }
        }
        g_printerr("%.*s\n", row_width, row_separator);
    }
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        g_free(peak_demangled_strings[i]);
    }
    g_free(peak_demangled_strings);
    free(argv_o);
    free(row_separator);
}

#ifdef HAVE_MPI
static void
peak_general_listener_reduce_result(gulong* sum_num_calls,
                                    gdouble* sum_total_time,
                                    gdouble* max_total_time,
                                    gdouble* min_total_time,
                                    gdouble* sum_exclusive_time,
                                    gfloat* sum_max_time,
                                    gfloat* sum_min_time,
                                    gulong* thread_count)
{
    int rank, size;
    int init_flag;
    MPI_Initialized(&init_flag);
    if (!init_flag)
        MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    gulong* mpi_sum_num_calls = g_new0(gulong, peak_hook_address_count);
    gdouble* mpi_sum_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* mpi_max_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* mpi_min_total_time = g_new0(gdouble, peak_hook_address_count);
    gdouble* mpi_sum_exclusive_time = g_new0(gdouble, peak_hook_address_count);
    gfloat* mpi_sum_max_time = g_new0(gfloat, peak_hook_address_count);
    gfloat* mpi_sum_min_time = g_new0(gfloat, peak_hook_address_count);
    gulong* mpi_thread_count = g_new0(gulong, peak_hook_address_count);
    gboolean* mpi_array_listener_detached = g_new0(gboolean, peak_hook_address_count);
    MPI_Reduce(sum_num_calls, mpi_sum_num_calls, peak_hook_address_count, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_total_time, mpi_sum_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(max_total_time, mpi_max_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(min_total_time, mpi_min_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_exclusive_time, mpi_sum_exclusive_time, peak_hook_address_count, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_max_time, mpi_sum_max_time, peak_hook_address_count, MPI_FLOAT, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_min_time, mpi_sum_min_time, peak_hook_address_count, MPI_FLOAT, MPI_MIN, 0, MPI_COMM_WORLD);
    MPI_Reduce(thread_count, mpi_thread_count, peak_hook_address_count, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(array_listener_detached, mpi_array_listener_detached, peak_hook_address_count, MPI_INT, MPI_MAX, 0, MPI_COMM_WORLD);
    g_free(array_listener_detached);
    array_listener_detached = mpi_array_listener_detached;
    if (rank == 0) {
        peak_general_listener_print_result(mpi_sum_num_calls,
                                           mpi_sum_total_time,
                                           mpi_max_total_time,
                                           mpi_min_total_time,
                                           mpi_sum_exclusive_time,
                                           mpi_sum_max_time,
                                           mpi_sum_min_time,
                                           mpi_thread_count, size);
    }
    g_free(mpi_sum_num_calls);
    g_free(mpi_sum_total_time);
    g_free(mpi_max_total_time);
    g_free(mpi_min_total_time);
    g_free(mpi_sum_exclusive_time);
    g_free(mpi_sum_max_time);
    g_free(mpi_sum_min_time);
    g_free(mpi_thread_count);
}
#endif

void peak_general_listener_print(int is_MPI)
{
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
                sum_num_calls[i] += pg_listener->num_calls[index];
                sum_total_time[i] += pg_listener->total_time[index];
                sum_exclusive_time[i] += pg_listener->exclusive_time[index];
                if (pg_listener->num_calls[index] != 0) {
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
#ifdef HAVE_MPI
    if (is_MPI) {
        peak_general_listener_reduce_result(sum_num_calls,
                                            sum_total_time,
                                            max_total_time,
                                            min_total_time,
                                            sum_exclusive_time,
                                            sum_max_time,
                                            sum_min_time,
                                            thread_count);
    } else {
        peak_general_listener_print_result(sum_num_calls,
                                           sum_total_time,
                                           max_total_time,
                                           min_total_time,
                                           sum_exclusive_time,
                                           sum_max_time,
                                           sum_min_time,
                                           thread_count, 1);
    }
#else
    peak_general_listener_print_result(sum_num_calls,
                                       sum_total_time,
                                       max_total_time,
                                       min_total_time,
                                       sum_exclusive_time,
                                       sum_max_time,
                                       sum_min_time,
                                       thread_count, 1);
#endif
    g_free(sum_num_calls);
    g_free(sum_total_time);
    g_free(max_total_time);
    g_free(min_total_time);
    g_free(sum_exclusive_time);
    g_free(sum_max_time);
    g_free(sum_min_time);
    g_free(thread_count);
}

void peak_general_listener_dettach()
{
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i]) {
            gum_interceptor_detach(interceptor, array_listener[i]);
            peak_general_listener_free(PEAKGENERAL_LISTENER(array_listener[i]));
            g_object_unref(array_listener[i]);
        }
    }
    g_object_unref(interceptor);
    g_free(hook_address);
    g_free(array_listener_detached);
    g_free(array_listener_reattached);
    g_free(array_listener);
}
