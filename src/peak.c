/*
 * Compile with:
 *
 * gcc -ffunction-sections -fdata-sections frida-gum-example.c -o frida-gum-example -L. -lfrida-gum -ldl -lrt -lresolv -lm -pthread -static-libgcc -Wl,-z,noexecstack,--gc-sections
 *
 * Visit https://frida.re to learn more about Frida.
 */

#include "frida-gum.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "utils/env_parser.h"
#include "utils/utils.h"

#define PEAK_TARGET_ENV "PEAK_TARGET"
#define PEAK_TARGET_DELIM ','

typedef struct _PeakGeneralListener PeakGeneralListener;

struct _PeakGeneralListener {
    GObject parent;

    guint* num_calls;
    gdouble* total_time;
    // gdouble* current_time;
};

typedef struct _GeneralThreadData GeneralThreadData;

struct _GeneralThreadData {
    PeakGeneralListener* listener;
    GeneralThreadData* previous;
    size_t hook_id;
    guint tid;
    gdouble time;
    gdouble end_time;
    gboolean is_enter;
};

typedef struct _GeneralThreadKey GeneralThreadKey;

struct _GeneralThreadKey {
    size_t hook_id;
    guint tid;
};

guint general_thread_key_hash(gconstpointer key)
{
    const GeneralThreadKey *k = (const GeneralThreadKey *) key;
    return g_direct_hash(GUINT_TO_POINTER(k->hook_id)) ^ g_direct_hash(GUINT_TO_POINTER(k->tid));
}

gboolean general_thread_key_equal(gconstpointer a, gconstpointer b)
{
    const GeneralThreadKey *ka = (const GeneralThreadKey *) a;
    const GeneralThreadKey *kb = (const GeneralThreadKey *) b;
    return (ka->hook_id == kb->hook_id && ka->tid == kb->tid);
}

GumInterceptor* interceptor;
GumInvocationListener* listener;
size_t hook_address_count;
gpointer* hook_address = NULL;
char** hook_strings;
long num_cores;
GThreadPool* general_thread_pool;
GumMetalHashTable* general_thread_hash_table;

void general_thread_func(gpointer data, gpointer user_data)
{
    GeneralThreadData *thread_data = (GeneralThreadData*) data;
    size_t hook_id = thread_data->hook_id;
    guint tid = thread_data->tid;
    GeneralThreadKey key;
    key.hook_id = hook_id;
    key.tid = tid;
    if (thread_data->is_enter) {
        thread_data->listener->num_calls[hook_id]++;
        gum_metal_hash_table_insert(general_thread_hash_table, &key, &thread_data->time);
        // g_print ("inserted %f on %lu and %lu size %lu \n", thread_data->time, thread_data->hook_id, thread_data->tid, gum_metal_hash_table_size(general_thread_hash_table));
    } else {
        gdouble start_time = *((gdouble*)gum_metal_hash_table_lookup(general_thread_hash_table, &key));
        // g_print ("time is %f\n", *(gdouble*)gum_metal_hash_table_lookup(general_thread_hash_table, &key));
        gdouble end_time = thread_data->end_time;
        // g_print ("time is %f - %f on  %lu and %lu size %lu\n", end_time, start_time, thread_data->hook_id, thread_data->tid, gum_metal_hash_table_size(general_thread_hash_table));
        thread_data->listener->total_time[hook_id] += end_time - start_time;
    }
    //free(thread_data);
}

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
    // PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
    // size_t hook_id = GUM_IC_GET_FUNC_DATA(ic, size_t);
    // pid_t tid = syscall(SYS_gettid) % num_cores;
    // g_print ("hook_id %lu tid %lu tid_orig %lu gtid %lu\n", hook_id, tid, syscall(SYS_gettid), gum_invocation_context_get_thread_id(ic));
    // self->num_calls[hook_id * num_cores + tid]++;
    // self->current_time[hook_id * num_cores + tid] = peak_second();
    GeneralThreadData* thread_data = malloc(sizeof(GeneralThreadData));
    thread_data->is_enter = TRUE;
    thread_data->listener = PEAKGENERAL_LISTENER(listener);
    thread_data->hook_id = GUM_IC_GET_FUNC_DATA(ic, size_t);
    thread_data->tid = gum_invocation_context_get_thread_id(ic);
    thread_data->time = peak_second();
    // g_print ("start time is %f on  %lu and %lu \n", thread_data->time, thread_data->hook_id, thread_data->tid);
    g_thread_pool_push(general_thread_pool, thread_data, NULL);
}

static void
peak_general_listener_on_leave(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    double end_time = peak_second();
    // PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
    // size_t hook_id = GUM_IC_GET_FUNC_DATA(ic, size_t);
    // pid_t tid = syscall(SYS_gettid) % num_cores;
    // self->total_time[hook_id * num_cores + tid] += end_time - self->current_time[hook_id * num_cores + tid];
    GeneralThreadData* thread_data = malloc(sizeof(GeneralThreadData));
    thread_data->is_enter = FALSE;
    thread_data->listener = PEAKGENERAL_LISTENER(listener);
    thread_data->hook_id = GUM_IC_GET_FUNC_DATA(ic, size_t);
    thread_data->tid = gum_invocation_context_get_thread_id(ic);
    thread_data->end_time = end_time;
    // g_print ("end   time is %f on  %lu and %lu \n", thread_data->time, thread_data->hook_id, thread_data->tid);
    g_thread_pool_push(general_thread_pool, thread_data, NULL);
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
    size_t total_count = hook_address_count * num_cores;
    // g_print ("total count %lu hook_address_count %lu num_cores %lu\n", total_count, hook_address_count, num_cores);
    self->num_calls = malloc(sizeof(size_t) * total_count);
    memset(self->num_calls, 0, sizeof(size_t) * total_count);
    self->total_time = malloc(sizeof(double) * total_count);
    memset(self->total_time, 0, sizeof(double) * total_count);
    // self->current_time = malloc(sizeof(double) * total_count);
    // memset(self->current_time, 0, sizeof(double) * total_count);
}

void libprof_init()
{

    num_cores = 1; //sysconf(_SC_NPROCESSORS_ONLN) * 2;
    hook_address_count = parse_env_w_delim(PEAK_TARGET_ENV, PEAK_TARGET_DELIM, &hook_strings);

    gum_init_embedded();
    
    general_thread_pool = g_thread_pool_new(general_thread_func, NULL, 1, FALSE, NULL);
    general_thread_hash_table = gum_metal_hash_table_new((GHashFunc) general_thread_key_hash, (GEqualFunc) general_thread_key_equal);

    interceptor = gum_interceptor_obtain();
    listener = g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);

    hook_address = malloc(sizeof(gpointer) * hook_address_count);
    // g_print ("hook_address_count %lu num_cores %lu\n",  hook_address_count, num_cores);
    gum_interceptor_begin_transaction(interceptor);
    for (size_t i = 0; i < hook_address_count; i++) {
        hook_address[i] = gum_find_function (hook_strings[i]);
        if (hook_address[i]) {
            // g_print ("%s address = %p\n", hook_strings[i], hook_address[i]);
            gum_interceptor_attach(interceptor,
                                   hook_address[i],
                                   listener,
                                   GSIZE_TO_POINTER(i));
        }
    }
    gum_interceptor_end_transaction(interceptor);
}

void libprof_fini()
{
    g_thread_pool_free(general_thread_pool, FALSE, TRUE);
    for (size_t i = 0; i < hook_address_count; i++) {
        if (hook_address[i]) {
            for (size_t j = 1; j < num_cores; j++) {
                PEAKGENERAL_LISTENER(listener)->num_calls[i * num_cores] += PEAKGENERAL_LISTENER(listener)->num_calls[i * num_cores + j];
                PEAKGENERAL_LISTENER(listener)->total_time[i * num_cores] += PEAKGENERAL_LISTENER(listener)->total_time[i * num_cores + j];
            }
            g_print("%s is called %u times and costs %f s\n",
                    hook_strings[i],
                    PEAKGENERAL_LISTENER(listener)->num_calls[i * num_cores],
                    PEAKGENERAL_LISTENER(listener)->total_time[i * num_cores]);
        }
    }
    gum_interceptor_detach(interceptor, listener);
    g_object_unref(listener);
    g_object_unref(interceptor);
    gum_metal_hash_table_destroy(general_thread_hash_table);
    gum_deinit_embedded();
}

__attribute__((section(".init_array"))) void* __init = libprof_init;
__attribute__((section(".fini_array"))) void* __fini = libprof_fini;