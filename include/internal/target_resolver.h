#ifndef PEAK_TARGET_RESOLVER_H
#define PEAK_TARGET_RESOLVER_H

/* Shared target-selector resolution for startup, dynamic modules, and CLI. */

#include "internal/gum_peak_compat.h"

#include <stdio.h>

typedef enum {
    PEAK_TARGET_RESOLVE_NONE = 0,
    PEAK_TARGET_RESOLVE_UNIQUE,
    PEAK_TARGET_RESOLVE_AMBIGUOUS,
    PEAK_TARGET_RESOLVE_INVALID
} PeakTargetResolveResult;

typedef struct {
    gpointer address;
    gpointer symbol_address;
    gsize size;
    gchar* module;
    gchar* mangled;
    gchar* demangled;
    unsigned int match_tier;
} PeakTargetSymbolCandidate;

typedef struct {
    GPtrArray* candidates;
} PeakTargetResolution;

/*
 * Resolves a PEAK target selector. A selector may be qualified as
 * "module!symbol" and may have a +0xOFFSET suffix. When module_path is set,
 * resolution is additionally restricted to that loaded module. The caller
 * owns the result and must call peak_target_resolution_clear().
 */
PeakTargetResolveResult peak_target_resolver_resolve(
    const char* selector,
    const char* module_path,
    gboolean allow_legacy_short,
    PeakTargetResolution* resolution);

/* Validates selector syntax without enumerating or loading a module. */
gboolean peak_target_resolver_validate_selector(const char* selector);

/* Duplicates the validated selector's optional module portion. */
gboolean peak_target_resolver_dup_selector_module(const char* selector,
                                                   gchar** module_out);

/* Matches a target signature while excluding ABI/compiler-generated aliases. */
gboolean peak_target_resolver_full_signature_matches(const char* selector,
                                                      const char* demangled);

void peak_target_resolution_clear(PeakTargetResolution* resolution);

gboolean peak_target_resolver_module_matches(const char* requested,
                                             const char* module_path);

void peak_target_resolver_print(FILE* stream,
                                const char* selector,
                                const PeakTargetResolution* resolution);

#endif /* PEAK_TARGET_RESOLVER_H */
