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
#include OVERLAY_SOURCE
#undef getauxval

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
    size_t page=gum_query_page_size(), size=4096*page;
    char *many=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(many!=MAP_FAILED);
    for(size_t i=0;i<4096;i+=2) assert(mprotect(many+i*page,page,PROT_READ)==0);
    assert(maps_size()>65536);
    unsigned long before=fallback_calls;
    void *result=call(shared);
#ifdef EXPECT_OLD
    if(!pie) { assert(result==NULL && fallback_calls==before); }
    else { assert(result==&sentinel && fallback_calls==before+1); }
#else
    assert(result==&sentinel && fallback_calls==before+1);
#endif
    benchmark("shared-large",shared);
    missing_phdr=1;
    if(!pie) { before=fallback_calls; assert(call(shared)==NULL); assert(fallback_calls==before); }
    else { assert(call(shared)==&sentinel); }
    missing_phdr=0;
    if(!pie) { before=fallback_calls; assert(call((void *)&main)==NULL); assert(fallback_calls==before); }
    assert(munmap(many,size)==0);
    assert(call(shared)==&sentinel);
    puts("classifier assertions passed");
}
