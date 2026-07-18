#ifndef PEAK_UTILS_H
#define PEAK_UTILS_H

/**
 * @file utils.h
 * @brief Compatibility umbrella for PEAK utility interfaces.
 */

/*
 * Keep the historical transitive standard-library includes for source
 * compatibility while new code includes the focused utility headers below.
 */
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "parent_registry.h"
#include "process_filter.h"
#include "timing.h"

#endif /* PEAK_UTILS_H */
