.text
.macro START name
.globl \name
.type \name,@function
\name:
.endm
.macro END name
.size \name,.-\name
.endm
START peak_x86_unsafe
 test %edi,%edi
 je 2f
1: mov $7,%eax
 ret
 .fill 32,1,0x90
2: mov $1,%edi
 jmp 1b
END peak_x86_unsafe
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
 nop
1: mov $19,%eax
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
START peak_x86_mid_instruction
 test %edi,%edi
 je 2f
1: mov $0x90909090,%eax
 ret
2: jmp 1b+1
END peak_x86_mid_instruction
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
 .rept 4096
 nop
 .endr
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
peak_x86_negative_block:
 test %edi,%edi
 je peak_x86_negative+4
 ret
START peak_x86_negative
 test %edi,%edi
 je peak_x86_negative_block
1: mov $41,%eax
 ret
END peak_x86_negative
START peak_x86_rounding
 nop
1: nop
 nop
 nop
 .byte 0x0f,0x85
 .long 1b - . - 4
 .fill 8,1,0x90
 lea peak_x86_rounding+7(%rip),%rax
 ret
END peak_x86_rounding
START peak_x86_crossing_data
 .fill 5,1,0x90
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
.section .rodata
.p2align 3
peak_x86_ro_zero: .quad 0
.text
START peak_x86_xsave_extent
 xsave peak_x86_ro_zero(%rip)
 ret
END peak_x86_xsave_extent
START peak_x86_fxsave_extent
 fxsave peak_x86_ro_zero(%rip)
 ret
END peak_x86_fxsave_extent

/* Independent hook entries, including an internal Jcc in the callee. */
.text
START peak_x86_order_b
 test %edi,%edi
 je 1f
 nop
1: mov $61,%eax
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
START peak_x86_order_interior
 .fill 5,1,0x90
 jmp peak_x86_order_b+4
 .fill 32,1,0x90
END peak_x86_order_interior
START peak_x86_order_original_entry
 .fill 5,1,0x90
 jmp peak_x86_order_original_b
 .fill 32,1,0x90
END peak_x86_order_original_entry
START peak_x86_order_original_b
 test %edi,%edi
 je 1f
 jmp peak_x86_order_original_entry+1
1: mov $63,%eax
 ret
 .fill 32,1,0x90
END peak_x86_order_original_b

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
START peak_x86_call_middle
 call 1f+1
1: mov $0x90909090,%eax
 ret
END peak_x86_call_middle
START peak_x86_call_hook_interior
 sub $8,%rsp
 call peak_x86_order_b+4
 add $8,%rsp
 ret
END peak_x86_call_hook_interior

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
COPY_BACKEDGE peak_x86_plain_dl,%dl,0
COPY_BACKEDGE peak_x86_plain_edx,%edx,0

.text
START peak_x86_prefix_read
 .fill 5,1,0x90
 movb peak_x86_prefix_read(%rip),%al
 ret
END peak_x86_prefix_read

/* A real return path and a terminal nonreturning call, followed immediately by
 * deliberately invalid bytes outside the trusted function extent. */
.text
START peak_x86_extent_exit
 sub $8,%rsp
 test %edi,%edi
 jne 1f
 mov $79,%eax
 add $8,%rsp
 ret
1: mov $17,%edi
 call _exit
.globl peak_x86_extent_exit_end
peak_x86_extent_exit_end:
END peak_x86_extent_exit
 .byte 0x0f,0x04
 .fill 32,1,0x90
START peak_x86_extent_call_before_end
 .fill 5,1,0x90
 call peak_x86_indirect_callee
 jmp peak_x86_extent_call_before_end+1
.globl peak_x86_extent_call_before_end_end
peak_x86_extent_call_before_end_end:
END peak_x86_extent_call_before_end
START peak_x86_extent_fallthrough
 .fill 5,1,0x90
.globl peak_x86_extent_fallthrough_end
peak_x86_extent_fallthrough_end:
END peak_x86_extent_fallthrough
 jmp peak_x86_extent_fallthrough+1
START peak_x86_extent_cold
 test %edi,%edi
 je 1f
 nop
 ret
.globl peak_x86_extent_cold_end
peak_x86_extent_cold_end:
END peak_x86_extent_cold
1: mov $1,%edi
 jmp peak_x86_extent_cold+4
START peak_x86_extent_call_prefix
 .fill 5,1,0x90
 call peak_x86_extent_call_prefix+1
.globl peak_x86_extent_call_prefix_end
peak_x86_extent_call_prefix_end:
END peak_x86_extent_call_prefix
 .byte 0x0f,0x04
START peak_x86_extent_hook_interior
 sub $8,%rsp
 call peak_x86_order_b+4
.globl peak_x86_extent_hook_interior_end
peak_x86_extent_hook_interior_end:
END peak_x86_extent_hook_interior
 .byte 0x0f,0x04
 .fill 32,1,0x90
