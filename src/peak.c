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

#include "utils/env_parser.h"
#include "utils/utils.h"

#define PEAK_TARGET_ENV "PEAK_TARGET"
#define PEAK_TARGET_DELIM ','

typedef struct _PeakGeneralListener PeakGeneralListener;

struct _PeakGeneralListener
{
  GObject parent;

  guint* num_calls;
  gdouble* total_time;
  gdouble* current_time;
};

static void peak_general_listener_iface_init (gpointer g_iface, gpointer iface_data);

#define PEAKGENERAL_TYPE_LISTENER (peak_general_listener_get_type ())
G_DECLARE_FINAL_TYPE (PeakGeneralListener, peak_general_listener, PEAKGENERAL, LISTENER, GObject)
G_DEFINE_TYPE_EXTENDED (PeakGeneralListener,
                        peak_general_listener,
                        G_TYPE_OBJECT,
                        0,
                        G_IMPLEMENT_INTERFACE (GUM_TYPE_INVOCATION_LISTENER,
                            peak_general_listener_iface_init))

static void
peak_general_listener_on_enter (GumInvocationListener * listener,
                           GumInvocationContext * ic)
{
  PeakGeneralListener * self = PEAKGENERAL_LISTENER (listener);
  size_t hook_id = GUM_IC_GET_FUNC_DATA (ic, size_t);
  self->num_calls[hook_id]++;
  self->current_time[hook_id] = peak_second();
}

static void
peak_general_listener_on_leave (GumInvocationListener * listener,
                           GumInvocationContext * ic)
{
  PeakGeneralListener * self = PEAKGENERAL_LISTENER (listener);
  size_t hook_id = GUM_IC_GET_FUNC_DATA (ic, size_t);
  self->total_time[hook_id] += peak_second() - self->current_time[hook_id];
}

static void
peak_general_listener_class_init (PeakGeneralListenerClass * klass)
{
  (void) PEAKGENERAL_IS_LISTENER;
  (void) glib_autoptr_cleanup_PeakGeneralListener;
}

static void
peak_general_listener_iface_init (gpointer g_iface,
                             gpointer iface_data)
{
  GumInvocationListenerInterface * iface = g_iface;

  iface->on_enter = peak_general_listener_on_enter;
  iface->on_leave = peak_general_listener_on_leave;
}

GumInterceptor * interceptor;
GumInvocationListener * listener;
size_t hook_address_count;
gpointer* hook_address = NULL;
char **hook_strings;

static void
peak_general_listener_init (PeakGeneralListener * self)
{
  self->num_calls = malloc(sizeof(size_t) * hook_address_count);
  memset(self->num_calls, 0, sizeof(size_t) * hook_address_count);
  self->total_time = malloc(sizeof(double) * hook_address_count);
  memset(self->total_time, 0, sizeof(double) * hook_address_count);
  self->current_time = malloc(sizeof(double) * hook_address_count);
  memset(self->current_time, 0, sizeof(double) * hook_address_count);
}

void libprof_init(){

  gum_init_embedded ();

  interceptor = gum_interceptor_obtain ();
  listener = g_object_new (PEAKGENERAL_TYPE_LISTENER, NULL);
  
  hook_address_count = parse_env_w_delim(PEAK_TARGET_ENV, PEAK_TARGET_DELIM, &hook_strings);
  hook_address = malloc(sizeof(gpointer) * hook_address_count);
  gum_interceptor_begin_transaction (interceptor);
  for(size_t i=0; i<hook_address_count; i++) {
    hook_address[i] = GSIZE_TO_POINTER (gum_module_find_export_by_name (NULL, hook_strings[i]));
    if (hook_address[i]) {
      // g_print ("%s address = %p\n", hook_strings[i], hook_address[i]);
      gum_interceptor_attach (interceptor,
          hook_address[i],
          listener,
          GSIZE_TO_POINTER (i));
    }
  }
  gum_interceptor_end_transaction (interceptor);
}

void libprof_fini(){
  gboolean hook_flag = 0;
  for(size_t i=0; i<hook_address_count; i++) {
    if (hook_address[i]) {
      g_print ("%s is called %u times and costs %f s\n", hook_strings[i], PEAKGENERAL_LISTENER (listener)->num_calls[i], PEAKGENERAL_LISTENER (listener)->total_time[i]);
      hook_flag = 1;
    }
  }
  if (hook_flag) {
    gum_interceptor_detach (interceptor, listener);
    g_object_unref (listener);
    g_object_unref (interceptor);
  }
  gum_deinit_embedded ();
}

__attribute__((section(".init_array"))) void *__init = libprof_init;
__attribute__((section(".fini_array"))) void *__fini = libprof_fini;