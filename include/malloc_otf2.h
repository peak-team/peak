#ifndef MALLOC_OTF2_H
#define MALLOC_OTF2_H

/**
 * @file malloc_otf2.h
 * @brief OTF2 export for PEAK allocation-accounting events.
 */

#include "otf2/otf2.h"

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <inttypes.h>
#include "malloc_interceptor.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @name Memory-log export
 * @{ */

/**
 * @brief Writes allocation events as an OTF2 metric archive.
 *
 * The archive directory is `PEAK_MEMLOG_OTF2_DIR` when that variable is
 * nonempty, or the current directory otherwise; @p filename is used as the
 * archive name within that directory.  Event timestamps are copied as
 * nanosecond ticks from the `CLOCK_MONOTONIC_RAW` domain, with a 1 GHz timer
 * resolution and the minimum event timestamp as the OTF2 global offset.  They
 * are not Unix-epoch timestamps.
 *
 * @p filename and the @p events elements beginning at @p base are borrowed
 * for the duration of the call and are neither modified nor freed.  Passing a
 * null @p base or zero @p events is a no-op.  Other export failures are logged;
 * this void interface does not return an error and does not take ownership of
 * caller storage.
 *
 * @param[in] filename Non-null, NUL-terminated OTF2 archive name; borrowed.
 * @param[in] base First packed event after the binary-log header; borrowed.
 * @param[in] events Number of events available from @p base.
 */
void peak_memlog_export_otf2(char* filename, const PeakMemEvent* base, size_t events);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* MALLOC_OTF2_H */
