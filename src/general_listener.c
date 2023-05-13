#include "general_listener.h"

#define PEAK_SIG_STOP (SIGRTMIN+0)
#define PEAK_SIG_CONT (SIGRTMIN+1)

static GumInterceptor* interceptor;
static GumInvocationListener** array_listener;
static gboolean* array_listener_detached;
extern GumMetalHashTable* peak_tid_mapping;
static gpointer* hook_address = NULL;
static double peak_general_overhead;
static pthread_key_t thread_local_key;
extern size_t peak_hook_address_count;
extern char** peak_hook_strings;
extern gulong peak_max_num_threads;
extern double peak_main_time;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

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

sem_t pthread_pause_sem;
pthread_once_t pthread_pause_once_ctrl = PTHREAD_ONCE_INIT;

void pthread_pause_once(void) {
    sem_init(&pthread_pause_sem, 0, 1);
}

#define pthread_pause_init() (pthread_once(&pthread_pause_once_ctrl, &pthread_pause_once))

void pthread_pause_handler(int signal) {
    //Post semaphore to confirm that signal is handled
    sem_post(&pthread_pause_sem);
    //Suspend if needed
    if(signal == PEAK_SIG_STOP) {
        sigset_t sigset;
        sigfillset(&sigset);
        sigdelset(&sigset, PEAK_SIG_STOP);
        sigdelset(&sigset, PEAK_SIG_CONT);
        sigsuspend(&sigset); //Wait for next signal
    } else return;
}

void pthread_pause_enable() {
    pthread_pause_init();
    //Prepare sigset
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

    //UnBlock signals
    pthread_sigmask(SIG_UNBLOCK, &sigset, NULL);
}

void pthread_pause_disable() {
    //This is important for when you want to do some signal unsafe stuff
    //Eg.: locking mutex, calling printf() which has internal mutex, etc...
    //After unlocking mutex, you can enable pause again.

    pthread_pause_init();

    //Make sure all signals are dispatched before we block them
    sem_wait(&pthread_pause_sem);

    //Block signals
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, PEAK_SIG_STOP);
    sigaddset(&sigset, PEAK_SIG_CONT);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    sem_post(&pthread_pause_sem);
}

int pthread_pause(pthread_t thread) {
    sem_wait(&pthread_pause_sem);
    //If signal queue is full, we keep retrying
    while(pthread_kill(thread, PEAK_SIG_STOP) == EAGAIN) usleep(1000);
    //pthread_pause_yield();
    return 0;
}

int pthread_unpause(pthread_t thread) {
    sem_wait(&pthread_pause_sem);
    //If signal queue is full, we keep retrying
    while(pthread_kill(thread, PEAK_SIG_CONT) == EAGAIN) usleep(1000);
    //pthread_pause_yield();
    return 0;
}

static void
peak_general_listener_on_enter(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    if (!listener || g_object_is_floating(listener))
        return;
    PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
    PeakGeneralThreadState* thread_data = (PeakGeneralThreadState*)pthread_getspecific(thread_local_key);
    if (thread_data == NULL) {
        pthread_pause_disable();
        thread_data = g_new0(PeakGeneralThreadState, 1);
        pthread_setspecific(thread_local_key, thread_data);
        pthread_pause_enable();
    }
    pthread_t my_tid = pthread_self();
    size_t mapped_tid = (size_t)(gum_metal_hash_table_lookup(peak_tid_mapping, GUINT_TO_POINTER(my_tid)));
    // g_print ("hook_id %lu tid %lu mapped %lu\n", hook_id, pthread_self(), mapped_tid);
    // g_print ("hook_id %lu max %lu tid %lu ncall %p \n", hook_id, peak_max_num_threads, mapped_tid, self->num_calls);
    size_t index = mapped_tid;
    if (thread_data->child_time == NULL) {
        thread_data->level = 0;
        thread_data->capacity = 16;
        pthread_pause_disable();
        thread_data->child_time = g_new(gdouble, 16);
        pthread_pause_enable();
    }
    thread_data->child_time[thread_data->level] = 0.0;
    thread_data->level++;
    if (thread_data->level == thread_data->capacity) {
        thread_data->capacity *= 2;
        pthread_pause_disable();
        thread_data->child_time = g_renew(double, thread_data->child_time, thread_data->capacity);
        pthread_pause_enable();
    }
    self->num_calls[index]++;
    if (self->num_calls[index] > 3000) {
        pthread_mutex_lock(&lock);
        size_t hook_id = self->hook_id;
        if(!array_listener_detached[hook_id]) {
            array_listener_detached[hook_id] = TRUE;
            GumMetalHashTableIter peak_tid_iter;
            pthread_t peak_tid_key;
            gum_metal_hash_table_iter_init(&peak_tid_iter, peak_tid_mapping);
            while (gum_metal_hash_table_iter_next(&peak_tid_iter, (void **)&peak_tid_key, NULL)) {
                // g_print ("peak_tid_key %lu my_tid %lu\n", peak_tid_key, my_tid);
                if (peak_tid_key != my_tid)
                    pthread_pause(peak_tid_key);
            }
            gum_interceptor_begin_transaction(interceptor);
            gum_interceptor_detach(interceptor, listener);
            gum_interceptor_end_transaction(interceptor);
            gum_metal_hash_table_iter_init(&peak_tid_iter, peak_tid_mapping);
            while (gum_metal_hash_table_iter_next(&peak_tid_iter, (void **)&peak_tid_key, NULL)) {
                if (peak_tid_key != my_tid)
                    pthread_unpause(peak_tid_key);
            }
        }
        pthread_mutex_unlock(&lock);
        // gum_interceptor_revert(interceptor, hook_address[hook_id]);
        // g_printerr ("revert hook_id %lu %p\n", hook_id, hook_address[hook_id]);
    }
    double* current_time = GUM_IC_GET_INVOCATION_DATA(ic, double);
    pthread_pause_disable();
    *current_time = peak_second();
    // g_printerr ("hook_id %lu time %f count %lu\n", hook_id, *current_time, self->num_calls[mapped_tid]);
}

static void
peak_general_listener_on_leave(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    double end_time = peak_second();
    pthread_pause_enable();
    if (!listener || g_object_is_floating(listener)) {
        PeakGeneralThreadState* thread_data = (PeakGeneralThreadState*)pthread_getspecific(thread_local_key);
        thread_data->level--;
        if (thread_data->level == 0) {
            void* tmp_ptr = thread_data->child_time;
            thread_data->child_time = NULL;
            pthread_pause_disable();
            g_free(tmp_ptr);
            pthread_pause_enable();
        }
        return;
    }
    PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
    // PeakGeneralState* state = GUM_IC_GET_FUNC_DATA(ic, PeakGeneralState*);
    PeakGeneralThreadState* thread_data = (PeakGeneralThreadState*)pthread_getspecific(thread_local_key);
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
    thread_data->level--;
    if (thread_data->level > 0)
        thread_data->child_time[thread_data->level - 1] += end_time;
    self->exclusive_time[index] += end_time - thread_data->child_time[thread_data->level];
    if (thread_data->level == 0) {
        void* tmp_ptr = thread_data->child_time;
        thread_data->child_time = NULL;
        pthread_pause_disable();
        g_free(tmp_ptr);
        pthread_pause_enable();
    }
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

static void destr_fn(void* parm)
{
    // printf("Destructor function invoked %f %p\n", *((double*)parm), parm);
    PeakGeneralThreadState* thread_data = parm;
    g_free(thread_data->child_time);
    g_free(parm);
}

void peak_general_listener_attach()
{
    pthread_pause_enable();
    interceptor = gum_interceptor_obtain();
    array_listener = (GumInvocationListener**)g_new0(gpointer, peak_hook_address_count);
    array_listener_detached = g_new0(gboolean, peak_hook_address_count);

    pthread_key_create(&thread_local_key, destr_fn);

    hook_address = g_new0(gpointer, peak_hook_address_count);
    // g_printerr ("peak_hook_address_count %lu peak_max_num_threads %lu\n",  peak_hook_address_count, peak_max_num_threads);
    gum_interceptor_begin_transaction(interceptor);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        // replace certain function we are capturing already.
        if (strcmp(peak_hook_strings[i], "MPI_Finalize") == 0) {
            hook_address[i] = gum_find_function("peak_pmpi_finalize");
        } else if (strcmp(peak_hook_strings[i], "main") == 0) {
            hook_address[i] = NULL;
        } else {
            hook_address[i] = gum_find_function(peak_hook_strings[i]);
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
    if (peak_hook_address_count)
        peak_general_overhead_bootstrapping();
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
                g_printerr("|%*s|%*lu|%*lu|%*lu|%*.3e|%*.3e|\n",
                           max_function_width, peak_hook_strings[i],
                           max_col_width, sum_num_calls[i],
                           max_col_width, sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0),
                           max_col_width, sum_num_calls[i] / rank_count,
                           max_col_width, sum_max_time[i],
                           max_col_width, sum_min_time[i]);
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
                g_printerr("|%*s|%*.3f|%*.3f|%*.3f|%*.3f|%*.3e|\n",
                           max_function_width, peak_hook_strings[i],
                           max_col_width, sum_total_time[i],
                           max_col_width, sum_exclusive_time[i],
                           max_col_width, max_total_time[i],
                           max_col_width, min_total_time[i],
                           max_col_width, (sum_num_calls[i] / thread_count[i] + ((sum_num_calls[i] % thread_count[i] != 0) ? 1 : 0))
                                          * peak_general_overhead);
            }
        }
        g_printerr("%.*s\n", row_width, row_separator);
    }
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
    MPI_Reduce(sum_num_calls, mpi_sum_num_calls, peak_hook_address_count, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_total_time, mpi_sum_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(max_total_time, mpi_max_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(min_total_time, mpi_min_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_exclusive_time, mpi_sum_exclusive_time, peak_hook_address_count, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_max_time, mpi_sum_max_time, peak_hook_address_count, MPI_FLOAT, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_min_time, mpi_sum_min_time, peak_hook_address_count, MPI_FLOAT, MPI_MIN, 0, MPI_COMM_WORLD);
    MPI_Reduce(thread_count, mpi_thread_count, peak_hook_address_count, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
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
    g_free(array_listener);
}
