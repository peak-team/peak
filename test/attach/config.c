#include "internal/attach_runtime_config.h"
#undef getenv
#include "frida-gum.h"
#include <string.h>

gboolean peak_attach_configure(GVariant *dictionary, gchar **message);

int
main(void)
{
    GVariantBuilder values;
    GVariant *dictionary;
    gchar *message = NULL;
    gum_init_embedded();
    if (setenv("PEAK_TARGET", "original-target", 1) != 0)
        return 1;
    g_variant_builder_init(&values, G_VARIANT_TYPE("a{ss}"));
    g_variant_builder_add(&values, "{ss}", "PEAK_TARGET", "injected-target");
    g_variant_builder_add(&values, "{ss}", "PEAK_DETACH_HELPER", "/configured/helper");
    dictionary = g_variant_ref_sink(g_variant_builder_end(&values));
    if (!peak_attach_configure(dictionary, &message))
        return 2;
    if (strcmp(peak_attach_getenv("PEAK_TARGET"), "injected-target") != 0 ||
        strcmp(peak_attach_glib_getenv("PEAK_TARGET"), "injected-target") != 0 ||
        strcmp(g_getenv("PEAK_DETACH_HELPER"), "/configured/helper") != 0)
        return 3;
    if (strcmp(getenv("PEAK_TARGET"), "original-target") != 0)
        return 4;
    if (peak_attach_getenv("PEAK_ABSENT_OPTION") != NULL)
        return 5;
    if (peak_attach_configure(dictionary, &message) || message == NULL)
        return 6;
    g_free(message);
    message = NULL;
    /* A serialized rejected preflight has no runtime consumer yet and can
     * discard the scoped overlay before the next corrected request. */
    peak_attach_config_reset_preflight();
    if (strcmp(getenv("PEAK_TARGET"), "original-target") != 0 ||
        !peak_attach_configure(dictionary, &message) || message != NULL ||
        strcmp(peak_attach_getenv("PEAK_TARGET"), "injected-target") != 0)
        return 7;
    g_variant_unref(dictionary);
    /* The real runtime intentionally stays resident until process exit. */
    return 0;
}
