#ifndef __MALLOC_INTERCEPTOR_H
#define __MALLOC_INTERCEPTOR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <linux/limits.h>
#include <pthread.h>
#include <stdint.h>

/**
 * @file malloc_interceptor.h
 * @brief Header file for memory allocation function interception using Gum library 
 */

#include "frida-gum.h"

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
void malloc_interceptor_dettach();

#endif /* __MALLOC_INTERCEPTOR_H */