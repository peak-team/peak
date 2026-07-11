#ifndef PEAK_INTERNAL_EXEC_MEMORY_H
#define PEAK_INTERNAL_EXEC_MEMORY_H

#include <stddef.h>

#ifndef PEAK_EXEC_MAX_ENV_ENTRIES
#define PEAK_EXEC_MAX_ENV_ENTRIES 32768U
#endif

#ifndef PEAK_EXEC_USER_STRING_MAX
#define PEAK_EXEC_USER_STRING_MAX (1024U * 1024U)
#endif

typedef enum {
    PEAK_EXEC_PREFLIGHT_VALID = 0,
    PEAK_EXEC_PREFLIGHT_INVALID,
    PEAK_EXEC_PREFLIGHT_UNKNOWN
} PeakExecPreflightResult;

PeakExecPreflightResult peak_exec_args_readable(const char* path,
                                                char* const argv[],
                                                char* const envp[]);
PeakExecPreflightResult peak_exec_argv_envp_readable(char* const argv[],
                                                     char* const envp[]);

#endif
