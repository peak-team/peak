#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>

void startup_local_callable_loader_call(void);

static int
require_local_scope(const char* symbol)
{
    void* address;
    const char* error;

    dlerror();
    address = dlsym(RTLD_DEFAULT, symbol);
    error = dlerror();
    if (address != NULL || error == NULL) {
        fprintf(stderr, "%s_was_not_rtld_local\n", symbol);
        return 1;
    }
    return 0;
}

int
main(void)
{
    if (require_local_scope("peak_ifunc_target") != 0 ||
        require_local_scope("peak_notype_target") != 0) {
        return 2;
    }

    startup_local_callable_loader_call();
    fputs("startup_local_callable_ok\n", stderr);
    return 0;
}
