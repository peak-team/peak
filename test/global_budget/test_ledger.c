#define _GNU_SOURCE
#include "../../src/global_budget.c"
#include "internal/global_budget_protocol.h"
#include <assert.h>

static atomic_uint credit_wakes;
static atomic_bool admission_waiting;
bool peak_global_budget_listener_admission_waiting(void) { return atomic_load(&admission_waiting); }
void peak_global_budget_listener_credit_ready(void) { atomic_fetch_add(&credit_wakes, 1); }

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
    unsigned char wire[PEAK_GLOBAL_BUDGET_WIRE_SIZE], again[PEAK_GLOBAL_BUDGET_WIRE_SIZE];
    peak_global_budget_frame_encode(wire, &source);
    static const unsigned char prefix[] = {
        0x50, 0x47, 0x42, 0x31, 0, 0, 0, 6,
        1, 2, 3, 4, 1, 2, 3, 5
    };
    static const unsigned char nonce[] = {1, 2, 3, 4, 5, 6, 7, 8};
    static const unsigned char one[] = {0x3f, 0xf0, 0, 0, 0, 0, 0, 0};
    static const unsigned char targets[] = {0x11, 0x22, 0x33, 0x44};
    assert(sizeof wire == 434);
    assert(!memcmp(wire, prefix, sizeof prefix));
    assert(!memcmp(wire + 40, nonce, sizeof nonce));
    assert(wire[80] == 1 && wire[196] == 1);
    assert(!memcmp(wire + 82, one, sizeof one));
    assert(!memcmp(wire + 186, targets, sizeof targets));
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
    receive(&rank, (PeakGlobalBudgetSample){.sequence = 1});
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
    peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence = 1});
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
    peak_global_budget_publish(&(PeakGlobalBudgetSample){.sequence = 2, .attached_targets = 1});
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
}

int main(void)
{
    check_wire_format();
    check_credit_conservation();
    check_control_settlement();
    check_background_rounds();
    puts("global_budget_ledger_ok");
}
