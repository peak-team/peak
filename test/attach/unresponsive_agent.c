#include <errno.h>
#include <time.h>

/* Keep a real Core injection pending long enough for ancestry observation and
 * cancellation. The bounded delay never connects to PEAK's control socket. */
__attribute__((visibility("default"))) void
peak_attach_agent_main(const char *configuration, int *stay_resident)
{
    struct timespec remaining = {3, 0};
    (void)configuration;
    *stay_resident = 1;
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
    }
}
