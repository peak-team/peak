#define _GNU_SOURCE
#define PEAK_GLOBAL_BUDGET_NATIVE 1
#include "../../src/global_budget.c"
#include <assert.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>

static void wait_active(void)
{
    uint64_t until = ns_now(CLOCK_MONOTONIC) + UINT64_C(2000000000);
    while (!peak_global_budget_active() && ns_now(CLOCK_MONOTONIC) < until)
        usleep(1000);
    assert(peak_global_budget_active());
}

static void finish(void)
{
    peak_global_budget_retire();
    peak_global_budget_publish(&(PeakGlobalBudgetSample){
        .sequence = 4, .retiring = true, .final = true});
    assert(peak_global_budget_final_flush(2000));
    pid_t child = broker_pid;
    if (!rank_id)
        assert(peak_global_budget_view().broker_final_written);
    peak_global_budget_stop();
    if (child > 0)
        assert(waitpid(child, NULL, WNOHANG) < 0 && errno == ECHILD);
}

int main(int argc, char **argv)
{
    alarm(15);
    assert(argc == 3);
    assert(!setenv("PEAK_DETACH_HELPER", argv[1], 1));
    assert(!setenv("LD_PRELOAD", argv[2], 1));
    unsigned short port;
    assert(!peak_global_budget_listen(&port));
    uint64_t nonce = peak_global_budget_nonce();
    double began = seconds_now();
    int commands[2], ready[2];
    assert(!pipe(commands) && !pipe(ready));
    pid_t parent = getpid(), root = fork();
    assert(root >= 0);
    if (!root) {
        /* Keep the paused test rank bounded even if its test parent fails. */
        assert(!prctl(PR_SET_PDEATHSIG, SIGKILL));
        assert(getppid() == parent);
        close(commands[1]);
        close(ready[0]);
        assert(!peak_global_budget_start("127.0.0.1", port, 0, 2, .05, NULL));
        peak_global_budget_origin(10);
        peak_global_budget_publish(&(PeakGlobalBudgetSample){
            .sequence = 1, .initial_unexposed = true});
        wait_active();
        assert(broker_pid != getpid());
        assert(write(ready[1], &broker_pid, sizeof broker_pid) == sizeof broker_pid);
        char command;
        assert(read(commands[0], &command, 1) == 1);
        peak_global_budget_startup(.001);
        assert(write(ready[1], "S", 1) == 1);
        assert(read(commands[0], &command, 1) == 1);
        peak_global_budget_physical_targets(1);
        peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence = 2, .attached_targets = 1});
        peak_global_budget_request_credit(400000000);
        assert(write(ready[1], "C", 1) == 1);
        assert(read(commands[0], &command, 1) == 1);
        finish();
        _exit(0);
    }
    close(commands[0]);
    close(ready[1]);
    close_fd(&listener);
    peak_global_budget_set_nonce(nonce);
    assert(!peak_global_budget_start("127.0.0.1", port, 1, 2, .05, NULL));
    peak_global_budget_publish(&(PeakGlobalBudgetSample){
        .sequence = 1, .initial_unexposed = true});
    wait_active();
    pid_t broker;
    assert(read(ready[0], &broker, sizeof broker) == sizeof broker);
    peak_global_budget_request_credit(1000000);
    peak_global_budget_publish(&(PeakGlobalBudgetSample){
        .sequence = 2, .initial_unexposed = true});
    uint64_t flush_deadline = ns_now(CLOCK_MONOTONIC) + UINT64_C(2000000000);
    bool flushed;
    do { flushed = peak_global_budget_flush(100); }
    while (!flushed && ns_now(CLOCK_MONOTONIC) < flush_deadline);
    assert(flushed);
    PeakGlobalBudgetView view = peak_global_budget_view();
    assert(view.active && !view.startup_sealed && view.granted_total_ns == 0);
    assert(write(commands[1], "S", 1) == 1);
    char sealed;
    assert(read(ready[0], &sealed, 1) == 1);
    /* Pausing rank zero must not pause the independent budget broker. */
    assert(!kill(root, SIGSTOP));
    int status;
    assert(waitpid(root, &status, WUNTRACED) == root && WIFSTOPPED(status));
    uint64_t until = ns_now(CLOCK_MONOTONIC) + UINT64_C(1000000000);
    while (peak_global_budget_view().local_credit_ns < 1000000 &&
           ns_now(CLOCK_MONOTONIC) < until)
        usleep(1000);
    view = peak_global_budget_view();
    assert(view.active && view.startup_sealed && view.local_credit_ns >= 1000000);
    assert(view.broker_process_id == (uint64_t)broker && view.broker_cpu_seconds > 0);
    assert(view.global_spent_seconds >= .001 + view.broker_cpu_seconds);
    char maps[64], line[1024];
    snprintf(maps, sizeof maps, "/proc/%ld/maps", (long)broker);
    FILE *file = fopen(maps, "r");
    assert(file);
    while (fgets(line, sizeof line, file))
        assert(!strstr(line, "libpeak"));
    fclose(file);
    assert(!kill(root, SIGCONT));
    assert(write(commands[1], "C", 1) == 1);
    assert(read(ready[0], &sealed, 1) == 1);
    peak_global_budget_physical_targets(1);
    peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence = 3, .attached_targets = 1});
    peak_global_budget_request_credit(400000000);
    until = ns_now(CLOCK_MONOTONIC) + UINT64_C(2000000000);
    do { view = peak_global_budget_view(); usleep(1000); }
    while (view.reserved_seconds < .45 && ns_now(CLOCK_MONOTONIC) < until);
    /* Both ranks compete for one bank, rather than each receiving q*T. */
    assert(view.reserved_seconds >= .45);
    assert(view.global_spent_seconds + view.reserved_seconds +
               view.control_reserve_seconds + view.remaining_seconds <=
           view.budget_seconds + 1e-8);
    assert(view.budget_seconds <= .05 * (10 + seconds_now() - began));
    assert(write(commands[1], "F", 1) == 1);
    finish();
    assert(waitpid(root, &status, 0) == root && WIFEXITED(status) && !WEXITSTATUS(status));
    close(commands[1]);
    close(ready[0]);
    puts("global_budget_native_broker_ok");
}
