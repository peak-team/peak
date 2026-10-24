#define _GNU_SOURCE
#include "frida-gum.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#if defined(__aarch64__)
extern gboolean gum_arm64_relocator_can_relocate(gpointer, guint,
    GumRelocationScenario, GumRelocationPolicy, guint*, arm64_reg*);
#else
extern gboolean gum_x86_relocator_can_relocate(gpointer, guint,
    GumRelocationScenario, guint*);
#endif

#define WARM_QUERIES 1000u
#define TRANSACTION_TARGETS 64u

static unsigned long callbacks;

static uint64_t
now_ns(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + value.tv_nsec;
}

static void
entered(GumInvocationContext* context, gpointer data)
{
    (void)context;
    (void)data;
    callbacks++;
}

static unsigned char*
allocate_code(size_t bytes, size_t* mapped_size)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    *mapped_size = (bytes + page - 1) / page * page;
    unsigned char* code = mmap(NULL, *mapped_size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED)
        return NULL;
#if defined(__aarch64__)
    for (size_t offset = 0; offset < *mapped_size; offset += 4)
        *(uint32_t*)(code + offset) = UINT32_C(0xd503201f); /* nop */
#else
    memset(code, 0x90, *mapped_size);
#endif
    return code;
}

static void
write_return_one(unsigned char* code)
{
#if defined(__aarch64__)
    ((uint32_t*)code)[0] = UINT32_C(0xd2800020); /* mov x0, #1 */
    ((uint32_t*)code)[1] = UINT32_C(0xd65f03c0); /* ret */
#else
    static const unsigned char body[] = { 0xb8, 1, 0, 0, 0, 0xc3 };
    memcpy(code, body, sizeof(body));
#endif
}

static int
publish_code(unsigned char* code, size_t size)
{
    __builtin___clear_cache((char*)code, (char*)code + size);
    return mprotect(code, size, PROT_READ | PROT_EXEC) == 0;
}

static gboolean
classify(gpointer address, guint minimum, guint* maximum)
{
#if defined(__aarch64__)
    arm64_reg scratch = ARM64_REG_INVALID;
    return gum_arm64_relocator_can_relocate(address, minimum,
        GUM_SCENARIO_ONLINE, GUM_RELOCATION_CHECKED, maximum, &scratch);
#else
    return gum_x86_relocator_can_relocate(address, minimum,
                                         GUM_SCENARIO_ONLINE, maximum);
#endif
}

static int
detach_and_flush(GumInterceptor* interceptor, GumInvocationListener* listener)
{
    gum_interceptor_begin_transaction(interceptor);
    gum_interceptor_detach(interceptor, listener);
    gum_interceptor_end_transaction(interceptor);
    uint64_t deadline = now_ns() + UINT64_C(2000000000);
    do {
        if (gum_interceptor_flush(interceptor))
            return 1;
        usleep(1000);
    } while (now_ns() < deadline);
    fputs("entry attach benchmark: detach did not flush\n", stderr);
    return 0;
}

static int
linear_case(GumInterceptor* interceptor, GumInvocationListener* listener,
            size_t instructions)
{
#if defined(__aarch64__)
    const guint minimums[] = { 4, 8, 16 };
    size_t bytes = instructions * 4, tail = (instructions - 2) * 4;
#else
    const guint minimums[] = { 5, 16 };
    size_t bytes = instructions + 4, tail = instructions - 2;
#endif
    size_t mapped;
    unsigned char* code = allocate_code(bytes, &mapped);
    if (code == NULL)
        return 0;
    write_return_one(code + tail);
    int ok = publish_code(code, mapped);
    if (!ok) {
        munmap(code, mapped);
        return 0;
    }
    for (size_t test = 0; test < sizeof(minimums) / sizeof(minimums[0]); test++) {
        guint maximum = 0;
        uint64_t begin = now_ns();
        gboolean cold_ok = classify(code, minimums[test], &maximum);
        uint64_t cold = now_ns() - begin;
        ok &= cold_ok && maximum >= minimums[test];
        begin = now_ns();
        for (unsigned int query = 0; query < WARM_QUERIES; query++) {
            gboolean accepted = classify(code, minimums[test], &maximum);
            ok &= accepted && maximum >= minimums[test];
        }
        uint64_t warm = now_ns() - begin;
        printf("{\"mode\":\"classify\",\"instructions\":%zu,\"bytes\":%zu,"
               "\"minimum\":%u,\"maximum\":%u,\"cold_ns\":%" PRIu64 ","
               "\"warm_queries\":%u,\"warm_total_ns\":%" PRIu64 ","
               "\"warm_ns_per_query\":%.2f}\n", instructions, bytes,
               minimums[test], maximum, cold, WARM_QUERIES, warm,
               (double)warm / WARM_QUERIES);
    }
    callbacks = 0;
    GumAttachOptions options = {
        .instrumentation = { .relocation_policy = GUM_RELOCATION_CHECKED }
    };
    uint64_t begin = now_ns();
    GumAttachReturn status = gum_interceptor_attach(interceptor, code, listener,
                                                    &options);
    uint64_t elapsed = now_ns() - begin;
    int value = ((int (*)(void))code)();
    ok &= status == GUM_ATTACH_OK && value == 1 && callbacks == 1;
    printf("{\"mode\":\"linear_attach\",\"instructions\":%zu,"
           "\"attach_ns\":%" PRIu64 ",\"status\":%d,\"value\":%d,"
           "\"callbacks\":%lu}\n", instructions, elapsed, status, value, callbacks);
    if (!detach_and_flush(interceptor, listener))
        return 0; /* Keep executable storage alive if Gum still refers to it. */
    munmap(code, mapped);
    return ok;
}

static int
batch_case(GumInterceptor* interceptor, GumInvocationListener* listener,
           size_t targets)
{
    size_t mapped;
    unsigned char* code = allocate_code(targets * 16, &mapped);
    if (code == NULL)
        return 0;
    for (size_t target = 0; target < targets; target++)
        write_return_one(code + target * 16);
    if (!publish_code(code, mapped)) {
        munmap(code, mapped);
        return 0;
    }
    GumAttachOptions options = {
        .instrumentation = { .relocation_policy = GUM_RELOCATION_CHECKED }
    };
    size_t attached = 0, segments = 0, first_count = 0, last_count = 0;
    uint64_t first = 0, last = 0, begin = now_ns();
    for (size_t start = 0; start < targets; start += TRANSACTION_TARGETS) {
        size_t count = targets - start;
        if (count > TRANSACTION_TARGETS)
            count = TRANSACTION_TARGETS;
        uint64_t segment_begin = now_ns();
        gum_interceptor_begin_transaction(interceptor);
        for (size_t target = start; target < start + count; target++) {
            if (gum_interceptor_attach(interceptor, code + target * 16,
                                       listener, &options) == GUM_ATTACH_OK)
                attached++;
        }
        gum_interceptor_end_transaction(interceptor);
        last = now_ns() - segment_begin;
        last_count = count;
        if (segments++ == 0) {
            first = last;
            first_count = count;
        }
    }
    uint64_t elapsed = now_ns() - begin;
    callbacks = 0;
    size_t values = 0;
    for (size_t target = 0; target < targets; target++)
        values += ((int (*)(void))(code + target * 16))() == 1;
    int ok = attached == targets && values == targets && callbacks == targets;
    printf("{\"mode\":\"batch_attach\",\"targets\":%zu,\"transaction_size\":%u,"
           "\"segments\":%zu,\"attach_ns\":%" PRIu64 ",\"ns_per_target\":%.2f,"
           "\"first_segment_targets\":%zu,\"first_segment_ns\":%" PRIu64 ","
           "\"last_segment_targets\":%zu,\"last_segment_ns\":%" PRIu64 ","
           "\"attached\":%zu,\"values_ok\":%zu,\"callbacks\":%lu}\n",
           targets, TRANSACTION_TARGETS, segments, elapsed, (double)elapsed / targets,
           first_count, first, last_count, last, attached, values, callbacks);
    if (!detach_and_flush(interceptor, listener))
        return 0;
    munmap(code, mapped);
    return ok;
}

int
main(void)
{
    gum_init_embedded();
    GumInterceptor* interceptor = gum_interceptor_obtain();
    GumInvocationListener* listener = gum_make_call_listener(entered, NULL, NULL, NULL);
    int ok = listener != NULL;
    const size_t lengths[] = { 129, 4097, 65537 };
    for (size_t i = 0; ok && i < sizeof(lengths) / sizeof(lengths[0]); i++)
        ok = linear_case(interceptor, listener, lengths[i]);
    if (ok)
        ok = batch_case(interceptor, listener, 100);
    if (ok)
        ok = batch_case(interceptor, listener, 1000);
    if (listener != NULL)
        g_object_unref(listener);
    g_object_unref(interceptor);
    gum_deinit_embedded();
    if (!ok)
        fputs("entry attach benchmark: behavior or callback validation failed\n", stderr);
    return ok ? 0 : 1;
}
