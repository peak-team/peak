#include "pthread_listener.h"

GumInterceptor* pthread_create_interceptor;
GumInvocationListener* pthread_create_listener;
PthreadState state;
GumMetalHashTable* tid_mapping;
GMutex tid_mapping_mutex;
pthread_t current_tid = 0;

static void pthread_listener_iface_init(gpointer g_iface, gpointer iface_data);

#define PTHREAD_TYPE_LISTENER (pthread_listener_get_type())
G_DECLARE_FINAL_TYPE(PthreadListener, pthread_listener, PTHREAD, LISTENER, GObject)
G_DEFINE_TYPE_EXTENDED(PthreadListener,
                       pthread_listener,
                       G_TYPE_OBJECT,
                       0,
                       G_IMPLEMENT_INTERFACE(GUM_TYPE_INVOCATION_LISTENER,
                                             pthread_listener_iface_init))

static void
pthread_listener_on_enter(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    PthreadState* thread_state = (PthreadState*) (gum_invocation_context_get_listener_thread_data(ic, sizeof(PthreadState)));
    pthread_t* tid = (pthread_t *) (gum_invocation_context_get_nth_argument(ic, 0));
    //g_print ("%lu pthread_listener_on_enter %lu\n", my_tid, *tid);
    if (tid == NULL) {
        pthread_t* replaced_tid = g_new0(pthread_t, 1);
        gum_invocation_context_replace_nth_argument(ic, 0, replaced_tid);
        thread_state->child_tid = replaced_tid;
        thread_state->is_original = FALSE;
    } else {
        thread_state->child_tid = tid;
        thread_state->is_original = TRUE;
    }
}

static void
pthread_listener_on_leave(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    PthreadState* thread_state = (PthreadState*) (gum_invocation_context_get_listener_thread_data(ic, sizeof(PthreadState)));
    pthread_t tid = *(thread_state->child_tid);

    //g_print ("%lu pthread_listener_on_leave %lu\n", my_tid, tid);
    g_mutex_lock(&tid_mapping_mutex);
    gum_metal_hash_table_insert(tid_mapping, GUINT_TO_POINTER(tid), GUINT_TO_POINTER(current_tid));
    current_tid++;
    g_mutex_unlock(&tid_mapping_mutex);
    if (!thread_state->is_original)
        g_free(thread_state->child_tid);
}

static void
pthread_listener_class_init(PthreadListenerClass* klass)
{
    (void)PTHREAD_IS_LISTENER;
    (void)glib_autoptr_cleanup_PthreadListener;
}

static void
pthread_listener_iface_init(gpointer g_iface,
                                 gpointer iface_data)
{
    GumInvocationListenerInterface* iface = g_iface;
    iface->on_enter = pthread_listener_on_enter;
    iface->on_leave = pthread_listener_on_leave;
}

static void
pthread_listener_init(PthreadListener* self)
{

}

void pthread_listener_attach()
{
    // g_print ("hook_address_count %lu num_cores %lu\n",  hook_address_count, num_cores);
    tid_mapping = gum_metal_hash_table_new(g_direct_hash, g_direct_equal);
    g_mutex_init(&tid_mapping_mutex);
    gum_metal_hash_table_insert(tid_mapping, GUINT_TO_POINTER(pthread_self()), GUINT_TO_POINTER(current_tid));
    current_tid++;
    
    pthread_create_interceptor = gum_interceptor_obtain();
    pthread_create_listener = g_object_new(PTHREAD_TYPE_LISTENER, NULL);

    gum_interceptor_begin_transaction(pthread_create_interceptor);
    gpointer* hook_address = gum_find_function ("pthread_create");
    if (hook_address) {
        // g_print ("pthread_create found at %p\n",  hook_address);
        gum_interceptor_attach(pthread_create_interceptor,
                               hook_address,
                               pthread_create_listener,
                               &state);
    }
    gum_interceptor_end_transaction(pthread_create_interceptor);
}
// gboolean print_key_value_pair(gpointer key, gpointer value, gpointer user_data)
// {
//     g_print("Key: %lu, Value: %lu\n", *((unsigned long *)(key)), value);
//     return FALSE;
// }
void pthread_listener_dettach()
{
    // g_print("Hash table contents %p:\n", tid_mapping);
    // gum_metal_hash_table_foreach(tid_mapping, (GHFunc)print_key_value_pair, NULL);

    gum_interceptor_detach(pthread_create_interceptor, pthread_create_listener);
    gum_metal_hash_table_unref(tid_mapping);
    g_mutex_clear(&tid_mapping_mutex);
    g_object_unref(pthread_create_listener);
    g_object_unref(pthread_create_interceptor);
}
