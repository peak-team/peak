.text
/* Hpsort-shaped synthetic control flow: first branch at +8, then a suffix
 * backedge into +12. The real QE entry uses two loads instead of setup here. */
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
.global peak_prefix_branch_entry
.type peak_prefix_branch_entry,%function
peak_prefix_branch_entry:
 b 1f
1:
 mov w0,#7
 ret
.size peak_prefix_branch_entry,.-peak_prefix_branch_entry
/* Direct regression: a suffix conditional branch re-enters at entry+4. */
.p2align 4
.global peak_prefix_loop3
.type peak_prefix_loop3,%function
peak_prefix_loop3:
 mov w0,#0
0:
 add w0,w0,#1
 cmp w0,#3
 nop
 b.lt 0b
 ret
.size peak_prefix_loop3,.-peak_prefix_loop3
/* A supported entry conditional must retain taken and fallthrough behavior. */
.p2align 4
.global peak_prefix_early_return
.type peak_prefix_early_return,%function
peak_prefix_early_return:
 cbz w0,1f
 add w0,w0,#1
 nop
 nop
 nop
 ret
1:
 ret
.size peak_prefix_early_return,.-peak_prefix_early_return
.section .note.GNU-stack,"",%progbits
