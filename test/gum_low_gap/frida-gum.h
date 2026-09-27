#ifndef PEAK_FRIDA_GUM_H
#define PEAK_FRIDA_GUM_H
/* Declaration-only mirror of Gum17.15.3 gummemory.h for typed test doubles. */
#include <stddef.h>
typedef void *gpointer;
typedef unsigned int guint;
typedef size_t gsize;
typedef int gboolean;
typedef enum {GUM_PAGE_NO_ACCESS=0,GUM_PAGE_READ=1,GUM_PAGE_WRITE=2,GUM_PAGE_EXECUTE=4,GUM_PAGE_RW=3,GUM_PAGE_RX=5} GumPageProtection;
typedef struct {gpointer near_address;gsize max_distance;} GumAddressSpec;
extern guint gum_query_page_size(void);
extern gpointer gum_try_alloc_n_pages_near(guint,GumPageProtection,const GumAddressSpec *);
extern gpointer gum_memory_allocate(gpointer,gsize,gsize,GumPageProtection);
extern gboolean gum_memory_free(gpointer,gsize);
#endif /* PEAK_FRIDA_GUM_H */
