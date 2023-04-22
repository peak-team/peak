#include "general_listener.h"

static GumInterceptor* interceptor;
static GumInvocationListener* listener;
extern GumMetalHashTable* tid_mapping;
static PeakGeneralState* state;
static gpointer* hook_address = NULL;
extern size_t hook_address_count;
extern char** hook_strings;
extern long max_num_threads;

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
    PeakGeneralState* thread_state = GUM_IC_GET_THREAD_DATA(ic, PeakGeneralState);
    // PeakGeneralState* state = (PeakGeneralState*)(gum_invocation_context_get_listener_function_data(ic));
    // PeakGeneralState* thread_state = (PeakGeneralState*)(gum_invocation_context_get_listener_thread_data(ic, sizeof(PeakGeneralState)));
    size_t hook_id = state->hook_id;
    pthread_t tid = pthread_self();
    // g_print ("hook_id %lu tid %lu tid_orig %lu\n", hook_id, tid, syscall(SYS_gettid));
    pthread_t mapped_tid = (pthread_t)(gum_metal_hash_table_lookup(tid_mapping, GUINT_TO_POINTER(tid)));
    // g_print ("hook_id %lu tid %lu mapped %lu\n", hook_id, pthread_self(), mapped_tid);
    self->num_calls[hook_id * max_num_threads + mapped_tid]++;
    thread_state->current_time = peak_second();
    // g_print ("hook_id %lu time %f\n", hook_id, thread_state->current_time);
}

static void
peak_general_listener_on_leave(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    double end_time = peak_second();
    PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
    PeakGeneralState* state = GUM_IC_GET_FUNC_DATA(ic, PeakGeneralState*);
    PeakGeneralState* thread_state = GUM_IC_GET_THREAD_DATA(ic, PeakGeneralState);
    // PeakGeneralState* state = (PeakGeneralState*)(gum_invocation_context_get_listener_function_data(ic));
    // PeakGeneralState* thread_state = (PeakGeneralState*)(gum_invocation_context_get_listener_thread_data(ic, sizeof(PeakGeneralState)));
    size_t hook_id = state->hook_id;
    pthread_t tid = pthread_self();
    pthread_t mapped_tid = (pthread_t)(gum_metal_hash_table_lookup(tid_mapping, GUINT_TO_POINTER(tid)));
    self->total_time[hook_id * max_num_threads + mapped_tid] += end_time - thread_state->current_time;
    // g_print ("hook_id %lu time %f\n", hook_id, thread_state->current_time);
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
    size_t total_count = hook_address_count * max_num_threads;
    // g_print ("total count %lu hook_address_count %lu num_cores %lu\n", total_count, hook_address_count, max_num_threads);
    self->num_calls = g_new0(gulong, total_count);
    self->total_time = g_new0(double, total_count);
}

static void
peak_general_listener_free(PeakGeneralListener* self)
{
    g_free(self->num_calls);
    g_free(self->total_time);
}


void peak_general_listener_attach()
{
    interceptor = gum_interceptor_obtain();
    listener = g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);

    hook_address = g_new0(gpointer, hook_address_count);
    state = g_new0(PeakGeneralState, hook_address_count);
    // g_print ("hook_address_count %lu num_cores %lu\n",  hook_address_count, num_cores);
    gum_interceptor_begin_transaction(interceptor);
    for (size_t i = 0; i < hook_address_count; i++) {
        hook_address[i] = gum_find_function(hook_strings[i]);
        if (hook_address[i]) {
            // g_print ("%s address = %p\n", hook_strings[i], hook_address[i]);

            state[i].hook_id = i;
            state[i].current_time = 0.0;
            gum_interceptor_attach(interceptor,
                                   hook_address[i],
                                   listener,
                                   &state[i]);
        }
    }
    gum_interceptor_end_transaction(interceptor);
}

void peak_general_listener_print()
{
    for (size_t i = 0; i < hook_address_count; i++) {
        if (hook_address[i]) {
            size_t thread_count = 0;
            for (size_t j = 1; j < max_num_threads; j++) {
                PEAKGENERAL_LISTENER(listener)->num_calls[i * max_num_threads] += PEAKGENERAL_LISTENER(listener)->num_calls[i * max_num_threads + j];
                PEAKGENERAL_LISTENER(listener)->total_time[i * max_num_threads] += PEAKGENERAL_LISTENER(listener)->total_time[i * max_num_threads + j];
                if (PEAKGENERAL_LISTENER(listener)->num_calls[i * max_num_threads + j] != 0)
                    thread_count++;
            }
            if(thread_count == 0) thread_count = 1;
            g_print("%30s  %10lu times  %10.3f s total  %10.3f s per thread\n",
                    hook_strings[i],
                    PEAKGENERAL_LISTENER(listener)->num_calls[i * max_num_threads],
                    PEAKGENERAL_LISTENER(listener)->total_time[i * max_num_threads],
                    PEAKGENERAL_LISTENER(listener)->total_time[i * max_num_threads]/thread_count);
        }
    }
}

void peak_general_listener_dettach()
{
    gum_interceptor_detach(interceptor, listener);
    peak_general_listener_free(PEAKGENERAL_LISTENER(listener));
    g_object_unref(listener);
    g_object_unref(interceptor);
    g_free(hook_address);
    g_free(state);
}
