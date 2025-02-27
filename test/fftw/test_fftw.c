#include <stdio.h>
#include <stdlib.h>

double rfftwnd_print_plan() {
    printf("Calling dfftw_destroy_plan_\n");
    double sum = 0;
    for (int i = 0; i < 1000000; i++) {
        sum += i * 0.000001;
    }
    printf("dfftw_destroy_plan_ completed\n");
    return sum;
}

double rfftwnd_threads_one_complex_to_real() {
    printf("Calling rfftwnd_threads_one_complex_to_real\n");
    double product = 1;
    for (int i = 1; i <= 100000; i++) {
        product *= 1.000001;
    }
    printf("rfftwnd_threads_one_complex_to_real completed\n");
    return product;
}

int main() {
    double sum_result = rfftwnd_print_plan();
    double product_result = rfftwnd_threads_one_complex_to_real();
    
    printf("rfftwnd_print_plan result: %f\n", sum_result);
    printf("rfftwnd_threads_one_complex_to_real result: %f\n", product_result);

    return 0;
}
