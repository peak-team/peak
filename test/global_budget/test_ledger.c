#define _GNU_SOURCE
#include "../../src/global_budget.c"
#include "internal/global_budget_protocol.h"
#include <assert.h>

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
        0x50, 0x47, 0x42, 0x31, 0, 0, 0, 5,
        1, 2, 3, 4, 1, 2, 3, 5
    };
    static const unsigned char nonce[] = {1, 2, 3, 4, 5, 6, 7, 8};
    static const unsigned char one[] = {0x3f, 0xf0, 0, 0, 0, 0, 0, 0};
    static const unsigned char targets[] = {0x11, 0x22, 0x33, 0x44};
    assert(sizeof wire == 409);
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

int main(void)
{
    check_wire_format();
    check_credit_conservation();
    check_control_settlement();
    puts("global_budget_ledger_ok");
}
