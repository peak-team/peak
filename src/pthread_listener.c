#include "pthread_listener.h"
#include "general_listener.h"
#include "peak_detach_controller.h"
#include "peak_logging.h"
#include "peak_signal_policy_internal.h"

#include <string.h>

#undef g_printerr
#define g_printerr(...) peak_log_warn(__VA_ARGS__)

static GumInterceptor* pthread_create_interceptor;
static GumInvocationListener* pthread_create_listener;
static PthreadState pthread_create_state;
static GumAttachOptions pthread_create_attach_options = {
    .listener_function_data = &pthread_create_state
};
GHashTable* peak_tid_mapping;
static GMutex tid_mapping_mutex;
static gboolean tid_mapping_initialized = FALSE;
static size_t current_tid = 0;
static GQueue reusable_tid_ids = G_QUEUE_INIT;
static gpointer pthread_create_hook_address;
static gpointer pthread_join_hook_address;
extern pthread_t heartbeat_thread;

static void pthread_listener_iface_init(gpointer g_iface, gpointer iface_data);
static void pthread_listener_insert_thread_unlocked(pthread_t tid);

typedef void* (*pthread_start_routine_t)(void*);

typedef struct {
    pthread_start_routine_t start_routine;
    void* start_arg;
    gboolean skip_tracking;
} PeakPthreadStartContext;

static void
pthread_listener_remove_thread(pthread_t tid)
{
    g_mutex_lock(&tid_mapping_mutex);
    if (tid_mapping_initialized && peak_tid_mapping != NULL) {
        size_t* mapped_id = g_hash_table_lookup(peak_tid_mapping, &tid);
        if (mapped_id != NULL) {
            g_queue_push_tail(&reusable_tid_ids, GSIZE_TO_POINTER(*mapped_id));
        }
        g_hash_table_remove(peak_tid_mapping, &tid);
    }
    g_mutex_unlock(&tid_mapping_mutex);
}

static void
peak_pthread_start_cleanup(void* data)
{
    PeakPthreadStartContext* context = data;

    if (context != NULL) {
        if (!context->skip_tracking) {
            pthread_listener_remove_thread(pthread_self());
        }
        g_free(context);
    }
}

static void*
peak_pthread_start(void* data)
{
    PeakPthreadStartContext* context = data;
    pthread_start_routine_t start_routine = context->start_routine;
    void* start_arg = context->start_arg;
    void* ret = NULL;

    if (!context->skip_tracking) {
        g_mutex_lock(&tid_mapping_mutex);
        if (tid_mapping_initialized && peak_tid_mapping != NULL) {
            pthread_listener_insert_thread_unlocked(pthread_self());
        }
        g_mutex_unlock(&tid_mapping_mutex);
    }
    (void)peak_signal_policy_unblock_reserved_for_current_thread();

    pthread_cleanup_push(peak_pthread_start_cleanup, context);
    peak_detach_controller_wait_for_mutation_window();
    ret = start_routine(start_arg);
    pthread_cleanup_pop(1);

    return ret;
}

static guint
pthread_tid_hash(gconstpointer value)
{
    const unsigned char* bytes = value;
    guint hash = 5381;

    for (size_t i = 0; i < sizeof(pthread_t); i++) {
        hash = ((hash << 5) + hash) ^ bytes[i];
    }

    return hash;
}

static gboolean
pthread_tid_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, sizeof(pthread_t)) == 0;
}

static void
pthread_listener_insert_thread_unlocked(pthread_t tid)
{
    pthread_t* key;
    size_t* mapped_id;

    if (g_hash_table_contains(peak_tid_mapping, &tid)) {
        return;
    }

    key = g_new(pthread_t, 1);
    mapped_id = g_new(size_t, 1);
    *key = tid;
    if (!g_queue_is_empty(&reusable_tid_ids)) {
        *mapped_id = GPOINTER_TO_SIZE(g_queue_pop_head(&reusable_tid_ids));
    } else {
        *mapped_id = current_tid++;
    }
    g_hash_table_insert(peak_tid_mapping, key, mapped_id);
}

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
    PthreadState* thread_state = GUM_IC_GET_THREAD_DATA(ic, PthreadState);
    pthread_t* tid = (pthread_t*)(gum_invocation_context_get_nth_argument(ic, 0));
    // g_print ("pthread_listener_on_enter %lu\n", *tid);
    if (tid == NULL) {
        pthread_t* replaced_tid = g_new0(pthread_t, 1);
        gum_invocation_context_replace_nth_argument(ic, 0, replaced_tid);
        thread_state->child_tid = replaced_tid;
        thread_state->is_original = FALSE;
    } else {
        thread_state->child_tid = tid;
        thread_state->is_original = TRUE;
    }

    /*
     * Park the creator before the real pthread_create() call while a strict
     * mutation window is active. Waiting only in the child wrapper leaves a
     * kernel-visible newborn task that the helper backend may observe before
     * it has reached PEAK's gate.
     */
    peak_detach_controller_wait_for_mutation_window();

    PeakPthreadStartContext* start_context = g_new0(PeakPthreadStartContext, 1);
    start_context->start_routine =
        (pthread_start_routine_t)gum_invocation_context_get_nth_argument(ic, 2);
    start_context->start_arg = gum_invocation_context_get_nth_argument(ic, 3);
    start_context->skip_tracking = (tid == &heartbeat_thread);
    thread_state->start_context = start_context;
    gum_invocation_context_replace_nth_argument(ic, 2, (gpointer)peak_pthread_start);
    gum_invocation_context_replace_nth_argument(ic, 3, start_context);
}

static void
pthread_listener_on_leave(GumInvocationListener* listener,
                          GumInvocationContext* ic)
{
    PthreadState* thread_state = GUM_IC_GET_THREAD_DATA(ic, PthreadState);
    int create_ret = GPOINTER_TO_INT(gum_invocation_context_get_return_value(ic));
    if (create_ret != 0 && thread_state->start_context != NULL) {
        g_free(thread_state->start_context);
        thread_state->start_context = NULL;
    }
    // g_print ("pthread_listener_on_leave %lu\n", tid);
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

static int (*original_pthread_join)(pthread_t thread, void **retval);

static int
peak_pthread_join(pthread_t thread, void **retval)
{
    // g_printerr ("peak_pthread_join called on thread %ld\n",  thread);
    int ret = original_pthread_join(thread, retval);
    if (ret == 0) {
        pthread_listener_remove_thread(thread);
    }
    return ret;
}

void pthread_listener_attach()
{
    // g_print ("peak_hook_address_count %lu num_cores %lu\n",  peak_hook_address_count, num_cores);
    peak_tid_mapping = g_hash_table_new_full(pthread_tid_hash,
                                             pthread_tid_equal,
                                             g_free,
                                             g_free);
    g_mutex_init(&tid_mapping_mutex);
    g_queue_clear(&reusable_tid_ids);
    current_tid = 0;
    tid_mapping_initialized = TRUE;
    pthread_listener_insert_thread_unlocked(pthread_self());

    pthread_create_interceptor = gum_interceptor_obtain();
    pthread_create_listener = g_object_new(PTHREAD_TYPE_LISTENER, NULL);
    peak_detach_controller_note_thread_creation_gate_installed(FALSE);

    gum_interceptor_begin_transaction(pthread_create_interceptor);
    pthread_create_hook_address =
        peak_general_listener_find_function("pthread_create");
    if (pthread_create_hook_address) {
        // g_print ("pthread_create found at %p\n",  hook_address);
        GumAttachReturn attach_status =
            gum_interceptor_attach(pthread_create_interceptor,
                                   pthread_create_hook_address,
                                   pthread_create_listener,
                                   &pthread_create_attach_options);
        if (attach_status == GUM_ATTACH_OK) {
            peak_detach_controller_note_thread_creation_gate_installed(TRUE);
        }
    }
    pthread_join_hook_address =
        peak_general_listener_find_function("pthread_join");
    if (pthread_join_hook_address) {
        gum_interceptor_replace_fast(pthread_create_interceptor,
                                    pthread_join_hook_address, &peak_pthread_join,
                                    (gpointer*)(&original_pthread_join),
                                    NULL);
    }
    gum_interceptor_end_transaction(pthread_create_interceptor);
}
// gboolean print_key_value_pair(gpointer key, gpointer value, gpointer user_data)
// {
//     g_print("Key: %lu, Value: %lu\n", *((unsigned long *)(key)), value);
//     return FALSE;
// }
static gboolean
pthread_listener_flush_teardown(void)
{
    const unsigned int max_attempts = 100;

    if (pthread_create_interceptor == NULL) {
        return TRUE;
    }

    for (unsigned int attempt = 0; attempt < max_attempts; attempt++) {
        if (gum_interceptor_flush(pthread_create_interceptor)) {
            return TRUE;
        }
        usleep(1000);
    }

    return gum_interceptor_flush(pthread_create_interceptor);
}

gboolean pthread_listener_dettach()
{
    // g_print("Hash table contents %p:\n", peak_tid_mapping);
    // gum_metal_hash_table_foreach(peak_tid_mapping, (GHFunc)print_key_value_pair, NULL);

    if (pthread_create_interceptor == NULL) {
        return TRUE;
    }

    gum_interceptor_begin_transaction(pthread_create_interceptor);
    if (pthread_create_listener != NULL) {
        gum_interceptor_detach(pthread_create_interceptor, pthread_create_listener);
    }
    if (pthread_join_hook_address != NULL) {
        gum_interceptor_revert(pthread_create_interceptor, pthread_join_hook_address);
    }
    gum_interceptor_end_transaction(pthread_create_interceptor);

    if (!pthread_listener_flush_teardown()) {
        g_printerr("[peak] pthread listener teardown did not flush; leaving pthread listener state alive\n");
        return FALSE;
    }

    /*
     * Do not clear peak_tid_mapping or tid_mapping_mutex here. Threads created
     * while PEAK was active may still run the wrapped start routine cleanup
     * after pthread_create interception has been detached.
     */
    if (pthread_create_listener != NULL) {
        g_object_unref(pthread_create_listener);
    }
    if (pthread_create_interceptor != NULL) {
        g_object_unref(pthread_create_interceptor);
    }

    pthread_create_listener = NULL;
    pthread_create_interceptor = NULL;
    pthread_create_hook_address = NULL;
    pthread_join_hook_address = NULL;

    return TRUE;
}

size_t pthread_listener_lookup_thread(pthread_t thread, gboolean* found)
{
    size_t* value = NULL;
    size_t mapped_id = 0;
    gboolean mapped_found = FALSE;

    g_mutex_lock(&tid_mapping_mutex);
    if (tid_mapping_initialized && peak_tid_mapping != NULL) {
        value = g_hash_table_lookup(peak_tid_mapping, &thread);
        if (value != NULL) {
            mapped_id = *value;
            mapped_found = TRUE;
        }
    }
    g_mutex_unlock(&tid_mapping_mutex);

    if (found != NULL) {
        *found = mapped_found;
    }
    return mapped_id;
}

size_t pthread_listener_snapshot_threads(pthread_t* tids,
                                         size_t* mapped,
                                         size_t capacity,
                                         gboolean* complete)
{
    size_t count = 0;
    GHashTableIter it;
    gpointer tid_key;
    gpointer mapped_id;
    gboolean copied_all = TRUE;

    if (complete != NULL) {
        *complete = TRUE;
    }

    if (tids == NULL || mapped == NULL || capacity == 0) {
        g_mutex_lock(&tid_mapping_mutex);
        if (tid_mapping_initialized &&
            peak_tid_mapping != NULL &&
            g_hash_table_size(peak_tid_mapping) > 0) {
            copied_all = FALSE;
        }
        g_mutex_unlock(&tid_mapping_mutex);
        if (complete != NULL) {
            *complete = copied_all;
        }
        return 0;
    }

    g_mutex_lock(&tid_mapping_mutex);
    if (tid_mapping_initialized && peak_tid_mapping != NULL) {
        g_hash_table_iter_init(&it, peak_tid_mapping);
        while (g_hash_table_iter_next(&it, &tid_key, &mapped_id)) {
            if (count < capacity) {
                tids[count] = *((pthread_t*) tid_key);
                mapped[count] = *((size_t*) mapped_id);
                count++;
            } else {
                copied_all = FALSE;
            }
        }
    }
    g_mutex_unlock(&tid_mapping_mutex);

    if (complete != NULL) {
        *complete = copied_all;
    }
    return count;
}
