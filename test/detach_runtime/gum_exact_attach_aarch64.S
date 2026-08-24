    .text
    .p2align 4

    .global peak_test_exact_entry_one
    .type peak_test_exact_entry_one, %function
peak_test_exact_entry_one:
    b peak_test_exact_dispatch
    .size peak_test_exact_entry_one, . - peak_test_exact_entry_one

    .p2align 4
    .global peak_test_exact_entry_two
    .type peak_test_exact_entry_two, %function
peak_test_exact_entry_two:
    b peak_test_exact_dispatch
    .size peak_test_exact_entry_two, . - peak_test_exact_entry_two

    .p2align 4
    .global peak_test_exact_dispatch
    .type peak_test_exact_dispatch, %function
peak_test_exact_dispatch:
    add w0, w0, #11
    ret
    .size peak_test_exact_dispatch, . - peak_test_exact_dispatch

    .section .note.GNU-stack,"",%progbits
