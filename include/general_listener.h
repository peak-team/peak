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

#include "utils/utils.h"

#define PEAK_TARGET_ENV "PEAK_TARGET"
#define PEAK_TARGET_DELIM ','

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

    gulong* num_calls;
    gdouble* total_time;
};

typedef struct _PeakGeneralState PeakGeneralState;

/**
 * @struct _PeakGeneralState
 * @brief Struct representing the Peak General State
 *
 * This struct represents the Peak General State, which keeps track of the hook ID and current time for each hooked function.
 */
struct _PeakGeneralState {
    size_t hook_id;
    gdouble current_time;
};

void peak_general_listener_attach();
void peak_general_listener_print();
void peak_general_listener_dettach();

#endif /* __GENERAL_LISTENER_H */
