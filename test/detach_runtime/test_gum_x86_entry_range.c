#include "frida-gum.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <limits.h>
#include <fcntl.h>
#include <stdlib.h>
extern gboolean gum_x86_relocator_can_relocate(gpointer,guint,GumRelocationScenario,guint*);
extern int peak_x86_unsafe(int), peak_x86_recoverable(int);
extern int peak_x86_internal_jcc(int), peak_x86_internal_jmp(int);
extern int peak_x86_mid_instruction(int), peak_x86_indirect(int);
extern int peak_x86_budget(int), peak_x86_rip_data(int);
extern int peak_x86_rip_lea(int), peak_x86_call_next(int);
extern int peak_x86_prefix_data(int),peak_x86_negative(int),peak_x86_crossing_data(int);
extern int peak_x86_rounding(int);
extern int peak_x86_indirect_call(int);
#ifdef PEAK_GUM_ENTRY_RANGE_CHECKS
extern guint gum_interceptor_peak_entry_range_revision(void);
#endif
extern int peak_x86_full_span(int),peak_x86_far_branch(int),peak_x86_syscall(int);
typedef struct _GumX86Relocator GumX86Relocator;
extern GumX86Relocator *gum_x86_relocator_new(gconstpointer,GumX86Writer*);
extern guint gum_x86_relocator_read_one(GumX86Relocator*,const cs_insn**);
extern void gum_x86_relocator_write_all(GumX86Relocator*);
extern void gum_x86_relocator_unref(GumX86Relocator*);
static int (*fast_original)(int);
static int fast_replacement(int n) { return fast_original(n)+1; }
extern int peak_x86_readonly_zero(int),peak_x86_readonly_nonzero(int);
extern int peak_x86_writable_zero(int),peak_x86_readonly_bypass(int);
extern int peak_x86_readonly_alias(int),peak_x86_readonly_call(int);
extern int peak_x86_readonly_chain(int),peak_x86_suffix_trap(int),peak_x86_prefix_trap(int);
extern int peak_x86_entry_condition(int),peak_x86_call_into_test(int),peak_x86_call_into_instruction(int);
extern int peak_x86_readonly_other_gprs(int),peak_x86_readonly_r10d(int),peak_x86_readonly_r15b(int);
extern int peak_x86_xsave_extent(int),peak_x86_fxsave_extent(int);
static guint calls;
static void enter(GumInvocationContext *c,gpointer d) { (void)c;(void)d;calls++; }
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#expr); return 1; } } while(0)
static int range(gpointer p,guint request,gboolean expected,guint maximum)
{
 guint n=999; gboolean r=gum_x86_relocator_can_relocate(p,request,GUM_SCENARIO_ONLINE,&n);
 if(r!=expected || n!=maximum) {
  fprintf(stderr,"range %p request%u: result%d max%u expected%d/%u\n",p,request,r,n,expected,maximum);return 1;
 }return 0;
}
static int refuse(GumInterceptor*i,GumInvocationListener*l,gpointer p)
{
 guint8 before[32];memcpy(before,p,sizeof before);
 CHECK(gum_interceptor_attach(i,p,l,NULL)!=GUM_ATTACH_OK);
 CHECK(memcmp(before,p,sizeof before)==0);return 0;
}
static int run(GumInterceptor*i,GumInvocationListener*l,int(*f)(int),int expected)
{
 guint generation;guint8 before[32];memcpy(before,(gpointer)f,sizeof before);
 CHECK(f(0)==expected && f(1)==expected);
 for(generation=0;generation!=2;generation++) {
  guint old=calls,length;guint8 active[32],original[32];
  CHECK(gum_interceptor_attach(i,(gpointer)f,l,NULL)==GUM_ATTACH_OK);
  CHECK(gum_interceptor_peak_get_function_patch(i,(gpointer)f,l,active,original,&length));
  CHECK(length>=5 && length<=32);
  CHECK(memcmp(original,before,length)==0);
  CHECK(memcmp((guint8*)f+length,before+length,32-length)==0);
  if(f==peak_x86_recoverable)CHECK(length==5);
  CHECK(f(0)==expected && f(1)==expected);
  CHECK(calls==old+2);
  gum_interceptor_detach(i,l);
  CHECK(memcmp(before,(gpointer)f,sizeof before)==0);
  CHECK(f(0)==expected && f(1)==expected);
 }return 0;
}
/* FAST always prepares a full16 redirect budget, even when the eventual
 * replacement address fits a short JMP. Its NOP padding covers all20 decoded
 * bytes; query that span instead of assuming an opcode width from allocation. */
static int full_span(GumInterceptor *i)
{
 guint generation, length;
 guint8 before[32],active[32],original[32];
 memcpy(before,(gpointer)peak_x86_full_span,sizeof before);
 CHECK(peak_x86_full_span(0)==50);
 for(generation=0;generation!=2;generation++) {
  CHECK(gum_interceptor_replace_fast(i,peak_x86_full_span,fast_replacement,
      (gpointer*)&fast_original,NULL)==GUM_REPLACE_OK);
  CHECK(gum_interceptor_peak_get_function_patch(i,peak_x86_full_span,NULL,
      active,original,&length));
  CHECK(length==20);
  CHECK(memcmp(original,before,length)==0);
  CHECK(memcmp(active,before,length)!=0);
  CHECK(memcmp((guint8*)peak_x86_full_span+length,before+length,32-length)==0);
  CHECK(fast_original(0)==50 && peak_x86_full_span(0)==51);
  gum_interceptor_revert(i,peak_x86_full_span);
  CHECK(memcmp(before,(gpointer)peak_x86_full_span,sizeof before)==0);
  CHECK(peak_x86_full_span(0)==50);
 }
 return 0;
}
/* Use a real distant mapping, not a fictitious writer PC: generated code is
 * executed, exercising far RIP / CALL / external-Jcc encodings and jump-back.
 * Hints are advisory and every returned mapping is checked before use. */
static int far_rewrite(int(*source)(int),int zero,int one)
{
 long page=sysconf(_SC_PAGESIZE);guint attempt,span=0;
 guintptr origin=(guintptr)source,output;
 gpointer code=MAP_FAILED;
 GumX86Writer *writer;GumX86Relocator *relocator;
 CHECK(page>0);
 for(attempt=0;attempt!=3;attempt++) {
  gpointer hint=attempt==0?NULL:(gpointer)(origin ^
      ((guintptr)1 << (attempt==1?34:40)));
  code=mmap(hint,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(code==MAP_FAILED)continue;
  output=(guintptr)code;
  if((output>origin?output-origin:origin-output)>(guintptr)INT32_MAX+page)
    break;
  CHECK(munmap(code,page)==0);code=MAP_FAILED;
 }
 CHECK(code!=MAP_FAILED);
 writer=gum_x86_writer_new(code);
 relocator=gum_x86_relocator_new((gpointer)source,writer);
 do {
  span=gum_x86_relocator_read_one(relocator,NULL);
  CHECK(span!=0);
 }while(span<16);
 gum_x86_relocator_write_all(relocator);
 CHECK(gum_x86_writer_put_jmp_address(writer,(GumAddress)source+span));
 CHECK(gum_x86_writer_flush(writer));
 CHECK(gum_x86_writer_offset(writer)<(guint)page);
 CHECK(mprotect(code,page,PROT_READ|PROT_EXEC)==0);
 CHECK(((int(*)(int))code)(0)==zero && ((int(*)(int))code)(1)==one);
 gum_x86_relocator_unref(relocator);gum_x86_writer_unref(writer);
 CHECK(munmap(code,page)==0);
 return 0;
}
/* A readable first half must not turn a partially unreadable eight-byte
 * operand into a constant. Keep this a classifier test: the original load
 * intentionally cannot execute. */
static int readonly_boundaries(void)
{
 static const guint8 instructions[]={0x48,0x8b,0x05,0,0,0,0,
   0x48,0x85,0xc0,0x74,0x02,0xff,0xe0,0xb8,43,0,0,0,0xc3};
 long page=sysconf(_SC_PAGESIZE);guint8 *mapping;gint32 displacement;
 CHECK(page>0);
 mapping=mmap(NULL,page*3,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 CHECK(mapping!=MAP_FAILED);
 memcpy(mapping,instructions,sizeof instructions);
 displacement=(gint32)(page*2-4-7);memcpy(mapping+3,&displacement,4);
 CHECK(mprotect(mapping,page,PROT_READ|PROT_EXEC)==0);
 CHECK(mprotect(mapping+page,page,PROT_READ)==0);
 CHECK(mprotect(mapping+page*2,page,PROT_NONE)==0);
 CHECK(range(mapping,5,FALSE,0)==0);
 CHECK(munmap(mapping,page*3)==0);
 /* Executable mappings are mutable by Gum and cannot certify constants. */
 mapping=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 CHECK(mapping!=MAP_FAILED);memcpy(mapping,instructions,sizeof instructions);
 displacement=64-7;memcpy(mapping+3,&displacement,4);
 CHECK(mprotect(mapping,page,PROT_READ|PROT_EXEC)==0);
 CHECK(range(mapping,5,FALSE,0)==0);
 CHECK(munmap(mapping,page)==0);
 return 0;
}
/* Establish the pinned decoder precondition rather than testing a silently
 * different opcode: these state images access >=512B but report an8B operand.
 * Never execute the state-store fixtures against their read-only destination. */
static int state_image_width(gpointer target,guint expected)
{
 csh capstone;cs_insn *instruction;size_t n;
 cs_arch_register_x86();
 CHECK(cs_open(CS_ARCH_X86,CS_MODE_64,&capstone)==CS_ERR_OK);
 CHECK(cs_option(capstone,CS_OPT_DETAIL,CS_OPT_ON)==CS_ERR_OK);
 n=cs_disasm(capstone,target,15,(guint64)target,1,&instruction);
 CHECK(n==1 && instruction->id==expected);
 CHECK(instruction->detail->x86.op_count==1);
 CHECK(instruction->detail->x86.operands[0].type==X86_OP_MEM);
 CHECK(instruction->detail->x86.operands[0].size==8);
 cs_free(instruction,n);cs_close(&capstone);
 CHECK(range(target,5,FALSE,0)==0);return 0;
}
/* RO permissions alone do not certify constants. In all three cases the
 * instruction stream sees a full readable zero8B value; the shared alias also
 * demonstrates a change through another mapping without changing RO access. */
static int readonly_provenance(void)
{
 static const guint8 instructions[]={0x48,0x8b,0x05,0,0,0,0,
   0x48,0x85,0xc0,0x74,0x02,0xff,0xe0,0xb8,43,0,0,0,0xc3};
 long page=sysconf(_SC_PAGESIZE);guint kind;gint32 displacement;
 CHECK(page>0);
 for(kind=0;kind!=3;kind++) {
  guint8 *mapping,*alias=NULL;gint fd=-1;
  gchar path[]="/tmp/peak entry provenance-XXXXXX";
  mapping=mmap(NULL,page*3,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  CHECK(mapping!=MAP_FAILED);
  if(kind!=0) {
   guint8 *zeros=calloc(1,page);CHECK(zeros!=NULL);
   fd=mkstemp(path);CHECK(fd!=-1);
   CHECK(write(fd,zeros,page)==page);free(zeros);
   CHECK(mmap(mapping+page,page,PROT_READ,MAP_FIXED|
       (kind==1?MAP_SHARED:MAP_PRIVATE),fd,0)==mapping+page);
   if(kind==1) {
    alias=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    CHECK(alias!=MAP_FAILED);
   }
  }else CHECK(mprotect(mapping+page,page,PROT_READ)==0);
  memcpy(mapping,instructions,sizeof instructions);
  displacement=(gint32)(page+64-7);memcpy(mapping+3,&displacement,4);
  CHECK(mprotect(mapping,page,PROT_READ|PROT_EXEC)==0);
  CHECK(range(mapping,5,FALSE,0)==0);
  if(alias!=NULL) {
   alias[64]=1;
   CHECK(((volatile guint8*)mapping)[page+64]==1);
   CHECK(range(mapping,5,FALSE,0)==0);
   CHECK(munmap(alias,page)==0);
  }
  CHECK(munmap(mapping,page*3)==0);
  if(fd!=-1) { CHECK(close(fd)==0);CHECK(unlink(path)==0); }
 }
 return 0;
}
extern int peak_x86_order_b(int),peak_x86_order_call(int),peak_x86_order_tail(int);
extern int peak_x86_order_interior(int),peak_x86_order_original_entry(int);
extern int peak_x86_order_original_b(int);
/* Every attach owns a distinct listener so A can be detached with B active. */
static int installation_order(GumInterceptor *i,int(*a)(int),gboolean b_first)
{
 GumInvocationListener *la=gum_make_call_listener(enter,NULL,NULL,NULL);
 GumInvocationListener *lb=gum_make_call_listener(enter,NULL,NULL,NULL);
 guint8 before[32];guint generation;
 memcpy(before,a,sizeof before);
 if(b_first)CHECK(gum_interceptor_peak_attach_exact(i,peak_x86_order_b,lb,NULL)==GUM_ATTACH_OK);
 CHECK(gum_interceptor_peak_attach_exact(i,a,la,NULL)==GUM_ATTACH_OK);
 if(!b_first)CHECK(gum_interceptor_peak_attach_exact(i,peak_x86_order_b,lb,NULL)==GUM_ATTACH_OK);
 for(generation=0;generation!=2;generation++) {
  guint old=calls;gsize n=0;guint8 *live;
  CHECK(a(0)==61 && a(1)==61);CHECK(calls==old+4);
  CHECK(peak_x86_order_b(0)==61 && peak_x86_order_b(1)==61);
  /* Successful preparation must leave no original-byte overlay in scope. */
  live=gum_interceptor_peak_read_relocation_code(a,32,&n);
  CHECK(live!=NULL && n==32 && memcmp(live,a,32)==0);g_free(live);
  gum_interceptor_detach(i,la);CHECK(memcmp(before,a,32)==0);
  CHECK(a(0)==61 && a(1)==61);
  if(generation==0)CHECK(gum_interceptor_peak_attach_exact(i,a,la,NULL)==GUM_ATTACH_OK);
 }
 gum_interceptor_detach(i,lb);g_object_unref(la);g_object_unref(lb);return 0;
}
static int scope_conflicts(GumInterceptor *i)
{
 GumInvocationListener *la=gum_make_call_listener(enter,NULL,NULL,NULL);
 GumInvocationListener *lb=gum_make_call_listener(enter,NULL,NULL,NULL);
 guint8 before[32];gsize n;guint8 *live;
 memcpy(before,peak_x86_order_b,32);
 gum_interceptor_begin_transaction(i);
 CHECK(gum_interceptor_peak_attach_exact(i,peak_x86_order_b,lb,NULL)==GUM_ATTACH_OK);
 CHECK(gum_interceptor_peak_attach_exact(i,(guint8*)peak_x86_order_b+4,la,NULL)!=GUM_ATTACH_OK);
 gum_interceptor_end_transaction(i);
 CHECK(gum_interceptor_peak_attach_exact(i,(guint8*)peak_x86_order_b+4,la,NULL)!=GUM_ATTACH_OK);
 CHECK(gum_interceptor_peak_attach_exact(i,peak_x86_order_interior,la,NULL)!=GUM_ATTACH_OK);
 live=gum_interceptor_peak_read_relocation_code(peak_x86_order_b,32,&n);
 CHECK(live!=NULL && n==32 && memcmp(live,peak_x86_order_b,32)==0);g_free(live);
 gum_interceptor_detach(i,lb);CHECK(memcmp(before,peak_x86_order_b,32)==0);
 CHECK(gum_interceptor_peak_attach_exact(i,peak_x86_order_original_b,lb,NULL)==GUM_ATTACH_OK);
 CHECK(gum_interceptor_peak_attach_exact(i,peak_x86_order_original_entry,la,NULL)!=GUM_ATTACH_OK);
 gum_interceptor_detach(i,lb);
 CHECK(gum_interceptor_replace(i,peak_x86_order_b,fast_replacement,
     (gpointer*)&fast_original,NULL)==GUM_REPLACE_OK);
 CHECK(gum_interceptor_peak_attach_exact(i,peak_x86_order_tail,la,NULL)!=GUM_ATTACH_OK);
 gum_interceptor_revert(i,peak_x86_order_b);
 CHECK(gum_interceptor_peak_attach_exact(i,peak_x86_order_tail,la,NULL)==GUM_ATTACH_OK);
 gum_interceptor_detach(i,la);g_object_unref(la);g_object_unref(lb);return 0;
}
int main(void)
{
#ifndef PEAK_GUM_ENTRY_RANGE_CHECKS
 puts("caller-provided Gum lacks entry range checks");return 77;
#endif
 GumInterceptor*i;GumInvocationListener*l;guint n;
 long page;guint8 * mapping;
#ifdef PEAK_GUM_ENTRY_RANGE_CHECKS
 CHECK(gum_interceptor_peak_entry_range_revision()==1);
#endif
 gum_init_embedded();i=gum_interceptor_obtain();l=gum_make_call_listener(enter,NULL,NULL,NULL);
 CHECK(range(peak_x86_unsafe,5,FALSE,4)==0);
 CHECK(range(peak_x86_negative,5,FALSE,4)==0);
 CHECK(range(peak_x86_prefix_data,5,FALSE,0)==0);
 CHECK(range(peak_x86_crossing_data,5,FALSE,0)==0);
 CHECK(range(peak_x86_rounding,16,FALSE,1)==0);
 CHECK(range(peak_x86_unsafe,16,FALSE,4)==0);
 CHECK(range(peak_x86_recoverable,16,FALSE,12)==0);
 CHECK(range(peak_x86_recoverable,5,TRUE,5)==0);
 CHECK(range(peak_x86_mid_instruction,5,FALSE,0)==0);
 CHECK(range(peak_x86_indirect,5,FALSE,0)==0);
 CHECK(range(peak_x86_indirect_call,5,TRUE,7)==0);
 CHECK(range(peak_x86_budget,5,FALSE,0)==0);
 CHECK(range(peak_x86_internal_jmp,7,FALSE,4)==0);
 CHECK(gum_x86_relocator_can_relocate(peak_x86_internal_jcc,10,GUM_SCENARIO_ONLINE,&n));
 CHECK(n==10);
 CHECK(refuse(i,l,peak_x86_unsafe)==0);
 CHECK(refuse(i,l,peak_x86_negative)==0);
 CHECK(refuse(i,l,peak_x86_prefix_data)==0);
 CHECK(refuse(i,l,peak_x86_mid_instruction)==0);
 CHECK(refuse(i,l,peak_x86_indirect)==0);
 CHECK(run(i,l,peak_x86_indirect_call,37)==0);
 CHECK(refuse(i,l,peak_x86_budget)==0);
 CHECK(run(i,l,peak_x86_recoverable,11)==0);
 CHECK(run(i,l,peak_x86_internal_jcc,19)==0);
 CHECK(run(i,l,peak_x86_rip_data,23)==0);
 CHECK(run(i,l,peak_x86_rip_lea,23)==0);
 CHECK(run(i,l,peak_x86_call_next,31)==0);
 CHECK(run(i,l,peak_x86_syscall,(int)getpid())==0);
 CHECK(run(i,l,peak_x86_readonly_zero,43)==0);
 CHECK(run(i,l,peak_x86_readonly_chain,47)==0);
 CHECK(run(i,l,peak_x86_readonly_other_gprs,53)==0);
 CHECK(range(peak_x86_readonly_r10d,5,FALSE,0)==0);
 CHECK(range(peak_x86_readonly_r15b,5,FALSE,0)==0);
 CHECK(range(peak_x86_readonly_nonzero,5,FALSE,0)==0);
 CHECK(range(peak_x86_writable_zero,5,FALSE,0)==0);
 CHECK(range(peak_x86_readonly_bypass,5,FALSE,0)==0);
 CHECK(range(peak_x86_entry_condition,5,FALSE,0)==0);
 CHECK(range(peak_x86_call_into_test,5,FALSE,0)==0);
 CHECK(range(peak_x86_call_into_instruction,5,FALSE,0)==0);
 CHECK(range(peak_x86_readonly_alias,5,FALSE,0)==0);
 CHECK(range(peak_x86_readonly_call,5,FALSE,0)==0);
 CHECK(range(peak_x86_suffix_trap,5,TRUE,5)==0);
 CHECK(range(peak_x86_prefix_trap,5,FALSE,0)==0);
 CHECK(readonly_boundaries()==0);
 CHECK(readonly_provenance()==0);
 CHECK(state_image_width(peak_x86_xsave_extent,X86_INS_XSAVE)==0);
 CHECK(state_image_width(peak_x86_fxsave_extent,X86_INS_FXSAVE)==0);
 CHECK(installation_order(i,peak_x86_order_call,FALSE)==0);
 CHECK(installation_order(i,peak_x86_order_call,TRUE)==0);
 CHECK(installation_order(i,peak_x86_order_tail,FALSE)==0);
 CHECK(installation_order(i,peak_x86_order_tail,TRUE)==0);
 CHECK(scope_conflicts(i)==0);
 CHECK(full_span(i)==0);
 CHECK(far_rewrite(peak_x86_full_span,50,50)==0);
 CHECK(far_rewrite(peak_x86_far_branch,17,13)==0);
 page=sysconf(_SC_PAGESIZE);CHECK(page>0);
 mapping=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 CHECK(mapping!=MAP_FAILED);
 memset(mapping+page-4,0x90,4);
 CHECK(mprotect(mapping+page,page,PROT_NONE)==0);
 CHECK(range(mapping+page-4,5,FALSE,0)==0);
 CHECK(munmap(mapping,page*2)==0);
 g_object_unref(l);g_object_unref(i);gum_deinit_embedded();
 puts("gum_x86_entry_range_ok");return 0;
}
