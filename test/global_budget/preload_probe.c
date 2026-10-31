#include <stdlib.h>
#include <unistd.h>

/* Observable only if a loader actually honors the test's preload variable. */
__attribute__((constructor)) static void loaded(void)
{
    const char *value = getenv("BUDGET_TEST_PRELOAD_FD");
    if (value) {
        pid_t pid = getpid();
        ssize_t written = write(atoi(value), &pid, sizeof pid);
        (void)written;
    }
}
