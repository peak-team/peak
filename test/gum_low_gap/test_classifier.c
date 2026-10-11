#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <frida-gum.h>
#include <sys/syscall.h>
#include "internal/exec_raw_syscall.h"

/* Limit regular-file reads to exercise short-read exhaustion independently
 * of the byte, line-length, and line-count guards. */
static int maps_test_fd = -1;
static size_t maps_test_chunk;
static unsigned maps_test_reads;
static long test_raw_syscall(long nr, long a1, long a2, long a3,
                             long a4, long a5, long a6) {
    if (nr == SYS_read && a1 == maps_test_fd) {
        maps_test_reads++;
        if ((size_t)a3 > maps_test_chunk) a3 = (long)maps_test_chunk;
    }
    return peak_exec_raw_syscall6(nr, a1, a2, a3, a4, a5, a6);
}

static unsigned long fallback_calls;
static gpointer expected_target;
static int expected_errno;
static char sentinel;
static char main_data[8192] __attribute__((aligned(4096))) = {1};
guint gum_query_page_size(void) { return (guint)sysconf(_SC_PAGESIZE); }
gpointer gum_try_alloc_n_pages_near(guint n, GumPageProtection p,
                                    const GumAddressSpec *s) {
    assert(n == 1 && p == GUM_PAGE_RX && s && s->near_address == expected_target);
    assert(errno == expected_errno);
    fallback_calls++;
    return &sentinel;
}
gpointer gum_memory_allocate(gpointer hint, gsize size, gsize alignment,
                            GumPageProtection prot) {
    assert(alignment == gum_query_page_size() && prot == GUM_PAGE_RW);
    void *p = mmap(hint, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
gboolean gum_memory_free(gpointer p, gsize size) { return munmap(p, size) == 0; }
static int missing_phdr;
static __attribute__((unused)) unsigned long test_getauxval(unsigned long type) {
    return missing_phdr && type == AT_PHDR ? 0 : getauxval(type);
}
#define getauxval test_getauxval
#define peak_exec_raw_syscall6 test_raw_syscall
#include OVERLAY_SOURCE
#undef peak_exec_raw_syscall6
#undef getauxval

static void check_maps_limit(size_t bytes, size_t chunk, int expected,
                             unsigned reads) {
    FILE *f = tmpfile(); assert(f);
    /* Sixty-four sorted mappings; padding is a pathname, not more VMAs. */
    for (unsigned i=0; i<64; i++) {
        char line[4096];
        size_t length = bytes / 64 + (i < bytes % 64);
        int prefix = snprintf(line, sizeof(line),
            "%08x-%08x r-xp 00000000 00:00 0 /",
            0x400000+i*4096, 0x401000+i*4096);
        assert(prefix > 0 && (size_t)prefix < length && length < sizeof(line));
        memset(line+prefix, 'x', length-(size_t)prefix-1);
        line[length-1] = '\n';
        assert(fwrite(line, 1, length, f) == length);
    }
    assert(fflush(f)==0 && fseek(f,0,SEEK_SET)==0);
    maps_test_fd = fileno(f);
    maps_test_chunk = chunk;
    maps_test_reads = 0;
    PeakNearSnapshot snapshot = {.target=0x400000, .first=UINTPTR_MAX,
                                .page_size=4096};
    assert(peak_near_read_maps(maps_test_fd, &snapshot)==expected);
    assert(maps_test_reads==reads);
    if (expected) assert(snapshot.target_matches==1 && snapshot.main_matches==1);
    printf("maps-limit bytes=%zu chunk=%zu accepted=%d reads=%u\n",
           bytes, chunk, expected, maps_test_reads);
    maps_test_fd = -1;
    assert(fclose(f)==0);
}

static double elapsed(struct timespec a, struct timespec b) {
    return (b.tv_sec-a.tv_sec) + (b.tv_nsec-a.tv_nsec)*1e-9;
}
static void *call(void *target) {
    expected_target = target;
    expected_errno = EDOM;
    errno = EDOM;
    GumAddressSpec s = { target, INT32_MAX };
    void *p = peak_gum_try_alloc_n_pages_near_main_low_gap(1, GUM_PAGE_RX, &s);
    assert(errno == EDOM);
    return p;
}
static void check_permissions(void *address, const char *expected) {
    FILE *f=fopen("/proc/self/maps","r"); assert(f);
    char line[4096], permissions[5]; unsigned long low,high; int found=0;
    while(fgets(line,sizeof(line),f)) {
        if(sscanf(line,"%lx-%lx %4s",&low,&high,permissions)==3 &&
           low <= (uintptr_t)address && (uintptr_t)address < high) {
            assert(strcmp(permissions,expected)==0); found=1; break;
        }
    }
    fclose(f); assert(found);
}
static long maps_size(void) {
    FILE *f = fopen("/proc/self/maps", "r"); assert(f);
    long count = 0; while (fgetc(f) != EOF) count++; fclose(f); return count;
}
static void benchmark(const char *name, void *target) {
    struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a);
    unsigned long before=fallback_calls;
    for (int i=0;i<2000;i++) (void)call(target);
    clock_gettime(CLOCK_MONOTONIC,&b);
    printf("%s maps_bytes=%ld ns_per_call=%.1f fallback_calls=%lu\n",
           name,maps_size(),elapsed(a,b)*1e9/2000,fallback_calls-before);
}
int main(void) {
#ifdef EXPECT_OLD
    check_maps_limit(65536, 4096, 1, 17);
    check_maps_limit(65537, 4096, 0, 17);
#else
    check_maps_limit(258048, 4096, 1, 64); /* Full reads, then confirmed EOF. */
    check_maps_limit(258049, 4096, 0, 64); /* Byte cap, only 64 valid lines. */
#endif
    check_maps_limit(64512, 1024, 1, 64); /* Short reads, EOF on read 64. */
    check_maps_limit(64513, 1024, 0, 64); /* Short reads, EOF needs read 65. */
    void *shared = dlsym(RTLD_DEFAULT,"getpid"); assert(shared);
    int pie = 0;
#if defined(__PIE__)
    pie = 1;
#endif
    assert(call(shared) == &sentinel);
    benchmark("shared-small",shared);
    /* Missing auxiliary metadata must retain the previous classifier, not
     * become an unconditional fallback or a newly unconditional rejection. */
    missing_phdr=1;
    assert(call(shared)==&sentinel);
    missing_phdr=0;
    if (!pie) {
        void *code = call((void *)&main); assert(code && code != &sentinel);
        size_t page=gum_query_page_size();
        size_t size=*((gsize *)((char *)code-page)); assert(size==2*page);
        /* Metadata read-only, code RX; allocation must stay below main image. */
        assert((uintptr_t)code < (uintptr_t)&main);
        check_permissions(code,"r-xp");
        check_permissions((char *)code-page,"r--p");
        assert(gum_memory_free((char *)code-page,size));
        /* A main-image data page may become executable after startup. It
         * still needs the protected classifier instead of shared fallback. */
        assert(mprotect(main_data,page,PROT_READ|PROT_EXEC)==0);
        code=call(main_data); assert(code && code!=&sentinel);
        size=*((gsize *)((char *)code-page));
        assert(gum_memory_free((char *)code-page,size));
        assert(mprotect(main_data,page,PROT_READ|PROT_WRITE)==0);
    } else { assert(call((void *)&main)==&sentinel); }
    size_t page=gum_query_page_size(), mid_size=1536*page;
    char *mid=mmap(NULL,mid_size,PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(mid!=MAP_FAILED);
    for(size_t i=0;i<1536;i+=2) assert(mprotect(mid+i*page,page,PROT_READ)==0);
    long mid_maps_bytes=maps_size();
    assert(mid_maps_bytes>65536 && mid_maps_bytes<4096*63);
    unsigned long before=fallback_calls;
    if(!pie) {
        before=fallback_calls;
        void *code=call((void *)&main);
#ifdef EXPECT_OLD
        assert(code==NULL);
#else
        assert(code && code!=&sentinel);
        assert(gum_memory_free((char *)code-page,*((gsize *)((char *)code-page))));
#endif
        assert(fallback_calls==before);
    }
    printf("main-mid maps_bytes=%ld fallback_calls=%lu\n",
           mid_maps_bytes,fallback_calls);
    assert(munmap(mid,mid_size)==0);
    size_t size=6144*page;
    char *many=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(many!=MAP_FAILED);
    for(size_t i=0;i<6144;i+=2) assert(mprotect(many+i*page,page,PROT_READ)==0);
    assert(maps_size()>4096*63);
    before=fallback_calls;
    void *result=call(shared);
    assert(result==&sentinel && fallback_calls==before+1);
    printf("shared-large maps_bytes=%ld fallback_calls=%lu\n",
           maps_size(),fallback_calls-before);
    missing_phdr=1;
    if(!pie) { before=fallback_calls; assert(call(shared)==NULL); assert(fallback_calls==before); }
    else { assert(call(shared)==&sentinel); }
    missing_phdr=0;
    if(!pie) { before=fallback_calls; assert(call((void *)&main)==NULL); assert(fallback_calls==before); }
    assert(munmap(many,size)==0);
    assert(call(shared)==&sentinel);
    puts("classifier assertions passed");
}
