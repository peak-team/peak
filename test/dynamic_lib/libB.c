#include <stdio.h>

/* local (static) function in libB -- NOT exported */
static void b_static(void) {
    puts("libB: b_static() (static)");
}

/* exported function in libB */
void b_dynamic(void) {
    puts("libB: b_dynamic() start");
    b_static(); /* allowed: internal call */
    puts("libB: b_dynamic() end");
}
