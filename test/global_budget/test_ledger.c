#define _GNU_SOURCE
static void reply_claim_hook(void);
static void admission_snapshot_hook(void);
#define PEAK_GLOBAL_BUDGET_TEST_REPLY_CLAIM_HOOK reply_claim_hook
#define PEAK_GLOBAL_BUDGET_TEST_ADMISSION_SNAPSHOT_HOOK admission_snapshot_hook
#include "../../src/global_budget.c"
#include "internal/global_budget_protocol.h"
#include <assert.h>

static bool reply_newer_during_claim;
static bool lower_admission_during_snapshot;
static void admission_snapshot_hook(void)
{
    if (!lower_admission_during_snapshot) return;
    lower_admission_during_snapshot=false;
    peak_global_budget_request_admission(1000000,3000000);
}
static void reply_claim_hook(void)
{
    if (!reply_newer_during_claim) return;
    reply_newer_during_claim=false;
    pthread_mutex_lock(&sample_mutex);request_reply_goal_locked(5);pthread_mutex_unlock(&sample_mutex);
}
static atomic_uint credit_wakes;
static atomic_bool admission_waiting, budget_wake_pending;
bool peak_global_budget_listener_admission_waiting(void) { return atomic_load(&admission_waiting); }
void peak_global_budget_listener_credit_ready(void)
{
    assert(atomic_load_explicit(&running, memory_order_acquire));
    atomic_fetch_add(&credit_wakes, 1);
    atomic_store(&budget_wake_pending, true);
}

static void check_wire_format(void)
{
    PeakGlobalBudgetFrame source, decoded;
    memset(&source, 0xa5, sizeof source);
    unsigned field_number = 1;
#define FILL_U32(field) source.field = UINT32_C(0x11220000) + field_number++;
#define FILL_U64(field) source.field = UINT64_C(0x0102030405060000) + field_number++;
#define FILL_BOOL(field) source.field = (field_number++ & 1) != 0;
#define FILL_F64(field) source.field = (double)field_number++ + .25;
    PEAK_GLOBAL_BUDGET_WIRE_FIELDS(FILL_U32, FILL_U64, FILL_BOOL, FILL_F64)
#undef FILL_U32
#undef FILL_U64
#undef FILL_BOOL
#undef FILL_F64
    source.magic = PEAK_GLOBAL_BUDGET_PROTOCOL_MAGIC;
    source.version = PEAK_GLOBAL_BUDGET_PROTOCOL_VERSION;
    source.rank = UINT32_C(0x01020304);
    source.size = UINT32_C(0x01020305);
    source.nonce = UINT64_C(0x0102030405060708);
    source.request_credit = true;
    source.root_origin_seconds = 1.0;
    source.sample.attached_targets = UINT32_C(0x11223344);
    source.view.active = true;
    source.sample.ignored_seconds = 1.0;
    source.sample.profile_inside_seconds = 2.0;
    source.consumed_frontend_ns = UINT64_C(0x0102030405060708);
    source.payload_words = 2;
    source.minimum_ns=UINT64_C(0x0102030405060708);
    source.sample.runtime_supported=true;
    source.sample.runtime_raw_ns=UINT64_C(0x3132333435363738);
    source.sample.runtime_outside_ns=UINT64_C(0x0102030405060708);
    source.sample.runtime_sum_ns=UINT64_C(0x1112131415161718);
    source.view.causal_confirmed_seconds = 1.0;
    source.view.causal_pending_seconds = 2.0;
    unsigned char wire[PEAK_GLOBAL_BUDGET_WIRE_SIZE], again[PEAK_GLOBAL_BUDGET_WIRE_SIZE];
    peak_global_budget_frame_encode(wire, &source);
    static const unsigned char prefix[] = {
        0x50, 0x47, 0x42, 0x31, 0, 0, 0, 8,
        1, 2, 3, 4, 1, 2, 3, 5
    };
    static const unsigned char nonce[] = {1, 2, 3, 4, 5, 6, 7, 8};
    static const unsigned char one[] = {0x3f, 0xf0, 0, 0, 0, 0, 0, 0};
    static const unsigned char targets[] = {0x11, 0x22, 0x33, 0x44};
    assert(sizeof wire == 563);
    assert(wire[538] == 1);
    static const unsigned char runtime_raw[]={0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38};
    static const unsigned char runtime_sum[]={0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18};
    assert(!memcmp(wire + 539, runtime_raw, sizeof runtime_raw));
    assert(!memcmp(wire + 547, nonce, sizeof nonce));
    assert(!memcmp(wire + 555, runtime_sum, sizeof runtime_sum));
    assert(!memcmp(wire, prefix, sizeof prefix));
    assert(!memcmp(wire + 40, nonce, sizeof nonce));
    assert(!memcmp(wire + 530, nonce, sizeof nonce));
    assert(wire[96] == 1 && wire[252] == 1);
    assert(!memcmp(wire + 80, nonce, sizeof nonce));
    assert(wire[95] == 2);
    assert(!memcmp(wire + 154, one, sizeof one));
    assert(!memcmp(wire + 385, one, sizeof one));
    assert(!memcmp(wire + 98, one, sizeof one));
    assert(!memcmp(wire + 234, targets, sizeof targets));
    /* Different host padding must never affect the serialized result. */
    memset(&decoded, 0x5a, sizeof decoded);
    assert(peak_global_budget_frame_decode(&decoded, wire));
    peak_global_budget_frame_encode(again, &decoded);
    assert(!memcmp(wire, again, sizeof wire));
    wire[7]++;
    assert(!peak_global_budget_frame_decode(&decoded, wire));

    PeakGlobalBudgetEndpoint endpoint = {.host = "mpi-node", .port = 0x1234,
        .nonce = UINT64_C(0x0102030405060708)}, restored;
    unsigned char address[PEAK_GLOBAL_BUDGET_ENDPOINT_WIRE_SIZE];
    peak_global_budget_endpoint_encode(address, &endpoint);
    assert(!memcmp(address, "mpi-node", sizeof "mpi-node"));
    assert(address[256] == 0x12 && address[257] == 0x34);
    assert(!memcmp(address + 258, nonce, sizeof nonce));
    peak_global_budget_endpoint_decode(&restored, address);
    assert(!strcmp(restored.host, endpoint.host));
    assert(restored.port == endpoint.port && restored.nonce == endpoint.nonce);
}

static void check_coalesced_partial_write(void)
{
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    int capacity=4096;
    assert(!setsockopt(sockets[0],SOL_SOCKET,SO_SNDBUF,&capacity,sizeof capacity));
    for(unsigned i=0;i<2;i++)assert(!fcntl(sockets[i],F_SETFL,fcntl(sockets[i],F_GETFL,0)|O_NONBLOCK));
    unsigned char expected[32768];
    for(size_t i=0;i<sizeof expected;i++)expected[i]=(unsigned char)(i*37);
    Peer sender={.fd=sockets[0],.pending=true,.outgoing={.magic=MAGIC,.version=PROTOCOL_VERSION,
        .payload_words=sizeof expected/8,.view={.cohort_finalized=true,.broker_final_written=true}}};
    sender.outgoing_payload=malloc(sizeof expected);assert(sender.outgoing_payload);
    memcpy(sender.outgoing_payload,expected,sizeof expected);
    Peer receiver={.fd=sockets[1]};
    assert(!write_frame(&sender));
    if(IO_CHUNK>PEAK_GLOBAL_BUDGET_WIRE_SIZE)assert(sender.sent>PEAK_GLOBAL_BUDGET_WIRE_SIZE);
    assert(sender.pending && !sender.final_sent && !sender.proof_sent);
    bool complete=false;
    for(unsigned attempt=0;attempt<100000 && (sender.pending || !complete);attempt++) {
        assert(!write_frame(&sender));
        int result=read_frame(&receiver);assert(result>=0);
        complete|=result==1;
    }
    assert(!sender.pending && complete && sender.final_sent && sender.proof_sent);
    assert(!memcmp(receiver.incoming_payload,expected,sizeof expected));
    read_frame_done(&receiver);close(sockets[0]);close(sockets[1]);
}

static void check_broker_readiness(void)
{
#if defined(__linux__)
    ServiceStorage *storage = service_storage_new(1);
    assert(storage);
    service_waiter_init(storage, true);
    assert(storage->epoll_fd >= 0);
    for (unsigned i = 0; i < 4; ++i) storage->ready[i].fd = -1;
    int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    int original = sockets[0];
    storage->ready[3] = (struct pollfd){original, POLLIN, 0};
    assert(!service_wait(storage, 0));
    assert(write(sockets[1], "a", 1) == 1);
    assert(service_wait(storage, 100) == 1 && (storage->ready[3].revents & POLLIN));
    char byte; assert(read(original, &byte, 1) == 1 && byte == 'a');
    storage->ready[3].events |= POLLOUT;
    assert(service_wait(storage, 0) == 1 && (storage->ready[3].revents & POLLOUT));
    storage->ready[3].events = POLLIN;
    assert(!service_wait(storage, 0)); /* No permanently writable busy loop. */
    assert(!shutdown(sockets[1], SHUT_WR));
    assert(service_wait(storage, 100) == 1 && (storage->ready[3].revents & POLLHUP));
    assert(read(original, &byte, 1) == 0);
    service_waiter_forget(storage, 3);
    close(sockets[0]); close(sockets[1]);
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    if (sockets[0] != original) {
        assert(dup2(sockets[0], original) == original);
        close(sockets[0]); sockets[0] = original;
    }
    storage->ready[3] = (struct pollfd){original, POLLIN, 0};
    assert(write(sockets[1], "b", 1) == 1);
    assert(service_wait(storage, 100) == 1 && (storage->ready[3].revents & POLLIN));
    assert(read(original, &byte, 1) == 1 && byte == 'b');
    /* Backend failure retains the original poll behavior, including readiness. */
    close(storage->epoll_fd);
    assert(write(sockets[1], "c", 1) == 1);
    assert(service_wait(storage, 100) == 1 && (storage->ready[3].revents & POLLIN));
    assert(storage->epoll_fd == -1);
    close(sockets[0]); close(sockets[1]);
    service_storage_free(storage);
#endif
}

static void check_metadata_prefetch(void)
{
    atomic_store(&owner,getpid());atomic_store(&running,true);
    atomic_store(&metadata_enabled,true);atomic_store(&local_retiring,false);
    atomic_store(&local_attached,0);atomic_store(&public_local_closing,.003);
    atomic_store(&low_water_ns,100000);metadata_prefetch_goal=0;
    metadata_prefetch(1000);
    uint64_t generation=atomic_load(&demand_generation);
    assert(generation&&atomic_load(&pressure_notice));
    assert(atomic_load(&requested_ns)==164000);
    assert(!atomic_load(&recovery_target_ns)&&atomic_load(&request_metadata));
    metadata_prefetch(1000);
    assert(atomic_load(&demand_generation)==generation);
    metadata_prefetch(2000);
    assert(atomic_load(&demand_generation)==generation+1);
    assert(atomic_load(&requested_ns)==228000);
    peak_global_budget_request_credit(1000000);
    assert(atomic_load(&recovery_target_ns)==1000000);
    assert(atomic_load(&requested_ns)==1000000);
    assert(atomic_load(&demand_generation)==generation+2);
    assert(!atomic_load(&request_metadata));
    acknowledge_request(generation+1,ns_now(CLOCK_MONOTONIC));
    assert(atomic_load(&pressure_notice)); /* Old metadata ACK is not profile proof. */
    assert(!atomic_load(&available_ns)&&!atomic_load(&consumed_frontend_ns));
    atomic_store(&metadata_enabled,false);metadata_prefetch(4000);
    assert(atomic_load(&demand_generation)==generation+2);
    double previous_rtt=atomic_load(&refill_rtt);
    atomic_store(&request_metadata,true);atomic_store(&request_recovery,false);
    uint64_t now=ns_now(CLOCK_MONOTONIC);
    atomic_store(&request_started_ns,now-1000000000);
    acknowledge_request(generation+2,now);
    assert(atomic_load(&refill_rtt)==previous_rtt); /* Funding wait is not profile RTT. */
    atomic_store(&running,false);atomic_store(&local_attached,0);
    atomic_store(&public_local_closing,0);atomic_store(&low_water_ns,0);
    atomic_store(&pressure_notice,false);atomic_store(&demand_generation,0);
    atomic_store(&requested_ns,0);atomic_store(&request_started_ns,0);
    atomic_store(&public_deadline,0);atomic_store(&recovery_target_ns,0);
    atomic_store(&request_metadata,false);atomic_store(&request_recovery,false);metadata_prefetch_goal=0;
}

static void check_reply_snapshot_order(void)
{
    atomic_store(&owner,getpid());atomic_store(&running,true);rank_id=0;world=1;
    atomic_store(&heartbeat_period,BACKGROUND_NS);atomic_store(&transport_period,BACKGROUND_NS);
    local_sample=(PeakGlobalBudgetSample){.sequence=2};
    Frame old=request_snapshot();old.sample.background_period_ns=local_background_period();
    pthread_mutex_lock(&sample_mutex);
    local_sample.sequence=3;request_reply_goal_locked(3);
    pthread_mutex_unlock(&sample_mutex);
    assert(!frame_reply_requested(old.sample.sequence));
    assert(atomic_load(&flush_requested));
    Peer client={.fd=-1,.outgoing=old,.last_send=ns_now(CLOCK_MONOTONIC)};
    assert(service_poll_timeout(&client,0,false,false,client.last_send)==0);
    assert(frame_reply_requested(3)&&!atomic_load(&flush_requested));
    pthread_mutex_lock(&sample_mutex);request_reply_goal_locked(4);pthread_mutex_unlock(&sample_mutex);
    assert(!frame_reply_requested(3)&&atomic_load(&flush_requested));
    reply_newer_during_claim=true;
    assert(!frame_reply_requested(4)&&atomic_load(&flush_requested));
    assert(frame_reply_requested(5)); /* New producer between load and CAS survives. */
    atomic_store(&reply_goal_sequence,0);atomic_store(&flush_requested,false);
    atomic_store(&heartbeat_period,0);atomic_store(&transport_period,0);
    local_sample=(PeakGlobalBudgetSample){0};atomic_store(&running,false);
}

static void check_required_vs_desired(void)
{
    Rank rank={.wants_credit=true,.registered=true,.credit_total_ns=17000000,
        .requested_ns=212000000,.minimum_ns=10000,.demand_generation=2,.actioned_generation=1,
        .latest={.attached_targets=1}};
    assert(!rank_minimum_unfunded(&rank));
    assert(!stop_needed(true,true,false,rank_minimum_unfunded(&rank),true,false));
    rank.minimum_ns=18000000;
    assert(rank_minimum_unfunded(&rank));
    assert(stop_needed(true,true,false,rank_minimum_unfunded(&rank),true,false));
    rank.minimum_ns=0;
    assert(stop_needed(true,true,true,false,true,false)); /* Actual debt still stops. */
    atomic_store(&owner,getpid());atomic_store(&running,true);atomic_store(&local_attached,1);
    atomic_store(&metadata_enabled,false);atomic_store(&minimum_target_ns,0);
    request_credit_internal(212000000,false,false);
    assert(!atomic_load(&minimum_target_ns));
    uint64_t generation=atomic_load(&demand_generation);
    peak_global_budget_request_credit(10000);
    assert(atomic_load(&minimum_target_ns)==10000);
    assert(atomic_load(&demand_generation)>generation);
    atomic_store(&debit_ns,4000);
    assert(atomic_load(&minimum_target_ns)-atomic_load(&debit_ns)==6000);
    peak_global_budget_return_idle();
    assert(!atomic_load(&minimum_target_ns));
    atomic_store(&debit_ns,0);atomic_store(&local_attached,0);atomic_store(&running,false);
    atomic_store(&demand_generation,0);
}
static void check_admission_revision(void)
{
    atomic_store(&owner,getpid());atomic_store(&running,true);atomic_store(&local_retiring,false);
    atomic_store(&local_attached,1);atomic_store(&debit_ns,0);
    atomic_store(&minimum_target_ns,2000000);atomic_store(&requested_ns,212000000);
    atomic_store(&public_local_closing,.003);atomic_store(&available_ns,17000000);
    peak_global_budget_request_admission(137000000,68535000);
    uint64_t generation=atomic_load(&demand_generation);
    assert(current_minimum(0)==137000000);
    peak_global_budget_request_admission(1000000,3000000);
    assert(atomic_load(&demand_generation)>generation);
    assert(current_minimum(0)==2000000); /* Concurrent profile demand survives. */
    acknowledge_request(generation,ns_now(CLOCK_MONOTONIC));
    assert(atomic_load(&pressure_notice));
    assert(atomic_load(&available_ns)==17000000 && !atomic_load(&debit_ns));
    assert(atomic_load(&requested_ns)==212000000); /* Desired forecast unchanged. */
    request_admission(4000000,0,false); /* Failed control cannot poison profile minimum. */
    assert(atomic_load(&minimum_target_ns)==2000000 && current_minimum(0)==4000000);
    peak_global_budget_ensure_admission(5000000,6000000);
    peak_global_budget_ensure_admission(1000000,1000000);
    assert(current_minimum(0)==5000000 && atomic_load(&admission_closing_ns)==6000000);
    peak_global_budget_request_admission(1000000,3000000);
    assert(current_minimum(0)==2000000);
    peak_global_budget_request_admission(0,0);
    assert(current_minimum(0)==2000000);
    assert(current_minimum(1500000)==500000);
    atomic_store(&minimum_target_ns,0);atomic_store(&requested_ns,0);
    peak_global_budget_request_admission(9000000,3000000);
    lower_admission_during_snapshot=true;
    Frame frame={0};frame_credit_demand(&frame,0);
    assert(frame.minimum_ns==9000000 && frame.requested_ns==9000000);
    assert(current_minimum(0)==1000000);
    peak_global_budget_request_admission(0,0);
    assert(!atomic_load(&pressure_notice));
    frame_credit_demand(&frame,0);
    assert(!frame.minimum_ns && !frame.requested_ns);
    atomic_store(&available_ns,0);atomic_store(&running,false);atomic_store(&local_attached,0);
    atomic_store(&pressure_notice,false);atomic_store(&demand_generation,0);
}

static void *spend_credit(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 1000; i++)
        assert(i & 1 ? peak_global_budget_try_spend_control(1000)
                     : peak_global_budget_try_spend(1000));
    return NULL;
}

static void check_credit_conservation(void)
{
    atomic_store(&owner, getpid());
    atomic_store(&running, true);
    atomic_store(&local_attached, 1);
    publish_view((PeakGlobalBudgetView){
        .active = true, .granted_total_ns = 10000000, .granted_total_seconds = .01});
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++)
        assert(!pthread_create(&threads[i], NULL, spend_credit, NULL));
    for (unsigned i = 0; i < 4; i++)
        assert(!pthread_join(threads[i], NULL));
    assert(atomic_load(&consumed_ns) == 2000000);
    assert(atomic_load(&consumed_control_ns) == 2000000);
    assert(atomic_load(&available_ns) == 6000000);
    peak_global_budget_return_idle();
    assert(atomic_load(&available_ns) == 0);
    assert(atomic_load(&returned_ns) == 6000000);
    assert(atomic_load(&consumed_ns) + atomic_load(&consumed_control_ns) +
               atomic_load(&returned_ns) == atomic_load(&granted_ns));
    peak_global_budget_return_idle();
    assert(atomic_load(&returned_ns) == 6000000);
    atomic_store(&running, false);
}

static void receive(Rank *rank, PeakGlobalBudgetSample sample)
{
    assert(valid_sample(&sample, rank));
    rank->latest = sample;
    rank->seen = true;
    rank->previous_cost = fmax(rank->previous_cost, ledger_cost(&sample, rank->profile_ns));
}

static void check_control_settlement(void)
{
    Rank rank = {.credit_total_ns = 6000000, .consumed_ns = 3000000,
                 .control_ns = 3000000, .reservation = .003};
    receive(&rank, (PeakGlobalBudgetSample){.sequence = 1, .closing_seconds = .003});
    assert(fabs(control_escrow(&rank) - .003) < 1e-12);
    /* The controller finishes after heartbeat captured its settlement mark.
     * Actual cost must not permanently absorb the still-pending prepayment. */
    receive(&rank, (PeakGlobalBudgetSample){.sequence = 2, .control_seconds = .003});
    assert(fabs(rank.previous_cost - .003) < 1e-12);
    assert(fabs(control_escrow(&rank) - .003) < 1e-12);
    receive(&rank, (PeakGlobalBudgetSample){.sequence = 3, .control_seconds = .003,
                                          .settled_control_credit_ns = 3000000});
    assert(fabs(rank.previous_cost - .003) < 1e-12);
    assert(control_escrow(&rank) == 0);
    assert(rank.credit_total_ns - rank.consumed_ns - rank.returned_ns == 3000000);
}

static void check_closing_quote_epoch(void)
{
    Rank rank = {.registered = true, .registration_exposed = true,
        .registration_sample_sequence = 10, .member_sample_sequence = 10,
        .closing_estimate = .06853546, .grace_exposed = true,
        .credit_total_ns = 1000, .control_ns = 1000, .consumed_ns = 1000};
    receive(&rank, (PeakGlobalBudgetSample){.sequence = 10,
        .control_seconds = .06853546, .closing_seconds = .002,
        .credit_exposure_known = true, .credit_exposed_targets = 1});
    assert(!release_closing_registration(&rank, 0));
    assert(closing_reserve_ns(&rank) >= 68535460);
    rank.latest.credit_exposed_targets = 0; /* Stale closed proof cannot reprice D. */
    assert(!release_closing_registration(&rank, 0));
    receive(&rank, (PeakGlobalBudgetSample){.sequence = 11,
        .control_seconds = .06853546, .closing_seconds = .002,
        .credit_exposure_known = false});
    assert(!release_closing_registration(&rank, 0));
    rank.latest.credit_exposure_known = true;
    assert(!release_closing_registration(&rank, 0)); /* Control still unsettled. */
    rank.latest.settled_control_credit_ns = 1000;
    assert(release_closing_registration(&rank, 0));
    assert(fabs(rank.previous_cost - .06853546) < 1e-12);
    assert(closing_reserve_ns(&rank) == 2000000);
    assert(rank.credit_total_ns == 1000 && rank.consumed_ns == 1000);
    rank.wants_credit = true; rank.requested_ns = 100000;
    rank.latest.attached_targets = 79; /* Protected cold hooks are not paid exposure. */
    CreditGrant fragment = grant_rank_credit(&rank, 2050000);
    assert(fragment.credit == 0 && !rank.registered && rank.credit_total_ns == 1000);
    CreditGrant grant = grant_rank_credit(&rank, 2100000);
    assert(grant.closing == 2000000 && grant.credit == 100000);
    assert(rank.credit_total_ns - rank.consumed_ns - rank.returned_ns == 100000);
    assert(fabs(rank.previous_cost - .06853546) < 1e-12);
}

static void pause_ms(unsigned ms)
{
    struct timespec delay = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}
static PeakGlobalBudgetView wait_view(uint64_t sequence, bool funded)
{
    for (unsigned i = 0; i < 500; ++i) {
        PeakGlobalBudgetView view = peak_global_budget_view();
        if (view.active && view.acknowledged_sequence >= sequence &&
            (!funded || view.granted_total_ns)) return view;
        pause_ms(2);
    }
    assert(!"global budget service did not respond");
    return (PeakGlobalBudgetView){0};
}
/* Ordered version of the startup race: heartbeat published while inactive,
 * then learns a long ordinary period after start. Its sticky lifecycle wake
 * must survive until the heartbeat reaches the condition-wait boundary. */
static void check_startup_handoff(void)
{
    PeakGlobalBudgetSample sample = {.sequence = 1, .management_seconds = .0001};
    assert(!atomic_load(&running));
    peak_global_budget_publish(&sample);
    assert(!local_sample.sequence);
    atomic_store(&budget_wake_pending, false);
    atomic_store(&admission_waiting, false);
    unsigned before = atomic_load(&credit_wakes);
    unsigned short port;
    assert(!peak_global_budget_listen(&port));
    assert(!peak_global_budget_start("127.0.0.1", port, 0, 1, .05, NULL));
    assert(atomic_load(&credit_wakes) == before + 1);
    BackgroundCost cost = {0};
    uint64_t period = background_period(&cost, 45568, 512, (double).05f);
    assert(period > UINT64_C(1000000000));
    atomic_store(&heartbeat_period, period);
    Peer idle = {0};
    assert(service_poll_timeout(&idle, 0, false, false, ns_now(CLOCK_MONOTONIC)) > 1000);
    /* Production heartbeat_wait_us checks this sticky predicate before wait;
     * no sleep or invented zero-cost HELLO is needed for the first sample. */
    assert(atomic_exchange(&budget_wake_pending, false));
    peak_global_budget_publish(&sample);
    PeakGlobalBudgetView ready = wait_view(1, false);
    assert(ready.active && ready.granted_total_ns == 0);
    peak_global_budget_stop();
}

static void check_background_rounds(void)
{
    BackgroundCost estimate = {0};
    uint64_t slow = background_period(&estimate, 100000, 128, .05);
    assert(slow > FRESH_NS); /* No hidden one-second or rank-count ceiling. */
    for (unsigned i = 0; i < 40; ++i)
        (void)background_period(&estimate, 1000, 128, .05);
    assert(background_period(&estimate, 1000, 128, .05) < slow / 4);

    unsigned short port = 0;
    assert(peak_global_budget_listen(&port) == 0);
    assert(peak_global_budget_start("127.0.0.1", port, 0, 2, .5, NULL) == 0);
    /* Measured ordinary heartbeat CPU backs off an otherwise unfunded rank. */
    uint64_t period = peak_global_budget_heartbeat_period_ns(62500000, 10000000);
    assert(period == UINT64_C(2000000000));
    peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence = 1, .management_seconds = 10});
    peak_global_budget_request_credit(1000000);
    Peer remote = {.fd = connect_host("127.0.0.1", port), .rank = 1};
    assert(remote.fd >= 0);
    Frame report = {.magic = MAGIC, .version = PROTOCOL_VERSION, .nonce = job_nonce,
        .rank = 1, .size = 2, .message_sequence = 1, .sample = {.sequence = 1}};
    remote.outgoing = report; remote.pending = true;
    assert(write_frame(&remote) == 0 && !remote.pending);
    PeakGlobalBudgetView first = wait_view(1, false);
    assert(first.background_period_ns == period);
    assert(first.freshness_ns == 4 * period);
    assert(first.granted_total_ns == 0);
    struct pollfd remote_ready = {.fd = remote.fd, .events = POLLIN};
    assert(poll(&remote_ready, 1, 100) == 1);
    assert(read_frame(&remote) == 1);
    remote.received = 0;
    /* A delivered ordinary sequence also must not force a broker round/ACK. */
    report.message_sequence++; report.sample.sequence++;
    remote.outgoing = report; remote.pending = true;
    assert(write_frame(&remote) == 0 && !remote.pending);
    assert(poll(&remote_ready, 1, 150) == 0);
    report.message_sequence++; report.request_reply = true;
    remote.outgoing = report; remote.pending = true;
    assert(write_frame(&remote) == 0 && !remote.pending);
    assert(poll(&remote_ready, 1, 100) == 1);
    assert(read_frame(&remote) == 1 && remote.incoming.view.acknowledged_sequence == 2);
    peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence = 2, .management_seconds = 10});
    pause_ms(150);
    PeakGlobalBudgetView idle = peak_global_budget_view();
    assert(idle.decision_sequence == first.decision_sequence);
    assert(idle.acknowledged_sequence == 1);
    /* Explicit settlement bypasses ordinary sequence coalescing. */
    assert(peak_global_budget_flush(100));
    assert(peak_global_budget_view().acknowledged_sequence == 2);
    close_fd(&remote.fd);
    peak_global_budget_stop();

    assert(peak_global_budget_listen(&port) == 0);
    assert(peak_global_budget_start("127.0.0.1", port, 0, 1, .5, NULL) == 0);
    (void)peak_global_budget_heartbeat_period_ns(125000000, 10000000);
    peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence = 1, .closing_seconds = .003});
    (void)wait_view(1, false);
    pause_ms(25); /* Earn the existing closing reservation before requesting. */
    unsigned prior_wakes = atomic_load(&credit_wakes);
    atomic_store(&admission_waiting, true);
    peak_global_budget_request_credit(1000000);
    PeakGlobalBudgetView funded = wait_view(1, true);
    for (unsigned i = 0; i < 100 && atomic_load(&credit_wakes) == prior_wakes; ++i) pause_ms(1);
    assert(atomic_load(&credit_wakes) > prior_wakes);
    /* Ordinary active-profile refills must not trigger a full heartbeat. */
    atomic_store(&admission_waiting, false);
    prior_wakes = atomic_load(&credit_wakes);
    peak_global_budget_physical_targets(1);
    peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence = 2, .attached_targets = 1, .closing_seconds = .003});
    assert(peak_global_budget_try_spend(1000));
    peak_global_budget_request_credit(2000000);
    PeakGlobalBudgetView refilled = {0};
    for (unsigned i = 0; i < 500; ++i) {
        refilled = peak_global_budget_view();
        if (refilled.granted_total_ns > funded.granted_total_ns) break;
        pause_ms(2);
    }
    assert(refilled.granted_total_ns > funded.granted_total_ns);
    assert(atomic_load(&credit_wakes) == prior_wakes);
    peak_global_budget_stop();
    PeakGlobalBudgetLocalBill bill = peak_global_budget_local_bill();
    assert(bill.written && bill.sample_sequence == 2 && bill.message_sequence > 0);
    assert(bill.profile_sample_seconds == 0 && bill.profile_debit_ns == 1000);
    assert(bill.profile_billed_seconds == .000001);
    assert(bill.total_seconds == bill.profile_billed_seconds + bill.control_seconds +
        bill.management_sent_seconds + bill.transport_seconds);
}

static void consume_queued(PeakGlobalBudgetCausal *model, Rank ranks[2], unsigned rank)
{
    uint64_t count;
    unsigned char *payload = metadata_batch(&count);
    assert(payload && count);
    Frame frame = {.rank=rank,.payload_words=count};
    BrokerMetadata metadata;
    assert(broker_metadata_init(&metadata,model,ranks,2));
    assert(consume_metadata(&metadata,&frame,payload));
    broker_metadata_clear(&metadata);
    free(payload);
}
static void causal_event(PeakGlobalBudgetCausal *model, Rank ranks[2], unsigned rank,
    uint64_t seq[2], uint64_t last[2], PeakMpiEventKind kind, PeakMpiEventOp op,
    uint64_t cost, uint64_t request, int peer)
{
    rank_id=rank;
    uint64_t id=request;
    PeakMpiBudgetEvent event={.rank=(int)rank,.seq=++seq[rank],.last_mpi_seq=last[rank],
        .kind=kind,.op=op,.context_owner=0,.comm=1,.round=1,.tag=7,.peer=peer,
        .request=request};
    if(kind==PEAK_MPI_EVENT_WAIT||kind==PEAK_MPI_EVENT_COMPLETE){event.requests=&id;event.count=1;}
    if(kind!=PEAK_MPI_EVENT_FRONTIER)last[rank]=seq[rank];
    PeakMpiBudgetCostSnapshot snapshot={.active_outside_ns=cost};
    peak_global_budget_mpi_event_submit(&event,&snapshot);
    id=UINT64_MAX; /* The async sink must already own the copied request set. */
    consume_queued(model,ranks,rank);
}
static void check_causal_wire_integration(void)
{
    atomic_store(&owner,getpid());atomic_store(&running,true);atomic_store(&metadata_enabled,true);
    atomic_store(&local_retiring,false);atomic_store(&frontend_cpu_ns,0);world=2;rank_id=0;
    PeakGlobalBudgetCausal *model=peak_global_budget_causal_new(2);assert(model);
    Rank ranks[2]={0};int members[2]={0,1};
    peak_global_budget_mpi_context_register(0,1,members,2);
    members[1]=0; /* Context membership must be copied as well. */
    consume_queued(model,ranks,0);
    uint64_t seq[2]={0},last[2]={0};
    peak_global_budget_causal_reserve_outside(model,0,1000000);
    peak_global_budget_causal_reserve_outside(model,1,1000000);
    PeakGlobalBudgetCausalView pending=peak_global_budget_causal_view(model);
    assert(pending.confirmed_ns==0&&pending.unresolved_ns==2000000);
    for(unsigned r=0;r<2;r++)causal_event(model,ranks,r,seq,last,PEAK_MPI_EVENT_POST,PEAK_MPI_OP_COLLECTIVE,1000000,1,0);
    PeakGlobalBudgetCausalView view=peak_global_budget_causal_view(model);
    assert(view.raw_cost_ns==2000000&&view.confirmed_ns==1000000&&view.unresolved_ns==0);
    for(unsigned r=0;r<2;r++)causal_event(model,ranks,r,seq,last,PEAK_MPI_EVENT_WAIT,PEAK_MPI_OP_COLLECTIVE,1000000,1,0);
    for(unsigned r=0;r<2;r++)causal_event(model,ranks,r,seq,last,PEAK_MPI_EVENT_COMPLETE,PEAK_MPI_OP_COLLECTIVE,1000000,1,0);
    view=peak_global_budget_causal_view(model);
    assert(view.confirmed_ns==1000000&&view.unresolved_ns==0&&!view.sum_mode);
    causal_event(model,ranks,0,seq,last,PEAK_MPI_EVENT_FRONTIER,PEAK_MPI_OP_SEND,1500000,0,1);
    causal_event(model,ranks,0,seq,last,PEAK_MPI_EVENT_POST,PEAK_MPI_OP_SEND,1500000,2,1);
    causal_event(model,ranks,1,seq,last,PEAK_MPI_EVENT_POST,PEAK_MPI_OP_RECV,1000000,2,0);
    causal_event(model,ranks,0,seq,last,PEAK_MPI_EVENT_WAIT,PEAK_MPI_OP_SEND,1500000,2,1);
    causal_event(model,ranks,1,seq,last,PEAK_MPI_EVENT_WAIT,PEAK_MPI_OP_RECV,1000000,2,0);
    causal_event(model,ranks,0,seq,last,PEAK_MPI_EVENT_COMPLETE,PEAK_MPI_OP_SEND,1500000,2,1);
    causal_event(model,ranks,1,seq,last,PEAK_MPI_EVENT_COMPLETE,PEAK_MPI_OP_RECV,1000000,2,0);
    causal_event(model,ranks,1,seq,last,PEAK_MPI_EVENT_FRONTIER,PEAK_MPI_OP_RECV,1800000,0,0);
    view=peak_global_budget_causal_view(model);
    assert(view.confirmed_ns==2300000&&view.unresolved_ns==0&&!view.sum_mode);
    PeakGlobalBudgetSample additive={.profile_seconds=.001,.ignored_seconds=.002};
    assert(fabs(ledger_cost(&additive,3000000)-.005)<1e-12);
    peak_global_budget_causal_free(model);metadata_clear();atomic_store(&running,false);
}

static void prefix_record(BrokerMetadata *metadata, unsigned rank,
    uint64_t seq[2], uint64_t last[2], PeakGlobalBudgetCausalKind kind,
    uint64_t round, uint64_t outside, uint64_t inside, unsigned certification)
{
    uint64_t record[21]={21,2,0,1,kind,PEAK_CAUSAL_COLLECTIVE,
        ++seq[rank],last[rank],round,0,0,rank,0,outside,inside,outside+inside,0,
        certification};
    if(kind!=PEAK_CAUSAL_FRONTIER)last[rank]=seq[rank];
    unsigned char payload[21*8],*output=payload;
    for(size_t i=0;i<21;++i)peak_budget_wire_put(&output,record[i],8);
    if(certification==(PEAK_MPI_CLOSED_WORLD_PREFIX|PEAK_MPI_ENTRY_FENCE))
        assert(payload[17*8+7]==3); /* Canonical flags word, independent byte value. */
    Frame frame={.rank=rank,.payload_words=21};
    assert(consume_metadata(metadata,&frame,payload));
}
static void runtime_record(BrokerMetadata *metadata, unsigned rank,
    uint64_t seq[2], uint64_t last[2], PeakGlobalBudgetCausalKind kind,
    PeakGlobalBudgetCausalOperation op, uint64_t raw, uint64_t outside, uint64_t sum)
{
    uint64_t count=(kind==PEAK_CAUSAL_WAIT || kind==PEAK_CAUSAL_COMPLETE)?1:0;
    uint64_t record[22]={21+count,2,0,1,kind,op,++seq[rank],last[rank],
        1,7,1,rank,1-rank,0,0,0,count,0};
    if(count)record[18]=1;
    record[18+count]=raw;record[19+count]=outside;record[20+count]=sum;
    if(kind!=PEAK_CAUSAL_FRONTIER)last[rank]=seq[rank];
    unsigned char payload[22*8],*output=payload;
    for(size_t i=0;i<record[0];i++)peak_budget_wire_put(&output,record[i],8);
    Frame frame={.rank=rank,.payload_words=record[0]};
    assert(consume_metadata(metadata,&frame,payload));
}
static uint64_t runtime_bill(PeakGlobalBudgetCausal *model, Rank ranks[2])
{
    PeakGlobalBudgetCausalView v=peak_global_budget_causal_view(model);
    uint64_t bill=ns_add(v.confirmed_ns,v.unresolved_ns);
    for(unsigned i=0;i<2;i++) {
        bill=ns_add(bill,ranks[i].runtime_sum);
        uint64_t represented=ns_add(ranks[i].runtime_outside,ranks[i].runtime_sum);
        if(ranks[i].runtime_raw>represented)bill=ns_add(bill,ranks[i].runtime_raw-represented);
    }
    return bill;
}
static void check_runtime_causal_cost(void)
{
    PeakGlobalBudgetCausal *model=peak_global_budget_causal_new(2);assert(model);
    Rank ranks[2]={0};BrokerMetadata metadata;uint64_t seq[2]={0},last[2]={0};
    assert(broker_metadata_init(&metadata,model,ranks,2));
    size_t members[]={0,1};assert(peak_global_budget_causal_context(model,0,1,members,2));
    /* Parallel MPI-free runtime: full raw20, causal10, no cash mutation. */
    for(unsigned r=0;r<2;r++)runtime_record(&metadata,r,seq,last,PEAK_CAUSAL_FRONTIER,PEAK_CAUSAL_SEND,10,10,0);
    assert(runtime_bill(model,ranks)==10 && ranks[0].runtime_raw+ranks[1].runtime_raw==20);
    assert(!ranks[0].credit_total_ns && !ranks[1].credit_total_ns);
    broker_metadata_clear(&metadata);peak_global_budget_causal_free(model);
    model=peak_global_budget_causal_new(2);memset(ranks,0,sizeof ranks);memset(seq,0,sizeof seq);memset(last,0,sizeof last);
    assert(model && broker_metadata_init(&metadata,model,ranks,2));
    assert(peak_global_budget_causal_context(model,0,1,members,2));
    /* Serial SEND -> RECV carries runtime cost across the real dependency. */
    runtime_record(&metadata,0,seq,last,PEAK_CAUSAL_POST,PEAK_CAUSAL_SEND,10,10,0);
    runtime_record(&metadata,0,seq,last,PEAK_CAUSAL_COMPLETE,PEAK_CAUSAL_SEND,10,10,0);
    runtime_record(&metadata,1,seq,last,PEAK_CAUSAL_POST,PEAK_CAUSAL_RECV,0,0,0);
    runtime_record(&metadata,1,seq,last,PEAK_CAUSAL_WAIT,PEAK_CAUSAL_RECV,0,0,0);
    runtime_record(&metadata,1,seq,last,PEAK_CAUSAL_COMPLETE,PEAK_CAUSAL_RECV,0,0,0);
    runtime_record(&metadata,1,seq,last,PEAK_CAUSAL_FRONTIER,PEAK_CAUSAL_RECV,10,10,0);
    assert(runtime_bill(model,ranks)==20);
    /* Raw-first unknown gap remains fully reserved; class-first supplies raw25
     * including a separate writer's5, even when the HB sample still says15. */
    ranks[0].runtime_raw=15;assert(runtime_bill(model,ranks)==25);
    runtime_record(&metadata,0,seq,last,PEAK_CAUSAL_FRONTIER,PEAK_CAUSAL_SEND,25,20,5);
    assert(ranks[0].runtime_raw==25 && ranks[0].runtime_sum==5);
    uint64_t before=runtime_bill(model,ranks);
    receive(&ranks[0],(PeakGlobalBudgetSample){.sequence=1,.runtime_supported=true,
        .runtime_raw_ns=15,.runtime_outside_ns=10,.runtime_sum_ns=5});
    assert(ranks[0].runtime_raw==25 && runtime_bill(model,ranks)==before);
    PeakGlobalBudgetSample raw_bill={.runtime_supported=true,.runtime_raw_ns=25,
        .management_seconds=15e-9};
    assert(fabs(ledger_cost(&raw_bill,0)-25e-9)<1e-20);
    raw_bill.runtime_supported=false;
    assert(fabs(ledger_cost(&raw_bill,0)-15e-9)<1e-20);
    /* An existing rejected-sequence fallback must reserve runtime as well as
     * profile: representation already advanced before submit rejects it. */
    --seq[0];
    runtime_record(&metadata,0,seq,last,PEAK_CAUSAL_FRONTIER,PEAK_CAUSAL_SEND,35,30,5);
    assert(metadata.invalid && runtime_bill(model,ranks)==before+10);
    broker_metadata_clear(&metadata);peak_global_budget_causal_free(model);
    model=peak_global_budget_causal_new(2);memset(ranks,0,sizeof ranks);memset(seq,0,sizeof seq);memset(last,0,sizeof last);
    assert(model && broker_metadata_init(&metadata,model,ranks,2));
    assert(peak_global_budget_causal_context(model,0,1,members,2));
    for(unsigned r=0;r<2;r++)prefix_record(&metadata,r,seq,last,PEAK_CAUSAL_MPI_ENTER,1,0,0,0);
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_MPI_EXIT,1,0,0,1);
    runtime_record(&metadata,0,seq,last,PEAK_CAUSAL_FRONTIER,PEAK_CAUSAL_SEND,25,20,5);
    assert(metadata.lanes[0].head && ranks[0].runtime_raw==25 && !ranks[0].runtime_outside);
    assert(runtime_bill(model,ranks)==25); /* Held OUTSIDE remains U, SUM stays SUM. */
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_MPI_EXIT,1,0,0,1);
    assert(!metadata.lanes[0].head && runtime_bill(model,ranks)==25);
    runtime_record(&metadata,1,seq,last,PEAK_CAUSAL_FRONTIER,PEAK_CAUSAL_SEND,20,20,0);
    assert(runtime_bill(model,ranks)==25 && ranks[0].runtime_raw+ranks[1].runtime_raw==45);
    broker_metadata_clear(&metadata);peak_global_budget_causal_free(model);
}
static void check_closed_cost_prefix(void)
{
    PeakGlobalBudgetCausal *model=peak_global_budget_causal_new(2);assert(model);
    Rank ranks[2]={0};BrokerMetadata metadata;
    assert(broker_metadata_init(&metadata,model,ranks,2));
    size_t members[]={0,1};assert(peak_global_budget_causal_context(model,0,1,members,2));
    uint64_t seq[2]={0},last[2]={0};
    for(unsigned r=0;r<2;r++){
        prefix_record(&metadata,r,seq,last,PEAK_CAUSAL_UNKNOWN,0,100+r*100,0,false);
        prefix_record(&metadata,r,seq,last,PEAK_CAUSAL_MPI_ENTER,1,100+r*100,0,false);
    }
    assert(peak_global_budget_causal_view(model).sum_mode);
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_MPI_EXIT,1,100,0,true);
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_FRONTIER,1,1000,50,false);
    assert(metadata.lanes[0].head&&metadata.checkpoints==0);
    assert(ranks[0].represented_outside==100&&ranks[0].represented_inside==50);
    assert(ranks[0].raw_profile-ranks[0].represented_outside-ranks[0].represented_inside==900);
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_MPI_EXIT,1,200,0,true);
    assert(metadata.checkpoints==1&&!metadata.lanes[0].head);
    assert(!peak_global_budget_causal_view(model).sum_mode);
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_FRONTIER,1,1100,0,false);
    PeakGlobalBudgetCausalView view=peak_global_budget_causal_view(model);
    assert(view.confirmed_ns==1200&&view.raw_cost_ns==2100&&view.unresolved_ns==0);
    /* A same-round noncertificate must release held data conservatively,
     * never turn the other rank's local certificate into a global cut. */
    for(unsigned r=0;r<2;r++){
        prefix_record(&metadata,r,seq,last,PEAK_CAUSAL_UNKNOWN,0,1000+r*100,0,false);
        prefix_record(&metadata,r,seq,last,PEAK_CAUSAL_MPI_ENTER,2,1000+r*100,0,false);
    }
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_MPI_EXIT,2,1000,0,true);
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_FRONTIER,2,2000,0,false);
    assert(metadata.lanes[0].head);
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_MPI_EXIT,2,1100,0,false);
    assert(metadata.checkpoints==1&&!metadata.cut_active&&!metadata.lanes[0].head);
    assert(peak_global_budget_causal_view(model).sum_mode);
    assert(ranks[0].represented_outside==2000);
    /* A newer certified round can replace an abandoned candidate without
     * recursive replay or admitting that rank's future records too early. */
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_MPI_ENTER,3,2000,0,false);
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_MPI_EXIT,3,2000,0,true);
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_MPI_ENTER,4,2100,0,false);
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_MPI_EXIT,4,2100,0,true);
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_MPI_ENTER,4,1200,0,false);
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_MPI_EXIT,4,1200,0,true);
    assert(metadata.checkpoints==2&&!metadata.cut_active);
    assert(!metadata.lanes[0].head&&!metadata.lanes[1].head);
    assert(!peak_global_budget_causal_view(model).sum_mode);
    broker_metadata_clear(&metadata);peak_global_budget_causal_free(model);
}

static void prefix_point(BrokerMetadata *metadata, unsigned rank,
    uint64_t seq[2],uint64_t last[2],PeakGlobalBudgetCausalKind kind,
    PeakGlobalBudgetCausalOperation operation,uint64_t cost)
{
    uint64_t record[22]={21,2,0,1,kind,operation,++seq[rank],last[rank],
        1,7,1,rank,1-rank,cost,0,cost,0,0,0,0,0,0};
    if(kind==PEAK_CAUSAL_WAIT||kind==PEAK_CAUSAL_COMPLETE){record[0]=22;record[16]=1;record[18]=1;}
    last[rank]=seq[rank];
    unsigned char payload[22*8],*output=payload;
    for(size_t i=0;i<record[0];++i)peak_budget_wire_put(&output,record[i],8);
    Frame frame={.rank=rank,.payload_words=record[0]};
    assert(consume_metadata(metadata,&frame,payload));
}
static void check_asymmetric_closed_prefix(void)
{
    PeakGlobalBudgetCausal *model=peak_global_budget_causal_new(2);assert(model);
    Rank ranks[2]={0};BrokerMetadata metadata;
    assert(broker_metadata_init(&metadata,model,ranks,2));
    size_t members[]={0,1};assert(peak_global_budget_causal_context(model,0,1,members,2));
    uint64_t seq[2]={0},last[2]={0};
    for(unsigned r=0;r<2;r++)prefix_record(&metadata,r,seq,last,PEAK_CAUSAL_UNKNOWN,0,0,0,false);
    /* Nonbarrier Bcast: rank0 returns, then sends to rank1 BEFORE rank1
     * enters Bcast. Identical collective EXITs do not define a closed cut. */
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_MPI_ENTER,1,0,0,false);
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_MPI_EXIT,1,0,0,true);
    prefix_point(&metadata,0,seq,last,PEAK_CAUSAL_POST,PEAK_CAUSAL_AMBIGUOUS_SEND,10);
    prefix_point(&metadata,0,seq,last,PEAK_CAUSAL_WAIT,PEAK_CAUSAL_AMBIGUOUS_SEND,10);
    prefix_point(&metadata,0,seq,last,PEAK_CAUSAL_COMPLETE,PEAK_CAUSAL_AMBIGUOUS_SEND,10);
    prefix_point(&metadata,1,seq,last,PEAK_CAUSAL_POST,PEAK_CAUSAL_RECV,0);
    prefix_point(&metadata,1,seq,last,PEAK_CAUSAL_WAIT,PEAK_CAUSAL_RECV,0);
    prefix_point(&metadata,1,seq,last,PEAK_CAUSAL_COMPLETE,PEAK_CAUSAL_RECV,0);
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_FRONTIER,0,10,0,false);
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_MPI_ENTER,1,10,0,false);
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_MPI_EXIT,1,10,0,true);
    assert(!metadata.checkpoints&&!metadata.cut_active&&!metadata.lanes[0].head);
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_FRONTIER,1,20,0,false);
    assert(peak_global_budget_causal_view(model).effective_ns>=30);
    /* Matching the held send permits a later genuinely closed prefix. */
    for(unsigned r=0;r<2;r++){
        uint64_t cost=r?20:10;
        prefix_record(&metadata,r,seq,last,PEAK_CAUSAL_MPI_ENTER,2,cost,0,false);
        prefix_record(&metadata,r,seq,last,PEAK_CAUSAL_MPI_EXIT,2,cost,0,true);
    }
    assert(metadata.checkpoints==1);
    assert(peak_global_budget_causal_view(model).effective_ns>=30);
    broker_metadata_clear(&metadata);peak_global_budget_causal_free(model);
}

static void check_reverse_and_entry_fence(void)
{
    PeakGlobalBudgetCausal *model=peak_global_budget_causal_new(2);assert(model);
    Rank ranks[2]={0};BrokerMetadata metadata;
    assert(broker_metadata_init(&metadata,model,ranks,2));
    size_t members[]={0,1};assert(peak_global_budget_causal_context(model,0,1,members,2));
    uint64_t seq[2]={0},last[2]={0};
    for(unsigned r=0;r<2;r++)prefix_record(&metadata,r,seq,last,PEAK_CAUSAL_UNKNOWN,0,0,0,0);
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_MPI_ENTER,1,0,0,0);
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_MPI_EXIT,1,0,0,PEAK_MPI_CLOSED_WORLD_PREFIX);
    prefix_point(&metadata,0,seq,last,PEAK_CAUSAL_POST,PEAK_CAUSAL_RECV,10);
    prefix_point(&metadata,0,seq,last,PEAK_CAUSAL_WAIT,PEAK_CAUSAL_RECV,10);
    prefix_point(&metadata,0,seq,last,PEAK_CAUSAL_COMPLETE,PEAK_CAUSAL_RECV,10);
    /* A pre-cut rendezvous SEND completed only because the held post-cut
     * RECV was posted. Completed SEND alone is not a forward-edge proof. */
    prefix_point(&metadata,1,seq,last,PEAK_CAUSAL_POST,PEAK_CAUSAL_AMBIGUOUS_SEND,0);
    prefix_point(&metadata,1,seq,last,PEAK_CAUSAL_WAIT,PEAK_CAUSAL_AMBIGUOUS_SEND,0);
    prefix_point(&metadata,1,seq,last,PEAK_CAUSAL_COMPLETE,PEAK_CAUSAL_AMBIGUOUS_SEND,0);
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_FRONTIER,0,10,0,0);
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_MPI_ENTER,1,10,0,0);
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_MPI_EXIT,1,10,0,PEAK_MPI_CLOSED_WORLD_PREFIX);
    assert(!metadata.checkpoints&&!metadata.lanes[0].head);
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_FRONTIER,1,20,0,0);
    assert(peak_global_budget_causal_view(model).effective_ns>=30);
    broker_metadata_clear(&metadata);peak_global_budget_causal_free(model);

    model=peak_global_budget_causal_new(2);assert(model);
    memset(ranks,0,sizeof ranks);memset(seq,0,sizeof seq);memset(last,0,sizeof last);
    assert(broker_metadata_init(&metadata,model,ranks,2));
    assert(peak_global_budget_causal_context(model,0,1,members,2));
    for(unsigned r=0;r<2;r++)prefix_record(&metadata,r,seq,last,PEAK_CAUSAL_UNKNOWN,0,0,0,0);
    prefix_point(&metadata,0,seq,last,PEAK_CAUSAL_POST,PEAK_CAUSAL_SEND,10);
    prefix_point(&metadata,0,seq,last,PEAK_CAUSAL_WAIT,PEAK_CAUSAL_SEND,10);
    prefix_point(&metadata,0,seq,last,PEAK_CAUSAL_COMPLETE,PEAK_CAUSAL_SEND,10);
    for(unsigned r=0;r<2;r++){
        prefix_record(&metadata,r,seq,last,PEAK_CAUSAL_MPI_ENTER,1,r?0:10,0,0);
        prefix_record(&metadata,r,seq,last,PEAK_CAUSAL_MPI_EXIT,1,r?0:10,0,
            PEAK_MPI_CLOSED_WORLD_PREFIX|(r?0:PEAK_MPI_ENTRY_FENCE));
    }
    assert(!metadata.checkpoints); /* AND all members, never any-member fence. */
    for(unsigned r=0;r<2;r++){
        prefix_record(&metadata,r,seq,last,PEAK_CAUSAL_MPI_ENTER,2,r?0:10,0,0);
        prefix_record(&metadata,r,seq,last,PEAK_CAUSAL_MPI_EXIT,2,r?0:10,0,
            PEAK_MPI_CLOSED_WORLD_PREFIX|PEAK_MPI_ENTRY_FENCE);
    }
    assert(metadata.checkpoints==1&&metadata.fenced_checkpoints==1);
    prefix_point(&metadata,1,seq,last,PEAK_CAUSAL_POST,PEAK_CAUSAL_RECV,0);
    prefix_point(&metadata,1,seq,last,PEAK_CAUSAL_WAIT,PEAK_CAUSAL_RECV,0);
    prefix_point(&metadata,1,seq,last,PEAK_CAUSAL_COMPLETE,PEAK_CAUSAL_RECV,0);
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_FRONTIER,2,10,0,0);
    assert(peak_global_budget_causal_view(model).effective_ns>=20);
    broker_metadata_clear(&metadata);peak_global_budget_causal_free(model);
}

static void check_opaque_live_closure(void)
{
    PeakGlobalBudgetCausal *model=peak_global_budget_causal_new(2);assert(model);
    Rank ranks[2]={0};BrokerMetadata metadata;
    assert(broker_metadata_init(&metadata,model,ranks,2));
    size_t members[]={0,1};assert(peak_global_budget_causal_context(model,0,1,members,2));
    uint64_t seq[2]={0},last[2]={0};
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_FRONTIER,0,10,0,0);
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_MPI_ENTER,1,10,0,0);
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_MPI_EXIT,1,10,0,PEAK_MPI_CLOSED_WORLD_PREFIX);
    prefix_record(&metadata,0,seq,last,PEAK_CAUSAL_FRONTIER,1,20,0,0);
    assert(metadata.lanes[0].head);
    prefix_record(&metadata,1,seq,last,PEAK_CAUSAL_UNKNOWN,0,5,0,PEAK_MPI_OPAQUE_LIVE);
    assert(metadata.closed&&!metadata.invalid&&!metadata.cut_active&&!metadata.lanes[0].head);
    PeakGlobalBudgetCausalView closed=peak_global_budget_causal_view(model);
    assert(closed.sum_mode&&!closed.resource_failed&&!closed.saturated);
    uint64_t tail=0;for(unsigned r=0;r<2;r++)tail+=ranks[r].raw_profile-ranks[r].represented_outside-ranks[r].represented_inside;
    assert(closed.effective_ns+tail>=25); /* Held outside cost is not forgiven. */
    prefix_point(&metadata,0,seq,last,PEAK_CAUSAL_POST,PEAK_CAUSAL_SEND,50);
    assert(peak_global_budget_causal_view(model).raw_cost_ns==closed.raw_cost_ns);
    assert(ranks[0].raw_profile==50); /* Raw SUM bill grows, core allocation stops. */
    assert(!metadata.checkpoints&&!metadata.invalid);
    broker_metadata_clear(&metadata);peak_global_budget_causal_free(model);
}

static void *interleaved_frontier(void *unused)
{
    (void)unused;
    /* HB owns this call's CPU in management SUM; no foreground operation,
     * seed token or frontend charge may be invented for its frontier. */
    PeakMpiBudgetEvent event={.kind=PEAK_MPI_EVENT_FRONTIER,.rank=0,.seq=2,.last_mpi_seq=1};
    PeakMpiBudgetCostSnapshot cost={0};peak_global_budget_mpi_event_submit(&event,&cost);
    return NULL;
}
static void check_settled_operation_prefix(void)
{
    atomic_store(&owner,getpid());atomic_store(&running,true);rank_id=0;world=2;
    atomic_store(&local_retiring,false);atomic_store(&metadata_enabled,true);atomic_store(&metadata_seed,64);
    atomic_store(&metadata_tokens,0);atomic_store(&metadata_inflight,0);atomic_store(&frontend_cpu_ns,0);
    atomic_store(&metadata_block_credit,0);atomic_store(&metadata_unassigned_frontend,0);metadata_ordered_frontend=0;
    assert(peak_global_budget_mpi_trace_begin());
    PeakMpiBudgetEvent event={.kind=PEAK_MPI_EVENT_ENTER,.rank=0,.seq=1,.request=UINT64_C(0x0102030405060708)};
    PeakMpiBudgetCostSnapshot cost={0};peak_global_budget_mpi_event_submit(&event,&cost);
    assert(atomic_load(&metadata_pending)&&!atomic_load(&metadata_ready));
    pthread_t heartbeat;assert(!pthread_create(&heartbeat,NULL,interleaved_frontier,NULL));assert(!pthread_join(heartbeat,NULL));
    uint64_t count=99;assert(!metadata_batch(&count)&&count==0);
    assert(!atomic_load(&metadata_ready)); /* The complete later operation cannot pass ENTER. */
    event=(PeakMpiBudgetEvent){.kind=PEAK_MPI_EVENT_EXIT,.rank=0,.seq=3,.last_mpi_seq=1};
    peak_global_budget_mpi_event_submit(&event,&cost);peak_global_budget_mpi_frontend_charge(1000);
    assert(atomic_load(&metadata_ready));
    unsigned char *payload=metadata_batch(&count);assert(payload&&count==63);
    static const unsigned char identity[]={1,2,3,4,5,6,7,8};
    assert(!memcmp(payload+10*8,identity,sizeof identity)); /* Independent dynamic-payload endian golden. */
    uint64_t words[63];const unsigned char *cursor=payload;
    for(unsigned i=0;i<63;i++)words[i]=peak_budget_wire_get(&cursor,8);
    assert(words[13]>0&&words[21+13]==words[13]);
    assert(words[42+13]==words[21+13]&&words[42+13]==atomic_load(&frontend_cpu_ns));
    /* Force partial fixed header AND partial dynamic payload, no ABI casts. */
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    Frame frame={.magic=MAGIC,.version=PROTOCOL_VERSION,.payload_words=count};
    unsigned char fixed[PEAK_GLOBAL_BUDGET_WIRE_SIZE];peak_global_budget_frame_encode(fixed,&frame);
    Peer receiver={.fd=sockets[1]};
    assert(send(sockets[0],fixed,17,0)==17);assert(read_frame(&receiver)==0);
    assert(send(sockets[0],fixed+17,sizeof fixed-17,0)==sizeof fixed-17);
    assert(send(sockets[0],payload,7,0)==7);assert(read_frame(&receiver)==0);
    assert(send(sockets[0],payload+7,count*8-7,0)==count*8-7);assert(read_frame(&receiver)==1);
    assert(!memcmp(receiver.incoming_payload,payload,count*8));read_frame_done(&receiver);
    free(payload);close(sockets[0]);close(sockets[1]);
    metadata_clear();
    /* Retirement cancels the unused remainder of a paid block only after
     * admitted operations have published their real costs. */
    atomic_store(&metadata_seed, 0);
    atomic_store(&metadata_tokens, 0);
    atomic_store(&metadata_estimate, 1000);
    atomic_store(&available_ns, 64000);
    atomic_store(&consumed_frontend_ns, 0);
    atomic_store(&settled_frontend_ns, 0);
    assert(peak_global_budget_mpi_trace_begin());
    assert(atomic_load(&metadata_tokens) == 63);
    assert(atomic_load(&consumed_frontend_ns) == 64000);
    assert(!atomic_load(&available_ns));
    peak_global_budget_retire();
    assert(!atomic_load(&metadata_enabled));
    assert(!atomic_load(&settled_frontend_ns));
    peak_global_budget_mpi_frontend_charge(1000);
    assert(!atomic_load(&metadata_inflight));
    assert(!atomic_load(&metadata_tokens));
    assert(!atomic_load(&metadata_block_credit));
    assert(atomic_load(&settled_frontend_ns) == 64000);
    assert(!peak_global_budget_mpi_trace_begin());
    atomic_store(&local_retiring,false);
    atomic_store(&consumed_frontend_ns,0);
    atomic_store(&debit_ns,0);
    atomic_store(&settled_frontend_ns,0);
    local_sample=(PeakGlobalBudgetSample){0};
    atomic_store(&running,false);
}

int main(void)
{
    check_wire_format();
    check_coalesced_partial_write();
    check_broker_readiness();
    check_metadata_prefetch();
    check_reply_snapshot_order();
    check_required_vs_desired();
    check_admission_revision();
    check_causal_wire_integration();
    check_runtime_causal_cost();
    check_closed_cost_prefix();
    check_asymmetric_closed_prefix();
    check_reverse_and_entry_fence();
    check_opaque_live_closure();
    check_settled_operation_prefix();
    check_credit_conservation();
    check_control_settlement();
    check_closing_quote_epoch();
    check_startup_handoff();
    check_background_rounds();
    puts("global_budget_ledger_ok");
}
