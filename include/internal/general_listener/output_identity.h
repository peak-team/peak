#ifndef PEAK_OUTPUT_IDENTITY_H
#define PEAK_OUTPUT_IDENTITY_H

/* Build a report filename from launcher metadata available before MPI init. */

#include <stdbool.h>
#include <stddef.h>

/* Capture the process session before any MPI initialization can occur. */
void peak_output_identity_initialize(void);

/* Create missing parent directories for an explicitly selected template. */
bool peak_output_identity_make_parent(const char* path);

/* Render a cached statistics checkpoint pathname without consulting getenv. */
/* 1=ready, 0=pre-init fallback allowed, -1=initialized but invalid. */
int peak_output_identity_checkpoint_path(char* out,
                                         size_t out_size,
                                         unsigned long long checkpoint_index);

bool peak_output_identity_path(char* out,
                               size_t out_size,
                               const char* base,
                               const char* template_value,
                               const char* extension,
                               long rank);

#endif /* PEAK_OUTPUT_IDENTITY_H */
