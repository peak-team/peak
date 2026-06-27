#ifndef PEAK_UNSAFE_GUM_PROLOGUE_H
#define PEAK_UNSAFE_GUM_PROLOGUE_H

#include "frida-gum.h"

#define PEAK_ALLOW_UNSAFE_GUM_PROLOGUE_ENV "PEAK_ALLOW_UNSAFE_GUM_PROLOGUE"
#define PEAK_UNSAFE_GUM_PROLOGUE_POLICY_ENV "PEAK_UNSAFE_GUM_PROLOGUE_POLICY"

typedef enum {
    PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT = 0,
    PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE = 1
} PeakUnsafeGumProloguePolicy;

PeakUnsafeGumProloguePolicy
peak_unsafe_gum_prologue_policy_from_env(const char* value,
                                         gboolean* valid_out);

const char*
peak_unsafe_gum_prologue_policy_name(PeakUnsafeGumProloguePolicy policy);

gboolean
peak_unsafe_gum_prologue_check(gpointer address,
                               PeakUnsafeGumProloguePolicy policy,
                               const char** reason_out);

gboolean
peak_gum_prologue_too_short_for_attach(gpointer address,
                                       const char** reason_out);

/*
 * Support hooks are PEAK runtime wrappers, not user profiling targets. PEAK
 * lets Gum decide whether a support replacement can be patched and does not
 * proactively apply target relocation policy to them.
 */
gboolean
peak_unsafe_gum_support_prologue_check(gpointer address,
                                       const char** reason_out);

#endif
