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

typedef struct _ExampleListener ExampleListener;
typedef enum _ExampleHookId ExampleHookId;

struct _ExampleListener
{
  GObject parent;

  guint num_calls;
};

enum _ExampleHookId
{
  EXAMPLE_HOOK_DGEMM,
  EXAMPLE_HOOK_CLOSE
};

static void example_listener_iface_init (gpointer g_iface, gpointer iface_data);

#define EXAMPLE_TYPE_LISTENER (example_listener_get_type ())
G_DECLARE_FINAL_TYPE (ExampleListener, example_listener, EXAMPLE, LISTENER, GObject)
G_DEFINE_TYPE_EXTENDED (ExampleListener,
                        example_listener,
                        G_TYPE_OBJECT,
                        0,
                        G_IMPLEMENT_INTERFACE (GUM_TYPE_INVOCATION_LISTENER,
                            example_listener_iface_init))

static void
example_listener_on_enter (GumInvocationListener * listener,
                           GumInvocationContext * ic)
{
  ExampleListener * self = EXAMPLE_LISTENER (listener);
  ExampleHookId hook_id = GUM_IC_GET_FUNC_DATA (ic, ExampleHookId);
  // if(self->num_calls == 1)
    // g_print ("[*] dgemm_(M = %d, N = %d, K = %d) %d\n", 
    //   *((int*) (gum_invocation_context_get_nth_argument (ic, 2))),
    //   *((int*) (gum_invocation_context_get_nth_argument (ic, 3))),
    //   *((int*) (gum_invocation_context_get_nth_argument (ic, 4))), self->num_calls
    // );

  // switch (hook_id)
  // {
  //   case EXAMPLE_HOOK_DGEMM:
  //     g_print ("[*] dgemm_(M = %d, N = %d, K = %d)\n", 
  //       GPOINTER_TO_INT (gum_invocation_context_get_nth_argument (ic, 2)),
  //       GPOINTER_TO_INT (gum_invocation_context_get_nth_argument (ic, 3)),
  //       GPOINTER_TO_INT (gum_invocation_context_get_nth_argument (ic, 4))
  //     );
  //     break;
  //   case EXAMPLE_HOOK_CLOSE:
  //     g_print ("Error\n");
  //     break;
  // }

  self->num_calls++;
}

static void
example_listener_on_leave (GumInvocationListener * listener,
                           GumInvocationContext * ic)
{
}

static void
example_listener_class_init (ExampleListenerClass * klass)
{
  (void) EXAMPLE_IS_LISTENER;
  (void) glib_autoptr_cleanup_ExampleListener;
}

static void
example_listener_iface_init (gpointer g_iface,
                             gpointer iface_data)
{
  GumInvocationListenerInterface * iface = g_iface;

  iface->on_enter = example_listener_on_enter;
  iface->on_leave = example_listener_on_leave;
}

static void
example_listener_init (ExampleListener * self)
{
}

GumInterceptor * interceptor;
GumInvocationListener * listener;
gpointer hook_address;
void libprof_init(){

  gum_init_embedded ();

  interceptor = gum_interceptor_obtain ();
  listener = g_object_new (EXAMPLE_TYPE_LISTENER, NULL);
  
  char **strings;
  int count = parse_env_w_comma("PEAK_TARGET", &strings);
  if(count>0) {
    hook_address = GSIZE_TO_POINTER (gum_module_find_export_by_name (NULL, strings[0]));
  }
  else
    hook_address = NULL;
  if (hook_address) {
    g_print ("dgemm address = %p\n", hook_address);
    
    gum_interceptor_begin_transaction (interceptor);
    gum_interceptor_attach (interceptor,
        hook_address,
        listener,
        GSIZE_TO_POINTER (EXAMPLE_HOOK_DGEMM));
    gum_interceptor_end_transaction (interceptor);
  }

  // if (count == 0) {
  //     printf("No strings found in PEAK_TARGET\n");
  // } else {
  //     printf("Parsed %d strings from PEAK_TARGET:\n", count);
  //     for (int i = 0; i < count; i++) {
  //         printf("  %s\n", strings[i]);
  //         free(strings[i]);
  //     }
  //     free(strings);
  // }
}

void libprof_fini(){
  if (hook_address) {
    gum_interceptor_detach (interceptor, listener);

    g_print ("[*] listener has %u calls\n", EXAMPLE_LISTENER (listener)->num_calls);

    g_object_unref (listener);
    g_object_unref (interceptor);
  }
  gum_deinit_embedded ();
}

__attribute__((section(".init_array"))) void *__init = libprof_init;
__attribute__((section(".fini_array"))) void *__fini = libprof_fini;