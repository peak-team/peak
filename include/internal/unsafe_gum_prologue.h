#ifndef PEAK_UNSAFE_GUM_PROLOGUE_H
#define PEAK_UNSAFE_GUM_PROLOGUE_H

#include "frida-gum.h"

#define PEAK_ALLOW_UNSAFE_GUM_PROLOGUE_ENV "PEAK_ALLOW_UNSAFE_GUM_PROLOGUE"
#define PEAK_UNSAFE_GUM_PROLOGUE_POLICY_ENV "PEAK_UNSAFE_GUM_PROLOGUE_POLICY"

typedef enum {
    PEAK_UNSAFE_GUM_PROLOGUE_POLICY_DEFAULT = 0,
    PEAK_UNSAFE_GUM_PROLOGUE_POLICY_CONSERVATIVE = 1
} PeakUnsafeGumProloguePolicy;

typedef struct {
    GumAttachOptions options;
    gpointer mutation_address;
    gsize mutation_guard_size;
    gboolean attach_exact_entry;
} PeakGumTargetAttachPlan;

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
 * Plans a profiling-target attach. Linux/x86-64 exact-entry support keeps
 * rel32 and RIP-indirect tail jumps at their exported boundaries.
 * Linux/AArch64 narrowly opts canonical B-to-PLT thunks into Gum's forced
 * relocation path and reports the PLT entry that Gum will actually mutate.
 * Other targets retain Gum's checked default.
 */
void
peak_gum_target_attach_plan(gpointer address,
                            PeakGumTargetAttachPlan* plan_out);

/* Applies a target plan, including exact-entry semantics when available. */
GumAttachReturn
peak_gum_interceptor_attach_target(GumInterceptor* interceptor,
                                   gpointer address,
                                   GumInvocationListener* listener,
                                   const PeakGumTargetAttachPlan* plan);

/*
 * Support hooks are PEAK runtime wrappers, not user profiling targets. PEAK
 * lets Gum decide whether a support replacement can be patched and does not
 * proactively apply target relocation policy to them.
 */
gboolean
peak_unsafe_gum_support_prologue_check(gpointer address,
                                       const char** reason_out);

#endif /* PEAK_UNSAFE_GUM_PROLOGUE_H */
