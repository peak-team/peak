#define _POSIX_C_SOURCE 200809L
#include "internal/mpi_causal_transport.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define HDR 40u /* Retained allocation/payload reservation, not wire length. */

#define MAGIC UINT32_C(0x504d4337)
#define DENSE_MAGIC (MAGIC ^ UINT32_C(0x80000000))
enum { SEND_BATCH_FRAMES = 16 };
enum { HELLO = 1, RECORD, OBJECT, RAW, ACK, FEEDBACK, WMRQ, PACKED_RECORD, PACKED_RAW, PACKED_FEEDBACK };
typedef struct {
  int fd, ready, feedback_sent;
  uint64_t feedback_generation, nonce_sent, pending_ack;
  unsigned ack_count;
  int ack_due;
  /* have may include further frames; need describes only the front frame. */
  size_t have, need, out_n, out_at;
  unsigned rank;
  unsigned char *in, *out;
} Peer;
struct PeakMpiCausalTransport {
  PeakMpiCausalServiceConfig c;
  PeakMpiCausalTransportSink sink;
  struct sockaddr_storage addr;
  socklen_t addrlen;
  int listener;
  size_t cap, stride, head, count, send_index, send_at, slot;
  unsigned char *queue, *raw_peers, *decode_peers;
  size_t *lengths;
  uint64_t next_object_uid, next_seq, requested_nonce, *accepted, *incarnations,
      *raw_prefixes, *decode_prefixes, *capture_remaining;
  Peer *peers;
  size_t peer_n, peer_cursor;
  PeakMpiCausalRaw raw;
  int raw_pending, raw_seen, held, object_done, have_feedback;
  /* Buffered dispatch advances a turn without charging network bytes again. */
  int receive_progress;
  int capture_first;
  PeakMpiObserverRecord record;
  PeakMpiPolicyFeedback feedback;
  PeakMpiCausalServiceStats stats;
  double retry_at;
};
static void p32(unsigned char **p, uint32_t v) {
  unsigned i;
  for (i = 0; i < 4; i++)
    *(*p)++ = (unsigned char)(v >> (24 - 8 * i));
}
static void p64(unsigned char **p, uint64_t v) {
  unsigned i;
  for (i = 0; i < 8; i++)
    *(*p)++ = (unsigned char)(v >> (56 - 8 * i));
}
static uint32_t g32(const unsigned char **p) {
  uint32_t v = 0;
  unsigned i;
  for (i = 0; i < 4; i++)
    v = (v << 8) | *(*p)++;
  return v;
}
static uint64_t g64(const unsigned char **p) {
  uint64_t v = 0;
  unsigned i;
  for (i = 0; i < 8; i++)
    v = (v << 8) | *(*p)++;
  return v;
}
static void pd(unsigned char **p, double d) {
  uint64_t u;
  memcpy(&u, &d, 8);
  p64(p, u);
}
static double gd(const unsigned char **p) {
  uint64_t u = g64(p);
  double d;
  memcpy(&d, &u, 8);
  return d;
}
#define R64(X)                                                                 \
  X(incarnation)                                                               \
  X(thread_uid)                                                                \
  X(thread_generation)                                                         \
  X(sequence)                                                                  \
  X(op_uid)                                                                    \
  X(input_ref)                                                                 \
  X(result_ref)                                                                \
  X(progress_begin)                                                            \
  X(progress_end)                                                              \
  X(L_cut) X(A_cut)                                                            \
  X(P_cut)                                                                     \
  X(control_generation) X(communicator_uid) X(request_uid)                     \
      X(request_generation) X(activation) X(collective_ordinal) X(batch_uid)   \
          X(wait_ns) X(capability_uid) X(policy_generation) X(group_L_cut) X(group_P_cut) X(group_A_cut)
#define R32(X)                                                                 \
  X(peer)                                                                      \
  X(tag)                                                                       \
  X(root)                                                                      \
  X(actual_source)                                                             \
  X(actual_tag)                                                                \
  X(mpi_error)                                                                 \
  X(kind)                                                                      \
  X(operation)                                                                 \
  X(phase)                                                                     \
  X(flags)                                                                     \
  X(progress_scope) X(partition) X(completion_index) X(completion_count)
static size_t record_encode(unsigned char *b, const PeakMpiObserverRecord *r) {
  unsigned char *p = b;
#define E64(f) p64(&p, r->f);
#define E32(f) p32(&p, (uint32_t)r->f);
  R64(E64)
  R32(E32)
#undef E64
#undef E32
  return (size_t)(p - b);
}
static void record_decode(const unsigned char *p, PeakMpiObserverRecord *r) {
  memset(r, 0, sizeof(*r));
#define D64(f) r->f = g64(&p);
#define D32(f) r->f = g32(&p);
  R64(D64) R32(D32)
#undef D64
#undef D32
}
/* Bounded exact integer encoding; floating values retain their original bits. */
typedef struct { unsigned char *p, *end; int ok; } PackedOut;
typedef struct { const unsigned char *p, *end; int ok; } PackedIn;
static void pack_uint(PackedOut *w, uint64_t v) {
  do {
    if (!w->ok || w->p == w->end) { w->ok = 0; return; }
    unsigned char b = (unsigned char)(v & 127); v >>= 7;
    *w->p++ = b | (v ? 128 : 0);
  } while (v);
}
static uint64_t unpack_uint(PackedIn *r) {
  uint64_t v = 0;
  for (unsigned i = 0; i < 10; ++i) {
    if (!r->ok || r->p == r->end) { r->ok = 0; return 0; }
    unsigned char b = *r->p++;
    if (i == 9 && b > 1) { r->ok = 0; return 0; }
    v |= (uint64_t)(b & 127) << (7 * i);
    if (!(b & 128)) return v;
  }
  r->ok = 0; return 0;
}
static uint32_t unpack32(PackedIn *r) {
  uint64_t v = unpack_uint(r);
  if (v > UINT32_MAX) r->ok = 0;
  return (uint32_t)v;
}
static void pack_double(PackedOut *w, double d) {
  if (!w->ok || (size_t)(w->end - w->p) < 8) { w->ok = 0; return; }
  pd(&w->p, d);
}
static double unpack_double(PackedIn *r) {
  if (!r->ok || (size_t)(r->end - r->p) < 8) { r->ok = 0; return 0; }
  return gd(&r->p);
}
#define FIELD_COUNT(f) +1
enum { RECORD_FIELDS = 0 R64(FIELD_COUNT) R32(FIELD_COUNT),
       RECORD_MASK_BYTES = (RECORD_FIELDS + 7) / 8 };
#undef FIELD_COUNT
_Static_assert(RECORD_FIELDS < 64, "record field-presence bitmap");
static size_t packed_record(unsigned char *b, const PeakMpiObserverRecord *r, uint64_t incarnation) {
  if (r->incarnation != incarnation) return 256;
  PackedOut w = {b + RECORD_MASK_BYTES, b + 256, 1};
  uint64_t mask = 0; unsigned bit = 0;
#define E64(f) if (offsetof(PeakMpiObserverRecord, f) != offsetof(PeakMpiObserverRecord, incarnation) && r->f) { mask |= UINT64_C(1) << bit; pack_uint(&w, r->f); } ++bit;
#define E32(f) if ((uint32_t)r->f) { uint32_t v = (uint32_t)r->f; mask |= UINT64_C(1) << bit; pack_uint(&w, (v << 1) ^ (0u - (v >> 31))); } ++bit;
  R64(E64) R32(E32)
#undef E64
#undef E32
  for (unsigned i = 0; i < RECORD_MASK_BYTES; ++i)
    b[i] = (unsigned char)(mask >> (8*i));
  return w.ok ? (size_t)(w.p - b) : 256;
}
static int unpack_record(const unsigned char *b, size_t n, uint64_t incarnation,
                         PeakMpiObserverRecord *r) {
  if (n < RECORD_MASK_BYTES) return 0;
  PackedIn v = {b + RECORD_MASK_BYTES, b + n, 1};
  uint64_t mask = 0; unsigned bit = 0;
  for (unsigned i = 0; i < RECORD_MASK_BYTES; ++i)
    mask |= (uint64_t)b[i] << (8*i);
  if (mask & 1) return 0; /* Incarnation is explicit in this same frame header. */
  memset(r, 0, sizeof *r);
  r->incarnation = incarnation;
#define D64(f) if (mask & (UINT64_C(1) << bit)) r->f = unpack_uint(&v); ++bit;
#define D32(f) if (mask & (UINT64_C(1) << bit)) { uint32_t u = unpack32(&v); r->f = (u >> 1) ^ (0u - (u & 1)); } ++bit;
  R64(D64) R32(D32)
#undef D64
#undef D32
  return v.ok && v.p == v.end && !(mask >> bit);
}
#define REC64(X)                                                               \
  X(epoch)                                                                     \
  X(state) X(comm) X(ordinal) X(operation) X(uid_floor) X(loss) X(fault_mask)
static void recovery_encode(unsigned char **p, const PeakMpiCausalRecovery *r) {
#define E(f) p64(p, r->f);
  REC64(E)
#undef E
  pd(p, r->L);
  pd(p, r->P);
  pd(p, r->elapsed);
  pd(p, r->A);
  p64(p, r->app_cost_known);
  p64(p, r->cut.P_ns); p64(p, r->cut.A_ns); p64(p, r->cut.L_ns);
}
static void recovery_decode(const unsigned char **p, PeakMpiCausalRecovery *r) {
#define D(f) r->f = g64(p);
  REC64(D)
#undef D
  r->L = gd(p);
  r->P = gd(p);
  r->elapsed = gd(p);
  r->A = gd(p);
  r->app_cost_known = g64(p);
  r->cut.P_ns = g64(p); r->cut.A_ns = g64(p); r->cut.L_ns = g64(p);
}
#define RECOVERY_BYTES 128u
static void pack_recovery(PackedOut *w, const PeakMpiCausalRecovery *r) {
  unsigned char bytes[RECOVERY_BYTES], *end = bytes;
  recovery_encode(&end, r);
  unsigned nonzero = 0;
  for (size_t i = 0; i < sizeof bytes; ++i) nonzero |= bytes[i];
  pack_uint(w, nonzero != 0);
  if (nonzero) {
    const unsigned char *words = bytes;
    for (size_t i = 0; i < RECOVERY_BYTES / 8; ++i) pack_uint(w, g64(&words));
  }
}
static int unpack_recovery(PackedIn *v, PeakMpiCausalRecovery *r) {
  uint64_t present = unpack_uint(v);
  if (!v->ok || present > 1) return 0;
  memset(r, 0, sizeof *r);
  if (present) {
    unsigned char bytes[RECOVERY_BYTES], *words = bytes;
    for (size_t i = 0; i < RECOVERY_BYTES / 8; ++i) p64(&words, unpack_uint(v));
    if (!v->ok) return 0;
    const unsigned char *start = bytes; recovery_decode(&start, r);
  }
  return v->ok;
}
#define F64(X)                                                                 \
  X(cohort.job_incarnation)                                                    \
  X(cohort.cohort_id)                                                          \
  X(cohort.epoch)                                                              \
  X(member.process)                                                            \
  X(member.incarnation)                                                        \
  X(cut.P_ns) X(cut.A_ns) X(cut.L_ns) X(local_latest_A_ns)                    \
  X(local_latest_L_ns)                                                         \
  X(generation)                                                                \
  X(event_cut) X(control_generation) X(policy_generation) X(lost_events)
#define FD(X)                                                                  \
  X(q)                                                                         \
  X(g)                                                                         \
  X(L_cut)                                                                     \
  X(P_cut)                                                                     \
  X(D_cut)                                                                     \
  X(local_elapsed_at_cut)                                                      \
  X(F)                                                                         \
  X(risk)                                                                      \
  X(U_tail)                                                                    \
  X(T_safe)                                                                    \
  X(local_latest_L) X(received_raw_sum) X(age_seconds) X(oldest_unresolved_age) \
  X(A_cut) X(local_latest_A) X(received_app_sum)
#define FI(X)                                                                  \
  X(mode)                                                                      \
  X(reason)                                                                    \
  X(membership_complete)                                                       \
  X(coverage_complete)                                                         \
  X(risk_known)                                                                \
  X(tails_complete) X(received_raw_complete) X(rate_baseline_valid) X(app_cost_known)
static size_t feedback_encode(unsigned char *b,
                              const PeakMpiPolicyFeedback *r) {
  unsigned char *p = b;
#define E(f) p64(&p, r->f);
  F64(E)
#undef E
  p64(&p, r->member_count);
#define E(f) pd(&p, r->f);
  FD(E)
#undef E
#define E(f) p32(&p, (uint32_t)r->f);
  FI(E)
#undef E
  recovery_encode(&p, &r->recovery);
  p32(&p, (uint32_t)r->approximate);
  return (size_t)(p - b);
}
static void feedback_decode(const unsigned char *p, PeakMpiPolicyFeedback *r) {
  memset(r, 0, sizeof(*r));
#define D(f) r->f = g64(&p);
  F64(D)
#undef D
  r->member_count = (size_t)g64(&p);
#define D(f) r->f = gd(&p);
  FD(D)
#undef D
#define D(f) r->f = g32(&p);
  FI(D)
#undef D
  recovery_decode(&p, &r->recovery);
  r->approximate = (int)g32(&p);
}
#define FIELD_COUNT(f) +1
enum { FEEDBACK_FIELDS = 2 F64(FIELD_COUNT) FD(FIELD_COUNT) FI(FIELD_COUNT),
       FEEDBACK_MASK_BYTES = (FEEDBACK_FIELDS + 7) / 8 };
#undef FIELD_COUNT
_Static_assert(FEEDBACK_FIELDS < 64, "feedback field-presence bitmap");
static size_t packed_feedback(unsigned char *b, const PeakMpiPolicyFeedback *r) {
  PackedOut w = {b + FEEDBACK_MASK_BYTES, b + 300 + RECOVERY_BYTES + 4, 1};
  uint64_t mask = 0; unsigned bit = 0;
#define U(f) if (r->f) { mask |= UINT64_C(1) << bit; pack_uint(&w, r->f); } ++bit;
#define I(f) if ((uint32_t)r->f) { mask |= UINT64_C(1) << bit; pack_uint(&w, (uint32_t)r->f); } ++bit;
#define D(f) { uint64_t u; memcpy(&u, &r->f, 8); if (u) { mask |= UINT64_C(1) << bit; pack_double(&w, r->f); } ++bit; }
  F64(U) U(member_count) FD(D) FI(I) I(approximate)
#undef U
#undef I
#undef D
  pack_recovery(&w, &r->recovery);
  for (unsigned i = 0; i < FEEDBACK_MASK_BYTES; ++i)
    b[i] = (unsigned char)(mask >> (8*i));
  return w.ok ? (size_t)(w.p - b) : 300 + RECOVERY_BYTES + 4;
}
static int unpack_feedback(const unsigned char *b, size_t n,
                            PeakMpiPolicyFeedback *r) {
  if (n < FEEDBACK_MASK_BYTES) return 0;
  PackedIn v = {b + FEEDBACK_MASK_BYTES, b + n, 1};
  uint64_t mask = 0; unsigned bit = 0;
  for (unsigned i = 0; i < FEEDBACK_MASK_BYTES; ++i)
    mask |= (uint64_t)b[i] << (8*i);
  memset(r, 0, sizeof *r);
#define U(f) if (mask & (UINT64_C(1) << bit)) r->f = unpack_uint(&v); ++bit;
#define I(f) if (mask & (UINT64_C(1) << bit)) r->f = unpack32(&v); ++bit;
#define D(f) if (mask & (UINT64_C(1) << bit)) r->f = unpack_double(&v); ++bit;
  F64(U) U(member_count) FD(D) FI(I) I(approximate)
#undef U
#undef I
#undef D
  return !(mask >> bit) && unpack_recovery(&v, &r->recovery) && v.p == v.end;
}
static size_t object_encode(unsigned char *b, const PeakMpiObserverObject *o) {
  unsigned char *p = b;
  PeakMpiCausalCommPart c;
  unsigned i;
  memcpy(&c, o->payload, sizeof(c));
  p64(&p, o->uid);
  p64(&p, o->generation);
  p64(&p, o->parent_uid);
  p32(&p, o->kind);
  p32(&p, o->flags);
  p32(&p, o->part_index);
  p32(&p, o->part_count);
  p64(&p, c.local_uid);
  p64(&p, c.parent_uid);
  p64(&p, c.creation_ordinal);
  p32(&p, c.creation_kind);
  p32(&p, c.world_size);
  p32(&p, (uint32_t)c.color);
  p32(&p, (uint32_t)c.local_rank);
  p32(&p, (uint32_t)c.local_size);
  p32(&p, c.member_offset);
  p32(&p, c.member_count);
  for (i = 0; i < 16; i++)
    p32(&p, (uint32_t)c.members[i]);
  return (size_t)(p - b);
}
static void object_decode(const unsigned char *p, PeakMpiObserverObject *o) {
  PeakMpiCausalCommPart c;
  unsigned i;
  memset(o, 0, sizeof(*o));
  memset(&c, 0, sizeof(c));
  o->uid = g64(&p);
  o->generation = g64(&p);
  o->parent_uid = g64(&p);
  o->kind = g32(&p);
  o->flags = g32(&p);
  o->part_index = g32(&p);
  o->part_count = g32(&p);
  c.local_uid = g64(&p);
  c.parent_uid = g64(&p);
  c.creation_ordinal = g64(&p);
  c.creation_kind = g32(&p);
  c.world_size = g32(&p);
  c.color = (int32_t)g32(&p);
  c.local_rank = (int32_t)g32(&p);
  c.local_size = (int32_t)g32(&p);
  c.member_offset = g32(&p);
  c.member_count = g32(&p);
  for (i = 0; i < 16; i++)
    c.members[i] = (int32_t)g32(&p);
  memcpy(o->payload, &c, sizeof(c));
}
static size_t raw_encode(unsigned char *b, const PeakMpiCausalRaw *r) {
  unsigned char *p = b;
  size_t i;
  p64(&p, r->generation);
  p64(&p, r->watermark_nonce);
  p64(&p, r->idle_group_uid);
  p32(&p, (uint32_t)r->idle_cut);
  p64(&p, r->incarnation);
  p64(&p, r->control_generation);
  pd(&p, r->L);
  pd(&p, r->A);
  p32(&p, (uint32_t)r->app_cost_known);
  p64(&p, r->cut.P_ns); p64(&p, r->cut.A_ns); p64(&p, r->cut.L_ns);
  pd(&p, r->P);
  pd(&p, r->elapsed);
  p32(&p, r->contract_profile);
  p32(&p, (uint32_t)r->active);
  p32(&p, (uint32_t)r->unknown_members);
  p32(&p, (uint32_t)r->finalized);
  p32(&p, r->coarse_reason);
  p64(&p, r->lost_events);
  p32(&p, (uint32_t)r->peer_count);
  memcpy(p, r->known_peers, r->peer_count);
  p += r->peer_count;
  p32(&p, (uint32_t)r->slot_count);
  for (i = 0; i < r->slot_count; ++i)
    p64(&p, r->slot_prefixes[i]);
  recovery_encode(&p, &r->recovery);
  return (size_t)(p - b);
}
static int raw_decode(const unsigned char *p, size_t n, PeakMpiCausalRaw *r,
                      unsigned char *peers, unsigned world, uint64_t *prefixes,
                      size_t slots) {
  size_t i;
  if (n < 140 + RECOVERY_BYTES)
    return 0;
  memset(r, 0, sizeof(*r));
  r->generation = g64(&p);
  r->watermark_nonce = g64(&p);
  r->idle_group_uid = g64(&p);
  r->idle_cut = (int)g32(&p);
  r->incarnation = g64(&p);
  r->control_generation = g64(&p);
  r->L = gd(&p);
  r->A = gd(&p);
  r->app_cost_known = (int)g32(&p);
  r->cut.P_ns = g64(&p); r->cut.A_ns = g64(&p); r->cut.L_ns = g64(&p);
  r->P = gd(&p);
  r->elapsed = gd(&p);
  r->contract_profile = g32(&p);
  r->active = (int)g32(&p);
  r->unknown_members = (int)g32(&p);
  r->finalized = (int)g32(&p);
  r->coarse_reason = g32(&p);
  r->lost_events = g64(&p);
  r->peer_count = g32(&p);
  if (r->peer_count != world || n < 140 + RECOVERY_BYTES + r->peer_count)
    return 0;
  memcpy(peers, p, r->peer_count);
  r->known_peers = peers;
  p += r->peer_count;
  r->slot_count = g32(&p);
  if ((r->slot_count && r->slot_count != slots) ||
      n != 140 + RECOVERY_BYTES + r->peer_count + 8 * r->slot_count)
    return 0;
  for (i = 0; i < r->slot_count; ++i)
    prefixes[i] = g64(&p);
  r->slot_prefixes = prefixes;
  recovery_decode(&p, &r->recovery);
  return 1;
}
static size_t packed_raw(unsigned char *b, const PeakMpiCausalRaw *r, uint64_t incarnation) {
  size_t dense = 140 + RECOVERY_BYTES + r->peer_count + 8 * r->slot_count;
  if (r->incarnation != incarnation) return dense;
  PackedOut w = {b, b + dense, 1};
#define U(f) pack_uint(&w, r->f)
#define I(f) pack_uint(&w, (uint32_t)r->f)
#define D(f) pack_double(&w, r->f)
  U(generation); U(watermark_nonce); U(idle_group_uid); I(idle_cut);
  U(control_generation); D(L); D(A); I(app_cost_known);
  U(cut.P_ns); U(cut.A_ns); U(cut.L_ns); D(P); D(elapsed);
  I(contract_profile); I(active); I(unknown_members); I(finalized);
  I(coarse_reason); U(lost_events); U(peer_count);
  size_t ones = 0;
  for (size_t i = 0; i < r->peer_count; ++i) {
    if (r->known_peers[i] > 1) return dense;
    ones += r->known_peers[i];
  }
  unsigned def = ones > r->peer_count / 2;
  size_t minority = def ? r->peer_count - ones : ones;
  size_t sparse_bytes = 1;
  for (size_t v = minority; v >= 128; v >>= 7) ++sparse_bytes;
  for (size_t i = 0; i < r->peer_count; ++i)
    if (r->known_peers[i] != def) {
      ++sparse_bytes;
      for (size_t v = i; v >= 128; v >>= 7) ++sparse_bytes;
    }
  int sparse = sparse_bytes < r->peer_count / 8 + (r->peer_count % 8 != 0);
  pack_uint(&w, sparse ? def + 1 : 0);
  if (sparse) {
    pack_uint(&w, minority);
    for (size_t i = 0; i < r->peer_count; ++i)
      if (r->known_peers[i] != def) pack_uint(&w, i);
  } else for (size_t i = 0; i < r->peer_count; i += 8) {
    unsigned char bits = 0;
    for (size_t j = 0; j < 8 && i + j < r->peer_count; ++j)
      bits |= r->known_peers[i + j] << j;
    if (!w.ok || w.p == w.end) return dense;
    *w.p++ = bits;
  }
  U(slot_count);
  size_t used = r->slot_count;
  while (used && !r->slot_prefixes[used - 1]) --used;
  pack_uint(&w, used);
  for (size_t i = 0; i < used; ++i) pack_uint(&w, r->slot_prefixes[i]);
  pack_recovery(&w, &r->recovery);
#undef U
#undef I
#undef D
  return w.ok ? (size_t)(w.p - b) : dense;
}
static int unpack_raw(const unsigned char *b, size_t n, uint64_t incarnation, PeakMpiCausalRaw *r,
                       unsigned char *peers, unsigned world,
                       uint64_t *prefixes, size_t slots) {
  PackedIn v = {b, b + n, 1};
  memset(r, 0, sizeof *r);
  r->incarnation = incarnation;
#define U(f) r->f = unpack_uint(&v)
#define I(f) r->f = unpack32(&v)
#define D(f) r->f = unpack_double(&v)
  U(generation); U(watermark_nonce); U(idle_group_uid); I(idle_cut);
  U(control_generation); D(L); D(A); I(app_cost_known);
  U(cut.P_ns); U(cut.A_ns); U(cut.L_ns); D(P); D(elapsed);
  I(contract_profile); I(active); I(unknown_members); I(finalized);
  I(coarse_reason); U(lost_events); U(peer_count);
  unsigned mode = unpack32(&v);
  if (!v.ok || r->peer_count != world || mode > 2) return 0;
  if (mode) {
    unsigned def = mode - 1;
    memset(peers, def, world);
    uint64_t count = unpack_uint(&v), previous = 0;
    if (count > world) return 0;
    for (size_t i = 0; i < count; ++i) {
      uint64_t index = unpack_uint(&v);
      if (!v.ok || index >= world || (i && index <= previous)) return 0;
      peers[index] = (unsigned char)!def; previous = index;
    }
  } else {
    size_t peer_bytes = (size_t)world / 8 + (world % 8 != 0);
    if ((size_t)(v.end-v.p) < peer_bytes) return 0;
    for (size_t i = 0; i < world; ++i) peers[i] = (v.p[i/8] >> (i%8)) & 1;
    if (world % 8 && (v.p[peer_bytes-1] >> (world % 8))) return 0;
    v.p += peer_bytes;
  }
  r->known_peers = peers;
  U(slot_count);
  uint64_t used = unpack_uint(&v);
  if (!v.ok || (r->slot_count && r->slot_count != slots) || used > r->slot_count)
    return 0;
  memset(prefixes, 0, slots * sizeof *prefixes);
  for (size_t i = 0; i < used; ++i) prefixes[i] = unpack_uint(&v);
  r->slot_prefixes = prefixes;
  if (!unpack_recovery(&v, &r->recovery)) return 0;
#undef U
#undef I
#undef D
  return v.ok && v.p == v.end;
}

typedef struct {
  unsigned type, rank;
  size_t length, bytes;
  uint64_t job, incarnation, sequence;
} FrameHeader;
/* 0 means a partial header; -1 means an invalid bounded integer. */
static int header_uint(const unsigned char **p, const unsigned char *end,
                        unsigned limit, uint64_t *value) {
  *value = 0;
  for (unsigned i = 0; i < limit; ++i) {
    if (*p == end) return 0;
    unsigned char b = *(*p)++;
    if (i + 1 == limit && b > (limit == 5 ? 15 : 1)) return -1;
    *value |= (uint64_t)(b & 127) << (7*i);
    if (!(b & 128)) return 1;
  }
  return -1;
}
static int frame_header(PeakMpiCausalTransport *t, const unsigned char *b,
                         size_t have, FrameHeader *h) {
  const unsigned char *p = b, *end = b + have;
  uint64_t n, rank;
  if (have < 4) return 0;
  uint32_t magic = g32(&p);
  if (magic == DENSE_MAGIC) {
    if (have < HDR) return 0;
    h->type = g32(&p); h->length = g32(&p); h->rank = g32(&p);
    h->job = g64(&p); h->incarnation = g64(&p); h->sequence = g64(&p);
  } else {
    if (magic != MAGIC) return -1;
    if (p == end) return 0;
    h->type = *p++;
    int rc = header_uint(&p, end, 5, &n);
    if (rc != 1) return rc;
    h->length = (size_t)n;
    rc = header_uint(&p, end, 5, &rank);
    if (rc != 1) return rc;
    h->rank = (unsigned)rank;
    rc = header_uint(&p, end, 10, &h->job);
    if (rc != 1) return rc;
    rc = header_uint(&p, end, 10, &h->incarnation);
    if (rc != 1) return rc;
    rc = header_uint(&p, end, 10, &h->sequence);
    if (rc != 1) return rc;
    if ((size_t)(p-b) >= HDR) return -1;
  }
  if (h->type < HELLO || h->type > PACKED_FEEDBACK || h->length > t->cap - HDR)
    return -1;
  h->bytes = (size_t)(p-b);
  return 1;
}
static size_t frame(PeakMpiCausalTransport *t, unsigned char *b, unsigned type,
                    size_t n, uint64_t seq) {
  unsigned char *p = b;
  p32(&p, MAGIC); *p++ = (unsigned char)type;
  PackedOut w = {p, b + HDR, 1};
  pack_uint(&w, n); pack_uint(&w, t->c.rank);
  pack_uint(&w, t->c.job_context); pack_uint(&w, t->c.incarnation);
  pack_uint(&w, seq);
  size_t header = (size_t)(w.p-b);
  if (!w.ok || header >= HDR) {
    p = b; p32(&p, DENSE_MAGIC); p32(&p, type); p32(&p, (uint32_t)n);
    p32(&p, t->c.rank); p64(&p, t->c.job_context);
    p64(&p, t->c.incarnation); p64(&p, seq); header = HDR;
  }
  memmove(b + header, b + HDR, n);
  return header + n;
}

static void fault(PeakMpiCausalTransport *t, unsigned rank,
                  PeakMpiPolicyReason why) {
  t->stats.unavailable = 1;
  if (t->sink.fault)
    t->sink.fault(t->sink.context, rank, why);
}
static void close_peer(Peer *p) {
  if (p->fd >= 0)
    close(p->fd);
  p->fd = -1;
  p->ready = 0;
  p->feedback_sent = 0;
  p->nonce_sent = 0;
  p->pending_ack = 0;
  p->ack_count = 0;
  p->ack_due = 0;
  p->have = 0;
  p->need = HDR;
  p->out_n = p->out_at = 0;
}
static int nonblock(int fd) {
  int one = 1;
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  int f = fcntl(fd, F_GETFL, 0);
  return f >= 0 && fcntl(fd, F_SETFL, f | O_NONBLOCK) >= 0;
}
static int write_buf(PeakMpiCausalTransport *t, int fd, unsigned char *b,
                     size_t n, size_t *at, size_t *budget) {
  ssize_t z;
  size_t k;
  if (*at == n)
    return 1;
  if (!*budget)
    return 0;
  k = n - *at;
  if (k > *budget)
    k = *budget;
  z = send(fd, b + *at, k, MSG_NOSIGNAL);
  if (z > 0) {
    *at += (size_t)z;
    *budget -= (size_t)z;
    t->stats.sent_bytes += (uint64_t)z;
    return *at == n;
  }
  if (z < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    return 0;
  return -1;
}
static int write_queued(PeakMpiCausalTransport *t, int fd, size_t *budget) {
  /* Gather retained frames without copying or changing their wire boundaries.
   * Both the vector count and total submitted bytes are bounded. */
  struct iovec vectors[SEND_BATCH_FRAMES];
  struct msghdr message = {0};
  size_t count = 0, remaining = *budget;
  /* Finish a partial event before inserting pending RAW control, without
   * beginning another event ahead of that control frame. */
  size_t limit = t->raw_pending && t->send_at ? 1 : SEND_BATCH_FRAMES;
  for (size_t j = t->send_index; j < t->count && count < limit && remaining;
       ++j) {
    size_t i = (t->head + j) % t->c.queue_frames;
    size_t offset = j == t->send_index ? t->send_at : 0;
    size_t bytes = t->lengths[i] - offset;
    if (bytes > remaining)
      bytes = remaining;
    vectors[count].iov_base = t->queue + i * t->cap + offset;
    vectors[count++].iov_len = bytes;
    remaining -= bytes;
  }
  if (!count)
    return 0;
  message.msg_iov = vectors;
  message.msg_iovlen = count;
  ssize_t written = sendmsg(fd, &message, MSG_NOSIGNAL);
  if (written > 0) {
    size_t bytes = (size_t)written;
    *budget -= bytes;
    t->stats.sent_bytes += (uint64_t)bytes;
    while (bytes) {
      size_t i = (t->head + t->send_index) % t->c.queue_frames;
      size_t rest = t->lengths[i] - t->send_at;
      if (bytes < rest) {
        t->send_at += bytes;
        break;
      }
      bytes -= rest;
      t->send_index++;
      t->send_at = 0;
    }
    return 1;
  }
  if (written < 0 &&
      (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    return 0;
  return -1;
}
static int read_frame(PeakMpiCausalTransport *t, Peer *p, size_t *budget) {
  FrameHeader h;
  int rc = frame_header(t, p->in, p->have, &h);
  if (rc < 0) return -1;
  if (rc) { p->need = h.bytes + h.length; if (p->have >= p->need) return 1; }
  if (!*budget) return 0;
  size_t k = t->cap - p->have;
  if (k > *budget) k = *budget;
  ssize_t z = recv(p->fd, p->in + p->have, k, 0);
  if (z > 0) {
    p->have += (size_t)z; *budget -= (size_t)z;
    t->stats.received_bytes += (uint64_t)z;
    rc = frame_header(t, p->in, p->have, &h);
    if (rc < 0) return -1;
    if (rc) { p->need = h.bytes + h.length; return p->have >= p->need; }
    return 0;
  }
  if (z < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    return 0;
  return -1;
}
static void reset_input(Peer *p) {
  p->have -= p->need;
  if (p->have)
    memmove(p->in, p->in + p->need, p->have);
  p->need = HDR;
}
static int enqueue(PeakMpiCausalTransport *t, unsigned type, const void *v) {
  size_t i = (t->head + t->count) % t->c.queue_frames, n;
  unsigned char *b = t->queue + i * t->cap;
  if (t->count == t->c.queue_frames)
    return 0;
  if (type == RECORD) {
    n = packed_record(b + HDR, v, t->c.incarnation);
    if (n < 256) type = PACKED_RECORD;
    else n = record_encode(b + HDR, v);
  }
  else if (type == OBJECT)
    n = object_encode(b + HDR, v);
  else
    n = raw_encode(b + HDR, v);
  t->lengths[i] = frame(t, b, type, n, ++t->next_seq);
  t->count++;
  if (t->count > t->stats.pending_peak)
    t->stats.pending_peak = t->count;
  return 1;
}
static int deliver(PeakMpiCausalTransport *t, unsigned rank, uint64_t inc,
                   unsigned type, const unsigned char *b, size_t n,
                   double now) {
  if ((type == RECORD && n == 256) || type == PACKED_RECORD) {
    PeakMpiObserverRecord r;
    if (type == PACKED_RECORD) {
      if (!unpack_record(b, n, inc, &r)) {
        fault(t, rank, PEAK_MPI_POLICY_BAD_IDENTITY); return -1;
      }
    } else record_decode(b, &r);
    if (r.incarnation != inc) {
      fault(t, rank, PEAK_MPI_POLICY_BAD_IDENTITY);
      return -1;
    }
    return !t->sink.record || t->sink.record(t->sink.context, rank, &r);
  }
  if (type == OBJECT && n == 156) {
    PeakMpiObserverObject o;
    object_decode(b, &o);
    return !t->sink.object || t->sink.object(t->sink.context, rank, inc, &o);
  }
  if (type == RAW || type == PACKED_RAW) {
    PeakMpiCausalRaw r;
    if ((type == PACKED_RAW ?
         unpack_raw(b, n, inc, &r, t->decode_peers, t->c.world_size, t->decode_prefixes, t->c.slots) :
         raw_decode(b, n, &r, t->decode_peers, t->c.world_size, t->decode_prefixes, t->c.slots)) &&
        r.incarnation == inc)
      return !t->sink.raw || t->sink.raw(t->sink.context, rank, &r, now);
  }
  fault(t, rank, PEAK_MPI_POLICY_BAD_IDENTITY);
  return -1;
}
PeakMpiCausalTransport *
peak_mpi_causal_transport_create(const PeakMpiCausalServiceConfig *c,
                                 const PeakMpiCausalTransportSink *sink) {
  PeakMpiCausalTransport *t;
  size_t i;
  struct sockaddr_in *a;
  struct sockaddr_in6 *a6;
  if (!c || !sink || !c->address || !c->world_size || !c->incarnation ||
      c->rank >= c->world_size || c->owner_rank >= c->world_size ||
      c->queue_frames < 4 || !c->port || c->port > 65535 ||
      c->world_size > UINT32_MAX - 1024 ||
      c->slots > (UINT32_MAX - 1024 - c->world_size) / 8)
    return NULL;
  t = calloc(1, sizeof(*t));
  if (!t)
    return NULL;
  t->c = *c;
  t->sink = *sink;
  t->listener = -1;
  t->cap = 1024 + c->world_size + 8 * c->slots;
  t->peer_n = c->rank == c->owner_rank ? c->world_size : 1;
  if (c->queue_frames > SIZE_MAX / t->cap)
    goto fail;
  t->peers = calloc(t->peer_n, sizeof(*t->peers));
  if (!t->peers)
    goto fail;
  for (i = 0; i < t->peer_n; i++)
    t->peers[i].fd = -1;
  t->queue = calloc(c->queue_frames, t->cap);
  t->lengths = calloc(c->queue_frames, sizeof(size_t));
  t->raw_peers = calloc(c->world_size, 1);
  t->decode_peers = calloc(c->world_size, 1);
  t->raw_prefixes = calloc(c->slots ? c->slots : 1, sizeof(uint64_t));
  t->decode_prefixes = calloc(c->slots ? c->slots : 1, sizeof(uint64_t));
  t->capture_remaining = calloc(c->slots ? c->slots : 1, sizeof(uint64_t));
  t->accepted = calloc(c->world_size, sizeof(uint64_t));
  t->incarnations = calloc(c->world_size, sizeof(uint64_t));
  if (!t->queue || !t->lengths || !t->raw_peers || !t->decode_peers ||
      !t->accepted || !t->incarnations || !t->raw_prefixes ||
      !t->decode_prefixes || !t->capture_remaining)
    goto fail;
  for (i = 0; i < t->peer_n; i++) {
    t->peers[i].in = malloc(t->cap);
    t->peers[i].out = malloc(t->cap);
    t->peers[i].need = HDR;
    if (!t->peers[i].in || !t->peers[i].out)
      goto fail;
  }
  a = (struct sockaddr_in *)&t->addr;
  a->sin_family = AF_INET;
  a->sin_port = htons((uint16_t)c->port);
  if (inet_pton(AF_INET, c->address, &a->sin_addr) == 1)
    t->addrlen = sizeof(*a);
  else {
    memset(&t->addr, 0, sizeof(t->addr));
    a6 = (struct sockaddr_in6 *)&t->addr;
    a6->sin6_family = AF_INET6;
    a6->sin6_port = htons((uint16_t)c->port);
    if (inet_pton(AF_INET6, c->address, &a6->sin6_addr) != 1)
      goto fail;
    t->addrlen = sizeof(*a6);
  }
  return t;
fail:
  peak_mpi_causal_transport_destroy(t);
  return NULL;
}
void peak_mpi_causal_transport_destroy(PeakMpiCausalTransport *t) {
  size_t i;
  if (!t)
    return;
  if (t->listener >= 0)
    close(t->listener);
  if (t->peers)
    for (i = 0; i < t->peer_n; i++) {
      close_peer(&t->peers[i]);
      free(t->peers[i].in);
      free(t->peers[i].out);
    }
  free(t->peers);
  free(t->queue);
  free(t->lengths);
  free(t->raw_peers);
  free(t->decode_peers);
  free(t->raw_prefixes);
  free(t->decode_prefixes);
  free(t->capture_remaining);
  free(t->accepted);
  free(t->incarnations);
  free(t);
}
static void capture(PeakMpiCausalTransport *t, PeakMpiObserver *o,
                    size_t *budget, int local) {
  size_t i, empty = 0;
  if (!o || !t->c.slots)
    return;
  /* Freeze the published queue prefixes once. Producers may keep publishing,
   * but this heartbeat never follows beyond these bounded observations. */
  for (i = 0; i < t->c.slots; ++i) {
    PeakMpiObserverSlotState state;
    t->capture_remaining[i] = peak_mpi_observer_slot_state(o, i, &state) &&
                                      state.published >= state.consumed
                                  ? state.published - state.consumed
                                  : 0;
  }
  /* Definitions live in an independent append-only slab. Runtime object
   * creation does not consume a producer-ring record. Freeze producer heads
   * first: every definition referenced by this batch was published before
   * those heads, so it can be delivered before any dependent event. */
  size_t object_limit = peak_mpi_observer_object_count(o);
  if (!t->next_object_uid)
    t->next_object_uid = 1;
  while (t->next_object_uid <= object_limit) {
    const PeakMpiObserverObject *definition =
        peak_mpi_observer_object_get(o, t->next_object_uid);
    if (*budget < 256 || (!local && t->count == t->c.queue_frames))
      return;
    int accepted = local ? (!t->sink.object ||
                            t->sink.object(t->sink.context, t->c.rank,
                                           t->c.incarnation, definition))
                         : enqueue(t, OBJECT, definition);
    if (!accepted)
      return;
    t->next_object_uid++;
    *budget -= 256;
  }
  while (*budget >= 256 && (t->held || empty < t->c.slots)) {
    const PeakMpiObserverObject *obj;
    int ok;
    if (!local && t->count + 2 >= t->c.queue_frames)
      return;
    if (!t->held) {
      if (!t->capture_remaining[t->slot] ||
          !peak_mpi_observer_consume(o, t->slot, &t->record)) {
        t->capture_remaining[t->slot] = 0;
        t->slot = (t->slot + 1) % t->c.slots;
        ++empty;
        continue;
      }
      --t->capture_remaining[t->slot];
      t->held = 1;
      t->object_done = 0;
    }
    if (t->record.kind == PEAK_MPI_OBSERVER_EVENT_OBJECT &&
        t->record.op_uid < t->next_object_uid)
      t->object_done = 1;
    if (t->record.kind == PEAK_MPI_OBSERVER_EVENT_OBJECT && !t->object_done) {
      obj = peak_mpi_observer_object_get(o, t->record.op_uid);
      if (!obj) {
        fault(t, t->c.rank, PEAK_MPI_POLICY_BAD_IDENTITY);
        t->held = 0;
        return;
      }
      ok = local
               ? (!t->sink.object || t->sink.object(t->sink.context, t->c.rank,
                                                    t->c.incarnation, obj))
               : enqueue(t, OBJECT, obj);
      if (!ok)
        return;
      t->object_done = 1;
    }
    ok = local ? (!t->sink.record ||
                  t->sink.record(t->sink.context, t->c.rank, &t->record))
               : enqueue(t, RECORD, &t->record);
    if (!ok)
      return;
    t->held = 0;
    t->stats.records++;
    empty = 0;
    *budget -= 256;
    t->slot = (t->slot + 1) % t->c.slots;
  }
}
static void prepare_control(PeakMpiCausalTransport *t, Peer *p, double now) {
  size_t n;
  if (!p->out_n && p->ready && t->sink.request_nonce) {
    uint64_t nonce = t->sink.request_nonce(t->sink.context, p->rank);
    if (nonce > p->nonce_sent) {
      unsigned char *out = p->out + HDR;
      p64(&out, nonce);
      p->out_n = frame(t, p->out, WMRQ, 8, 0);
      p->out_at = 0;
      p->nonce_sent = nonce;
    }
  }
  if (!p->out_n && p->ready && t->sink.feedback) {
    PeakMpiPolicyFeedback f;
    if (t->sink.feedback(t->sink.context, p->rank, now, &f) &&
        (!p->feedback_sent || p->feedback_generation != f.generation)) {
      p->feedback_sent = 1;
      p->feedback_generation = f.generation;
      n = packed_feedback(p->out + HDR, &f);
      unsigned type = PACKED_FEEDBACK;
      if (n >= 300 + RECOVERY_BYTES + 4) {
        n = feedback_encode(p->out + HDR, &f); type = FEEDBACK;
      }
      p->out_n = frame(t, p->out, type, n, 0);
      p->out_at = 0;
    }
  }
}
static void prepare_ack(PeakMpiCausalTransport *t, Peer *p, int force) {
  if (!p->out_n && p->ack_count &&
      (force || p->ack_due || p->ack_count >= SEND_BATCH_FRAMES)) {
    p->out_n = frame(t, p->out, ACK, 0, p->pending_ack);
    p->out_at = 0;
    p->pending_ack = 0;
    p->ack_count = 0;
    p->ack_due = 0;
  }
}
static void owner_tick(PeakMpiCausalTransport *t, double now, size_t *budget) {
  size_t i, start = t->peer_cursor;
  int fd, on = 1;
  if (t->listener < 0 && now >= t->retry_at) {
    fd = socket(t->addr.ss_family, SOCK_STREAM, 0);
    if (fd >= 0) {
      if (nonblock(fd) &&
          setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == 0 &&
          bind(fd, (struct sockaddr *)&t->addr, t->addrlen) == 0 &&
          listen(fd, (int)t->peer_n) == 0)
        t->listener = fd;
      else
        close(fd);
    }
    t->retry_at = now + 1;
  }
  if (t->listener >= 0) {
    for (i = 0; i < t->peer_n && *budget; i++)
      if (t->peers[i].fd < 0) {
        fd = accept(t->listener, NULL, NULL);
        if (fd < 0)
          break;
        if (!nonblock(fd)) {
          close(fd);
          break;
        }
        t->peers[i].fd = fd;
        *budget -= 1;
      }
  }
  for (i = 0; i < t->peer_n && *budget; i++) {
    size_t at = (start + i) % t->peer_n;
    Peer *p = &t->peers[at];
    /* Resume after the last attempted peer when the shared allowance ends. */
    t->peer_cursor = (at + 1) % t->peer_n;
    const unsigned char *q;
    unsigned type, rank;
    uint64_t job, inc, seq;
    size_t n;
    int rc, read_idle = 0;
    if (p->fd < 0)
      continue;
    if (p->out_n) {
      rc = write_buf(t, p->fd, p->out, p->out_n, &p->out_at, budget);
      if (rc < 0) {
        close_peer(p);
        continue;
      }
      if (!rc)
        continue;
      p->out_n = p->out_at = 0;
    }
    prepare_ack(t, p, 0);
    if (p->out_n) {
      rc = write_buf(t, p->fd, p->out, p->out_n, &p->out_at, budget);
      if (rc < 0) {
        close_peer(p);
        continue;
      }
      if (!rc)
        continue;
      p->out_n = p->out_at = 0;
    }
    /* Pending ACKs are flushed first; then request/feedback control gets a
     * bounded turn before reading another event, even under continuous load. */
    prepare_control(t, p, now);
    if (p->out_n) {
      rc = write_buf(t, p->fd, p->out, p->out_n, &p->out_at, budget);
      if (rc < 0) {
        close_peer(p);
        continue;
      }
      if (!rc)
        continue;
      p->out_n = p->out_at = 0;
    }
    /* A zero result can also mean a header/partial-frame read made progress.
     * Only a turn that consumed no bytes is idle. */
    size_t before_read = *budget;
    rc = read_frame(t, p, budget);
    read_idle = !rc && before_read == *budget;
    if (rc < 0) {
      close_peer(p);
      continue;
    }
    if (rc) {
      FrameHeader h;
      if (frame_header(t, p->in, p->have, &h) != 1) { close_peer(p); continue; }
      type = h.type; n = h.length; rank = h.rank; job = h.job;
      inc = h.incarnation; seq = h.sequence; q = p->in + h.bytes;
      if (rank >= t->c.world_size || rank == t->c.owner_rank ||
          job != t->c.job_context || !inc || (p->ready && p->rank != rank)) {
        fault(t, rank, PEAK_MPI_POLICY_BAD_IDENTITY);
        close_peer(p);
        continue;
      }
      if (type == HELLO && n == 0 && !seq) {
        if (t->incarnations[rank] && t->incarnations[rank] != inc) {
          fault(t, rank, PEAK_MPI_POLICY_BAD_IDENTITY);
          close_peer(p);
          continue;
        }
        t->incarnations[rank] = inc;
        p->rank = rank;
        p->ready = 1;
        reset_input(p);
        t->receive_progress = 1;
      } else if (p->ready && t->incarnations[rank] == inc && (type == RAW || type == PACKED_RAW) &&
                 !seq) {
        rc = deliver(t, rank, inc, type, q, n, now);
        if (rc < 0) {
          close_peer(p);
          continue;
        }
        if (!rc)
          continue;
        reset_input(p);
        t->receive_progress = 1;
      } else if (p->ready && t->incarnations[rank] == inc && seq) {
        if (seq <= t->accepted[rank]) {
          t->stats.duplicates++;
          rc = 1;
        } else if (seq == t->accepted[rank] + 1)
          rc = deliver(t, rank, inc, type, q, n, now);
        else {
          fault(t, rank, PEAK_MPI_POLICY_EVENT_LOSS);
          rc = -1;
        }
        if (rc < 0) {
          close_peer(p);
          continue;
        }
        if (!rc)
          continue;
        if (seq > t->accepted[rank])
          t->accepted[rank] = seq;
        p->pending_ack = t->accepted[rank];
        p->ack_count++;
        reset_input(p);
        t->receive_progress = 1;
      } else {
        fault(t, rank, PEAK_MPI_POLICY_BAD_IDENTITY);
        close_peer(p);
        continue;
      }
    }
    prepare_control(t, p, now);
    if (p->out_n) {
      rc = write_buf(t, p->fd, p->out, p->out_n, &p->out_at, budget);
      if (rc < 0)
        close_peer(p);
      else if (rc)
        p->out_n = p->out_at = 0;
    }
    prepare_ack(t, p, read_idle);
    if (p->out_n) {
      rc = write_buf(t, p->fd, p->out, p->out_n, &p->out_at, budget);
      if (rc < 0)
        close_peer(p);
      else if (rc)
        p->out_n = p->out_at = 0;
    }
  }
}
static void client_tick(PeakMpiCausalTransport *t, double now, size_t *budget) {
  Peer *p = &t->peers[0];
  int rc;
  const unsigned char *q;
  unsigned type, rank;
  uint64_t inc, seq;
  size_t n;
  if (p->fd < 0 && now >= t->retry_at) {
    int fd = socket(t->addr.ss_family, SOCK_STREAM, 0);
    t->retry_at = now + 0.25;
    if (fd >= 0) {
      if (!nonblock(fd)) {
        close(fd);
        return;
      }
      rc = connect(fd, (struct sockaddr *)&t->addr, t->addrlen);
      if (rc == 0 || errno == EINPROGRESS) {
        p->fd = fd;
        p->out_n = frame(t, p->out, HELLO, 0, 0);
        p->out_at = 0;
        t->send_index = t->send_at = 0;
        if (t->raw_seen)
          t->raw_pending = 1;
        t->stats.reconnects++;
      } else
        close(fd);
    }
  }
  if (p->fd < 0)
    return;
  if (p->out_n) {
    rc = write_buf(t, p->fd, p->out, p->out_n, &p->out_at, budget);
    if (rc < 0) {
      close_peer(p);
      return;
    }
    if (!rc)
      return;
    p->out_n = p->out_at = 0;
    p->ready = 1;
  }
  /* RAW is cumulative control, independent of the fine replay/ACK lane.
   * Do not interleave its bytes into a partially written event frame. */
  if (t->raw_pending && !t->send_at && p->ready) {
    n = packed_raw(p->out + HDR, &t->raw, t->c.incarnation);
    unsigned raw_type = PACKED_RAW;
    size_t dense = 140 + RECOVERY_BYTES + t->raw.peer_count + 8 * t->raw.slot_count;
    if (n >= dense) { n = raw_encode(p->out + HDR, &t->raw); raw_type = RAW; }
    p->out_n = frame(t, p->out, raw_type, n, 0);
    p->out_at = 0;
    t->raw_pending = 0;
    rc = write_buf(t, p->fd, p->out, p->out_n, &p->out_at, budget);
    if (rc < 0) {
      close_peer(p);
      return;
    }
    if (!rc)
      return;
    p->out_n = p->out_at = 0;
  }
  /* A batch of events must not outrun its ACKs and delay watermark feedback.
   * Keep the original control framing and shared byte allowance. */
  for (unsigned frames = 0, reads = 0;
       frames < SEND_BATCH_FRAMES && reads < 2 * SEND_BATCH_FRAMES && *budget;
       ++reads) {
    size_t before = *budget;
    rc = read_frame(t, p, budget);
    if (rc < 0) {
      close_peer(p);
      return;
    }
    if (!rc) {
      if (before == *budget)
        break;
      continue;
    }
    FrameHeader h;
    if (frame_header(t, p->in, p->have, &h) != 1) { close_peer(p); return; }
    q = p->in + h.bytes;
    type = h.type; n = h.length; inc = h.incarnation; seq = h.sequence;
    rank = h.rank;
    if (!p->ready || type == HELLO || rank != t->c.owner_rank ||
        h.job != t->c.job_context || !inc ||
        (t->incarnations[rank] && t->incarnations[rank] != inc)) {
      fault(t, t->c.rank, PEAK_MPI_POLICY_BAD_IDENTITY);
      close_peer(p);
      return;
    }
    t->incarnations[rank] = inc;
    if (type == ACK && !n) {
      if (seq > t->next_seq) {
        fault(t, t->c.rank, PEAK_MPI_POLICY_BAD_IDENTITY);
        close_peer(p);
        return;
      }
      while (t->count) {
        FrameHeader queued;
        if (frame_header(t, t->queue + t->head * t->cap,
                         t->lengths[t->head], &queued) != 1) {
          fault(t, t->c.rank, PEAK_MPI_POLICY_BAD_IDENTITY); close_peer(p); return;
        }
        uint64_t s = queued.sequence;
        if (s > seq)
          break;
        t->head = (t->head + 1) % t->c.queue_frames;
        t->count--;
        if (t->send_index)
          t->send_index--;
        else
          t->send_at = 0;
      }
    } else if (type == WMRQ && n == 8 && !seq) {
      uint64_t nonce = g64(&q);
      if (nonce > t->requested_nonce)
        t->requested_nonce = nonce;
    } else if ((type == FEEDBACK && n == 300 + RECOVERY_BYTES + 4) ||
               type == PACKED_FEEDBACK) {
      PeakMpiPolicyFeedback next;
      if (type == PACKED_FEEDBACK) {
        if (!unpack_feedback(q, n, &next)) {
          fault(t, t->c.rank, PEAK_MPI_POLICY_BAD_IDENTITY);
          close_peer(p); return;
        }
      } else feedback_decode(q, &next);
      t->feedback = next;
      t->have_feedback = 1;
      t->stats.feedbacks++;
    } else {
      fault(t, t->c.rank, PEAK_MPI_POLICY_BAD_IDENTITY);
      close_peer(p);
      return;
    }
    reset_input(p);
    t->receive_progress = 1;
    ++frames;
  }
  if (t->send_index < t->count && *budget) {
    rc = write_queued(t, p->fd, budget);
    if (rc < 0) {
      close_peer(p);
      return;
    }
  }
}
static void transport_io(PeakMpiCausalTransport *t,
                         const PeakMpiCausalRaw *raw, double now,
                         size_t *budget) {
  int owner = t->c.rank == t->c.owner_rank;
  size_t rounds, before;
  if (owner) {
    for (rounds = 0; rounds < t->c.queue_frames + 2; ++rounds) {
      before = *budget;
      t->receive_progress = 0;
      owner_tick(t, now, budget);
      if (!*budget || (before == *budget && !t->receive_progress))
        break;
    }
    /* A bounded end-of-heartbeat tailflush. If the shared allowance is
     * exhausted, preserve the queued ACK or due flag for the next turn. */
    for (size_t peer = 0; peer < t->peer_n; peer++) {
      Peer *p = &t->peers[peer];
      if (p->fd < 0 || !p->ready)
        continue;
      if (p->ack_count) {
        p->ack_due = 1;
      }
      prepare_ack(t, p, 1);
      if (p->out_n && *budget) {
        int rc = write_buf(t, p->fd, p->out, p->out_n, &p->out_at, budget);
        if (rc < 0)
          close_peer(p);
        else if (rc)
          p->out_n = p->out_at = 0;
      }
    }
  } else if (raw->active) {
    for (rounds = 0; rounds < t->c.queue_frames + 2; ++rounds) {
      before = *budget;
      t->receive_progress = 0;
      client_tick(t, now, budget);
      if (!*budget || (before == *budget && !t->receive_progress))
        break;
    }
  }
}
void peak_mpi_causal_transport_tick(PeakMpiCausalTransport *t,
                                    PeakMpiObserver *o,
                                    const PeakMpiCausalRaw *raw, double now,
                                    size_t budget) {
  int owner;
  if (!t || !raw)
    return;
  owner = t->c.rank == t->c.owner_rank;
  if (raw->active) {
    if (raw->peer_count != t->c.world_size || !raw->known_peers ||
        (raw->slot_count &&
         (raw->slot_count != t->c.slots || !raw->slot_prefixes))) {
      fault(t, t->c.rank, PEAK_MPI_POLICY_BAD_IDENTITY);
      return;
    }
    if (!t->raw_seen || raw->generation != t->raw.generation ||
        memcmp(&raw->recovery, &t->raw.recovery, sizeof raw->recovery) != 0 ||
        raw->watermark_nonce != t->raw.watermark_nonce ||
        raw->idle_group_uid != t->raw.idle_group_uid ||
        raw->idle_cut != t->raw.idle_cut ||
        raw->coarse_reason != t->raw.coarse_reason ||
        raw->lost_events != t->raw.lost_events ||
        raw->unknown_members != t->raw.unknown_members ||
        raw->finalized != t->raw.finalized) {
      t->raw = *raw;
      memcpy(t->raw_peers, raw->known_peers, raw->peer_count);
      t->raw.known_peers = t->raw_peers;
      if (raw->slot_count)
        memcpy(t->raw_prefixes, raw->slot_prefixes,
               raw->slot_count * sizeof(uint64_t));
      t->raw.slot_prefixes = t->raw_prefixes;
      t->raw_pending = 1;
      t->raw_seen = 1;
    }
    if (owner) {
      if (t->raw_pending &&
          (!t->sink.raw ||
           t->sink.raw(t->sink.context, t->c.rank, &t->raw, now)))
        t->raw_pending = 0;
    }
  }
  /* Keep a local-record quantum available without starving readable peers or
   * control. Unused IO allowance is lent to the single frozen capture, then
   * unused capture allowance returns to IO. The total allowance never grows. */
  size_t local_budget = 0;
  if (raw->active && o) {
    if (budget >= 512)
      local_budget = budget / 2;
    else if (budget >= 256) {
      t->capture_first = !t->capture_first;
      local_budget = t->capture_first ? 256 : 0;
    }
  }
  size_t io_budget = budget - local_budget;
  transport_io(t, raw, now, &io_budget);
  local_budget += io_budget;
  if (raw->active && o) {
    capture(t, o, &local_budget, owner);
    if (local_budget)
      transport_io(t, raw, now, &local_budget);
  }
  if (owner) {
    if (raw->active && t->sink.feedback &&
        t->sink.feedback(t->sink.context, t->c.rank, now, &t->feedback)) {
      t->have_feedback = 1;
      t->stats.feedbacks++;
    }
  }
  t->stats.pending_records = t->count + (size_t)t->held;
  t->stats.listening = t->listener >= 0;
  t->stats.connected = owner ? t->stats.listening : t->peers[0].ready;
}
int peak_mpi_causal_transport_feedback(const PeakMpiCausalTransport *t,
                                       PeakMpiPolicyFeedback *f) {
  if (!t || !f || !t->have_feedback)
    return 0;
  *f = t->feedback;
  return 1;
}
void peak_mpi_causal_transport_stats(const PeakMpiCausalTransport *t,
                                     PeakMpiCausalServiceStats *s) {
  if (t && s)
    *s = t->stats;
}

uint64_t
peak_mpi_causal_transport_requested_nonce(const PeakMpiCausalTransport *t) {
  if (!t)
    return 0;
  if (t->c.rank == t->c.owner_rank && t->sink.request_nonce)
    return t->sink.request_nonce(t->sink.context, t->c.rank);
  return t->requested_nonce;
}
