.text
.macro START name
.globl \name
.type \name,@function
\name:
.endm
.macro END name
.size \name,.-\name
.endm
START peak_x86_recoverable
 test %edi,%edi
 je 2f
 .fill 8,1,0x90
1: mov $11,%eax
 ret
 .fill 32,1,0x90
2: mov $1,%edi
 jmp 1b
END peak_x86_recoverable
START peak_x86_internal_jcc
 test %edi,%edi
 je 1f
1: nop
 mov $19,%eax
 ret
END peak_x86_internal_jcc
START peak_x86_internal_jmp
 nop
 nop
 nop
 nop
1: nop
 jmp 1b
END peak_x86_internal_jmp
START peak_x86_indirect
 nop
 nop
 nop
 nop
 nop
 jmp *%rax
END peak_x86_indirect
START peak_x86_indirect_call
 lea peak_x86_indirect_callee(%rip),%rax
 call *%rax
 ret
END peak_x86_indirect_call
peak_x86_indirect_callee:
 mov $37,%eax
 ret
START peak_x86_budget
 .rept 4097
 nop
 .endr
 mov $83,%eax
 ret
END peak_x86_budget
START peak_x86_rip_data
 mov peak_x86_data(%rip),%eax
 ret
END peak_x86_rip_data
START peak_x86_rip_lea
 lea peak_x86_data(%rip),%rax
 mov (%rax),%eax
 ret
END peak_x86_rip_lea
START peak_x86_call_next
 call 1f
1: pop %rax
 mov $31,%eax
 ret
END peak_x86_call_next
.text
START peak_x86_prefix_data
 .fill 5,1,0x90
 lea peak_x86_prefix_data(%rip),%rax
 ret
END peak_x86_prefix_data
START peak_x86_rounding
 nop
 lea peak_x86_rounding+3(%rip),%rax
 ret
END peak_x86_rounding
START peak_x86_crossing_data
 movq peak_x86_crossing_data-4(%rip),%rax
 ret
END peak_x86_crossing_data
.data
peak_x86_data: .long 23
.section .note.GNU-stack,"",@progbits
.text
START peak_x86_full_span
 mov peak_x86_data(%rip),%eax
 lea peak_x86_data(%rip),%rdx
 add (%rdx),%eax
 call peak_x86_full_callee
 add $1,%eax
 ret
END peak_x86_full_span
peak_x86_full_callee:
 add $3,%eax
 ret
START peak_x86_far_branch
 test %edi,%edi
 je 1f
 .fill 20,1,0x90
 mov $13,%eax
 ret
1: mov $17,%eax
 ret
END peak_x86_far_branch

.text
START peak_x86_syscall
 .fill 5,1,0x90
 mov $39,%eax
 syscall
 ret
END peak_x86_syscall
START peak_x86_suffix_trap
 .fill 5,1,0x90
 test %edi,%edi
 je 1f
 ud2
1: mov $7,%eax
 ret
END peak_x86_suffix_trap
START peak_x86_prefix_trap
 ud2
 .fill 16,1,0x90
 ret
END peak_x86_prefix_trap
.text
/* This overlap fixture is classifier-only: never store a state image into code. */
START peak_x86_fxsave_overlap
 fxsave peak_x86_fxsave_overlap-4(%rip)
 ret
END peak_x86_fxsave_overlap
START peak_x86_fxsave_buffered
 fxsave peak_x86_fxsave_buffer(%rip)
 mov $97,%eax
 ret
END peak_x86_fxsave_buffered
.bss
.p2align 4
.globl peak_x86_fxsave_buffer
.type peak_x86_fxsave_buffer,@object
peak_x86_fxsave_buffer: .skip 512
.size peak_x86_fxsave_buffer,512
.text

/* Independent hook entries, including an internal Jcc in the callee. */
.text
START peak_x86_order_b
 test %edi,%edi
 je 1f
1: nop
 mov $61,%eax
 ret
 .fill 32,1,0x90
END peak_x86_order_b
START peak_x86_order_call
 sub $8,%rsp
 call peak_x86_order_b
 add $8,%rsp
 ret
 .fill 32,1,0x90
END peak_x86_order_call
START peak_x86_order_tail
 .fill 5,1,0x90
 jmp peak_x86_order_b
 .fill 32,1,0x90
END peak_x86_order_tail

.text
START peak_x86_mutable_tail
 mov peak_x86_mutable_slot(%rip),%rax
 jmp *%rax
END peak_x86_mutable_tail
START peak_x86_prefix_tail
 jmp *%rax
 .fill 32,1,0x90
END peak_x86_prefix_tail
START peak_x86_mutable_one
 mov $71,%eax
 ret
END peak_x86_mutable_one
START peak_x86_mutable_two
 mov $73,%eax
 ret
END peak_x86_mutable_two
.data
.globl peak_x86_mutable_slot
peak_x86_mutable_slot: .quad peak_x86_mutable_one

.text

/* Explicit landing pads make the whole first overwrite ENDBR64 + XOR (6B),
 * leaving the loop entry at +8. Without the pad the same loop enters at +4. */
.text
.macro COPY_BACKEDGE name, counter, landing
START \name
 .if \landing
 .byte 0xf3,0x0f,0x1e,0xfa
 .endif
 xor \counter,\counter
 xor %eax,%eax
1: mov (%rdi,%rax,1),%ecx
 inc \counter
 mov %ecx,(%rsi,%rax,1)
 add $4,%rax
 cmp $4,\counter
 jb 1b
 ret
 .fill 16,1,0x90
END \name
.endm
COPY_BACKEDGE peak_x86_endbr_dl,%dl,1
COPY_BACKEDGE peak_x86_endbr_edx,%edx,1

.text
START peak_x86_prefix_read
 movb peak_x86_prefix_read(%rip),%al
 ret
END peak_x86_prefix_read


/* Locally visible destinations must be valid instruction starts, and CALL
 * keeps its original target rather than a relocated internal label. */
.text
START peak_x86_call_prefix
 call peak_x86_call_prefix+1
 .fill 32,1,0x90
END peak_x86_call_prefix
START peak_x86_internal_mid_jcc
 .byte 0x75,0x01
 mov $0x90909090,%eax
 ret
END peak_x86_internal_mid_jcc
