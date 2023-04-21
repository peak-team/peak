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

#include "pthread_listener.h"
#include "utils/env_parser.h"
#include "utils/utils.h"

#define PEAK_TARGET_ENV "PEAK_TARGET"
#define PEAK_TARGET_DELIM ','

typedef struct _PeakGeneralListener PeakGeneralListener;

struct _PeakGeneralListener {
    GObject parent;

    gulong* num_calls;
    gdouble* total_time;
    gdouble* current_time;
};

GumInterceptor* interceptor;
GumInvocationListener* listener;
size_t hook_address_count;
gpointer* hook_address = NULL;
char** hook_strings;
long max_num_threads;

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
    size_t hook_id = GUM_IC_GET_FUNC_DATA(ic, size_t);
    pthread_t tid = pthread_self();
    // g_print ("hook_id %lu tid %lu tid_orig %lu\n", hook_id, tid, syscall(SYS_gettid));
    pthread_t mapped_tid = (pthread_t) (gum_metal_hash_table_lookup(tid_mapping, GUINT_TO_POINTER(tid)));
    // g_print ("hook_id %lu tid %lu mapped %lu\n", hook_id, pthread_self(), mapped_tid);
    self->num_calls[hook_id * max_num_threads + mapped_tid]++;
    self->current_time[hook_id * max_num_threads + mapped_tid] = peak_second();
}

static void
peak_general_listener_on_leave(GumInvocationListener* listener,
                               GumInvocationContext* ic)
{
    double end_time = peak_second();
    PeakGeneralListener* self = PEAKGENERAL_LISTENER(listener);
    size_t hook_id = GUM_IC_GET_FUNC_DATA(ic, size_t);
    pthread_t tid = pthread_self();
    pthread_t mapped_tid = (pthread_t) (gum_metal_hash_table_lookup(tid_mapping, GUINT_TO_POINTER(tid)));
    self->total_time[hook_id * max_num_threads + mapped_tid] += end_time - self->current_time[hook_id * max_num_threads + mapped_tid];
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
    // g_print ("total count %lu hook_address_count %lu num_cores %lu\n", total_count, hook_address_count, num_cores);
    self->num_calls = g_new0(gulong, total_count);
    self->total_time = g_new0(double, total_count);
    self->current_time = g_new0(double, total_count);
}

static void
peak_general_listener_free(PeakGeneralListener* self)
{
    g_free(self->num_calls);
    g_free(self->total_time);
    g_free(self->current_time);
}

void libprof_init()
{

    max_num_threads = sysconf(_SC_NPROCESSORS_ONLN) * 2;
    hook_address_count = parse_env_w_delim(PEAK_TARGET_ENV, PEAK_TARGET_DELIM, &hook_strings);

    gum_init_embedded();

    pthread_listener_attach();

    interceptor = gum_interceptor_obtain();
    listener = g_object_new(PEAKGENERAL_TYPE_LISTENER, NULL);

    hook_address = g_new0(gpointer, hook_address_count);
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
    for (size_t i = 0; i < hook_address_count; i++) {
        if (hook_address[i]) {
            for (size_t j = 1; j < max_num_threads; j++) {
                PEAKGENERAL_LISTENER(listener)->num_calls[i * max_num_threads] += PEAKGENERAL_LISTENER(listener)->num_calls[i * max_num_threads + j];
                PEAKGENERAL_LISTENER(listener)->total_time[i * max_num_threads] += PEAKGENERAL_LISTENER(listener)->total_time[i * max_num_threads + j];
            }
            g_print("%s is called %lu times and costs %f s\n",
                    hook_strings[i],
                    PEAKGENERAL_LISTENER(listener)->num_calls[i * max_num_threads],
                    PEAKGENERAL_LISTENER(listener)->total_time[i * max_num_threads]);
        }
    }
    gum_interceptor_detach(interceptor, listener);
    peak_general_listener_free(PEAKGENERAL_LISTENER(listener));
    g_object_unref(listener);
    g_object_unref(interceptor);
    pthread_listener_dettach();
    g_free(hook_address);
    gum_deinit_embedded();
}

__attribute__((section(".init_array"))) void* __init = libprof_init;
__attribute__((section(".fini_array"))) void* __fini = libprof_fini;