#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#define PEAK_JIT_SYMBOL "peak_jit_hot"
#define PEAK_JIT_DEFAULT_ITERATIONS 1000000UL
#define PEAK_JIT_SKIP 77

typedef int (*PeakJitFn)(int);

typedef enum {
    PEAK_JIT_WITH_PERF_MAP,
    PEAK_JIT_WITHOUT_METADATA,
    PEAK_JIT_WITH_PARTIAL_PERF_MAP,
    PEAK_JIT_WITH_PRE_EXEC_PERF_MAP,
    PEAK_JIT_WITH_TWO_GENERATIONS,
    PEAK_JIT_WITH_STALE_THEN_VALID,
    PEAK_JIT_WITH_DUPLICATE_PERF_MAP,
    PEAK_JIT_WITH_MALFORMED_THEN_VALID,
    PEAK_JIT_WITH_OVERLONG_THEN_VALID
} PeakJitMode;

static int
host_has_supported_code_template(void)
{
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    return 1;
#else
    return 0;
#endif
}

static size_t
copy_jit_code(unsigned char* dst, size_t dst_size)
{
#if defined(__x86_64__)
    static const unsigned char prefix[] = {
        0x8d, 0x47, 0x01  /* leal 1(%rdi), %eax */
    };
    const unsigned char nop = 0x90;
    const unsigned char ret = 0xc3;
    const size_t nop_count = 32;
#elif defined(__aarch64__)
    static const unsigned char prefix[] = {
        0x00, 0x04, 0x00, 0x11  /* add w0, w0, #1 */
    };
    static const unsigned char nop[] = {
        0x1f, 0x20, 0x03, 0xd5
    };
    static const unsigned char ret[] = {
        0xc0, 0x03, 0x5f, 0xd6
    };
    const size_t nop_count = 8;
#else
    (void)dst;
    (void)dst_size;
    return 0;
#endif

#if defined(__x86_64__)
    size_t code_size = sizeof(prefix) + nop_count + sizeof(ret);
    if (dst_size < code_size) {
        return 0;
    }
    memcpy(dst, prefix, sizeof(prefix));
    memset(dst + sizeof(prefix), nop, nop_count);
    memcpy(dst + sizeof(prefix) + nop_count, &ret, sizeof(ret));
    return code_size;
#elif defined(__aarch64__)
    size_t code_size = sizeof(prefix) + nop_count * sizeof(nop) + sizeof(ret);
    if (dst_size < code_size) {
        return 0;
    }
    memcpy(dst, prefix, sizeof(prefix));
    for (size_t i = 0; i < nop_count; i++) {
        memcpy(dst + sizeof(prefix) + i * sizeof(nop), nop, sizeof(nop));
    }
    memcpy(dst + sizeof(prefix) + nop_count * sizeof(nop), ret, sizeof(ret));
    return code_size;
#endif
}

static void
print_usage(const char* argv0)
{
    fprintf(stderr,
            "usage: %s (--with-perf-map|--without-metadata|--with-partial-perf-map|"
            "--with-pre-exec-perf-map|--with-two-generations|--with-stale-then-valid|"
            "--with-duplicate-perf-map|--with-malformed-then-valid|"
            "--with-overlong-then-valid) "
            "[--iterations N] [--metadata-sleep-us N] [--symbol NAME]\n",
            argv0);
}

static int
parse_ulong_value(const char* text, unsigned long* value, int allow_zero)
{
    char* end = NULL;
    unsigned long parsed;

    if (text == NULL || *text == '\0') {
        return -1;
    }

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        (!allow_zero && parsed == 0)) {
        return -1;
    }

    *value = parsed;
    return 0;
}

static int
parse_args(int argc,
           char** argv,
           PeakJitMode* mode,
           unsigned long* iterations,
           unsigned long* metadata_sleep_us,
           const char** symbol_name)
{
    int saw_mode = 0;

    *iterations = PEAK_JIT_DEFAULT_ITERATIONS;
    *metadata_sleep_us = 50000UL;
    *symbol_name = PEAK_JIT_SYMBOL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--with-perf-map") == 0) {
            *mode = PEAK_JIT_WITH_PERF_MAP;
            saw_mode++;
        } else if (strcmp(argv[i], "--without-metadata") == 0) {
            *mode = PEAK_JIT_WITHOUT_METADATA;
            saw_mode++;
        } else if (strcmp(argv[i], "--with-partial-perf-map") == 0) {
            *mode = PEAK_JIT_WITH_PARTIAL_PERF_MAP;
            saw_mode++;
        } else if (strcmp(argv[i], "--with-pre-exec-perf-map") == 0) {
            *mode = PEAK_JIT_WITH_PRE_EXEC_PERF_MAP;
            saw_mode++;
        } else if (strcmp(argv[i], "--with-two-generations") == 0) {
            *mode = PEAK_JIT_WITH_TWO_GENERATIONS;
            saw_mode++;
        } else if (strcmp(argv[i], "--with-stale-then-valid") == 0) {
            *mode = PEAK_JIT_WITH_STALE_THEN_VALID;
            saw_mode++;
        } else if (strcmp(argv[i], "--with-duplicate-perf-map") == 0) {
            *mode = PEAK_JIT_WITH_DUPLICATE_PERF_MAP;
            saw_mode++;
        } else if (strcmp(argv[i], "--with-malformed-then-valid") == 0) {
            *mode = PEAK_JIT_WITH_MALFORMED_THEN_VALID;
            saw_mode++;
        } else if (strcmp(argv[i], "--with-overlong-then-valid") == 0) {
            *mode = PEAK_JIT_WITH_OVERLONG_THEN_VALID;
            saw_mode++;
        } else if (strcmp(argv[i], "--iterations") == 0) {
            if (i + 1 >= argc ||
                parse_ulong_value(argv[i + 1], iterations, 0) != 0) {
                fprintf(stderr, "invalid --iterations value\n");
                return -1;
            }
            i++;
        } else if (strcmp(argv[i], "--metadata-sleep-us") == 0) {
            if (i + 1 >= argc ||
                parse_ulong_value(argv[i + 1], metadata_sleep_us, 1) != 0) {
                fprintf(stderr, "invalid --metadata-sleep-us value\n");
                return -1;
            }
            i++;
        } else if (strcmp(argv[i], "--symbol") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                fprintf(stderr, "invalid --symbol value\n");
                return -1;
            }
            *symbol_name = argv[i + 1];
            i++;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return -1;
        }
    }

    if (saw_mode != 1) {
        fprintf(stderr, "exactly one JIT metadata mode is required\n");
        return -1;
    }

    return 0;
}

static int
perf_map_path(char* path, size_t path_size)
{
    const char* override = getenv("PEAK_JIT_MAP_PATH");
    if (override != NULL && override[0] != '\0') {
        int written = snprintf(path, path_size, "%s", override);
        if (written < 0 || (size_t)written >= path_size) {
            return -1;
        }
        return 0;
    }

    int written = snprintf(path,
                           path_size,
                           "/tmp/perf-%ld.map",
                           (long)getpid());
    if (written < 0 || (size_t)written >= path_size) {
        return -1;
    }
    return 0;
}

static int
write_perf_map_row(void* code, size_t code_size, const char* symbol_name)
{
    char path[256];
    FILE* fp;

    if (perf_map_path(path, sizeof(path)) != 0) {
        fprintf(stderr, "failed to build perf-map path\n");
        return -1;
    }

    fp = fopen(path, "a");
    if (fp == NULL) {
        fprintf(stderr,
                "failed to open perf-map '%s': %s\n",
                path,
                strerror(errno));
        return -1;
    }

    fprintf(fp,
            "%" PRIxPTR " %zx %s\n",
            (uintptr_t)code,
            code_size,
            symbol_name);
    if (fclose(fp) != 0) {
        fprintf(stderr,
                "failed to close perf-map '%s': %s\n",
                path,
                strerror(errno));
        return -1;
    }

    return 0;
}

static int
write_malformed_perf_map_row(void* code)
{
    char path[256];
    FILE* fp;

    if (perf_map_path(path, sizeof(path)) != 0) {
        fprintf(stderr, "failed to build perf-map path\n");
        return -1;
    }

    fp = fopen(path, "a");
    if (fp == NULL) {
        fprintf(stderr,
                "failed to open perf-map '%s': %s\n",
                path,
                strerror(errno));
        return -1;
    }

    fprintf(fp, "%" PRIxPTR " 10%s\n", (uintptr_t)code, PEAK_JIT_SYMBOL);
    if (fclose(fp) != 0) {
        fprintf(stderr,
                "failed to close perf-map '%s': %s\n",
                path,
                strerror(errno));
        return -1;
    }

    return 0;
}

static int
write_overlong_perf_map_row(void* code, size_t code_size)
{
    char path[256];
    FILE* fp;

    if (perf_map_path(path, sizeof(path)) != 0) {
        fprintf(stderr, "failed to build perf-map path\n");
        return -1;
    }

    fp = fopen(path, "a");
    if (fp == NULL) {
        fprintf(stderr,
                "failed to open perf-map '%s': %s\n",
                path,
                strerror(errno));
        return -1;
    }

    fprintf(fp, "%" PRIxPTR " %zx ", (uintptr_t)code, code_size);
    for (size_t i = 0; i < 5000; i++) {
        fputc('x', fp);
    }
    fputc('\n', fp);
    if (fclose(fp) != 0) {
        fprintf(stderr,
                "failed to close perf-map '%s': %s\n",
                path,
                strerror(errno));
        return -1;
    }

    return 0;
}

static int
write_partial_perf_map_prefix(void* code, size_t code_size)
{
    char path[256];
    FILE* fp;

    if (perf_map_path(path, sizeof(path)) != 0) {
        fprintf(stderr, "failed to build perf-map path\n");
        return -1;
    }

    fp = fopen(path, "a");
    if (fp == NULL) {
        fprintf(stderr,
                "failed to open perf-map '%s': %s\n",
                path,
                strerror(errno));
        return -1;
    }

    fprintf(fp, "%" PRIxPTR " %zx ", (uintptr_t)code, code_size);
    if (fclose(fp) != 0) {
        fprintf(stderr,
                "failed to close partial perf-map '%s': %s\n",
                path,
                strerror(errno));
        return -1;
    }

    return 0;
}

static int
complete_partial_perf_map_row(const char* symbol_name)
{
    char path[256];
    FILE* fp;

    if (perf_map_path(path, sizeof(path)) != 0) {
        fprintf(stderr, "failed to build perf-map path\n");
        return -1;
    }

    fp = fopen(path, "a");
    if (fp == NULL) {
        fprintf(stderr,
                "failed to reopen perf-map '%s': %s\n",
                path,
                strerror(errno));
        return -1;
    }

    fprintf(fp, "%s\n", symbol_name);
    if (fclose(fp) != 0) {
        fprintf(stderr,
                "failed to close completed perf-map '%s': %s\n",
                path,
                strerror(errno));
        return -1;
    }

    return 0;
}

static void
unlink_stale_perf_map(void)
{
    char path[256];

    if (perf_map_path(path, sizeof(path)) == 0) {
        unlink(path);
    }
}

static int
make_jit_code_executable(void* code, size_t code_size)
{
    long page_size = sysconf(_SC_PAGESIZE);

    if (page_size <= 0) {
        fprintf(stderr, "unsupported runtime: sysconf(_SC_PAGESIZE) failed\n");
        return PEAK_JIT_SKIP;
    }

    __builtin___clear_cache((char*)code, (char*)code + code_size);

    if (mprotect(code, (size_t)page_size, PROT_READ | PROT_EXEC) != 0) {
        fprintf(stderr,
                "unsupported runtime: mprotect RX failed: %s\n",
                strerror(errno));
        return PEAK_JIT_SKIP;
    }

    return 0;
}

static int
allocate_jit_code_with_mode(void** code, size_t* code_size, int executable)
{
    long page_size = sysconf(_SC_PAGESIZE);
    size_t map_size;
    unsigned char* mapping;
    size_t emitted_size;

    if (page_size <= 0) {
        fprintf(stderr, "unsupported runtime: sysconf(_SC_PAGESIZE) failed\n");
        return PEAK_JIT_SKIP;
    }

    map_size = (size_t)page_size;
    mapping = mmap(NULL,
                   map_size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr,
                "unsupported runtime: mmap RW failed: %s\n",
                strerror(errno));
        return PEAK_JIT_SKIP;
    }

    emitted_size = copy_jit_code(mapping, map_size);
    if (emitted_size == 0) {
        munmap(mapping, map_size);
        fprintf(stderr, "unsupported runtime: no JIT code template\n");
        return PEAK_JIT_SKIP;
    }

    __builtin___clear_cache((char*)mapping, (char*)mapping + emitted_size);

    if (executable) {
        int rc = make_jit_code_executable(mapping, emitted_size);
        if (rc != 0) {
            munmap(mapping, map_size);
            return rc;
        }
    }

    *code = mapping;
    *code_size = emitted_size;
    return 0;
}

static int
allocate_jit_code(void** code, size_t* code_size)
{
    return allocate_jit_code_with_mode(code, code_size, 1);
}

static const char*
mode_name(PeakJitMode mode)
{
    switch (mode) {
        case PEAK_JIT_WITH_PERF_MAP:
            return "with-perf-map";
        case PEAK_JIT_WITH_PARTIAL_PERF_MAP:
            return "with-partial-perf-map";
        case PEAK_JIT_WITH_PRE_EXEC_PERF_MAP:
            return "with-pre-exec-perf-map";
        case PEAK_JIT_WITH_TWO_GENERATIONS:
            return "with-two-generations";
        case PEAK_JIT_WITH_STALE_THEN_VALID:
            return "with-stale-then-valid";
        case PEAK_JIT_WITH_DUPLICATE_PERF_MAP:
            return "with-duplicate-perf-map";
        case PEAK_JIT_WITH_MALFORMED_THEN_VALID:
            return "with-malformed-then-valid";
        case PEAK_JIT_WITH_OVERLONG_THEN_VALID:
            return "with-overlong-then-valid";
        case PEAK_JIT_WITHOUT_METADATA:
        default:
            return "without-metadata";
    }
}

int
main(int argc, char** argv)
{
    PeakJitMode mode = PEAK_JIT_WITH_PERF_MAP;
    unsigned long iterations;
    void* code = NULL;
    size_t code_size = 0;
    volatile unsigned long long total = 0;
    volatile PeakJitFn hot_fn;
    unsigned long metadata_sleep_us;
    const char* symbol_name;
    int rc;
    int expected;

    if (!host_has_supported_code_template()) {
        fprintf(stderr,
                "unsupported runtime: fixture requires Linux x86_64 or aarch64\n");
        return PEAK_JIT_SKIP;
    }

    if (parse_args(argc,
                   argv,
                   &mode,
                   &iterations,
                   &metadata_sleep_us,
                   &symbol_name) != 0) {
        print_usage(argv[0]);
        return 2;
    }

    unlink_stale_perf_map();

    rc = (mode == PEAK_JIT_WITH_PRE_EXEC_PERF_MAP ||
          mode == PEAK_JIT_WITH_STALE_THEN_VALID) ?
             allocate_jit_code_with_mode(&code, &code_size, 0) :
             allocate_jit_code(&code, &code_size);
    if (rc != 0) {
        return rc;
    }

    if ((mode == PEAK_JIT_WITH_PERF_MAP ||
         mode == PEAK_JIT_WITH_PRE_EXEC_PERF_MAP ||
         mode == PEAK_JIT_WITH_TWO_GENERATIONS ||
         mode == PEAK_JIT_WITH_STALE_THEN_VALID ||
         mode == PEAK_JIT_WITH_DUPLICATE_PERF_MAP) &&
        write_perf_map_row(code, code_size, symbol_name) != 0) {
        munmap(code, (size_t)sysconf(_SC_PAGESIZE));
        return PEAK_JIT_SKIP;
    }
    if (mode == PEAK_JIT_WITH_DUPLICATE_PERF_MAP &&
        write_perf_map_row(code, code_size, symbol_name) != 0) {
        munmap(code, (size_t)sysconf(_SC_PAGESIZE));
        return PEAK_JIT_SKIP;
    }
    if (mode == PEAK_JIT_WITH_MALFORMED_THEN_VALID &&
        write_malformed_perf_map_row(code) != 0) {
        munmap(code, (size_t)sysconf(_SC_PAGESIZE));
        return PEAK_JIT_SKIP;
    }
    if (mode == PEAK_JIT_WITH_OVERLONG_THEN_VALID &&
        write_overlong_perf_map_row(code, code_size) != 0) {
        munmap(code, (size_t)sysconf(_SC_PAGESIZE));
        return PEAK_JIT_SKIP;
    }
    if (mode == PEAK_JIT_WITH_MALFORMED_THEN_VALID &&
        write_perf_map_row(code, code_size, symbol_name) != 0) {
        munmap(code, (size_t)sysconf(_SC_PAGESIZE));
        return PEAK_JIT_SKIP;
    }
    if (mode == PEAK_JIT_WITH_OVERLONG_THEN_VALID &&
        write_perf_map_row(code, code_size, symbol_name) != 0) {
        munmap(code, (size_t)sysconf(_SC_PAGESIZE));
        return PEAK_JIT_SKIP;
    }
    if (mode == PEAK_JIT_WITH_PARTIAL_PERF_MAP &&
        write_partial_perf_map_prefix(code, code_size) != 0) {
        munmap(code, (size_t)sysconf(_SC_PAGESIZE));
        return PEAK_JIT_SKIP;
    }
    if (mode == PEAK_JIT_WITH_PRE_EXEC_PERF_MAP) {
        if (metadata_sleep_us > 0) {
            usleep(metadata_sleep_us);
        }
        rc = make_jit_code_executable(code, code_size);
        if (rc != 0) {
            munmap(code, (size_t)sysconf(_SC_PAGESIZE));
            return rc;
        }
        if (metadata_sleep_us > 0) {
            usleep(metadata_sleep_us);
        }
    }
    if (mode == PEAK_JIT_WITH_STALE_THEN_VALID) {
        void* second_code = NULL;
        size_t second_code_size = 0;
        if (metadata_sleep_us > 0) {
            usleep(metadata_sleep_us);
        }
        rc = allocate_jit_code(&second_code, &second_code_size);
        if (rc != 0) {
            munmap(code, (size_t)sysconf(_SC_PAGESIZE));
            return rc;
        }
        if (write_perf_map_row(second_code, second_code_size, symbol_name) != 0) {
            munmap(second_code, (size_t)sysconf(_SC_PAGESIZE));
            munmap(code, (size_t)sysconf(_SC_PAGESIZE));
            return PEAK_JIT_SKIP;
        }
        code = second_code;
        code_size = second_code_size;
    }
    if (mode == PEAK_JIT_WITH_TWO_GENERATIONS) {
        void* second_code = NULL;
        size_t second_code_size = 0;
        if (metadata_sleep_us > 0) {
            usleep(metadata_sleep_us);
        }
        rc = allocate_jit_code(&second_code, &second_code_size);
        if (rc != 0) {
            munmap(code, (size_t)sysconf(_SC_PAGESIZE));
            return rc;
        }
        if (write_perf_map_row(second_code, second_code_size, symbol_name) != 0) {
            munmap(second_code, (size_t)sysconf(_SC_PAGESIZE));
            munmap(code, (size_t)sysconf(_SC_PAGESIZE));
            return PEAK_JIT_SKIP;
        }
        code = second_code;
        code_size = second_code_size;
    }
    if ((mode == PEAK_JIT_WITH_PERF_MAP ||
         mode == PEAK_JIT_WITH_TWO_GENERATIONS ||
         mode == PEAK_JIT_WITH_STALE_THEN_VALID ||
         mode == PEAK_JIT_WITH_DUPLICATE_PERF_MAP ||
         mode == PEAK_JIT_WITH_MALFORMED_THEN_VALID ||
         mode == PEAK_JIT_WITH_OVERLONG_THEN_VALID) &&
        metadata_sleep_us > 0) {
        usleep(metadata_sleep_us);
    }
    if (mode == PEAK_JIT_WITH_PARTIAL_PERF_MAP) {
        if (metadata_sleep_us > 0) {
            usleep(metadata_sleep_us);
        }
        if (complete_partial_perf_map_row(symbol_name) != 0) {
            munmap(code, (size_t)sysconf(_SC_PAGESIZE));
            return PEAK_JIT_SKIP;
        }
        if (metadata_sleep_us > 0) {
            usleep(metadata_sleep_us);
        }
    }

    hot_fn = (PeakJitFn)code;
    for (unsigned long i = 0; i < iterations; i++) {
        total += hot_fn((int)(i & 1023UL));
    }

    expected = hot_fn(41);
    printf("peak_jit_fixture_ok mode=%s pid=%ld symbol=%s calls=%lu "
           "result=%d checksum=%llu code=%p size=%zu\n",
           mode_name(mode),
           (long)getpid(),
           symbol_name,
           iterations + 1,
           expected,
           (unsigned long long)total,
           code,
           code_size);

    if (mode == PEAK_JIT_WITHOUT_METADATA) {
        munmap(code, (size_t)sysconf(_SC_PAGESIZE));
    }
    return expected == 42 ? 0 : 1;
}
