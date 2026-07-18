#include "internal/general_listener/attach_policy.h"

#include "internal/general_listener/runtime_config.h"
#include "internal/unsafe_gum_prologue.h"
#include "logging.h"

#include <dirent.h>

#undef g_printerr
#define g_printerr(...) peak_log_warn(__VA_ARGS__)

static gsize peak_attach_policy_initialized = 0;
static gboolean peak_allow_unsafe_gum_prologue = FALSE;
static PeakUnsafeGumProloguePolicy peak_unsafe_gum_prologue_policy =
    PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT;

static void
peak_general_listener_init_attach_policy_once(void)
{
    gboolean policy_valid = FALSE;

    peak_allow_unsafe_gum_prologue =
        peak_general_listener_env_value_truthy(
            g_getenv(PEAK_ALLOW_UNSAFE_GUM_PROLOGUE_ENV));

    peak_unsafe_gum_prologue_policy =
        peak_unsafe_gum_prologue_policy_from_env(
            g_getenv(PEAK_UNSAFE_GUM_PROLOGUE_POLICY_ENV),
            &policy_valid);
    if (!policy_valid) {
        peak_log_info("[peak] ignoring invalid %s=%s; using %s policy\n",
                   PEAK_UNSAFE_GUM_PROLOGUE_POLICY_ENV,
                   g_getenv(PEAK_UNSAFE_GUM_PROLOGUE_POLICY_ENV),
                   peak_unsafe_gum_prologue_policy_name(
                       PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT));
        peak_unsafe_gum_prologue_policy =
            PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT;
    }
}

void
peak_general_listener_init_attach_policy(void)
{
    if (g_once_init_enter(&peak_attach_policy_initialized)) {
        peak_general_listener_init_attach_policy_once();
        g_once_init_leave(&peak_attach_policy_initialized, 1);
    }
}

gboolean
peak_general_listener_attach_target_is_supported(const char* symbol_name,
                                                 gpointer address)
{
    const char* reason = NULL;

    peak_general_listener_init_attach_policy();

    if (peak_allow_unsafe_gum_prologue) {
        return TRUE;
    }

    if (peak_gum_prologue_too_short_for_attach(address, &reason)) {
        g_printerr("[peak] skipping Gum attach for hook %s: target prologue is too small for Gum entry patch (reason=%s); target will remain unprofiled\n",
                   symbol_name != NULL ? symbol_name : "<unknown>",
                   reason != NULL ? reason : "unknown");
        return FALSE;
    }

    if (peak_unsafe_gum_prologue_check(address,
                                       peak_unsafe_gum_prologue_policy,
                                       &reason)) {
        g_printerr("[peak] skipping Gum attach for hook %s: target prologue is not safe for Gum relocation (reason=%s, policy=%s); set %s=1 to override\n",
                   symbol_name != NULL ? symbol_name : "<unknown>",
                   reason != NULL ? reason : "unknown",
                   peak_unsafe_gum_prologue_policy_name(
                       peak_unsafe_gum_prologue_policy),
                   PEAK_ALLOW_UNSAFE_GUM_PROLOGUE_ENV);
        return FALSE;
    }

    return TRUE;
}

gboolean
peak_general_listener_support_attach_target_is_supported(const char* symbol_name,
                                                         gpointer address)
{
    peak_general_listener_init_attach_policy();

    (void)symbol_name;
    (void)address;
    return TRUE;
}

gboolean
peak_general_listener_startup_attach_can_skip_stop(void)
{
    DIR* dir = opendir("/proc/self/task");
    struct dirent* entry;
    unsigned int task_count = 0;

    if (dir == NULL) {
        return FALSE;
    }

    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;

        if (name[0] == '.') {
            continue;
        }
        task_count++;
        if (task_count > 1) {
            closedir(dir);
            return FALSE;
        }
    }

    closedir(dir);
    return task_count == 1;
}
