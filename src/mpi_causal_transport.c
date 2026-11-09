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

#define HDR 40u
#define MAGIC UINT32_C(0x504d4335)
enum { SEND_BATCH_FRAMES = 16 };
enum { HELLO = 1, RECORD, OBJECT, RAW, ACK, FEEDBACK, WMRQ };
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
          X(wait_ns) X(capability_uid) X(policy_generation)
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
static size_t frame(PeakMpiCausalTransport *t, unsigned char *b, unsigned type,
                    size_t n, uint64_t seq) {
  unsigned char *p = b;
  p32(&p, MAGIC);
  p32(&p, type);
  p32(&p, (uint32_t)n);
  p32(&p, t->c.rank);
  p64(&p, t->c.job_context);
  p64(&p, t->c.incarnation);
  p64(&p, seq);
  return HDR + n;
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
  ssize_t z;
  size_t k;
  const unsigned char *q;
  if (p->have >= HDR && p->need == HDR) {
    q = p->in;
    if (g32(&q) != MAGIC)
      return -1;
    (void)g32(&q);
    k = g32(&q);
    if (k > t->cap - HDR)
      return -1;
    p->need = HDR + k;
  }
  if (p->have >= p->need)
    return 1;
  if (!*budget)
    return 0;
  k = t->cap - p->have;
  if (k > *budget)
    k = *budget;
  z = recv(p->fd, p->in + p->have, k, 0);
  if (z > 0) {
    p->have += (size_t)z;
    *budget -= (size_t)z;
    t->stats.received_bytes += (uint64_t)z;
    if (p->have >= HDR && p->need == HDR) {
      q = p->in;
      if (g32(&q) != MAGIC)
        return -1;
      (void)g32(&q);
      k = g32(&q);
      if (k > t->cap - HDR)
        return -1;
      p->need = HDR + k;
    }
    return p->have >= p->need;
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
  if (type == RECORD)
    n = record_encode(b + HDR, v);
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
  if (type == RECORD && n == 232) {
    PeakMpiObserverRecord r;
    record_decode(b, &r);
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
  if (type == RAW) {
    PeakMpiCausalRaw r;
    if (raw_decode(b, n, &r, t->decode_peers, t->c.world_size,
                   t->decode_prefixes, t->c.slots) &&
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
      n = feedback_encode(p->out + HDR, &f);
      p->out_n = frame(t, p->out, FEEDBACK, n, 0);
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
  t->peer_cursor = (t->peer_cursor + 1) % t->peer_n;
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
    Peer *p = &t->peers[(start + i) % t->peer_n];
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
      q = p->in;
      (void)g32(&q);
      type = g32(&q);
      n = g32(&q);
      rank = g32(&q);
      job = g64(&q);
      inc = g64(&q);
      seq = g64(&q);
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
      } else if (p->ready && t->incarnations[rank] == inc && type == RAW &&
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
  uint64_t job, inc, seq;
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
    n = raw_encode(p->out + HDR, &t->raw);
    p->out_n = frame(t, p->out, RAW, n, 0);
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
    q = p->in;
    (void)g32(&q);
    type = g32(&q);
    n = g32(&q);
    rank = g32(&q);
    job = g64(&q);
    inc = g64(&q);
    seq = g64(&q);
    if (rank != t->c.owner_rank || job != t->c.job_context ||
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
        const unsigned char *h = t->queue + t->head * t->cap + 32;
        uint64_t s = g64(&h);
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
    } else if (type == FEEDBACK && n == 300 + RECOVERY_BYTES + 4) {
      feedback_decode(q, &t->feedback);
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
