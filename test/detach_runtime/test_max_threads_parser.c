#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef unsigned long (*PeakParseMaxThreads)(void);

int
main(void)
{
    static const struct {
        const char* value;
        unsigned long expected;
    } cases[] = {
        {"0", 1},
        {"1", 1},
        {"2", 2},
        {"0003", 3},
        {"4096", 4096},
        {"4097", 4096},
        {"18446744073709551615", 4096},
    };
    PeakParseMaxThreads parse = (PeakParseMaxThreads)dlsym(
        RTLD_DEFAULT, "peak_test_parse_max_num_threads");
    if (parse == NULL) {
        fputs("missing max-thread parser test hook\n", stderr);
        return 1;
    }

    unsetenv("PEAK_MAX_NUM_THREADS");
    unsigned long fallback = parse();
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        setenv("PEAK_MAX_NUM_THREADS", cases[index].value, 1);
        if (parse() != cases[index].expected) {
            fprintf(stderr, "parser mismatch for %s\n", cases[index].value);
            return 1;
        }
    }
    static const char* invalid[] = {
        "-1", "+1", " 2", "2 ", "2x", "18446744073709551616",
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        setenv("PEAK_MAX_NUM_THREADS", invalid[index], 1);
        if (parse() != fallback) {
            fprintf(stderr, "invalid parser input accepted: %s\n", invalid[index]);
            return 1;
        }
    }
    unsetenv("PEAK_MAX_NUM_THREADS");
    puts("max_threads_parser_ok");
    return 0;
}
