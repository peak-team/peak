#ifndef PEAK_PTHREAD_LISTENER_H
#define PEAK_PTHREAD_LISTENER_H

/**
 * @file pthread_listener.h
 * @brief Track PEAK thread IDs through pthread_create and pthread_join hooks.
 */

#include "frida-gum.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#if defined(__GNUC__) || defined(__clang__)
#define PEAK_PTHREAD_LISTENER_API __attribute__((visibility("default")))
#else
#define PEAK_PTHREAD_LISTENER_API
#endif

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

typedef struct {
    uint64_t scans;
    uint64_t entries_examined;
    uint64_t candidates_checked;
    uint64_t reclaimed;
    uint64_t deferred_alive;
    uint64_t ambiguous_checks;
    uint64_t retire_failures;
    size_t pending;
    size_t max_pending;
    unsigned int scan_budget;
    unsigned int scan_interval_ms;
} PeakPthreadReclamationDiagnostics;

/**
 * Tracks the child ID argument and wrapper context until pthread_create
 * returns. Successful joins release slots immediately. On Linux, detached
 * slots become reusable only after PEAK's final TLS-destructor pass and a
 * slow-path proof that the original kernel TID is absent from /proc/self/task.
 * Ambiguous or unsupported cases remain quarantined.
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
 * installs Gum hooks for pthread_create, pthread_join, and pthread_detach.
 * Created threads
 * receive compact PEAK IDs when their wrapped start routine begins; IDs are
 * are reused after a successful join or conservative Linux detached-thread
 * exit proof. The hooks are removed by
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

/**
 * @brief Thread-safe lookup from pthread_t to mapped thread id.
 *
 * @param thread pthread identifier to query.
 * @param found output flag set to TRUE when mapping exists.
 * @return mapped thread id when found, 0 otherwise.
 */
size_t pthread_listener_lookup_thread(pthread_t thread, gboolean* found);

/** Returns the calling thread's TLS slot without consulting the hash table. */
gboolean pthread_listener_current_thread_slot(size_t* slot_out);

/** Marks the calling PEAK helper thread as ineligible for user accounting. */
void pthread_listener_exclude_current_thread(void);

/** Tags the next pthread_create issued by this thread as a PEAK helper. */
PEAK_PTHREAD_LISTENER_API void pthread_listener_mark_next_created_thread_helper(void);

/** Returns TRUE for a PEAK helper thread that must silently bypass accounting. */
gboolean pthread_listener_current_thread_excluded(void);

/** Runs one bounded Linux detached-thread reclamation pass on a slow path. */
gboolean pthread_listener_reclaim_detached_slots(void);

/** Copies detached-thread reclamation counters and configured scan bounds. */
PEAK_PTHREAD_LISTENER_API void pthread_listener_get_reclamation_diagnostics(
    PeakPthreadReclamationDiagnostics* diagnostics);

#ifdef PEAK_ENABLE_TEST_HOOKS
void pthread_listener_test_fail_slot_publish(unsigned int count);
void pthread_listener_test_clear_current_thread_slot(void);
int pthread_listener_test_thread_is_tracked(pthread_t thread);
int pthread_listener_test_stale_generation_remove_preserves_mapping(
    pthread_t thread);
int pthread_listener_test_untracked_create_removes_ambiguous_mapping(
    pthread_t thread);
PEAK_PTHREAD_LISTENER_API void
pthread_listener_test_pause_start_publication_enable(void);
PEAK_PTHREAD_LISTENER_API void
pthread_listener_test_release_start_publication(void);
PEAK_PTHREAD_LISTENER_API int
pthread_listener_test_current_thread_has_slot(void);
PEAK_PTHREAD_LISTENER_API int
pthread_listener_test_mark_current_thread_final_destructor(void);
#endif

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

#undef PEAK_PTHREAD_LISTENER_API

#endif /* PEAK_PTHREAD_LISTENER_H */
