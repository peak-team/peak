#define _GNU_SOURCE
#define PEAK_GLOBAL_BUDGET_NATIVE 1
#include "../../src/global_budget.c"
#include <assert.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
static void wait_active(void)
{
    uint64_t end = ns_now(CLOCK_MONOTONIC) + UINT64_C(1000000000);
    while (!peak_global_budget_active() && ns_now(CLOCK_MONOTONIC) < end)
        usleep(1000);
    assert(peak_global_budget_active());
}
static void *thread_start(void *data)
{
    unsigned short port = *(unsigned short *)data;
    assert(!peak_global_budget_start("127.0.0.1", port, 0, 1, .05, NULL));
    return NULL;
}
int main(int argc, char **argv)
{
    alarm(15);
    assert(!prctl(PR_SET_CHILD_SUBREAPER, 1));
    assert(argc == 2);
    /* Native mode without the production listener guard must not auto-return. */
    assert(!idle_return_commit((PhysicalIdleSnapshot){0, true}, 10000000, 1000000));
    assert(!setenv("PEAK_DETACH_HELPER", argv[1], 1));
    assert(!setenv("PEAK_BROKER_ENV_MUST_BE_REMOVED", "yes", 1));
    assert(!setenv("LD_PRELOAD", "/not/loaded/by/native/broker.so", 1));
    assert(!setenv("LD_AUDIT", "/also/not/loaded.so", 1));
    unsigned short port = 0;
    assert(!peak_global_budget_listen(&port));
    uint64_t nonce = peak_global_budget_nonce();
    int communication[2];
    assert(!pipe(communication));
    pid_t root = fork();
    assert(root >= 0);
    if (!root) {
        close(communication[0]);
        assert(!peak_global_budget_start("127.0.0.1", port, 0, 2, .05, NULL));
        peak_global_budget_origin(10);
        peak_global_budget_startup(.001);
        peak_global_budget_publish(
            &(PeakGlobalBudgetSample){.sequence = 1, .initial_unexposed = true});
        wait_active();
        assert(broker_pid > 0 && broker_pid != getpid());
        assert(write(communication[1], &broker_pid, sizeof broker_pid) == sizeof broker_pid);
        usleep(500000); /* Parent pauses this client while the independent broker works. */
        peak_global_budget_retire();
        peak_global_budget_publish(&(PeakGlobalBudgetSample){
            .sequence = 2, .retiring = true, .final = true, .initial_unexposed = true});
        assert(peak_global_budget_final_flush(2000));
        assert(peak_global_budget_view().broker_final_written);
        pid_t broker = broker_pid;
        peak_global_budget_stop();
        assert(waitpid(broker, NULL, WNOHANG) < 0 && errno == ECHILD);
        _exit(0);
    }
    close(communication[1]);
    close_fd(&listener);
    peak_global_budget_set_nonce(nonce);
    assert(!peak_global_budget_start("127.0.0.1", port, 1, 2, .05, NULL));
    peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence = 1, .initial_unexposed = true});
    wait_active();
    pid_t broker;
    assert(read(communication[0], &broker, sizeof broker) == sizeof broker);
    close(communication[0]);
    char file[80];
    snprintf(file, sizeof file, "/proc/%ld/environ", (long)broker);
    FILE *f = fopen(file, "rb");
    assert(f);
    char env[65536];
    size_t bytes = fread(env, 1, sizeof(env) - 1, f);
    fclose(f);
    env[bytes] = 0;
    for (size_t i = 0; i < bytes;) {
        assert(strncmp(env + i, "PEAK_", 5) && strncmp(env + i, "LD_PRELOAD=", 11) &&
               strncmp(env + i, "LD_AUDIT=", 9));
        i += strlen(env + i) + 1;
    }
    snprintf(file, sizeof file, "/proc/%ld/maps", (long)broker);
    f = fopen(file, "r");
    assert(f);
    while (fgets(env, sizeof env, f))
        assert(!strstr(env, "libpeak") && !strstr(env, "frida"));
    fclose(f);
    assert(!kill(root, SIGSTOP));
    int status;
    assert(waitpid(root, &status, WUNTRACED) == root && WIFSTOPPED(status));
    uint64_t start = ns_now(CLOCK_MONOTONIC);
    peak_global_budget_request_credit(1000000);
    while (!peak_global_budget_view().local_credit_ns &&
           ns_now(CLOCK_MONOTONIC) - start < UINT64_C(200000000))
        usleep(1000);
    PeakGlobalBudgetView v = peak_global_budget_view();
    assert(v.active && v.local_credit_ns >= 1000000 && v.broker_cpu_seconds > 0 &&
           v.broker_process_id == (uint64_t)broker);
    assert(v.global_spent_seconds >= v.broker_cpu_seconds + .001);
    assert(!kill(root, SIGCONT));
    peak_global_budget_retire();
    peak_global_budget_publish(&(PeakGlobalBudgetSample){
        .sequence = 2, .retiring = true, .final = true, .initial_unexposed = true});
    assert(peak_global_budget_final_flush(2000));
    peak_global_budget_stop();
    assert(waitpid(root, &status, 0) == root && WIFEXITED(status) && !WEXITSTATUS(status));
    /* Missing executable is bounded and fail closed, with no child to reap. */
    assert(!setenv("PEAK_DETACH_HELPER", "/definitely/not/a/helper", 1));
    assert(!peak_global_budget_listen(&port));
    assert(peak_global_budget_start("127.0.0.1", port, 0, 1, .05, NULL) < 0);
    peak_global_budget_stop();
    /* Executable that never sends the readiness proof cannot start a session. */
    assert(!setenv("PEAK_DETACH_HELPER", "/bin/false", 1));
    assert(!peak_global_budget_listen(&port));
    assert(peak_global_budget_start("127.0.0.1", port, 0, 1, .05, NULL) < 0);
    peak_global_budget_stop();
    assert(waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);
    /* A live, non-FINAL broker death degrades every client and is reaped. */
    assert(!setenv("PEAK_DETACH_HELPER", argv[1], 1));
    assert(!peak_global_budget_listen(&port));
    assert(!peak_global_budget_start("127.0.0.1", port, 0, 1, .05, NULL));
    peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence = 1});
    wait_active();
    assert(!kill(broker_pid, SIGKILL));
    uint64_t expiry = ns_now(CLOCK_MONOTONIC) + UINT64_C(500000000);
    while (peak_global_budget_active() && ns_now(CLOCK_MONOTONIC) < expiry)
        usleep(1000);
    assert(!peak_global_budget_active());
    peak_global_budget_stop();
    assert(waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);
    /* Process-parent polling terminates an orphan; subreaper proves exit. */
    assert(!pipe(communication));
    root = fork();
    assert(root >= 0);
    if (!root) {
        close(communication[0]);
        assert(!peak_global_budget_listen(&port));
        assert(!peak_global_budget_start("127.0.0.1", port, 0, 1, .05, NULL));
        assert(write(communication[1], &broker_pid, sizeof broker_pid) == sizeof broker_pid);
        _exit(0);
    }
    close(communication[1]);
    assert(read(communication[0], &broker, sizeof broker) == sizeof broker);
    close(communication[0]);
    assert(waitpid(root, &status, 0) == root);
    expiry = ns_now(CLOCK_MONOTONIC) + UINT64_C(1000000000);
    pid_t reaped = 0;
    while ((reaped = waitpid(broker, &status, WNOHANG)) == 0 && ns_now(CLOCK_MONOTONIC) < expiry)
        usleep(1000);
    assert(reaped == broker && WIFEXITED(status) && !WEXITSTATUS(status));
    /* The spawning application pthread may exit while its rank stays alive. */
    assert(!peak_global_budget_listen(&port));
    pthread_t starter;
    assert(!pthread_create(&starter, NULL, thread_start, &port));
    assert(!pthread_join(starter, NULL));
    usleep(150000);
    assert(!kill(broker_pid, 0));
    peak_global_budget_origin(10);
    peak_global_budget_startup(0);
    peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence = 1, .initial_unexposed = true});
    wait_active();
    peak_global_budget_request_credit(1000000);
    expiry = ns_now(CLOCK_MONOTONIC) + UINT64_C(500000000);
    while (peak_global_budget_view().local_credit_ns < 1000000 && ns_now(CLOCK_MONOTONIC) < expiry)
        usleep(1000);
    assert(peak_global_budget_view().local_credit_ns >= 1000000);
    peak_global_budget_retire();
    peak_global_budget_publish(&(PeakGlobalBudgetSample){
        .sequence = 2, .initial_unexposed = true, .retiring = true, .final = true});
    assert(peak_global_budget_final_flush(2000));
    peak_global_budget_stop();
    puts("global_budget_native_broker_ok");
}
