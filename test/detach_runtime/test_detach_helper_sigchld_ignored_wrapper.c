#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char** argv)
{
    const char* real_helper = getenv("TEST_REAL_DETACH_HELPER");
    struct sigaction action;

    if (real_helper == NULL || real_helper[0] == '\0') {
        fprintf(stderr, "TEST_REAL_DETACH_HELPER is required\n");
        return 2;
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    action.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT;
    if (sigaction(SIGCHLD, &action, NULL) != 0) {
        perror("sigaction(SIGCHLD)");
        return 1;
    }

    execv(real_helper, argv);
    int saved_errno = errno;
    perror("execv(TEST_REAL_DETACH_HELPER)");
    return saved_errno == ENOENT ? 2 : 1;
}
