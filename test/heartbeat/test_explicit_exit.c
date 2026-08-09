#include <stdio.h>
#include <stdlib.h>

static void
explicit_exit_atexit_marker(void)
{
    puts("explicit exit atexit marker");
}

int
main(void)
{
    if (atexit(explicit_exit_atexit_marker) != 0) {
        return 1;
    }
    puts("explicit exit requested");
    exit(37);
}
