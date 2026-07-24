#ifndef PEAK_PTHREAD_LISTENER_H
#define PEAK_PTHREAD_LISTENER_H

/**
 * @file pthread_listener.h
 * @brief Track PEAK thread IDs through pthread_create and pthread_join hooks.
 */

#include "frida-gum.h"

#include <fcntl.h>
#include <unistd.h>

/**
 * @struct _PthreadListener
 * @brief Gum invocation listener used to wrap pthread_create calls.
 */
typedef struct _PthreadListener PthreadListener;

/** Gum listener object that supplies pthread_create enter/leave callbacks. */
struct _PthreadListener {
    GObject parent;
};

/**
 * @struct _PthreadState
 * @brief Per-invocation state retained around one pthread_create call.
 */
typedef struct _PthreadState PthreadState;

/**
 * Tracks the child ID argument and wrapper context until pthread_create
 * returns.
 */
struct _PthreadState {
    pthread_t* child_tid;
    gboolean is_original;
    void* start_context;
};

/**
 * @brief Attaches pthread creation/join hooks and starts thread-ID tracking.
 *
 * The function initializes the thread-ID map, registers the main thread, and
 * installs Gum hooks for pthread_create and pthread_join. Created threads
 * receive compact PEAK IDs when their wrapped start routine begins; IDs may be
 * reused after cleanup or a successful join. The hooks are removed by
 * pthread_listener_dettach(), but the mapping remains available because
 * wrapped start routines may finish after interception has stopped.
 */
void pthread_listener_attach();

/**
 * @brief Attempts to remove pthread hooks and release Gum listener objects.
 *
 * A successful flush releases the Gum listener and interceptor. The thread-ID
 * map and mutex intentionally remain alive for wrapped thread cleanup. If Gum
 * cannot flush safely, all listener state remains alive until process exit.
 *
 * @return TRUE when Gum hook teardown flushed and its listener objects were
 *         released. FALSE means PEAK intentionally retained them because
 *         callbacks may still be reachable.
 */
gboolean pthread_listener_dettach();

extern GHashTable* peak_tid_mapping;

/**
 * @brief Thread-safe lookup from pthread_t to mapped thread id.
 *
 * @param thread pthread identifier to query.
 * @param found output flag set to TRUE when mapping exists.
 * @return mapped thread id when found, 0 otherwise.
 */
size_t pthread_listener_lookup_thread(pthread_t thread, gboolean* found);

/**
 * @brief Thread-safe snapshot of tracked threads and mapped ids.
 *
 * The caller provides output buffers and capacity. The function returns number
 * of entries written (up to capacity). complete is set to FALSE when the
 * tracked-thread set did not fit in the caller's buffers.
 *
 * @param tids output buffer of pthread ids.
 * @param mapped output buffer of mapped thread ids.
 * @param capacity max entries to write.
 * @param complete output flag set to TRUE if all tracked threads were copied.
 * @return number of copied entries.
 */
size_t pthread_listener_snapshot_threads(pthread_t* tids,
                                         size_t* mapped,
                                         size_t capacity,
                                         gboolean* complete);

#endif /* PEAK_PTHREAD_LISTENER_H */
