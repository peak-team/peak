#define _POSIX_C_SOURCE 200809L

#include "target_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    char value[4097];
    char path[] = "/tmp/peak-target-config-fuzz-XXXXXX";
    char** names = NULL;
    FILE* file;
    int fd;
    size_t count;

    size_t payload_offset = size == 0 ? 0 : 1;
    size_t payload_size = size > payload_offset ? size - payload_offset : 0;
    unsigned int route = size == 0 ? 0U : data[0] % 3U;

    /* Fuzzing malformed input must not turn each generated case into stderr. */
    setenv("PEAK_VERBOSITY", "silent", 1);
    if (payload_size > sizeof(value) - 1) payload_size = sizeof(value) - 1;
    for (size_t i = 0; i < payload_size; i++) {
        unsigned char byte = data[i + payload_offset];
        value[i] = byte == '\0' ? ',' : (char)byte;
    }
    value[payload_size] = '\0';

    if (route == 0U) {
        setenv("PEAK_TARGET_CONFIG_FUZZ", value, 1);
        count = parse_env_w_delim("PEAK_TARGET_CONFIG_FUZZ", ',', &names);
        free_parsed_result(names, count);
        return 0;
    }

    if (route == 1U) {
        fd = mkstemp(path);
        if (fd >= 0 && (file = fdopen(fd, "w")) != NULL) {
            fwrite(value, 1, payload_size, file);
            fclose(file);
            setenv("PEAK_TARGET_CONFIG_FUZZ_FILE", path, 1);
            count = load_profiling_symbols("PEAK_TARGET_CONFIG_FUZZ_FILE", &names, 0);
            free_parsed_result(names, count);
            unlink(path);
        } else if (fd >= 0) {
            close(fd);
            unlink(path);
        }
        return 0;
    }

    switch (payload_size == 0 ? 0U : data[payload_offset] % 4U) {
    case 0:
        setenv("PEAK_TARGET_CONFIG_FUZZ_GROUP", "BLAS", 1);
        break;
    case 1:
        setenv("PEAK_TARGET_CONFIG_FUZZ_GROUP", ", PBLAS ,, BLAS ,", 1);
        break;
    case 2:
        setenv("PEAK_TARGET_CONFIG_FUZZ_GROUP", "PBLASX, ScaLAPACKX", 1);
        break;
    default:
        setenv("PEAK_TARGET_CONFIG_FUZZ_GROUP", value, 1);
        break;
    }
    names = NULL;
    count = load_symbols_from_array("PEAK_TARGET_CONFIG_FUZZ_GROUP", &names, 0);
    free_parsed_result(names, count);
    return 0;
}
