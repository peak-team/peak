"""Fault-inject reuse between release and free publication.

This is a metadata protocol harness, not a general async-signal-safety claim.
The real functions are extracted without copying their implementation; only
CFI/cache/lease dependencies are stubbed. The integration test covers actual
Gum dispatch, unwind and thread lifetimes separately.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('--source',type=Path,required=True)
parser.add_argument('--cc',required=True)
args=parser.parse_args()
new=args.source.read_text()
temporary=tempfile.TemporaryDirectory(prefix='peak-frame-boundary-')
out=Path(temporary.name)
def fn(s,name):
 start=s.index('static void '+name+'(')
 start=s.index('static void '+name+'(',start+1) if s[start:s.index('{',start)].endswith(';\n') else start
 # Forward declarations are skipped explicitly.
 while ';' in s[start:s.index('{',start)]:start=s.index('static void '+name+'(',start+1)
 end=s.index('{',start);depth=1;end+=1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
models={'candidate':new,'late-clear-flag':new}
results=[]
for name,s in models.items():
 structs=s[s.index('typedef struct PeakUwFrame'):s.index('/* These existing fields')]
 enter=fn(s,'peak_uw_enter');release=fn(s,'peak_uw_release')
 release=release.replace('  peak_uw_thread->free = f;','  peak_uw_thread->free = f;\n  boundary_hook();')
 if name=='late-clear-flag':
  release=release.replace('  __atomic_store_n(&f->diagnostic_active, 0, __ATOMIC_RELAXED);','')
  release=release.replace('  boundary_hook();','  boundary_hook();\n  __atomic_store_n(&f->diagnostic_active, 0, __ATOMIC_RELAXED);')
 count='__atomic_load_n(&frame.diagnostic_active,__ATOMIC_RELAXED)'
 source='''#define _GNU_SOURCE
#define PEAK_UW_SLOTS 32
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/types.h>
#include <signal.h>
#include <setjmp.h>
typedef void* gpointer;typedef unsigned guint;typedef int gboolean;typedef int gint;typedef uint64_t guint64;typedef uint8_t guint8;typedef size_t gsize;
typedef struct {int trampoline_usage_counter;void* on_leave_trampoline;void* function_address;} PeakGumFunctionContext17;
typedef struct {} PeakGumInterceptorThreadContext17;
#define g_atomic_int_inc(p) __atomic_add_fetch(p,1,__ATOMIC_SEQ_CST)
#define g_atomic_int_dec_and_test(p) (__atomic_sub_fetch(p,1,__ATOMIC_SEQ_CST)==0)
'''+structs+'''
static _Thread_local PeakUwThread* peak_uw_thread;
static void peak_uw_cache_clear(PeakUwThread* o) {o->cfi_cache=NULL;}
static void boundary_hook(void);
'''+enter+'\n'+release+'''
static PeakUwThread owner;static PeakUwFrame frame;static PeakGumFunctionContext17 context;
static void* slot;static unsigned char pc[16];static sigjmp_buf jump;
static int nested,nonlocal;
static void handler(int sig) {
 (void)sig;
 if (!sigsetjmp(jump,1)) {
   PeakUwFrame* f=owner.free;owner.free=f->free_next;
   peak_uw_enter(f,&context,&slot,NULL,1,0);
   if (nonlocal) siglongjmp(jump,1);
   peak_uw_release(&owner.active);
 }
}
static void boundary_hook(void) {if(!nested){nested=1;raise(SIGUSR1);}}
int main(int argc,char**argv) {
 (void)argv;nonlocal=argc>1;peak_uw_thread=&owner;frame.pc=pc;
 struct sigaction sa={};sa.sa_handler=handler;sigemptyset(&sa.sa_mask);sigaction(SIGUSR1,&sa,NULL);
 peak_uw_enter(&frame,&context,&slot,NULL,1,0);owner.cfi_cache=(void*)1;
 peak_uw_release(&owner.active);
 unsigned count='''+count+''';
 unsigned expected=nonlocal?1:0;
 int ok=count==expected && (owner.active!=NULL)==!!expected && (owner.cfi_cache!=NULL)==!!expected;
 printf("count=%u expected=%u active=%d cache=%d frame_size=%zu next_offset=%zu pass=%d\\n",count,expected,owner.active!=NULL,owner.cfi_cache!=NULL,sizeof(frame),offsetof(PeakUwFrame,next),ok);
 return ok?0:1;
}
'''
 path=out/(name+'.c');path.write_text(source)
 subprocess.run([args.cc,'-O2','-std=gnu11',str(path),'-pthread','-o',str(out/name)],check=True)
 for case in ('balanced','nonlocal'):
  r=subprocess.run([str(out/name)]+(['nonlocal'] if case=='nonlocal' else []),capture_output=True,text=True,timeout=2)
  results.append(dict(model=name,case=case,rc=r.returncode,stdout=r.stdout,stderr=r.stderr))
  print(name,case,r.returncode,r.stdout.strip())
  assert r.returncode==(1 if case=='nonlocal' and name=='late-clear-flag' else 0)
temporary.cleanup()
