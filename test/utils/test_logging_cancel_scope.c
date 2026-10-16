#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal/cancel_scope.h"
static int fail_buffer, writer_calls, writer_errno;
static int observed_setvbuf(FILE*f,char*b,int mode,size_t n)
{
    if(fail_buffer){errno=ENOMEM;return -1;}
    return setvbuf(f,b,mode,n);
}
static int observed_vfprintf(FILE*f,const char*format,va_list args)
{
    int result=vfprintf(f,format,args);
    writer_errno=errno;writer_calls++;
    return result;
}
/* Compile the real maintained logging implementation in this pure unit;
 * wrappers observe errno and force only the buffering-failure branch. */
#define setvbuf observed_setvbuf
#define vfprintf observed_vfprintf
#include "../../src/logging.c"
#undef setvbuf
#undef vfprintf
int main(int argc,char**argv)
{
    assert(argc==2);const char*mode=argv[1];
    int fallback=!strcmp(mode,"fallback"),filtered=!strcmp(mode,"filtered");
    int disabled=!strcmp(mode,"disabled"),empty=!strcmp(mode,"empty");
    fail_buffer=!strcmp(mode,"setvbuf-failed");
    assert(!setenv("PEAK_VERBOSITY",filtered?"report":"warn",1));
    if(!fallback)peak_log_initialize_output(disabled);
    PeakCancelScope outer,inner;peak_cancel_scope_enter(&outer);peak_cancel_scope_enter(&inner);
    errno=EDOM;peak_log_warn("%s",empty?"":"logging-scope-test\n");
    if(writer_calls)assert(errno==writer_errno);
    int expected=0;
#if defined(__linux__) && defined(__GLIBC__)
    expected=!fallback&&!filtered&&!disabled&&!empty&&!fail_buffer;
#endif
    assert(peak_cancel_scope_leave(&inner)==expected);
    assert(peak_cancel_scope_leave(&outer)==expected);
    assert(peak_cancel_scope_current==NULL);
    assert(writer_calls==(!filtered&&!disabled));
    errno=E2BIG;peak_cancel_scope_mark();assert(errno==E2BIG);
    /* Flush/close outside the observed scope, including buffered fallback. */
    peak_log_shutdown();
    printf("logging_cancel_scope_ok mode=%s seen=%d calls=%d\n",mode,expected,writer_calls);
    return 0;
}
