#ifndef PEAK_SOURCE_TARGET_H
#define PEAK_SOURCE_TARGET_H

/**
 * @file source_target.h
 * @brief Built-in symbol tables used by target-group expansion.
 *
 * The module owns every table and element string. Callers must not free or
 * modify either the arrays or their elements despite the legacy non-const type.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Module-owned BLAS symbol table; treat element strings as read-only. */
extern char* source_target_array_BLAS[];
/** @brief Module-owned BLAS element count; callers should treat it as read-only. */
extern size_t source_count_BLAS;

/** @brief Module-owned LAPACK symbol table; treat element strings as read-only. */
extern char* source_target_array_LAPACK[];
/** @brief Module-owned LAPACK element count; callers should treat it as read-only. */
extern size_t source_count_LAPACK;

/** @brief Module-owned PBLAS symbol table; treat element strings as read-only. */
extern char* source_target_array_PBLAS[];
/** @brief Module-owned PBLAS element count; callers should treat it as read-only. */
extern size_t source_count_PBLAS;

/** @brief Module-owned ScaLAPACK symbol table; treat element strings as read-only. */
extern char* source_target_array_ScaLAPACK[];
/** @brief Module-owned ScaLAPACK element count; callers should treat it as read-only. */
extern size_t source_count_ScaLAPACK;

/** @brief Module-owned FFTW symbol table; treat element strings as read-only. */
extern char* source_target_array_FFTW[];
/** @brief Module-owned FFTW element count; callers should treat it as read-only. */
extern size_t source_count_FFTW;

#ifdef __cplusplus
}
#endif

#endif /* PEAK_SOURCE_TARGET_H */
