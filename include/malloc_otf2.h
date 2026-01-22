#ifndef MALLOC_OTF2_H
#define MALLOC_OTF2_H

#include "otf2/otf2.h"

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <inttypes.h>
#include "malloc_interceptor.h"

void peak_memlog_export_otf2(char* filename, const PeakMemEvent* base, size_t events);

#endif /* MALLOC_OTF2_H */