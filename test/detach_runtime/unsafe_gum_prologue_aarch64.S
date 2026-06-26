    .text
    .align 2
    .global peak_unsafe_gum_arm64_ip_counter_target
    .type peak_unsafe_gum_arm64_ip_counter_target, %function
peak_unsafe_gum_arm64_ip_counter_target:
    mov x16, #0
1:
    ldr w8, [x0, x16]
    str w8, [x1, x16]
    add x16, x16, #4
    cmp x16, #256
    b.lo 1b
    ret
    .size peak_unsafe_gum_arm64_ip_counter_target, .-peak_unsafe_gum_arm64_ip_counter_target

    .align 2
    .global peak_unsafe_gum_arm64_ip_x17_counter_target
    .type peak_unsafe_gum_arm64_ip_x17_counter_target, %function
peak_unsafe_gum_arm64_ip_x17_counter_target:
    mov x17, #0
1:
    ldr w8, [x0, x17]
    str w8, [x1, x17]
    add x17, x17, #4
    cmp x17, #256
    b.lo 1b
    ret
    .size peak_unsafe_gum_arm64_ip_x17_counter_target, .-peak_unsafe_gum_arm64_ip_x17_counter_target

    .align 2
    .global peak_unsafe_gum_arm64_ip_cursor_target
    .type peak_unsafe_gum_arm64_ip_cursor_target, %function
peak_unsafe_gum_arm64_ip_cursor_target:
    mov x16, x0
    add x17, x0, #256
1:
    ldr w8, [x16], #4
    str w8, [x1], #4
    cmp x16, x17
    b.lo 1b
    ret
    .size peak_unsafe_gum_arm64_ip_cursor_target, .-peak_unsafe_gum_arm64_ip_cursor_target
