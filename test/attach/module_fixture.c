#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <sys/prctl.h>

int main(int argc, char **argv)
{
    if (argc != 2 || prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) != 0)
        return 2;
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL)
        return 3;
    int (*target)(int) = (int (*)(int))dlsym(handle, "peak_attach_plugin_target");
    if (target == NULL)
        return 4;
    puts("ready");
    fflush(stdout);
    if (getchar() != 'Q' || dlclose(handle) != 0)
        return 5;
    /* The fixture releases its only reference before checking that PEAK
     * still owns the patched entry and can handle subsequent calls. */
    handle = dlopen(argv[1], RTLD_NOW | RTLD_NOLOAD);
    if (handle == NULL || dlclose(handle) != 0)
        return 6;
    for (int i = 0; i < 10; ++i)
        if (target(i) != i + 1)
            return 7;
    return 0;
}
