#include <stdio.h>
#include <stdlib.h>

#include "frida-gum.h"

int
main(void)
{
    gum_init_embedded();
    (void)gum_module_registry_obtain();

    /*
     * Do not generate any dynamic-loader notifications before quiescing.
     * The worker must be woken by the quiesce request itself rather than by a
     * pending eventfd value left by loader activity.
     */
    gum_interceptor_peak_quiesce_deferred_module_sync();
    gum_deinit_embedded();

    printf("gum_module_sync_idle_quiesce_ok\n");
    return EXIT_SUCCESS;
}
