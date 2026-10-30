#define _GNU_SOURCE
#define PEAK_GLOBAL_BUDGET_NATIVE 1
#include "../../src/global_budget.c"
#include <assert.h>
#include <signal.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <spawn.h>
#include <sys/wait.h>

/* Exercise the v8 metadata payload from a capable application through the
 * MPI-free native broker, not just a local encoder/decoder round trip. */
void peak_mpi_budget_events_frontier(void) {}

extern char **environ;
static volatile sig_atomic_t owned_child = -1;
static int preload_writer = -1;

static void cleanup_signal(int signal_number)
{
    if (owned_child > 0) {
        pid_t child = (pid_t)owned_child;
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    }
    _exit(128 + signal_number);
}

static void preload_environment(const char *libraries)
{
#ifdef __APPLE__
    assert(!setenv("DYLD_INSERT_LIBRARIES", libraries, 1));
#else
    assert(!setenv("LD_PRELOAD", libraries, 1));
#endif
}

static int preload_probe(const char *broker, const char *library, const char *probe)
{
    int descriptors[2];
    assert(!pipe(descriptors));
    int output = fcntl(descriptors[1], F_DUPFD, 5);
    assert(output >= 5);
    preload_writer = output;
    close(descriptors[1]);
    char value[32];
    snprintf(value, sizeof value, "%d", output);
    assert(!setenv("BUDGET_TEST_PRELOAD_FD", value, 1));
    assert(!fcntl(descriptors[0], F_SETFL, O_NONBLOCK));
    preload_environment(probe);
    pid_t child, observed;
    char *args[] = {(char *)broker, NULL};
    /* Calibrate the loader proof before testing PEAK's sanitized spawn. */
    assert(!posix_spawn(&child, broker, NULL, NULL, args, environ));
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 2);
    assert(read(descriptors[0], &observed, sizeof observed) == sizeof observed);
    assert(observed == child);
    char libraries[2 * PATH_MAX + 2];
    int length = snprintf(libraries, sizeof libraries, "%s:%s", probe, library);
    assert(length > 0 && length < sizeof libraries);
    preload_environment(libraries);
    /* The write end deliberately remains inheritable until all broker
     * launches finish; a leaked preload would write its PID before main. */
    return descriptors[0];
}

static void require_native(int probe)
{
    pid_t observed;
    assert(read(probe, &observed, sizeof observed) < 0 && errno == EAGAIN);
}

static void reject_late_fork(void)
{
    /* MPI providers may reject fork after initialization; broker launch must
     * use spawn without invoking application/provider atfork callbacks. */
    _exit(86);
}

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

static void send_sample(int fd, const Frame *sample)
{
    unsigned char wire[PEAK_GLOBAL_BUDGET_WIRE_SIZE];
    peak_global_budget_frame_encode(wire, sample);
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0; /* connect_host installed SO_NOSIGPIPE. */
#endif
    assert(send(fd, wire, sizeof wire, flags) == sizeof wire);
}

static void receive_reply(int fd, Frame *reply)
{
    unsigned char wire[PEAK_GLOBAL_BUDGET_WIRE_SIZE];
    assert(recv(fd, wire, sizeof wire, MSG_WAITALL) == sizeof wire);
    assert(peak_global_budget_frame_decode(reply, wire));
}

/* Exercise membership beyond the old fixed array without launching many MPI
 * processes. Every simulated member uses the real TCP framing and broker. */
static void cohort(unsigned count)
{
    unsigned short port;
    assert(!peak_global_budget_listen(&port));
    assert(!peak_global_budget_start("127.0.0.1", port, 0, count, .05, NULL));
    peak_global_budget_origin(10);
    peak_global_budget_startup(.001);
    peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence = 1, .closing_seconds = .003});
    int *sockets = calloc(count, sizeof(*sockets));
    assert(sockets);
    for (unsigned rank = 1; rank < count; rank++) {
        sockets[rank] = connect_host("127.0.0.1", port);
        assert(sockets[rank] >= 0);
        assert(!fcntl(sockets[rank], F_SETFL, 0));
        struct timeval timeout = {.tv_sec = 2};
        assert(!setsockopt(sockets[rank], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout));
        Frame sample = {.magic = MAGIC, .version = PROTOCOL_VERSION,
            .nonce = job_nonce, .rank = rank, .size = count, .message_sequence = 1,
            .sample = {.sequence = 1, .closing_seconds = .003}, .request_credit = rank == count - 1,
            .requested_ns = 1000000, .demand_generation = 1};
        send_sample(sockets[rank], &sample);
    }
    wait_active();
    if (count == 1) {
        int member = 0;
        peak_global_budget_mpi_context_register(0, 1, &member, 1);
        assert(peak_global_budget_mpi_trace_begin());
        PeakMpiBudgetEvent event = {.rank=0, .kind=PEAK_MPI_EVENT_FRONTIER,
            .context_owner=0, .comm=1, .seq=1};
        PeakMpiBudgetCostSnapshot cost = {0};
        peak_global_budget_mpi_event_submit(&event, &cost);
        peak_global_budget_mpi_frontend_charge(1000);
        peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence=2, .closing_seconds=.003});
        assert(peak_global_budget_flush(2000));
        assert(!peak_global_budget_view().metadata_closed);
        assert(peak_global_budget_mpi_events_enabled());

        peak_global_budget_request_credit(1000000);
        uint64_t until = ns_now(CLOCK_MONOTONIC) + UINT64_C(2000000000);
        while (peak_global_budget_view().local_credit_ns < 1000000 &&
               ns_now(CLOCK_MONOTONIC) < until) usleep(1000);
        assert(peak_global_budget_view().local_credit_ns >= 1000000);
        /* Exhaust the shared observation seed, then pay for real metadata
         * admission from the broker-funded wallet, with independent cash. */
        for (unsigned operation=0;operation<64;++operation) {
            assert(peak_global_budget_mpi_trace_begin());
            peak_global_budget_mpi_frontend_charge(1000);
        }
        assert(atomic_load(&consumed_frontend_ns)>0);
        assert(peak_global_budget_mpi_trace_begin());
        event.seq=2;
        peak_global_budget_mpi_event_submit(&event,&cost);
        peak_global_budget_mpi_frontend_charge(1000);
        peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence=3,.closing_seconds=.003});
        assert(peak_global_budget_flush(2000));
        assert(!peak_global_budget_view().metadata_closed);
        assert(atomic_load(&consumed_frontend_ns)+atomic_load(&available_ns)==atomic_load(&granted_ns));
        assert(peak_global_budget_mpi_trace_begin());
        event.kind=PEAK_MPI_EVENT_UNKNOWN;event.seq=3;
        event.closure_flags=PEAK_MPI_OPAQUE_LIVE;
        peak_global_budget_mpi_event_submit(&event,&cost);
        peak_global_budget_mpi_frontend_charge(1000);
        uint64_t opaque_until=ns_now(CLOCK_MONOTONIC)+UINT64_C(2000000000);
        while(!peak_global_budget_view().metadata_closed && ns_now(CLOCK_MONOTONIC)<opaque_until)usleep(1000);
        PeakGlobalBudgetView opaque=peak_global_budget_view();
        assert(opaque.active&&opaque.metadata_closed&&!opaque.accounting_invalid);
        metadata_settle_closed();
        assert(atomic_load(&settled_frontend_ns)==atomic_load(&consumed_frontend_ns));
        assert(!atomic_load(&metadata_tokens));

    } else {
        Frame reply;
        do {
            receive_reply(sockets[count - 1], &reply);
            assert(reply.rank == count - 1 && reply.size == count);
        } while (reply.view.granted_total_ns < 1000000);
        assert(reply.view.active && reply.view.startup_sealed);
    }
    for (unsigned rank = 1; rank < count; rank++) {
        Frame final = {.magic = MAGIC, .version = PROTOCOL_VERSION,
            .nonce = job_nonce, .rank = rank, .size = count, .message_sequence = 2,
            .sample = {.sequence = 2, .retiring = true, .final = true},
            .demand_generation = 1};
        send_sample(sockets[rank], &final);
    }
    finish();
    for (unsigned rank = 1; rank < count; rank++) {
        Frame reply;
        do {
            receive_reply(sockets[rank], &reply);
        } while (!reply.view.cohort_finalized);
        assert(reply.rank == rank && reply.view.acknowledged_sequence == 2);
        close(sockets[rank]);
    }
    free(sockets);
    printf("native_cohort=%u settled_and_reaped=1\n", count);
}

/* Larger unpaid requests during one debt episode are not new exposure.
 * Keep cold hooks physically present while acknowledging each STOP. */
static void coalesced_debt(void)
{
    unsigned short port;
    assert(!peak_global_budget_listen(&port));
    assert(!peak_global_budget_start("127.0.0.1", port, 0, 2, .05, NULL));
    peak_global_budget_origin(1);
    peak_global_budget_startup(1);
    peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence = 1, .closing_seconds = .003, .retiring = true});
    int socket = connect_host("127.0.0.1", port);
    assert(socket >= 0 && !fcntl(socket, F_SETFL, 0));
    struct timeval timeout = {.tv_sec = 2};
    assert(!setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout));
    Frame sample = {.magic = MAGIC, .version = PROTOCOL_VERSION, .nonce = job_nonce,
        .rank = 1, .size = 2, .message_sequence = 1, .request_reply = true,
        .request_credit = true, .requested_ns = 1000000, .minimum_ns=1000000, .demand_generation = 1,
        .sample = {.sequence = 1, .closing_seconds = .003, .attached_targets = 2}};
    send_sample(socket, &sample);
    Frame reply;
    do { receive_reply(socket, &reply); } while (!reply.view.wave);
    assert(reply.view.wave == 1 && reply.view.granted_total_ns == 0);
    sample.sample.completed_wave = 1;
    for (unsigned generation = 2; generation <= 9; ++generation) {
        sample.message_sequence++;
        sample.sample.sequence++;
        sample.demand_generation = generation;
        sample.requested_ns = generation * UINT64_C(1000000);
        sample.minimum_ns=sample.requested_ns;
        sample.sample.control_seconds = generation * .00001;
        send_sample(socket, &sample);
        do { receive_reply(socket, &reply); }
        while (reply.view.acknowledged_sequence < sample.sample.sequence ||
               reply.view.root_completed_wave < 1);
        assert(reply.view.wave == 1 && reply.view.granted_total_ns == 0);
    }
    uint64_t quiet_decision = reply.view.decision_sequence;
    double quiet_cpu = reply.view.broker_cpu_seconds;
    usleep(250000);
    sample.message_sequence++;
    sample.sample.sequence++;
    send_sample(socket, &sample);
    do { receive_reply(socket, &reply); }
    while (reply.view.acknowledged_sequence < sample.sample.sequence);
    uint64_t quiet_replies = reply.view.decision_sequence - quiet_decision;
    double quiet_cpu_delta = reply.view.broker_cpu_seconds - quiet_cpu;
    assert(reply.view.wave == 1 && quiet_replies <= 32 && quiet_cpu_delta < .02);
    printf("coalesced_debt_idle_seconds=.25 broker_cpu_seconds=%.9g decisions=%llu\n",
        quiet_cpu_delta, (unsigned long long)quiet_replies);
    /* Real earned headroom now funds a positive lease and rearms STOP. */
    peak_global_budget_origin(100);
    do { receive_reply(socket, &reply); } while (!reply.view.granted_total_ns);
    assert(reply.view.wave == 1);
    uint64_t granted = reply.view.granted_total_ns;
    /* The real observed failure: partial desired-runway funding is not
     * insolvency while the immediate next quantum is fully reserved. */
    sample.message_sequence++;sample.sample.sequence++;sample.demand_generation++;
    sample.sample.profile_seconds=3.95;sample.requested_ns=212000000;sample.minimum_ns=10000;
    send_sample(socket,&sample);
    do { receive_reply(socket,&reply); } while(reply.view.acknowledged_sequence<sample.sample.sequence);
    assert(reply.view.wave==1 && reply.view.granted_total_ns>granted &&
        reply.view.granted_total_ns<212000000);
    granted=reply.view.granted_total_ns;
    sample.message_sequence++;
    sample.sample.sequence++;
    sample.sample.profile_seconds = 10;
    sample.consumed_profile_ns = granted;
    sample.demand_generation++;
    sample.requested_ns = 20000000;sample.minimum_ns=20000000;
    send_sample(socket, &sample);
    do { receive_reply(socket, &reply); } while (reply.view.wave < 2);
    assert(reply.view.wave == 2 && reply.view.root_completed_wave == 1);
    /* A repeated old DONE cannot acknowledge the newly funded wave. */
    sample.message_sequence++;
    sample.sample.sequence++;
    send_sample(socket, &sample);
    do { receive_reply(socket, &reply); }
    while (reply.view.acknowledged_sequence < sample.sample.sequence);
    assert(reply.view.wave == 2 && reply.view.root_completed_wave == 1);
    sample.message_sequence++;
    sample.sample.sequence++;
    sample.sample.completed_wave = 2;
    send_sample(socket, &sample);
    do { receive_reply(socket, &reply); } while (reply.view.root_completed_wave < 2);
    assert(reply.view.wave == 2);
    sample.message_sequence++;
    sample.sample.sequence++;
    sample.sample.attached_targets = 0;
    sample.sample.retiring = sample.sample.final = true;
    sample.request_credit = false;
    send_sample(socket, &sample);
    finish();
    do { receive_reply(socket, &reply); } while (!reply.view.cohort_finalized);
    close(socket);
    puts("native_debt_unfunded_coalesced_positive_credit_rearmed=1");
}

int main(int argc, char **argv)
{
    signal(SIGALRM, cleanup_signal);
    signal(SIGABRT, cleanup_signal);
    signal(SIGTERM, cleanup_signal);
    signal(SIGINT, cleanup_signal);
    alarm(15);
    assert(argc == 4);
    assert(!setenv("PEAK_GLOBAL_BUDGET_BROKER", argv[1], 1));
    int probe = preload_probe(argv[1], argv[2], argv[3]);
    cohort(1);
    cohort(65);
    coalesced_debt();
    require_native(probe);
    unsigned short port;
    assert(!peak_global_budget_listen(&port));
    uint64_t nonce = peak_global_budget_nonce();
    double began = seconds_now();
    int commands[2], ready[2];
    assert(!pipe(commands) && !pipe(ready));
    pid_t parent = getpid(), root = fork();
    assert(root >= 0);
    if (root > 0) owned_child = root;
    assert(!pthread_atfork(reject_late_fork, NULL, NULL));
    if (!root) {
        /* Keep the paused test rank bounded even if its test parent fails. */
#ifdef __linux__
        assert(!prctl(PR_SET_PDEATHSIG, SIGKILL));
#endif
        assert(getppid() == parent);
        close(commands[1]);
        close(ready[0]);
        assert(!peak_global_budget_start("127.0.0.1", port, 0, 2, .05, NULL));
        peak_global_budget_origin(10);
        peak_global_budget_publish(&(PeakGlobalBudgetSample){
            .sequence = 1, .closing_seconds = .003, .initial_unexposed = true});
        wait_active();
        assert(broker_pid != getpid());
        assert(write(ready[1], &broker_pid, sizeof broker_pid) == sizeof broker_pid);
        char command;
        assert(read(commands[0], &command, 1) == 1);
        peak_global_budget_startup(.001);
        /* Publish only after the independent broker acknowledges the seal.
         * Stopping this rank before that ACK would prevent correct admission. */
        uint64_t seal_deadline = ns_now(CLOCK_MONOTONIC) + UINT64_C(2000000000);
        while (!peak_global_budget_view().startup_sealed &&
               ns_now(CLOCK_MONOTONIC) < seal_deadline) usleep(1000);
        assert(peak_global_budget_view().startup_sealed);
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
        .sequence = 1, .closing_seconds = .003, .initial_unexposed = true});
    wait_active();
    pid_t broker;
    assert(read(ready[0], &broker, sizeof broker) == sizeof broker);
    peak_global_budget_request_credit(1000000);
    peak_global_budget_publish(&(PeakGlobalBudgetSample){
        .sequence = 2, .closing_seconds = .003, .initial_unexposed = true});
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
    view = peak_global_budget_view();
    uint64_t granted_before_pause = view.granted_total_ns;
    /* The pre-seal proof requested a detached recovery lease. Return that
     * unused lease to start a fresh demand, rather than coalescing its target. */
    peak_global_budget_return_idle();
    peak_global_budget_request_credit(2000000);
    uint64_t until = ns_now(CLOCK_MONOTONIC) + UINT64_C(1000000000);
    do {
        view = peak_global_budget_view();
        if (view.local_credit_ns >= 2000000 && view.granted_total_ns > granted_before_pause)
            break;
        usleep(1000);
    } while (ns_now(CLOCK_MONOTONIC) < until);
    assert(view.active && view.startup_sealed && view.local_credit_ns >= 2000000);
    assert(view.granted_total_ns > granted_before_pause);
    assert(view.broker_process_id == (uint64_t)broker && view.broker_cpu_seconds > 0);
    assert(view.global_spent_seconds >= .001 + view.broker_cpu_seconds);
#ifdef __linux__
    char maps[64], line[1024];
    snprintf(maps, sizeof maps, "/proc/%ld/maps", (long)broker);
    FILE *file = fopen(maps, "r");
    assert(file);
    while (fgets(line, sizeof line, file))
        assert(!strstr(line, "libpeak"));
    fclose(file);
#endif
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
    owned_child = -1;
    require_native(probe);
    close(probe);
    close(preload_writer);
    close(commands[1]);
    close(ready[0]);
    puts("global_budget_native_broker_ok");
}
