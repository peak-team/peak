#include <frida-gum.h>
#include "guminterceptor-private-exact.h"
#include "native-offsets.h"
#include <stddef.h>
_Static_assert(offsetof(GumFunctionContext,write_redirect)==CTX_REDIRECT,"CTX_REDIRECT");
_Static_assert(offsetof(GumFunctionContext,write_redirect_data)==CTX_FAST,"CTX_FAST");
_Static_assert(offsetof(GumFunctionContext,replacement_function)==CTX_REPLACEMENT,"CTX_REPLACEMENT");
_Static_assert(offsetof(GumFunctionContext,listener_entries)==CTX_ENTRIES,"CTX_ENTRIES");
_Static_assert(offsetof(GumFunctionContext,on_invoke_trampoline)==CTX_INVOKE,"CTX_INVOKE");
_Static_assert(offsetof(GumPeakFastListener,version)==FD_VERSION,"FD_VERSION");
_Static_assert(offsetof(GumPeakFastListener,enabled)==FD_ENABLED,"FD_ENABLED");
_Static_assert(offsetof(GumPeakFastListener,listener_entries_cookie)==FD_COOKIE,"FD_COOKIE");
_Static_assert(offsetof(GumPeakFastListener,native_pause)==FD_NATIVE,"FD_NATIVE");
_Static_assert(offsetof(GumPeakNativePause,enabled)==N_ENABLED,"N_ENABLED");
_Static_assert(offsetof(GumPeakNativePause,paused)==N_PAUSED,"N_PAUSED");
_Static_assert(offsetof(GumPeakNativePause,initial_remaining)==N_INITIAL,"N_INITIAL");
_Static_assert(offsetof(GumPeakNativePause,callbacks_suspended)==N_SUSPENDED,"N_SUSPENDED");
_Static_assert(offsetof(GumPeakNativePause,rent_base)==N_RENT_BASE,"N_RENT_BASE");
_Static_assert(offsetof(GumPeakNativePause,calls_base)==N_CALLS_BASE,"N_CALLS_BASE");
_Static_assert(offsetof(GumPeakNativePause,rent_ns)==N_RENT_NS,"N_RENT_NS");
_Static_assert(offsetof(GumPeakNativePause,capacity)==N_CAPACITY,"N_CAPACITY");
_Static_assert(offsetof(GumPeakNativePause,tls_initialized)==N_TLS_INITIALIZED,"N_TLS_INITIALIZED");
_Static_assert(offsetof(GumPeakNativePause,tls_mapped_valid)==N_TLS_MAPPED,"N_TLS_MAPPED");
_Static_assert(offsetof(GumPeakNativePause,tls_in_callback)==N_TLS_CALLBACK,"N_TLS_CALLBACK");
_Static_assert(offsetof(GumPeakNativePause,tls_ignore)==N_TLS_IGNORE,"N_TLS_IGNORE");
_Static_assert(offsetof(GumPeakNativePause,tls_slot)==N_TLS_SLOT,"N_TLS_SLOT");
_Static_assert(offsetof(GumPeakNativePause,tls_excluded)==N_TLS_EXCLUDED,"N_TLS_EXCLUDED");
_Static_assert(offsetof(GumPeakNativePause,tls_inside)==N_TLS_INSIDE,"N_TLS_INSIDE");
_Static_assert(offsetof(GumPeakNativePause,tls_registered)==N_TLS_REGISTERED,"N_TLS_REGISTERED");
_Static_assert(sizeof(gboolean)==4 && sizeof(guint)==4 && sizeof(gssize)==8 && sizeof(gsize)==8,"native scalar widths");

