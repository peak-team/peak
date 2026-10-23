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
/* The complete walk must not silently accept an exhausted scan budget. */
.p2align 4
.global peak_prefix_branch_budget
.type peak_prefix_branch_budget,%function
peak_prefix_branch_budget:
 .rept 4100
 nop
 .endr
 ret
.size peak_prefix_branch_budget,.-peak_prefix_branch_budget
/* Early IP0/IP1 kills cannot prove that a later literal load avoids entry. */
.p2align 4
.global peak_prefix_branch_budget4
.type peak_prefix_branch_budget4,%function
peak_prefix_branch_budget4:
 mov w16,#0
 mov w17,#0
 .rept 4100
 nop
 .endr
 ret
.size peak_prefix_branch_budget4,.-peak_prefix_branch_budget4
.macro peak_prefix_ip_target name, operation
.p2align 4
.global \name
.type \name,%function
\name:
 cbz w0,1f
 nop
 nop
 nop
 mov w0,#7
 ret
1:
 \operation
 mov w0,#7
 ret
.size \name,.-\name
.endm
peak_prefix_ip_target peak_prefix_branch_sxtw_kill, "sxtw x16,w1"
peak_prefix_ip_target peak_prefix_branch_ubfm_kill, "ubfx x16,x1,#0,#8"
peak_prefix_ip_target peak_prefix_branch_sxtw_live, "sxtw x16,w16"
peak_prefix_ip_target peak_prefix_branch_bfm_live, "bfi x16,x1,#1,#1"
peak_prefix_ip_target peak_prefix_branch_x16_live, "mov x0,x16"
/* Calls do not recurse through callees, but their known target still must
 * not land inside an overwritten entry. These two are classifier-only. */
.p2align 4
.global peak_prefix_branch_bl_inside
.type peak_prefix_branch_bl_inside,%function
peak_prefix_branch_bl_inside:
 nop
0:
 nop
 nop
 nop
 bl 0b
 ret
.size peak_prefix_branch_bl_inside,.-peak_prefix_branch_bl_inside
.p2align 4
.global peak_prefix_branch_bl_boundary
.type peak_prefix_branch_bl_boundary,%function
peak_prefix_branch_bl_boundary:
 nop
 nop
 nop
 nop
0:
 bl 0b
 ret
.size peak_prefix_branch_bl_boundary,.-peak_prefix_branch_bl_boundary
.macro peak_literal name, instruction, offset
.p2align 4
.global \name
.type \name,%function
\name:
0:
 nop
 nop
 nop
 nop
 \instruction, 0b + \offset
 ret
.size \name,.-\name
.endm
peak_literal peak_prefix_literal_w, "ldr w9", 12
peak_literal peak_prefix_literal_x, "ldr x9", 8
peak_literal peak_prefix_literal_sw, "ldrsw x9", 8
peak_literal peak_prefix_literal_s, "ldr s0", 8
peak_literal peak_prefix_literal_d, "ldr d0", 8
peak_literal peak_prefix_literal_q, "ldr q0", 4
peak_literal peak_prefix_literal_boundary, "ldr q0", 16
.p2align 4
.global peak_prefix_literal_entry
.type peak_prefix_literal_entry,%function
peak_prefix_literal_entry:
0:
 ldr w9,0b
 nop
 nop
 nop
 ret
.size peak_prefix_literal_entry,.-peak_prefix_literal_entry
.p2align 4
0:
 .quad 0
.global peak_prefix_literal_crossing
.type peak_prefix_literal_crossing,%function
peak_prefix_literal_crossing:
 ldr q0,0b
 nop
 nop
 nop
 ret
.size peak_prefix_literal_crossing,.-peak_prefix_literal_crossing
.p2align 4
.global peak_prefix_suffix_svc
.type peak_prefix_suffix_svc,%function
peak_prefix_suffix_svc:
 nop
 nop
 nop
 nop
 mov w8,#172 /* Linux AArch64 getpid */
 svc #0
 mov w0,#7
 ret
.size peak_prefix_suffix_svc,.-peak_prefix_suffix_svc
.p2align 4
.global peak_prefix_suffix_svc_live
.type peak_prefix_suffix_svc_live,%function
peak_prefix_suffix_svc_live:
 mov x16,x0
 nop
 nop
 nop
 svc #0
 mov x0,x17
 ret
.size peak_prefix_suffix_svc_live,.-peak_prefix_suffix_svc_live
/* A complete graph can still select and profile a genuine 4-byte patch. */
.p2align 4
.global peak_prefix_branch_safe4
.type peak_prefix_branch_safe4,%function
peak_prefix_branch_safe4:
 mov w8,w0
0:
 cmp w8,#2
 b.ge 1f
 mov w8,#3
 b 0b
1:
 mov w0,#7
 ret
.size peak_prefix_branch_safe4,.-peak_prefix_branch_safe4
/* A's reachable tail B has an eight-byte prologue across a cache boundary. */
.p2align 4
.global peak_order_a
.type peak_order_a,%function
peak_order_a:
 mov w8,w0
 nop
 cbz w8,1f
2:
 mov w0,#7
 ret
.size peak_order_a,.-peak_order_a
.balign 1024
.space 1020,0
.global peak_order_b
.type peak_order_b,%function
peak_order_b:
1:
 mov w0,#7
 ret
.size peak_order_b,.-peak_order_b
/* B's original backedge must remain visible when B is already hooked. */
.p2align 4
.global peak_order_back_a
.type peak_order_back_a,%function
peak_order_back_a:
 mov w8,w0
 nop
 cbz w8,1f
0:
 cmp w8,#2
 mov w0,#7
 ret
.size peak_order_back_a,.-peak_order_back_a
.space 512,0
.global peak_order_back_b
.type peak_order_back_b,%function
peak_order_back_b:
1:
 mov w8,#3
 b 0b
.size peak_order_back_b,.-peak_order_back_b
/* Occupied B starts inside A's nominal 16-byte patch. */
.p2align 4
.global peak_order_overlap_a
.type peak_order_overlap_a,%function
peak_order_overlap_a:
 mov w8,w0
 nop
.global peak_order_overlap_b
.type peak_order_overlap_b,%function
peak_order_overlap_b:
 mov w0,#7
 ret
.size peak_order_overlap_a,.-peak_order_overlap_a
.size peak_order_overlap_b,.-peak_order_overlap_b
/* This direct target must not enter an already active A's patch interior. */
.p2align 4
.global peak_order_interior_b
.type peak_order_interior_b,%function
peak_order_interior_b:
 mov w0,#7
 b 2b
.size peak_order_interior_b,.-peak_order_interior_b
.section .note.GNU-stack,"",%progbits
