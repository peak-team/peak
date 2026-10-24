.text
.macro START name
.globl \name
.type \name,@function
\name:
.endm
.macro END name
.size \name,.-\name
.endm

START peak_x86_linear
 mov $11,%eax
 ret
END peak_x86_linear

/* The landing pad leaves the loop entry at +8 beyond the 6B patch. */
START peak_x86_endbr_copy
 .byte 0xf3,0x0f,0x1e,0xfa
 xor %dl,%dl
 xor %eax,%eax
1: mov (%rdi,%rax,1),%ecx
 inc %dl
 mov %ecx,(%rsi,%rax,1)
 add $4,%rax
 cmp $4,%dl
 jb 1b
 ret
END peak_x86_endbr_copy

/* Internal conditional targets must coincide with an instruction start. */
START peak_x86_internal_mid_jcc
 .byte 0x75,0x01
 mov $0x90909090,%eax
 ret
END peak_x86_internal_mid_jcc

START peak_x86_internal_jcc
 test %edi,%edi
 je 1f
1: nop
 mov $19,%eax
 ret
END peak_x86_internal_jcc

START peak_x86_external_jcc
 test %edi,%edi
 je 1f
 nop
 mov $13,%eax
 ret
1: mov $17,%eax
 ret
END peak_x86_external_jcc

/* This read observes patch bytes: classification and attach must refuse it. */
START peak_x86_prefix_read
 movb peak_x86_prefix_read(%rip),%al
 ret
END peak_x86_prefix_read
.section .note.GNU-stack,"",@progbits
