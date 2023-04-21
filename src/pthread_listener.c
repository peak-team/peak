#include "pthread_listener.h"

GumInterceptor* pthread_create_interceptor;
GumInvocationListener* pthread_create_listener;
GumMetalHashTable* tid_mapping;
GMutex tid_mapping_mutex;
GumMetalHashTable* tid_store;
GMutex tid_store_mutex;
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
    pthread_t* tid = (pthread_t *) (gum_invocation_context_get_nth_argument(ic, 0));
    guint my_tid = pthread_self();
    //g_print ("%lu pthread_listener_on_enter %lu\n", my_tid, *tid);
    if (tid == NULL) {
        pthread_t* replaced_tid = malloc(sizeof(pthread_t));
        g_mutex_lock(&tid_store_mutex);
        gum_metal_hash_table_insert(tid_store, GUINT_TO_POINTER(my_tid), replaced_tid);
        g_mutex_unlock(&tid_store_mutex);
        gum_invocation_context_replace_nth_argument(ic, 0, replaced_tid);
    }
    g_mutex_lock(&tid_store_mutex);
    gum_metal_hash_table_insert(tid_store, GUINT_TO_POINTER(my_tid), tid);
    g_mutex_unlock(&tid_store_mutex);
}

static void
pthread_listener_on_leave(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    guint my_tid = pthread_self();
    g_mutex_lock(&tid_store_mutex);
    pthread_t tid = *((pthread_t *) (gum_metal_hash_table_lookup(tid_store, GUINT_TO_POINTER(my_tid))));
    g_mutex_unlock(&tid_store_mutex);

    //g_print ("%lu pthread_listener_on_leave %lu\n", my_tid, tid);
    g_mutex_lock(&tid_mapping_mutex);
    gum_metal_hash_table_insert(tid_mapping, GUINT_TO_POINTER(tid), GUINT_TO_POINTER(current_tid));
    current_tid++;
    g_mutex_unlock(&tid_mapping_mutex);
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
    tid_store = gum_metal_hash_table_new(g_direct_hash, g_direct_equal);
    g_mutex_init(&tid_mapping_mutex);
    g_mutex_init(&tid_store_mutex);
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
                               NULL);
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
    g_object_unref(pthread_create_listener);
    g_object_unref(pthread_create_interceptor);
}
