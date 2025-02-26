#ifndef __GENERAL_LISTENER_H
#define __GENERAL_LISTENER_H

/**
 * @file general_listener.h
 * @brief Peak General Listener header file
 *
 * This header file defines the Peak General Listener and State structs and their associated functions.
 * It also contains the main entrance of the library for interception.
 */

#include "frida-gum.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <semaphore.h>
#include <signal.h>
#include <pthread.h>

#ifdef HAVE_MPI
#include <mpi.h>
#endif

#include "utils/utils.h"

typedef struct _PeakGeneralListener PeakGeneralListener;

/**
 * @struct _PeakGeneralListener
 * @brief Struct representing the Peak General Listener
 *
 * This struct represents the Peak General Listener, which extends GObject and implements the GumInvocationListener interface.
 * It keeps track of the total time and number of calls for each hooked function.
 */
struct _PeakGeneralListener {
    GObject parent;

    size_t hook_id;
    gulong* num_calls;
    gdouble* total_time;
    gdouble* exclusive_time;
    gfloat* max_time;
    gfloat* min_time;
};

typedef struct {
    unsigned int heartbeat_time;
    unsigned int check_interval;
} PeakHeartbeatArgs;

/**
 * @brief Attaches the Peak General Listener.
 *
 * This function attaches the Peak General Listener to the function hooks specified
 * in `peak_hook_strings`. It will record the number of times each function is called as
 * well as its total execution time in seconds. The time spent in multiple threads 
 * will be summed up.
 *
 * @return void
 */
void peak_general_listener_attach();

/**
 * @brief Prints the results of the Peak General Listener.
 *
 * This function prints the results of the Peak General Listener for each function hook.
 *
 * @return void
 */
void peak_general_listener_print(int is_MPI);

/**
 * @brief Detaches the Peak General Listener.
 *
 * This function detaches the Peak General Listener and frees the memory allocated for it.
 *
 * @return void
 */
void peak_general_listener_dettach();

void* peak_heartbeat_monitor();
#endif /* __GENERAL_LISTENER_H */
