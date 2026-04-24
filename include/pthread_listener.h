#ifndef __PTHREAD_LISTENER_H
#define __PTHREAD_LISTENER_H

/**
 * @file pthread_listener.h
 * @brief Header file for pthread_listener.c
 */

#include "frida-gum.h"

#include <fcntl.h>
#include <unistd.h>

/**
 * @struct _PthreadListener
 * @brief Structure for PthreadListener
 *
 * This structure represents a PthreadListener.
 */
typedef struct _PthreadListener PthreadListener;

/**
 * @brief PthreadListener class
 */
struct _PthreadListener {
    GObject parent;
};

/**
 * @struct _PthreadState
 * @brief Structure for PthreadState
 *
 * This structure represents a PthreadState.
 */
typedef struct _PthreadState PthreadState;

/**
 * @brief PthreadState class
 */
struct _PthreadState {
    pthread_t* child_tid;
    gboolean is_original;
};

/**
 * @brief Attaches a PthreadListener to intercept calls to pthread_create and tracks newly created threads
 *
 * This function initializes a new thread id mapping hash table, inserts the main thread into the hash table,
 * creates a GumInterceptor object and a PthreadListener object, and attaches the listener to the pthread_create function
 * with the GumInterceptor object. Once this function is called, newly created threads will be tracked by PthreadListener
 * until the pthread_listener_dettach function is called.
 * 
 * When a new thread is created by pthread_create, the listener intercepts the call and assigns a new unique 
 * ID to the child thread, adds the ID to the hash table, and increments the counter for the next unique ID. 
 * The hash table is later used to retrieve the ID of a thread given its pthread_t identifier, allowing other 
 * parts of the program to keep track of the created threads.
 *
 * @return void
 */
void pthread_listener_attach();

/**
 * @brief Detaches the PthreadListener and cleans up related resources
 *
 * This function detaches the PthreadListener from the pthread_create function and releases related resources
 * including the thread id mapping hash table, the GumInterceptor object, and the PthreadListener object.
 *
 * @return void
 */
void pthread_listener_dettach();

extern GumMetalHashTable* peak_tid_mapping;

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
 * The caller provides output buffers and capacity. The function returns
 * number of entries written (up to capacity).
 *
 * @param tids output buffer of pthread ids.
 * @param mapped output buffer of mapped thread ids.
 * @param capacity max entries to write.
 * @return number of copied entries.
 */
size_t pthread_listener_snapshot_threads(pthread_t* tids, size_t* mapped, size_t capacity);

#endif /* __PTHREAD_LISTENER_H */
