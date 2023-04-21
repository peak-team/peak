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
 * @brief Attach the pthread listener
 */
void pthread_listener_attach();

/**
 * @brief Detach the pthread listener
 */
void pthread_listener_dettach();

extern GumMetalHashTable* tid_mapping;

#endif /* _PTHREAD_LISTENER_H_ */
