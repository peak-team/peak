#ifndef PEAK_MALLOC_INTERCEPTOR_H
#define PEAK_MALLOC_INTERCEPTOR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <linux/limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <signal.h>
#include <dlfcn.h>
#include <execinfo.h>   // backtrace()
#include <inttypes.h>   // PRIu64, etc.

#ifdef HAVE_MPI
#include <mpi.h>
#endif

/**
 * @file malloc_interceptor.h
 * @brief Allocation interception and binary memory-log record definitions.
 *
 * PEAK replaces the process allocation entry points and records changes in
 * tracked allocated bytes in a module-owned, memory-mapped temporary log.  The
 * temporary mapping, file descriptor, output paths, tracking tables, and Gum
 * interceptor are internal lifetime state; including this header does not
 * transfer ownership of any of them to a caller.
 */

#include "frida-gum.h"
#include "utils/cxx_utils.h"
#include "utils/mpi_utils.h"

/** @name Binary memory-log records
 * @{ */

/** One packed allocation-accounting event in the PEAK memory log. */
typedef struct {
    uint64_t ts_ns;     /**< Absolute `CLOCK_MONOTONIC_RAW` timestamp, in nanoseconds. */
    int64_t  delta;     /**< Signed change in tracked allocated bytes. */
    uint64_t current;   /**< Tracked allocated-byte total after applying @c delta. */
    uint32_t tid;       /**< Linux thread ID that recorded the event. */
    uint8_t  op;        /**< 1=add/allocation, 2=remove/free; realloc emits those operations. */
} __attribute__((packed)) PeakMemEvent;

/** Explicit lifecycle for the experimental memory-event log. */
typedef enum {
    PEAK_MEMLOG_UNINITIALIZED = 0,
    PEAK_MEMLOG_ACTIVE,
    PEAK_MEMLOG_DISABLED,
    PEAK_MEMLOG_FINALIZED,
} PeakMemLogState;

/** Fixed metadata at the start of a PEAK binary memory log. */
typedef struct {
    char     magic[8];       /**< The eight bytes `PEAKMEM\0`. */
    uint32_t header_bytes;   /**< Page-aligned offset from the mapping to the first event. */
    uint64_t t0_ns;          /**< Log-open time in the same clock domain as event timestamps. */
    uint64_t clock_id;       /**< Numeric value of `CLOCK_MONOTONIC_RAW`. */
    int32_t  mpi_rank;       /**< MPI rank, or -1 when unavailable. */
    int32_t  pid;            /**< Process ID captured with `getpid()`. */
    int32_t  ppid;           /**< Parent process ID captured with `getppid()`. */
} __attribute__((packed)) PeakMemHeader;

/** Module-owned mutable state for the mapped binary log and its exports. */
typedef struct {
    int      fd;                  /**< Descriptor for the temporary binary log. */
    void    *map;                 /**< Mapping base containing header followed by events. */
    size_t   map_bytes;           /**< Fixed mapping size, in bytes. */
    size_t   header_bytes;        /**< Page-aligned event-region offset. */
    size_t   capacity_events;     /**< Number of event slots in the fixed mapping. */
    size_t   ready_offset;        /**< Offset of one-byte atomic ready flags after events. */
    _Atomic size_t reserved;      /**< Successfully reserved event slots. */
    _Atomic size_t committed;     /**< Slots whose payload publication completed. */
    _Atomic size_t dropped;       /**< Events rejected after capacity is exhausted. */
    _Atomic size_t active_writers;/**< Writers admitted before DISABLED. */
    _Atomic PeakMemLogState state;/**< UNINITIALIZED, ACTIVE, DISABLED, or FINALIZED. */
    uint64_t t0_ns;               /**< Log-open `CLOCK_MONOTONIC_RAW` time, in nanoseconds. */
    char     tmp_path[512];       /**< Temporary mapped-file path, unlinked after export. */
    char     csv_path[512];       /**< Final CSV output path. */
    char     otf2_prefix[512];    /**< Final OTF2 archive prefix for this rank and process. */
} PeakMemLog;

/** @} */

#ifdef __cplusplus
extern "C" {
#endif

/** @name Interceptor lifecycle
 * @{ */

/**
 * @brief Installs allocation replacements and opens the memory log.
 *
 * The implementation prepares tracking tables and the binary log before any
 * replacement.  If replacement is only partially successful it reverts every
 * changed entry and flushes before returning a recoverable degraded outcome.
 *
 * @return PEAK_MALLOC_ATTACH_OK, PEAK_MALLOC_ATTACH_PREPARE_FAILED, or
 *         PEAK_MALLOC_ATTACH_ROLLED_BACK.  An unprovable post-mutation
 *         rollback terminates instead of returning.
 */
typedef enum {
    PEAK_MALLOC_ATTACH_OK = 0,
    PEAK_MALLOC_ATTACH_PREPARE_FAILED = -1,
    PEAK_MALLOC_ATTACH_ROLLED_BACK = -2,
} PeakMallocInterceptorAttachResult;

PeakMallocInterceptorAttachResult malloc_interceptor_attach(void);

/**
 * @brief Reverts allocation replacements and finalizes collected output.
 *
 * Cleanup first stops event admission, reverts the replacement entries, and
 * requires both a successful Gum flush and wrapper quiescence.  If either
 * condition fails, the function logs the failure and deliberately retains the
 * interceptor, tracking tables, mapping, file descriptor, and output state so
 * in-flight users cannot observe freed storage.  After successful quiescence,
 * it releases the tracking tables and interceptor, attempts the OTF2 and CSV
 * exports, and closes, unmaps, and unlinks the module-owned temporary log.
 */
void malloc_interceptor_detach();

#ifdef PEAK_ENABLE_TEST_HOOKS
#define PEAK_MALLOC_TEST_API __attribute__((visibility("default")))

typedef enum {
    PEAK_MEMLOG_TEST_FAIL_NONE = 0,
    PEAK_MEMLOG_TEST_FAIL_CREATE,
    PEAK_MEMLOG_TEST_FAIL_SIZING,
    PEAK_MEMLOG_TEST_FAIL_MAPPING,
    PEAK_MEMLOG_TEST_FAIL_ALLOCATION,
    PEAK_MEMLOG_TEST_FAIL_TRACKING_SETUP,
} PeakMemLogTestFailure;

typedef struct {
    PeakMemLogState state;
    size_t capacity;
    size_t reserved;
    size_t committed;
    size_t dropped;
    size_t active_writers;
    size_t exported;
    int mapping_live;
} PeakMemLogTestSnapshot;

PEAK_MALLOC_TEST_API void
peak_memlog_test_set_failure(PeakMemLogTestFailure failure);

PEAK_MALLOC_TEST_API int
peak_memlog_test_open(size_t capacity_events);

PEAK_MALLOC_TEST_API void
peak_memlog_test_log_event(uint64_t ts, int64_t delta, uint64_t current, uint8_t op);

PEAK_MALLOC_TEST_API size_t
peak_memlog_test_ready_records(void);

PEAK_MALLOC_TEST_API int
peak_memlog_test_read_event(size_t index, PeakMemEvent* out);

PEAK_MALLOC_TEST_API void
peak_memlog_test_snapshot(PeakMemLogTestSnapshot* out);

PEAK_MALLOC_TEST_API void
peak_memlog_test_pause_before_commit(int enabled);

PEAK_MALLOC_TEST_API int
peak_memlog_test_writer_is_paused(void);

PEAK_MALLOC_TEST_API void
peak_memlog_test_finalize(void);

PEAK_MALLOC_TEST_API int
peak_malloc_test_failed_realloc_preserves_accounting(void);

PEAK_MALLOC_TEST_API int
peak_malloc_test_tracking_allocation_failure(void);
#endif

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* PEAK_MALLOC_INTERCEPTOR_H */
