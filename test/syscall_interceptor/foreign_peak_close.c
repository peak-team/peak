#include <errno.h>

/*
 * This DSO intentionally exports peak_close without exporting close.  When
 * loaded before libpeak, any internal peak_close@plt call is preempted here.
 */
__attribute__((visibility("default")))
int
peak_close(int fd)
{
    (void)fd;
    errno = EIO;
    return -1;
}
