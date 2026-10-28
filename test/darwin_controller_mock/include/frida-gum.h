#ifndef PEAK_DARWIN_CONTROLLER_MOCK_FRIDA_GUM_H
#define PEAK_DARWIN_CONTROLLER_MOCK_FRIDA_GUM_H
#include <stddef.h>
#include <stdint.h>
typedef int gboolean;
typedef char gchar;
typedef void* gpointer;
typedef unsigned char guint8;
typedef unsigned int guint;
typedef size_t gsize;
typedef struct GumInterceptor GumInterceptor;
typedef struct GumInvocationListener GumInvocationListener;
typedef struct { int unused; } GumAttachOptions;
typedef struct { int unused; } GumReplaceOptions;
typedef int GumAttachReturn;
typedef int GumReplaceReturn;
#define TRUE 1
#define FALSE 0
#define GUM_ATTACH_OK 0
#define GUM_REPLACE_OK 0
typedef int GumPageProtection;
#define GUM_PAGE_RW 3
#define GUM_PAGE_RX 5
gboolean gum_memory_can_remap_writable(void);
gboolean gum_memory_query_protection(const void*, GumPageProtection*);
gpointer gum_try_alloc_n_pages(guint, GumPageProtection);
void gum_free_pages(gpointer);
gsize gum_query_page_size(void);
gpointer gum_memory_try_remap_writable_pages(gpointer, guint);
void gum_memory_dispose_writable_pages(gpointer, guint);
gpointer gum_strip_code_pointer(gpointer);
#endif /* PEAK_DARWIN_CONTROLLER_MOCK_FRIDA_GUM_H */
