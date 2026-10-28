#include "internal/attach_runtime_config.h"
#undef getenv
#undef g_getenv
#include "frida-gum.h"
#include <stdatomic.h>
#include <string.h>

/* Published once before any runtime/helper thread starts; strings live as
 * long as the resident attached runtime. No process environment is changed. */
static _Atomic(GHashTable *) attached_environment;

static gboolean
configuration_key_equal(gconstpointer a, gconstpointer b)
{
    return strcmp(a, b) == 0;
}

gboolean
peak_attach_configure(GVariant *dictionary, gchar **message)
{
    GVariantIter iter;
    const gchar *key, *value;
    GHashTable *environment = g_hash_table_new_full(g_str_hash, configuration_key_equal,
                                                   g_free, g_free);
    if (!g_variant_is_of_type(dictionary, G_VARIANT_TYPE("a{ss}"))) {
        *message = g_strdup("invalid attached-runtime environment");
        g_hash_table_unref(environment);
        return FALSE;
    }
    g_variant_iter_init(&iter, dictionary);
    while (g_variant_iter_next(&iter, "{&s&s}", &key, &value)) {
        gboolean valid = g_str_has_prefix(key, "PEAK_") && strlen(key) <= 256;
        for (const gchar *p = key; valid && *p != '\0'; p++)
            valid = g_ascii_isupper(*p) || g_ascii_isdigit(*p) || *p == '_';
        if (!valid || strlen(value) > 65536 ||
            g_hash_table_contains(environment, key)) {
            *message = g_strdup("invalid or duplicate PEAK configuration key");
            g_hash_table_unref(environment);
            return FALSE;
        }
        g_hash_table_insert(environment, g_strdup(key), g_strdup(value));
    }
    GHashTable *expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(&attached_environment,
            &expected, environment, memory_order_release, memory_order_acquire)) {
        *message = g_strdup("attached runtime has already been configured");
        g_hash_table_unref(environment);
        return FALSE;
    }
    return TRUE;
}

char *
peak_attach_getenv(const char *name)
{
    GHashTable *environment = atomic_load_explicit(&attached_environment,
                                                  memory_order_acquire);
    if (environment != NULL && g_str_has_prefix(name, "PEAK_"))
        return g_hash_table_lookup(environment, name);
    return getenv(name);
}

const char *
peak_attach_glib_getenv(const char *name)
{
    return peak_attach_getenv(name);
}
