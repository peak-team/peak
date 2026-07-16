#include <sys/types.h>
#include <unistd.h>

__attribute__((visibility("default")))
pid_t peak_fork_constructor_child_pid = -1;

__attribute__((constructor))
static void
peak_fork_constructor(void)
{
    peak_fork_constructor_child_pid = fork();
    if (peak_fork_constructor_child_pid == 0) {
        alarm(5);
    }
}

__attribute__((visibility("default"), noinline))
void
peak_fork_constructor_target(void)
{
}
