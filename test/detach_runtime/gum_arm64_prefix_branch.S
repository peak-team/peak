.text
/* Hpsort-shaped synthetic control flow. The real QE entry starts with two
 * loads; this executable fixture uses argument setup and the same +8 branch
 * and later +12 backedge, without pretending to execute the QE function. */
.p2align 4
.global peak_prefix_hpsort
.type peak_prefix_hpsort,%function
peak_prefix_hpsort:
 mov w8,w0
 nop
 cbz w8,1f
0:
 cmp w8,#2
 mov w0,#7
 ret
 .space 512,0
1:
 mov w8,#3
 b 0b
.size peak_prefix_hpsort,.-peak_prefix_hpsort
.p2align 4
.global peak_prefix_linear
.type peak_prefix_linear,%function
peak_prefix_linear:
 mov w8,w0
 nop
 nop
 nop
 mov w0,#7
 ret
.size peak_prefix_linear,.-peak_prefix_linear
.p2align 4
.global peak_prefix_branch4
.type peak_prefix_branch4,%function
peak_prefix_branch4:
 mov w8,w0
 cbz w8,1f
 mov w0,#7
 ret
1:
 mov w0,#7
 ret
.size peak_prefix_branch4,.-peak_prefix_branch4
.p2align 4
.global peak_prefix_known_backedge
.type peak_prefix_known_backedge,%function
peak_prefix_known_backedge:
 nop
0:
 nop
 b 0b
.size peak_prefix_known_backedge,.-peak_prefix_known_backedge
.p2align 4
.global peak_prefix_ret8
.type peak_prefix_ret8,%function
peak_prefix_ret8:
 mov w0,#7
 ret
.size peak_prefix_ret8,.-peak_prefix_ret8
.p2align 4
.global peak_prefix_branch_entry
.type peak_prefix_branch_entry,%function
peak_prefix_branch_entry:
 b 1f
1:
 mov w0,#7
 ret
.size peak_prefix_branch_entry,.-peak_prefix_branch_entry
.p2align 4
.global peak_prefix_indirect_entry
.type peak_prefix_indirect_entry,%function
peak_prefix_indirect_entry:
 br x0
.size peak_prefix_indirect_entry,.-peak_prefix_indirect_entry
.p2align 4
.global peak_prefix_trap_entry
.type peak_prefix_trap_entry,%function
peak_prefix_trap_entry:
 brk #0
.size peak_prefix_trap_entry,.-peak_prefix_trap_entry
/* These trap cases are classifier-only. No trap is executed by the test. */
.p2align 4
.global peak_prefix_trap4
.type peak_prefix_trap4,%function
peak_prefix_trap4:
 nop
 brk #0
.size peak_prefix_trap4,.-peak_prefix_trap4
.p2align 4
.global peak_prefix_trap_suffix
.type peak_prefix_trap_suffix,%function
peak_prefix_trap_suffix:
 nop
 nop
 nop
 nop
 cbz w0,1f
 ret
1:
 brk #1000
 .word 0
.size peak_prefix_trap_suffix,.-peak_prefix_trap_suffix
/* Suffix length never changes entry-prefix work or the selected 16 bytes. */
.macro peak_long name,count
.p2align 4
.global \name
.type \name,%function
\name:
 .rept \count
 nop
 .endr
 mov w0,#7
 ret
.size \name,.-\name
.endm
peak_long peak_prefix_long_4100,4100
peak_long peak_prefix_long_8200,8200
/* Only the literal accesses visible in the sizing prefix are examined. */
.p2align 4
.global peak_prefix_literal_self
.type peak_prefix_literal_self,%function
peak_prefix_literal_self:
0:
 ldr w9,0b
 nop
 nop
 nop
 ret
.size peak_prefix_literal_self,.-peak_prefix_literal_self
.p2align 4
0:
 .quad 0
.global peak_prefix_literal_cross
.type peak_prefix_literal_cross,%function
peak_prefix_literal_cross:
 ldr q0,0b
 nop
 nop
 nop
 ret
.size peak_prefix_literal_cross,.-peak_prefix_literal_cross
.p2align 4
.global peak_prefix_literal_boundary
.type peak_prefix_literal_boundary,%function
peak_prefix_literal_boundary:
0:
 ldr q0,0b+16
 nop
 nop
 nop
 mov w0,#7
 ret
.size peak_prefix_literal_boundary,.-peak_prefix_literal_boundary
/* A requested X9 scratch must notice the W9 alias in the actual prefix. */
.p2align 4
.global peak_prefix_requested_w9
.type peak_prefix_requested_w9,%function
peak_prefix_requested_w9:
 mov w9,w0
 nop
 nop
 nop
 mov w0,#7
 ret
.size peak_prefix_requested_w9,.-peak_prefix_requested_w9
.section .note.GNU-stack,"",%progbits
