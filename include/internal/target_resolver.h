#ifndef PEAK_TARGET_RESOLVER_H
#define PEAK_TARGET_RESOLVER_H

/* Shared target-selector resolution for startup, dynamic modules, and CLI. */

#include "internal/gum_peak_compat.h"

#include <stddef.h>
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

typedef struct {
    const char* selector;
    const char* module_path;
    gboolean allow_legacy_short;
    PeakTargetResolution resolution;
    PeakTargetResolveResult result;
} PeakTargetResolveRequest;

#if defined(PEAK_TARGET_RESOLVER_TESTING) && defined(PEAK_ENABLE_TEST_HOOKS)
#if defined(__GNUC__) || defined(__clang__)
#define PEAK_TARGET_RESOLVER_TEST_API __attribute__((visibility("default")))
#else
#define PEAK_TARGET_RESOLVER_TEST_API
#endif
typedef struct {
    guint64 module_passes;
    guint64 module_symbol_enumerations;
    guint64 symbol_visits;
    guint64 demangles;
    guint64 candidate_match_evaluations;
} PeakTargetResolverDiagnostics;

#endif

/*
 * Resolves a PEAK target selector. A selector may be qualified as
 * "module!symbol". When module_path is set,
 * resolution is additionally restricted to that loaded module. The caller
 * owns the result and must call peak_target_resolution_clear().
 */
PeakTargetResolveResult peak_target_resolver_resolve(
    const char* selector,
    const char* module_path,
    gboolean allow_legacy_short,
    PeakTargetResolution* resolution);

/* Resolves a startup/dynamic selector batch with one module-symbol walk and
 * at most one demangle per encountered symbol. */
void peak_target_resolver_resolve_many(PeakTargetResolveRequest* requests,
                                       size_t count);

#if defined(PEAK_TARGET_RESOLVER_TESTING) && defined(PEAK_ENABLE_TEST_HOOKS)
PEAK_TARGET_RESOLVER_TEST_API void peak_target_resolver_reset_diagnostics(void);
PEAK_TARGET_RESOLVER_TEST_API void peak_target_resolver_get_diagnostics(
    PeakTargetResolverDiagnostics* out);
#endif

/* Validates selector syntax without enumerating or loading a module. */
gboolean peak_target_resolver_validate_selector(const char* selector);

/* Duplicates the validated selector's optional module portion. */
gboolean peak_target_resolver_dup_selector_module(const char* selector,
                                                   gchar** module_out);

/* Formats the user-visible target name. Module-qualified selectors retain the
 * module spelling supplied by the user; unqualified selectors use only the
 * resolved demangled signature. The caller owns the returned string. */
gchar* peak_target_resolver_format_display_name(const char* selector,
                                                const char* demangled);

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
