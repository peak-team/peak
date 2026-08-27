#include <stddef.h>
#include <stdio.h>

extern int peak_cuda_test_kernel_mapping_lifetime(size_t count);

int
main(void)
{
    const int result = peak_cuda_test_kernel_mapping_lifetime(4096);
    if (result != 0) {
        fputs("CUDA key-lifetime test hook failed\n", stderr);
        return 1;
    }
    puts("cuda_key_lifetime_ok");
    return 0;
}
