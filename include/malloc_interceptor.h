#ifndef __MALLOC_INTERCEPTOR_H
#define __MALLOC_INTERCEPTOR_H

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
 * @brief Header file for memory allocation function interception using Gum library 
 */

#include "frida-gum.h"
#include "utils/cxx_utils.h"
#include "utils/mpi_utils.h"

typedef struct {
    uint64_t ts_ns;     // relative to t0_ns
    int64_t  delta;     // +alloc / -free / +/-realloc
    uint64_t current;   // current memory after applying delta
    uint32_t tid;       // Linux thread id
    uint8_t  op;        // 1=alloc,2=free,3=realloc_old,4=realloc_new
} __attribute__((packed)) PeakMemEvent;

typedef struct {
    char     magic[8];       // "PEAKMEM\0" magic string indicating PEAK memory log file format
    uint32_t header_bytes;   // page-aligned size of header
    uint64_t t0_ns;          // base time
    uint64_t clock_id;       // CLOCK_MONOTONIC_RAW
    int32_t  mpi_rank;       // -1 if unknown
    int32_t  pid;            // getpid()
    int32_t  ppid;           // getppid()
} __attribute__((packed)) PeakMemHeader;

typedef struct {
    int      fd;               // fd of temp binary file
    void    *map;              // base mapping (header + events)
    size_t   map_bytes;        // mapped size
    size_t   header_bytes;     // aligned header length
    size_t   capacity_events;  // how many events can fit now
    size_t   chunk_events;     // growth step
    _Atomic size_t index;      // next event slot
    pthread_mutex_t resize_mutex; // protects resize (ftruncate + mremap)
    uint64_t t0_ns;
    int      initialized;
    char     tmp_path[512];    // mmapped temp file path (unlinked at end)
    char     csv_path[512];    // final CSV output path
    char     otf2_prefix[512];   // final otf2 output prefix (rank, pid)
} PeakMemLog;

/**
 * @brief Attach memory allocation function interception
 *
 * This function attaches interception to the memory allocation functions using the Gum library.
 * Specifically, it intercepts the `malloc`, `calloc`, `realloc`, `aligned_alloc`, `posix_memalign`, and `free` functions, 
 * which can be used to perform additional actions before calling the original function.
 *
 * @return 0 if the interception was successful, a negative number in the GumReplaceReturn otherwise.
 */
int malloc_interceptor_attach();

/**
 * @brief Detach memory allocation function interception
 *
 * This function detaches the previously attached memory allocation function interception and releases any resources used by the Gum library.
 *
 * @return void
 */
void malloc_interceptor_detach();

#endif /* __MALLOC_INTERCEPTOR_H */