#define _GNU_SOURCE
#include "internal/general_listener/attach_policy.h"

#include "internal/general_listener/runtime_config.h"
#include "internal/unsafe_gum_prologue.h"
#include "logging.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#if defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#include <sys/auxv.h>
extern struct r_debug _r_debug __attribute__((weak));
#endif

#if defined(__APPLE__) && \
    (defined(__arm64__) || defined(__aarch64__))
/* The pinned devkit exports this public API but omits its reader header. */
GUM_API gpointer gum_arm64_reader_try_get_relative_jump_target(
    gconstpointer address);
#endif

#undef g_printerr
#define g_printerr(...) peak_log_warn(__VA_ARGS__)

static gsize peak_attach_policy_initialized = 0;
#if defined(__linux__)
static GArray* peak_elf_loader_ranges;

static int
peak_collect_loader_ranges(struct dl_phdr_info* info, size_t size, void* data)
{
    GumAddress base = *(GumAddress*)data;
    (void)size;
    if ((GumAddress)info->dlpi_addr != base)
        return 0;
    for (size_t i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)* header = &info->dlpi_phdr[i];
        if (header->p_type == PT_LOAD && (header->p_flags & PF_X)) {
            GumMemoryRange range = {base + header->p_vaddr, header->p_memsz};
            g_array_append_val(peak_elf_loader_ranges, range);
        }
    }
    return 1;
}
#endif
static gboolean peak_allow_unsafe_gum_prologue = FALSE;
static PeakUnsafeGumProloguePolicy peak_unsafe_gum_prologue_policy =
    PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT;

static void
peak_general_listener_init_attach_policy_once(void)
{
    gboolean policy_valid = FALSE;

#if defined(__linux__)
    GumAddress loader_base = getauxval(AT_BASE);
    /* Direct loader invocation has no PT_INTERP, but the ELF rendezvous still
     * identifies the actual loader without mistaking the main executable. */
    if (loader_base == 0 && &_r_debug != NULL)
        loader_base = _r_debug.r_ldbase;
    /* Gum's module extent can span anonymous mappings between ELF segments. */
    peak_elf_loader_ranges = g_array_new(FALSE, FALSE, sizeof(GumMemoryRange));
    if (loader_base != 0)
        dl_iterate_phdr(peak_collect_loader_ranges, &loader_base);
#endif

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

#if defined(__linux__)
    /* Gum reads errno and its TLS guard before invoking listener recursion
     * guards. Hooking these primitives recurses inside Gum itself. Compare
     * entry addresses so libc aliases receive the same protection. */
    if (address == (gpointer)__errno_location ||
        address == (gpointer)pthread_getspecific ||
        address == (gpointer)pthread_setspecific) {
        peak_log_info("[peak] skipping Gum attach for hook %s: errno/TLS accessor is required by the interceptor; target will remain unprofiled\n",
                      symbol_name != NULL ? symbol_name : "<unknown>");
        return FALSE;
    }
    /* PEAK's ownership wrappers require exclusive replacement of these
     * entries. An ordinary target listener would prevent that replacement.
     * The internal dlopen support listener calls this policy with its literal
     * name; ordinary startup dlopen targets are already skipped by the caller. */
    if (address == (gpointer)dlclose ||
        address == (gpointer)pthread_join ||
        address == (gpointer)pthread_detach ||
        (address == (gpointer)dlopen &&
         (symbol_name == NULL || strcmp(symbol_name, "dlopen") != 0))) {
        peak_log_info("[peak] skipping Gum attach for hook %s: target is owned by a profiler support wrapper; target will remain unprofiled\n",
                      symbol_name != NULL ? symbol_name : "<unknown>");
        return FALSE;
    }
    /* The ELF loader has private resolver calling conventions that cannot
     * be intercepted with ordinary function entry hooks. */
    for (guint i = 0; i < peak_elf_loader_ranges->len; i++) {
        const GumMemoryRange* range = &g_array_index(
            peak_elf_loader_ranges, GumMemoryRange, i);
        if (GUM_MEMORY_RANGE_INCLUDES(range, (GumAddress)address)) {
            peak_log_info("[peak] skipping Gum attach for hook %s: target belongs to ELF loader infrastructure; target will remain unprofiled\n",
                          symbol_name != NULL ? symbol_name : "<unknown>");
            return FALSE;
        }
    }
#endif

#if defined(__APPLE__) && \
    (defined(__arm64__) || defined(__aarch64__))
    gpointer stripped_address = gum_strip_code_pointer(address);

    if (stripped_address != NULL &&
        gum_arm64_reader_try_get_relative_jump_target(stripped_address) !=
            NULL) {
        peak_log_warn("[peak] skipping Gum attach for hook %s: target entry redirects to another address and Darwin exact-entry attach is unavailable; target will remain unprofiled\n",
                      symbol_name != NULL ? symbol_name : "<unknown>");
        return FALSE;
    }
#endif

    if (peak_allow_unsafe_gum_prologue) {
        return TRUE;
    }

    if (peak_gum_prologue_too_short_for_attach(address, &reason)) {
        peak_log_info("[peak] skipping Gum attach for hook %s: target prologue is too small for Gum entry patch (reason=%s); target will remain unprofiled\n",
                      symbol_name != NULL ? symbol_name : "<unknown>",
                      reason != NULL ? reason : "unknown");
        return FALSE;
    }

    if (peak_unsafe_gum_prologue_check(address,
                                       peak_unsafe_gum_prologue_policy,
                                       &reason)) {
        peak_log_info("[peak] skipping Gum attach for hook %s: target prologue is not safe for Gum relocation (reason=%s, policy=%s); set %s=1 to override\n",
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
#if defined(__APPLE__)
    return pthread_is_threaded_np() == 0;
#else
    DIR* dir = opendir("/proc/self/task");
    struct dirent* entry;
    unsigned int task_count = 0;
    int read_errno;

    if (dir == NULL) {
        return FALSE;
    }

    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (entry == NULL) {
            read_errno = errno;
            break;
        }
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
    return read_errno == 0 && task_count == 1;
#endif
}
