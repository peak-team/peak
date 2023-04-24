#include "general_listener.h"

static GumInterceptor* interceptor;
static GumInvocationListener* listener;
extern GumMetalHashTable* peak_tid_mapping;
static PeakGeneralState* state;
static gpointer* hook_address = NULL;
static double peak_general_overhead;
extern size_t peak_hook_address_count;
extern char** peak_hook_strings;
extern gulong peak_max_num_threads;
static pthread_key_t thread_local_key;

static void peak_general_listener_iface_init(gpointer g_iface, gpointer iface_data);

#define PEAKGENERAL_TYPE_LISTENER (peak_general_listener_get_type())
G_DECLARE_FINAL_TYPE(PeakGeneralListener, peak_general_listener, PEAKGENERAL, LISTENER, GObject)
G_DEFINE_TYPE_EXTENDED(PeakGeneralListener,
                       peak_general_listener,
                       G_TYPE_OBJECT,
                       0,
                       G_IMPLEMENT_INTERFACE(GUM_TYPE_INVOCATION_LISTENER,
                                             peak_general_listener_iface_init))

static void
peak_general_listener_on_enter(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
    PeakGeneralState* state = GUM_IC_GET_FUNC_DATA(ic, PeakGeneralState*);
    double* current_time = (double *) pthread_getspecific (thread_local_key);
    if (current_time == NULL) {
        current_time = g_new0 (double, 1);
        pthread_setspecific(thread_local_key, current_time);
    }
    // PeakGeneralState* state = (PeakGeneralState*)(gum_invocation_context_get_listener_function_data(ic));
    // PeakGeneralState* thread_state = (PeakGeneralState*)(gum_invocation_context_get_listener_thread_data(ic, sizeof(PeakGeneralState)));
    size_t hook_id = state->hook_id;
    pthread_t mapped_tid = (pthread_t)(gum_metal_hash_table_lookup(peak_tid_mapping, GUINT_TO_POINTER(pthread_self())));
    // g_print ("hook_id %lu tid %lu mapped %lu\n", hook_id, pthread_self(), mapped_tid);
    // g_print ("hook_id %lu max %lu tid %lu ncall %p \n", hook_id, peak_max_num_threads, mapped_tid, self->num_calls);
    self->num_calls[hook_id * peak_max_num_threads + mapped_tid]++;
    *current_time = peak_second();
    // g_printerr ("hook_id %lu time %f\n", hook_id, *current_time);
}

static void
peak_general_listener_on_leave(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    double end_time = peak_second();
    PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
    PeakGeneralState* state = GUM_IC_GET_FUNC_DATA(ic, PeakGeneralState*);
    double* current_time = (double *) pthread_getspecific (thread_local_key);
    // PeakGeneralState* state = (PeakGeneralState*)(gum_invocation_context_get_listener_function_data(ic));
    // PeakGeneralState* thread_state = (PeakGeneralState*)(gum_invocation_context_get_listener_thread_data(ic, sizeof(PeakGeneralState)));
    size_t hook_id = state->hook_id;
    pthread_t mapped_tid = (pthread_t)(gum_metal_hash_table_lookup(peak_tid_mapping, GUINT_TO_POINTER(pthread_self())));
    end_time = end_time - *current_time;
    size_t index = hook_id * peak_max_num_threads + mapped_tid;
    if (end_time > self->max_time[index])
        self->max_time[index] = end_time;
    if (end_time < self->min_time[index] || self->num_calls[index] == 1)
        self->min_time[index] = end_time;
    self->total_time[index] += end_time;
    // g_printerr ("hook_id %lu time %f\n", hook_id, *current_time);
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
    size_t total_count = peak_hook_address_count * peak_max_num_threads;
    // g_print ("total count %lu peak_hook_address_count %lu num_cores %lu\n", total_count, peak_hook_address_count, peak_max_num_threads);
    self->num_calls = g_new0(gulong, total_count);
    self->total_time = g_new0(gdouble, total_count);
    self->max_time = g_new0(gfloat, total_count);
    self->min_time = g_new0(gfloat, total_count);
}

static void
peak_general_listener_free(PeakGeneralListener* self)
{
    g_free(self->num_calls);
    g_free(self->total_time);
    g_free(self->max_time);
    g_free(self->min_time);
}

__attribute__((noinline)) static void peak_general_overhead_dummy_func()
{
    struct timespec ts = { 0, 1 }; // Sleep for 1 nanosecond
    nanosleep(&ts, NULL);
}

static int cmpfunc(const void* a, const void* b)
{
    if (*(double*)a < *(double*)b) {
        return -1;
    } else if (*(double*)a > *(double*)b) {
        return 1;
    } else {
        return 0;
    }
}

static double median(double* arr, size_t n)
{
    qsort(arr, n, sizeof(double), cmpfunc);
    if (n % 2 == 0) {
        return (double)(arr[n / 2 - 1] + arr[n / 2]) / 2.0;
    } else {
        return (double)arr[n / 2];
    }
}

static void
peak_general_overhead_bootstrapping()
{
    GumInvocationListener* listener_bootstrapping = g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);
    PeakGeneralState state_bootstrapping = { 0, 0.0 };
    gum_interceptor_begin_transaction(interceptor);
    gum_interceptor_attach(interceptor,
                           &peak_general_overhead_dummy_func,
                           listener_bootstrapping,
                           &state_bootstrapping);
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
    peak_general_overhead = (median(&time[n_tests], n_tests) - median(&time[0], n_tests));
    g_free(time);
}

static void destr_fn(void *parm)
{
    printf("Destructor function invoked %f %p\n", *((double*)parm), parm);
    g_free(parm);
}

void peak_general_listener_attach()
{
    interceptor = gum_interceptor_obtain();
    listener = g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);

    pthread_key_create(&thread_local_key, destr_fn);

    hook_address = g_new0(gpointer, peak_hook_address_count);
    state = g_new0(PeakGeneralState, peak_hook_address_count);
    // g_printerr ("peak_hook_address_count %lu peak_max_num_threads %lu\n",  peak_hook_address_count, peak_max_num_threads);
    gum_interceptor_begin_transaction(interceptor);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        hook_address[i] = gum_find_function(peak_hook_strings[i]);
        if (hook_address[i]) {
            // g_printerr ("%s address = %p\n", peak_hook_strings[i], hook_address[i]);

            state[i].hook_id = i;
            state[i].current_time = 0.0;
            gum_interceptor_attach(interceptor,
                                   hook_address[i],
                                   listener,
                                   &state[i]);
        }
    }
    gum_interceptor_end_transaction(interceptor);
    if (peak_hook_address_count)
        peak_general_overhead_bootstrapping();
}

static void
peak_general_listener_print_result(gulong* sum_num_calls, gdouble* sum_total_time, gfloat* sum_max_time, gfloat* sum_min_time, gulong* thread_count, const int rank_count)
{
    char* argv_o;
    get_argv0(&argv_o);
    gboolean have_output = FALSE;
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i] && sum_num_calls[i] != 0) {
            g_printerr("%30s  %10lu times  %10.4f /thread  %10lu /rank  %10.3f s total  %10.3e s max  %10.3e s min  %10.3e s overhead\n",
                       peak_hook_strings[i],
                       sum_num_calls[i],
                       sum_num_calls[i] / (double)thread_count[i],
                       sum_num_calls[i] / rank_count,
                       sum_total_time[i],
                       sum_max_time[i],
                       sum_min_time[i],
                       sum_num_calls[i] / (double)thread_count[i] * peak_general_overhead);
            have_output = TRUE;
        }
    }
    if (have_output) {
        g_printerr("Estimated overhead: %.6e s per call\n", peak_general_overhead);
        g_printerr("PEAK done with: %s\n", argv_o);
    }
    free(argv_o);
}

#ifdef HAVE_MPI
static void
peak_general_listener_reduce_result(gulong* sum_num_calls, gdouble* sum_total_time, gfloat* sum_max_time, gfloat* sum_min_time, gulong* thread_count)
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
    gfloat* mpi_sum_max_time = g_new0(gfloat, peak_hook_address_count);
    gfloat* mpi_sum_min_time = g_new0(gfloat, peak_hook_address_count);
    gulong* mpi_thread_count = g_new0(gulong, peak_hook_address_count);
    MPI_Reduce(sum_num_calls, mpi_sum_num_calls, peak_hook_address_count, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_total_time, mpi_sum_total_time, peak_hook_address_count, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_max_time, mpi_sum_max_time, peak_hook_address_count, MPI_FLOAT, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(sum_min_time, mpi_sum_min_time, peak_hook_address_count, MPI_FLOAT, MPI_MIN, 0, MPI_COMM_WORLD);
    MPI_Reduce(thread_count, mpi_thread_count, peak_hook_address_count, MPI_UNSIGNED_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    if (rank == 0) {
        peak_general_listener_print_result(mpi_sum_num_calls, mpi_sum_total_time, mpi_sum_max_time, mpi_sum_min_time, mpi_thread_count, size);
    }
    g_free(mpi_sum_num_calls);
    g_free(mpi_sum_total_time);
    g_free(mpi_thread_count);
}
#endif

void peak_general_listener_print(int is_MPI)
{
    gulong* sum_num_calls = g_new0(gulong, peak_hook_address_count);
    gdouble* sum_total_time = g_new0(gdouble, peak_hook_address_count);
    gfloat* sum_max_time = g_new0(gfloat, peak_hook_address_count);
    gfloat* sum_min_time = g_new0(gfloat, peak_hook_address_count);
    gulong* thread_count = g_new0(gulong, peak_hook_address_count);
    PeakGeneralListener* pg_listener = PEAKGENERAL_LISTENER(listener);
    for (size_t i = 0; i < peak_hook_address_count; i++) {
        if (hook_address[i]) {
            for (size_t j = 0; j < peak_max_num_threads; j++) {
                sum_num_calls[i] += pg_listener->num_calls[i * peak_max_num_threads + j];
                sum_total_time[i] += pg_listener->total_time[i * peak_max_num_threads + j];
                if (pg_listener->num_calls[i * peak_max_num_threads + j] != 0) {
                    thread_count[i]++;
                    if (pg_listener->max_time[i * peak_max_num_threads + j] > sum_max_time[i])
                        sum_max_time[i] = pg_listener->max_time[i * peak_max_num_threads + j];
                    if (pg_listener->min_time[i * peak_max_num_threads + j] < sum_min_time[i] || thread_count[i] == 1)
                        sum_min_time[i] = pg_listener->min_time[i * peak_max_num_threads + j];
                }
            }
            if (thread_count[i] == 0)
                thread_count[i] = 1;
        }
    }
#ifdef HAVE_MPI
    if (is_MPI) {
        peak_general_listener_reduce_result(sum_num_calls, sum_total_time, sum_max_time, sum_min_time, thread_count);
    } else {
        peak_general_listener_print_result(sum_num_calls, sum_total_time, sum_max_time, sum_min_time, thread_count, 1);
    }
#else
    peak_general_listener_print_result(sum_num_calls, sum_total_time, sum_max_time, sum_min_time, thread_count, 1);
#endif
    g_free(sum_num_calls);
    g_free(sum_total_time);
    g_free(thread_count);
}

void peak_general_listener_dettach()
{
    pthread_key_delete(thread_local_key);
    gum_interceptor_detach(interceptor, listener);
    peak_general_listener_free(PEAKGENERAL_LISTENER(listener));
    g_object_unref(listener);
    g_object_unref(interceptor);
    g_free(hook_address);
    g_free(state);
}
