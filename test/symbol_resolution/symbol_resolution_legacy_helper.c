#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

__attribute__((constructor)) static void
peak_symbol_cli_constructor_marker(void)
{
    const char* marker = getenv("PEAK_SYMBOL_CLI_CONSTRUCTOR_MARKER");
    int descriptor;

    if (marker == NULL || marker[0] == '\0') {
        return;
    }
    descriptor = open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor >= 0) {
        (void)write(descriptor, "loaded\n", 7);
        (void)close(descriptor);
    }
}

__attribute__((noinline, visibility("default")))
int
peak_symbol_legacy_helper(int value)
{
    volatile int observed = value;

    return observed;
}
