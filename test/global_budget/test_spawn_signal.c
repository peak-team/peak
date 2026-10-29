#define _GNU_SOURCE
#include "../../src/global_budget_spawn.c"
#include <assert.h>
static volatile sig_atomic_t signals;
static void notice(int sig)
{
    (void)sig;
    ++signals;
}
int main(void)
{
    alarm(3);
    struct sigaction action = {.sa_handler = notice};
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGUSR1, &action, NULL));
    int channel[2];
    assert(!pipe(channel));
    pid_t parent = getpid(), writer = fork();
    assert(writer >= 0);
    if (!writer) {
        close(channel[0]);
        for (unsigned i = 0; i < 30; i++) {
            usleep(2000);
            assert(!kill(parent, SIGUSR1));
        }
        unsigned char ready = 0xa7;
        assert(write(channel[1], &ready, 1) == 1);
        close(channel[1]);
        _exit(0);
    }
    close(channel[1]);
    assert(wait_ready(channel[0]));
    close(channel[0]);
    int status;
    pid_t reaped;
    do {
        reaped = waitpid(writer, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    assert(reaped == writer && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(signals >= 5);
    puts("global_budget_spawn_signal_ok");
}
