.text
.p2align 4
.global peak_prefix_branch_unsafe
.type peak_prefix_branch_unsafe,%function
peak_prefix_branch_unsafe:
 mov w8,w0
 mov w9,#0
 cbz w8,1f
0:
 cmp w8,#2
 mov w0,#7
 ret
 .space 512,0
1:
 mov w8,#3
 b 0b
.size peak_prefix_branch_unsafe,.-peak_prefix_branch_unsafe
.p2align 4
.global peak_prefix_branch_safe
.type peak_prefix_branch_safe,%function
peak_prefix_branch_safe:
 mov w8,w0
 mov w9,#0
 cbz w8,1f
 cmp w8,#2
0:
 mov w0,#7
 ret
 .space 512,0
1:
 mov w8,#3
 b 0b
.size peak_prefix_branch_safe,.-peak_prefix_branch_safe
.p2align 4
.global peak_prefix_branch_indirect
.type peak_prefix_branch_indirect,%function
peak_prefix_branch_indirect:
 mov w8,w0
 mov w9,#0
 cbz w8,1f
 cmp w8,#2
 mov w0,#7
 ret
 .space 512,0
1:
 br x0
.size peak_prefix_branch_indirect,.-peak_prefix_branch_indirect
.p2align 4
.global peak_prefix_branch_eret
.type peak_prefix_branch_eret,%function
peak_prefix_branch_eret:
 mov w8,w0
 mov w9,#0
 cbz w8,1f
 cmp w8,#2
 mov w0,#7
 ret
 .space 512,0
1:
 eret
.size peak_prefix_branch_eret,.-peak_prefix_branch_eret
.p2align 4
.global peak_prefix_branch_bc
.type peak_prefix_branch_bc,%function
peak_prefix_branch_bc:
 mov w8,w0
 mov w9,#0
 .inst 0x54001010 /* bc.eq with an immediate target beyond the entry patch */
 cmp w8,#2
 mov w0,#7
 ret
 .space 512,0
 ret
.size peak_prefix_branch_bc,.-peak_prefix_branch_bc
/* ONLINE stops on BL before reaching the requested 16 bytes. */
.p2align 4
.global peak_prefix_branch_unsafe8
.type peak_prefix_branch_unsafe8,%function
peak_prefix_branch_unsafe8:
 cbz w0,1f
0:
 bl 2f
 ret
2:
 ret
 .space 512,0
1:
 b 0b
.size peak_prefix_branch_unsafe8,.-peak_prefix_branch_unsafe8
.p2align 4
/* Classifier-only fixture: the BL continuation is not a callable ABI body. */
.global peak_prefix_branch_safe8
.type peak_prefix_branch_safe8,%function
peak_prefix_branch_safe8:
 cbz w0,1f
 bl 2f
0:
 ret
2:
 ret
 .space 512,0
1:
 b 0b
.size peak_prefix_branch_safe8,.-peak_prefix_branch_safe8
/* EOI at byte 8 prevents Gum's post-prefix walk despite maximum=12. */
.p2align 4
.global peak_prefix_branch_unsafe12
.type peak_prefix_branch_unsafe12,%function
peak_prefix_branch_unsafe12:
 nop
0:
 nop
 b 0b
.size peak_prefix_branch_unsafe12,.-peak_prefix_branch_unsafe12
/* A terminal RET must not cause the scan to visit unrelated trailing code. */
.p2align 4
.global peak_prefix_branch_ret8
.type peak_prefix_branch_ret8,%function
peak_prefix_branch_ret8:
 nop
 ret
 .inst 0xffffffff
.size peak_prefix_branch_ret8,.-peak_prefix_branch_ret8
/* The alternate path can precede the entry address. */
.p2align 4
1:
 b 0f
 .space 32,0
.global peak_prefix_branch_backward8
.type peak_prefix_branch_backward8,%function
peak_prefix_branch_backward8:
 cbz w0,1b
0:
 bl 2f
 ret
2:
 ret
.size peak_prefix_branch_backward8,.-peak_prefix_branch_backward8
/* An unconditional branch at the prefix end has no ordinary continuation. */
.p2align 4
.global peak_prefix_branch_terminal8
.type peak_prefix_branch_terminal8,%function
peak_prefix_branch_terminal8:
 nop
0:
 b 1f
 .inst 0xffffffff
 .space 512,0
1:
 b 0b
.size peak_prefix_branch_terminal8,.-peak_prefix_branch_terminal8
.section .note.GNU-stack,"",%progbits
