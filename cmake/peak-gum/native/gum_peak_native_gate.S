#include "native-offsets.h"
#if defined(__x86_64__)
.section peak_gum_fast_dispatch,"ax",@progbits
.p2align 4
.globl gum_peak_native_enter
.hidden gum_peak_native_enter
.type gum_peak_native_enter,@function
gum_peak_native_enter:
.cfi_startproc
.cfi_def_cfa_offset 24
.cfi_offset %rip,-8
.byte 0xf3,0x0f,0x1e,0xfa
pushfq
.cfi_adjust_cfa_offset 8
pushq %rax
.cfi_adjust_cfa_offset 8
pushq %rcx
.cfi_adjust_cfa_offset 8
pushq %rdx
.cfi_adjust_cfa_offset 8
pushq %r8
.cfi_adjust_cfa_offset 8
pushq %r10
.cfi_adjust_cfa_offset 8
pushq %r11
.cfi_adjust_cfa_offset 8
/* private original stack words are ctx, original-enter-thunk, caller return */
movq 56(%rsp),%r10
cmpq $0,CTX_REDIRECT(%r10)
jne .Lfallback
movq CTX_FAST(%r10),%r11
testq %r11,%r11
jz .Lfallback
cmpl $9,FD_VERSION(%r11)
jne .Lfallback
cmpl $0,FD_ENABLED(%r11)
je .Lfallback
cmpq $0,CTX_REPLACEMENT(%r10)
jne .Lfallback
movq FD_COOKIE(%r11),%rax
cmpq %rax,CTX_ENTRIES(%r10)
jne .Lfallback
movq FD_NATIVE(%r11),%r11
testq %r11,%r11
jz .Lfallback
cmpl $0,N_ENABLED(%r11)
je .Lfallback
movq N_PAUSED(%r11),%rax
cmpl $0,(%rax)
je .Lfallback
movq N_INITIAL(%r11),%rax
cmpl $0,(%rax)
jne .Lfallback
movq N_SUSPENDED(%r11),%rax
cmpl $0,(%rax)
jne .Lfallback
cmpq $0,N_CALLS_BASE(%r11)
je .Lfallback
movq %fs:0,%r10
movq N_TLS_CALLBACK(%r11),%rax
leaq (%r10,%rax),%r8
cmpl $0,(%r8)
jne .Lfallback
movq N_TLS_INITIALIZED(%r11),%rax
cmpl $0,(%r10,%rax)
je .Lfallback
movq N_TLS_MAPPED(%r11),%rax
cmpl $0,(%r10,%rax)
je .Lfallback
movq N_TLS_EXCLUDED(%r11),%rax
cmpl $0,(%r10,%rax)
jne .Lfallback
movq N_TLS_IGNORE(%r11),%rax
cmpl $0,(%r10,%rax)
jne .Lfallback
movq N_TLS_REGISTERED(%r11),%rax
cmpl $0,(%r10,%rax)
je .Lfallback
movq N_TLS_SLOT(%r11),%rax
movq (%r10,%rax),%rcx
cmpq N_CAPACITY(%r11),%rcx
jae .Lfallback
/* One writer owns this slot. The existing callback guard excludes signals. */
movl $1,(%r8)
movq %rcx,%rdx
shlq $N_RENT_SHIFT,%rdx
addq N_RENT_BASE(%r11),%rdx
movq (%rdx),%rax
addq N_RENT_NS(%r11),%rax
jnc .Lrent_ready
movq $-1,%rax
.Lrent_ready:
movq %rax,(%rdx)
shlq $N_COST_SHIFT,%rcx
addq N_CALLS_BASE(%r11),%rcx
movq N_TLS_INSIDE(%r11),%rax
cmpq $-1,%rax
je .Lcount_outside
cmpl $0,(%r10,%rax)
je .Lcount_outside
addq $8,%rcx
.Lcount_outside:
movq (%rcx),%rax
addq $1,%rax
jnc .Lcount_ready
movq $-1,%rax
.Lcount_ready:
movq %rax,(%rcx)
movl $0,(%r8)
movq 56(%rsp),%rax
movq CTX_INVOKE(%rax),%rax
movq %rax,56(%rsp)
/* Registers, flags and caller stack are exactly restored before original. */
popq %r11
.cfi_adjust_cfa_offset -8
popq %r10
.cfi_adjust_cfa_offset -8
popq %r8
.cfi_adjust_cfa_offset -8
popq %rdx
.cfi_adjust_cfa_offset -8
popq %rcx
.cfi_adjust_cfa_offset -8
popq %rax
.cfi_adjust_cfa_offset -8
popfq
.cfi_adjust_cfa_offset -8
leaq 16(%rsp),%rsp
.cfi_adjust_cfa_offset -16
notrack jmp *-16(%rsp)
/* Keep original normal-Gum ctx stack word, remove only extra thunk word. */
.Lfallback:
.cfi_def_cfa_offset 80
movq 56(%rsp),%rax
movq 64(%rsp),%rdx
movq %rax,64(%rsp)
movq %rdx,56(%rsp)
popq %r11
.cfi_adjust_cfa_offset -8
popq %r10
.cfi_adjust_cfa_offset -8
popq %r8
.cfi_adjust_cfa_offset -8
popq %rdx
.cfi_adjust_cfa_offset -8
popq %rcx
.cfi_adjust_cfa_offset -8
popq %rax
.cfi_adjust_cfa_offset -8
popfq
.cfi_adjust_cfa_offset -8
leaq 8(%rsp),%rsp
.cfi_adjust_cfa_offset -8
notrack jmp *-8(%rsp)
.cfi_endproc
.size gum_peak_native_enter,.-gum_peak_native_enter
.section .note.GNU-stack,"",@progbits
#endif

#if defined(__aarch64__)
.section peak_gum_fast_dispatch,"ax",%progbits
/* The Gum-selected IP register alone is scratch. No SIMD/argument/LR changes. */
.macro PEAK_NATIVE_ARM name, ip, auth
.p2align 4
.global \name
.hidden \name
.type \name,%function
\name:
.cfi_startproc
.cfi_def_cfa_offset 16
.inst 0xd50324df /* BTI JC: accepts Gum IP-register branch class. */
sub sp,sp,#64
.cfi_adjust_cfa_offset 64
stp x9,x10,[sp,#0]
stp x11,x12,[sp,#16]
stp x13,x14,[sp,#32]
mrs x9,nzcv
stp x15,x9,[sp,#48]
ldr x9,[sp,#64]
ldr x14,[x9,#CTX_REDIRECT]
cbnz x14,.Lfallback\@
add x14,x9,#CTX_FAST
ldar x10,[x14]
cbz x10,.Lfallback\@
ldr w14,[x10,#FD_VERSION]
cmp w14,#9
b.ne .Lfallback\@
add x14,x10,#FD_ENABLED
ldar w14,[x14]
cbz w14,.Lfallback\@
ldr x14,[x9,#CTX_REPLACEMENT]
cbnz x14,.Lfallback\@
ldr x14,[x10,#FD_COOKIE]
add x15,x9,#CTX_ENTRIES
ldar x15,[x15]
cmp x14,x15
b.ne .Lfallback\@
ldr x10,[x10,#FD_NATIVE]
cbz x10,.Lfallback\@
ldar w14,[x10]
cbz w14,.Lfallback\@
ldr x14,[x10,#N_PAUSED]
ldar w14,[x14]
cbz w14,.Lfallback\@
ldr x14,[x10,#N_INITIAL]
ldar w14,[x14]
cbnz w14,.Lfallback\@
ldr x14,[x10,#N_SUSPENDED]
ldar w14,[x14]
cbnz w14,.Lfallback\@
ldr x14,[x10,#N_CALLS_BASE]
cbz x14,.Lfallback\@
mrs x11,tpidr_el0
ldr x14,[x10,#N_TLS_CALLBACK]
add x12,x11,x14
ldr w14,[x12]
cbnz w14,.Lfallback\@
ldr x14,[x10,#N_TLS_INITIALIZED]
ldr w14,[x11,x14]
cbz w14,.Lfallback\@
ldr x14,[x10,#N_TLS_MAPPED]
ldr w14,[x11,x14]
cbz w14,.Lfallback\@
ldr x14,[x10,#N_TLS_EXCLUDED]
ldr w14,[x11,x14]
cbnz w14,.Lfallback\@
ldr x14,[x10,#N_TLS_IGNORE]
ldr w14,[x11,x14]
cbnz w14,.Lfallback\@
ldr x14,[x10,#N_TLS_REGISTERED]
ldr w14,[x11,x14]
cbz w14,.Lfallback\@
ldr x14,[x10,#N_TLS_SLOT]
ldr x13,[x11,x14]
ldr x14,[x10,#N_CAPACITY]
cmp x13,x14
b.hs .Lfallback\@
mov w14,#1
str w14,[x12]
ldr x15,[x10,#N_RENT_BASE]
add x15,x15,x13,lsl #N_RENT_SHIFT
ldr x14,[x15]
ldr x9,[x10,#N_RENT_NS]
adds x14,x14,x9
csinv x14,x14,xzr,cc /* saturate carry to UINT64_MAX */
str x14,[x15]
ldr x15,[x10,#N_CALLS_BASE]
add x15,x15,x13,lsl #N_COST_SHIFT
ldr x14,[x10,#N_TLS_INSIDE]
cmn x14,#1
b.eq .Loutside\@
ldr w14,[x11,x14]
cbz w14,.Loutside\@
add x15,x15,#8
.Loutside\@:
ldr x14,[x15]
adds x14,x14,#1
csinv x14,x14,xzr,cc
str x14,[x15]
str wzr,[x12]
ldr x9,[sp,#64]
ldr x\ip,[x9,#CTX_INVOKE]
b .Lrestore_native\@
.Lfallback\@:
ldr x\ip,[sp,#72]
b .Lrestore_fallback\@
.Lrestore_native\@:
ldp x15,x9,[sp,#48]
msr nzcv,x9
ldp x13,x14,[sp,#32]
ldp x11,x12,[sp,#16]
ldp x9,x10,[sp,#0]
add sp,sp,#80
.cfi_def_cfa_offset 0
.if \auth
.inst (0xd61f0000 | (\ip << 5) | 0x81f)
.else
br x\ip
.endif
.Lrestore_fallback\@:
.cfi_def_cfa_offset 80
ldp x15,x9,[sp,#48]
msr nzcv,x9
ldp x13,x14,[sp,#32]
ldp x11,x12,[sp,#16]
ldp x9,x10,[sp,#0]
add sp,sp,#64
.cfi_def_cfa_offset 16
.if \auth
.inst (0xd61f0000 | (\ip << 5) | 0x81f)
.else
br x\ip
.endif
.cfi_endproc
.size \name,.-\name
.endm
PEAK_NATIVE_ARM gum_peak_native_enter_arm64_x16,16,0
PEAK_NATIVE_ARM gum_peak_native_enter_arm64_x17,17,0
PEAK_NATIVE_ARM gum_peak_native_enter_arm64_x16_auth,16,1
PEAK_NATIVE_ARM gum_peak_native_enter_arm64_x17_auth,17,1
.section .note.GNU-stack,"",%progbits
#endif
