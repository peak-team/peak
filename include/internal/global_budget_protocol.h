#ifndef PEAK_GLOBAL_BUDGET_PROTOCOL_H
#define PEAK_GLOBAL_BUDGET_PROTOCOL_H

#include "internal/global_budget.h"
#include <float.h>
#include <string.h>

#define PEAK_GLOBAL_BUDGET_PROTOCOL_MAGIC UINT32_C(0x50474231)
#define PEAK_GLOBAL_BUDGET_PROTOCOL_VERSION 6

typedef struct {
    uint32_t magic, version, rank, size;
    uint64_t reply_sequence, consumed_profile_ns, consumed_control_ns, nonce,
        requested_ns, demand_generation, message_sequence, returned_ns;
    bool request_credit, root_startup_sealed, request_reply;
    double root_origin_seconds, root_startup_seconds;
    PeakGlobalBudgetSample sample;
    PeakGlobalBudgetView view;
} PeakGlobalBudgetFrame;

/* One field list fixes wire order independently of host layout and padding. */
#define PEAK_GLOBAL_BUDGET_WIRE_FIELDS(U32, U64, BOOL, F64) \
    U32(magic) U32(version) U32(rank) U32(size) \
    U64(reply_sequence) U64(consumed_profile_ns) U64(consumed_control_ns) \
    U64(nonce) U64(requested_ns) U64(demand_generation) \
    U64(message_sequence) U64(returned_ns) \
    BOOL(request_credit) BOOL(root_startup_sealed) \
    F64(root_origin_seconds) F64(root_startup_seconds) \
    U64(sample.sequence) U64(sample.completed_wave) \
    U64(sample.settled_control_credit_ns) \
    F64(sample.profile_seconds) F64(sample.control_seconds) \
    F64(sample.management_seconds) F64(sample.elapsed_seconds) \
    F64(sample.pressure) F64(sample.unfunded_seconds) \
    F64(sample.transport_seconds) F64(sample.closing_seconds) \
    U32(sample.attached_targets) BOOL(sample.retiring) BOOL(sample.final) \
    BOOL(sample.finalizer_invalid) BOOL(sample.initial_unexposed) \
    BOOL(view.startup_sealed) BOOL(view.broker_final_written) BOOL(view.active) \
    BOOL(view.wave_participant) BOOL(view.cohort_finalized) \
    BOOL(view.accounting_invalid) BOOL(view.recovery_pending) \
    U64(view.broker_process_id) U64(view.wave) U64(view.decision_sequence) \
    U64(view.view_epoch) U64(view.root_completed_wave) \
    U64(view.acknowledged_sequence) U64(view.acknowledged_demand_generation) \
    U64(view.request_deadline_ns) U64(view.local_credit_ns) \
    U64(view.returned_total_ns) U64(view.recovery_target_ns) \
    U64(view.rejected_spend_count) U64(view.granted_total_ns) \
    F64(view.broker_cpu_seconds) F64(view.global_spent_seconds) \
    F64(view.remaining_seconds) F64(view.allowance_seconds) \
    F64(view.wave_span_seconds) F64(view.granted_total_seconds) \
    F64(view.budget_seconds) F64(view.reserved_seconds) \
    F64(view.control_reserve_seconds) F64(view.startup_seconds) \
    F64(view.transport_seconds) F64(view.refill_rtt_seconds) \
    F64(view.consumption_rate) \
    U64(sample.background_period_ns) U64(view.background_period_ns) \
    U64(view.freshness_ns) BOOL(request_reply)

#define PEAK_WIRE_SIZE_U32(field) + 4
#define PEAK_WIRE_SIZE_U64(field) + 8
#define PEAK_WIRE_SIZE_BOOL(field) + 1
#define PEAK_WIRE_SIZE_F64(field) + 8
enum {
    PEAK_GLOBAL_BUDGET_WIRE_SIZE = 0 PEAK_GLOBAL_BUDGET_WIRE_FIELDS(
        PEAK_WIRE_SIZE_U32, PEAK_WIRE_SIZE_U64, PEAK_WIRE_SIZE_BOOL, PEAK_WIRE_SIZE_F64),
    PEAK_GLOBAL_BUDGET_ENDPOINT_WIRE_SIZE = 256 + 2 + 8
};
#undef PEAK_WIRE_SIZE_U32
#undef PEAK_WIRE_SIZE_U64
#undef PEAK_WIRE_SIZE_BOOL
#undef PEAK_WIRE_SIZE_F64

_Static_assert(sizeof(double) == sizeof(uint64_t) && DBL_MANT_DIG == 53 &&
               DBL_MAX_EXP == 1024, "budget wire doubles use IEEE 754 binary64");

static inline void peak_budget_wire_put(unsigned char **wire, uint64_t value,
                                        unsigned bytes)
{
    for (unsigned i = bytes; i > 0; --i)
        *(*wire)++ = (unsigned char)(value >> ((i - 1) * 8));
}

static inline uint64_t peak_budget_wire_get(const unsigned char **wire,
                                           unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i)
        value = (value << 8) | *(*wire)++;
    return value;
}

static inline void peak_global_budget_frame_encode(
    unsigned char wire[PEAK_GLOBAL_BUDGET_WIRE_SIZE], const PeakGlobalBudgetFrame *frame)
{
#define PEAK_WIRE_U32(field) peak_budget_wire_put(&wire, frame->field, 4);
#define PEAK_WIRE_U64(field) peak_budget_wire_put(&wire, frame->field, 8);
#define PEAK_WIRE_BOOL(field) *wire++ = frame->field ? 1 : 0;
#define PEAK_WIRE_F64(field) do { \
    uint64_t bits; \
    memcpy(&bits, &frame->field, sizeof bits); \
    peak_budget_wire_put(&wire, bits, 8); \
} while (0);
    PEAK_GLOBAL_BUDGET_WIRE_FIELDS(
        PEAK_WIRE_U32, PEAK_WIRE_U64, PEAK_WIRE_BOOL, PEAK_WIRE_F64)
#undef PEAK_WIRE_U32
#undef PEAK_WIRE_U64
#undef PEAK_WIRE_BOOL
#undef PEAK_WIRE_F64
}

static inline bool peak_global_budget_frame_decode(PeakGlobalBudgetFrame *frame,
    const unsigned char wire[PEAK_GLOBAL_BUDGET_WIRE_SIZE])
{
#define PEAK_WIRE_U32(field) frame->field = (uint32_t)peak_budget_wire_get(&wire, 4);
#define PEAK_WIRE_U64(field) frame->field = peak_budget_wire_get(&wire, 8);
#define PEAK_WIRE_BOOL(field) if (*wire > 1) return false; frame->field = *wire++ != 0;
#define PEAK_WIRE_F64(field) do { \
    uint64_t bits = peak_budget_wire_get(&wire, 8); \
    memcpy(&frame->field, &bits, sizeof bits); \
} while (0);
    PEAK_GLOBAL_BUDGET_WIRE_FIELDS(
        PEAK_WIRE_U32, PEAK_WIRE_U64, PEAK_WIRE_BOOL, PEAK_WIRE_F64)
#undef PEAK_WIRE_U32
#undef PEAK_WIRE_U64
#undef PEAK_WIRE_BOOL
#undef PEAK_WIRE_F64
    return frame->magic == PEAK_GLOBAL_BUDGET_PROTOCOL_MAGIC &&
        frame->version == PEAK_GLOBAL_BUDGET_PROTOCOL_VERSION;
}

typedef struct {
    char host[256];
    uint16_t port;
    uint64_t nonce;
} PeakGlobalBudgetEndpoint;

static inline void peak_global_budget_endpoint_encode(
    unsigned char wire[PEAK_GLOBAL_BUDGET_ENDPOINT_WIRE_SIZE],
    const PeakGlobalBudgetEndpoint *endpoint)
{
    memcpy(wire, endpoint->host, sizeof endpoint->host);
    wire += sizeof endpoint->host;
    peak_budget_wire_put(&wire, endpoint->port, 2);
    peak_budget_wire_put(&wire, endpoint->nonce, 8);
}

static inline void peak_global_budget_endpoint_decode(PeakGlobalBudgetEndpoint *endpoint,
    const unsigned char wire[PEAK_GLOBAL_BUDGET_ENDPOINT_WIRE_SIZE])
{
    memcpy(endpoint->host, wire, sizeof endpoint->host);
    wire += sizeof endpoint->host;
    endpoint->port = (uint16_t)peak_budget_wire_get(&wire, 2);
    endpoint->nonce = peak_budget_wire_get(&wire, 8);
}

#endif /* PEAK_GLOBAL_BUDGET_PROTOCOL_H */
