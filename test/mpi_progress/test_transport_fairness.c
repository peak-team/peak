#define _POSIX_C_SOURCE 200809L
#include <sys/socket.h>
#include <poll.h>
static ssize_t counted_recv(int, void *, size_t, int);
#define recv counted_recv
#include "../../src/mpi_causal_transport.c"
#undef recv
#include <assert.h>
#include <stdio.h>
#include <time.h>
static unsigned many_order[1024], many_count;
static int many_record(void *v, unsigned rank, const PeakMpiObserverRecord *r) {
  (void)v;
  (void)r;
  assert(many_count < sizeof many_order / sizeof many_order[0]);
  many_order[many_count++] = rank;
  return 1;
}
static void test_many_peer_fairness(void) {
  PeakMpiCausalServiceConfig c = {0};
  c.world_size = 8;
  c.owner_rank = 0;
  c.job_context = 113305103;
  c.address = "127.0.0.1";
  c.port = 23575;
  c.queue_frames = 32;
  c.slots = 288;
  PeakMpiCausalTransportSink sink = {.record = many_record};
  PeakMpiCausalTransport *t[8];
  for (unsigned r = 0; r < 8; r++) {
    c.rank = r;
    c.incarnation = UINT64_C(0x0012c93b14158000) + r;
    t[r] = peak_mpi_causal_transport_create(&c, &sink);
    assert(t[r]);
  }
  unsigned char peers[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  PeakMpiCausalRaw raw = {0}, dormant = {0};
  raw.active = 1;
  raw.generation = 1;
  raw.peer_count = 8;
  raw.known_peers = peers;
  for (unsigned k = 0; k < 32; k++) {
    peak_mpi_causal_transport_tick(t[0], NULL, &dormant, 1, 65536);
    for (unsigned r = 1; r < 8; r++) {
      raw.incarnation = t[r]->c.incarnation;
      peak_mpi_causal_transport_tick(t[r], NULL, &raw, 1, 65536);
    }
  }
  for (unsigned r = 1; r < 8; r++) {
    assert(t[r]->peers[0].ready);
    for (unsigned j = 0; j < 20; j++) {
      PeakMpiObserverRecord e = {0};
      e.incarnation = t[r]->c.incarnation;
      e.thread_uid = e.thread_generation = 1;
      e.sequence = j + 1;
      e.op_uid = e.batch_uid = 100 + j;
      e.L_cut = e.A_cut = e.P_cut = 1000000000;
      e.kind = PEAK_MPI_OBSERVER_EVENT_START;
      e.operation = PEAK_MPI_CAUSAL_ALLREDUCE;
      e.phase = PEAK_MPI_CAUSAL_CALL_BEGIN;
      e.flags = PEAK_MPI_CAUSAL_APP_COST_KNOWN | PEAK_MPI_CAUSAL_COLLECTIVE |
                PEAK_MPI_CAUSAL_ORDERED_CALLER;
      e.peer = e.root = e.tag = -1;
      assert(enqueue(t[r], RECORD, &e));
    }
    raw.incarnation = t[r]->c.incarnation;
    peak_mpi_causal_transport_tick(t[r], NULL, &raw, 1, 65536);
    assert(t[r]->send_index == t[r]->count);
  }
  peak_mpi_causal_transport_tick(t[0], NULL, &dormant, 2, 4096);
  unsigned unvisited = 0;
  for (unsigned i = 0; i < 8; i++) {
    Peer *p = &t[0]->peers[i];
    if (p->ready && !p->have && !t[0]->accepted[p->rank]) {
      unvisited = p->rank;
      break;
    }
  }
  assert(unvisited);
  unsigned before = many_count;
  peak_mpi_causal_transport_tick(t[0], NULL, &dormant, 3, 4096);
  fprintf(stderr, "firstunvisited=%u nextdelivered=%u firstturnrecords=%u\n",
          unvisited, many_order[before], before);
  assert(many_count > before && many_order[before] == unvisited);
  for (unsigned r = 0; r < 8; r++)
    peak_mpi_causal_transport_destroy(t[r]);
  puts("bounded remote peer roundrobin progress PASS");
  return;
}

static unsigned local_records, remote_records, faults;
static int replenish, control;
static PeakMpiObserver *local_capture;
static PeakMpiObserverProducer local_producer;
static int record(void *ctx, unsigned rank, const PeakMpiObserverRecord *r) {
  (void)ctx;
  if (rank)
    remote_records++;
  else {
    local_records++;
    if (replenish)
      assert(peak_mpi_observer_emit(local_capture, &local_producer, r) ==
             PEAK_MPI_OBSERVER_OK);
  }
  return 1;
}
static int raw_sink(void *ctx, unsigned rank, const PeakMpiCausalRaw *r,
                    double now) {
  (void)ctx;
  (void)rank;
  (void)r;
  (void)now;
  return 1;
}
static int feedback(void *ctx, unsigned rank, double now,
                    PeakMpiPolicyFeedback *f) {
  (void)ctx;
  (void)now;
  if (!control)
    return 0;
  memset(f, 0, sizeof *f);
  f->generation = 77;
  f->member.process = rank;
  f->member.incarnation = 123;
  f->g = 2;
  f->q = .1;
  f->mode = PEAK_MPI_POLICY_COARSE;
  f->reason = PEAK_MPI_POLICY_MISSING_CONTRACT;
  f->member_count = 2;
  return 1;
}
static uint64_t nonce(void *ctx, unsigned rank) {
  (void)ctx;
  (void)rank;
  return control ? 7 : 0;
}
static void sink_fault(void *ctx, unsigned rank, PeakMpiPolicyReason reason) {
  (void)ctx;
  (void)rank;
  (void)reason;
  faults++;
}
static unsigned attempts, accepted;
static double received;
static int drain_raw_sink(void *v, unsigned rank, const PeakMpiCausalRaw *r,
                    double now) {
  (void)v;
  (void)rank;
  assert(r->generation == 7);
  attempts++;
  if (attempts == 1)
    return 0;
  accepted++;
  received = now;
  return 1;
}
static void test_drain_raw(void) {
  PeakMpiCausalServiceConfig c = {0};
  c.world_size = 1;
  c.slots = 1;
  c.rank = c.owner_rank = 0;
  c.job_context = 11;
  c.incarnation = 22;
  c.address = "127.0.0.1";
  c.port = 23653;
  c.queue_frames = 8;
  PeakMpiCausalTransportSink sink = {.raw = drain_raw_sink};
  PeakMpiCausalTransport *t = peak_mpi_causal_transport_create(&c, &sink);
  assert(t);
  unsigned char peer = 0;
  uint64_t prefix = 0;
  PeakMpiCausalRaw raw = {0};
  raw.active = 1;
  raw.generation = 7;
  raw.incarnation = 22;
  raw.peer_count = raw.slot_count = 1;
  raw.known_peers = &peer;
  raw.slot_prefixes = &prefix;
  raw.elapsed = 1;
  peak_mpi_causal_transport_tick(t, NULL, &raw, 1, 65536);
  assert(attempts == 1 && !accepted && t->raw_pending);
  peak_mpi_causal_transport_tick(t, NULL, NULL, 2, 65536);
  assert(attempts == 2 && accepted == 1 && !t->raw_pending && received == 2);
  for (unsigned i = 0; i < 1000; i++)
    peak_mpi_causal_transport_tick(t, NULL, NULL, 3 + i, 65536);
  assert(attempts == 2 && accepted == 1 && received == 2 &&
         t->raw.generation == 7 && t->raw.elapsed == 1);
  peak_mpi_causal_transport_destroy(t);
  puts("NULL drain retries rejected RAW exactly once; 1000 drains never replay "
       "accepted RAW or refresh received_at");
}

/* A stable-control burst must not rescan each idle/backpressured peer for
 * every active frame. The recv wrapper counts real syscalls only in this case. */
static int watching, busy_fd, reject_rank;
static unsigned rx_calls, rx_success, rx_eagain, idle_eagain;
static unsigned busy_accepted, rejected_calls;
static unsigned nonce_calls, feedback_calls;
static PeakMpiCausalTransport *fresh_client;
static int fresh_fd, fresh_idle_seen, fresh_sent;
static unsigned fresh_received, fresh_after, blocked_accepted, reject_after;
static ssize_t counted_recv(int fd, void *b, size_t n, int flags) {
  ssize_t result = recv(fd, b, n, flags);
  if (watching) {
    ++rx_calls;
    rx_success += result > 0;
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      ++rx_eagain;
      idle_eagain += fd != busy_fd;
      if (fresh_client && fd == fresh_fd) fresh_idle_seen = 1;
    }
  }
  return result;
}
static int measured_record(void *p, unsigned rank,
                            const PeakMpiObserverRecord *r) {
  (void)p;
  (void)r;
  if (fresh_client && rank == 3) {
    ++fresh_received;
    fresh_after = busy_accepted;
    return 1;
  }
  if ((int)rank == reject_rank) {
    if (blocked_accepted < reject_after) {
      ++blocked_accepted;
      return 1;
    }
    ++rejected_calls;
    return 0;
  }
  assert(rank == 1);
  ++busy_accepted;
  if (fresh_client && !fresh_sent) {
    assert(busy_accepted == 1 && fresh_idle_seen);
    PeakMpiObserverRecord e = {0};
    e.incarnation = 103;
    e.thread_uid = e.thread_generation = e.sequence = e.op_uid = 1;
    e.kind = PEAK_MPI_OBSERVER_EVENT_START;
    assert(enqueue(fresh_client, RECORD, &e));
    watching = 0;
    peak_mpi_causal_transport_tick(fresh_client, NULL, NULL, 2, 65536);
    watching = 1;
    assert(fresh_client->send_index == fresh_client->count);
    fresh_sent = 1;
  }
  return 1;
}
static uint64_t measured_nonce(void *p, unsigned rank) {
  (void)p;
  (void)rank;
  if (watching) ++nonce_calls;
  return watching ? 4 : 0;
}
static int measured_feedback(void *p, unsigned rank, double now,
                               PeakMpiPolicyFeedback *f) {
  (void)p;
  (void)now;
  if (!watching) return 0;
  ++feedback_calls;
  memset(f, 0, sizeof *f);
  f->generation = 77;
  f->cohort = (PeakMpiPolicyCohortKey){123, 0, 1};
  f->member.process = rank;
  f->member.incarnation = 123;
  f->g = 64;
  f->q = .1;
  f->mode = PEAK_MPI_POLICY_COARSE;
  f->reason = PEAK_MPI_POLICY_MISSING_CONTRACT;
  f->member_count = 64;
  return 1;
}
static void test_owner_round_batches(void) {
  const unsigned world = 64;
  PeakMpiCausalServiceConfig c = {0};
  c.world_size = world;
  c.owner_rank = 0;
  c.job_context = 123;
  c.address = "127.0.0.1";
  c.port = 23671;
  c.slots = 1;
  PeakMpiCausalTransportSink sink = {
      .record = measured_record, .feedback = measured_feedback,
      .request_nonce = measured_nonce};
  PeakMpiCausalTransport *t[64];
  unsigned char members[64];
  memset(members, 1, sizeof members);
  uint64_t prefix = 0;
  PeakMpiCausalRaw raw = {0}, dormant = {0};
  raw.active = raw.generation = 1;
  raw.elapsed = 1;
  raw.peer_count = world;
  raw.known_peers = members;
  raw.slot_count = 1;
  raw.slot_prefixes = &prefix;
  for (unsigned r = 0; r < world; ++r) {
    c.rank = r;
    c.incarnation = 100 + r;
    c.queue_frames = !r ? 4096 : r == 1 ? 512 : r == 2 ? 16 : 4;
    t[r] = peak_mpi_causal_transport_create(&c, &sink);
    assert(t[r]);
  }
  watching = 0;
  for (unsigned k = 0; k < 1000; ++k) {
    peak_mpi_causal_transport_tick(t[0], NULL, &dormant, 1, 65536);
    for (unsigned r = 1; r < world; ++r) {
      raw.incarnation = 100 + r;
      peak_mpi_causal_transport_tick(t[r], NULL, &raw, 1, 65536);
    }
    unsigned ready = 0;
    for (unsigned i = 0; i < world; ++i)
      ready += t[0]->peers[i].ready != 0;
    if (ready == world - 1) break;
    const struct timespec pause = {0, 1000000};
    nanosleep(&pause, NULL);
  }
  unsigned ready = 0;
  for (unsigned i = 0; i < world; ++i) {
    ready += t[0]->peers[i].ready != 0;
    if (t[0]->peers[i].ready && t[0]->peers[i].rank == 1)
      busy_fd = t[0]->peers[i].fd;
  }
  assert(ready == world - 1 && t[1]->peers[0].ready);
  fresh_client = t[3];
  fresh_idle_seen = fresh_sent = fresh_received = fresh_after = 0;
  for (unsigned i = 0; i < world; ++i)
      if (t[0]->peers[i].ready && t[0]->peers[i].rank == 3) {
        fresh_fd = t[0]->peers[i].fd;
        t[0]->peer_cursor = i;
      }
  for (unsigned i = 0; i < 256; ++i) {
    PeakMpiObserverRecord r = {0};
    r.incarnation = 101;
    r.thread_uid = r.thread_generation = 1;
    r.sequence = i + 1;
    r.kind = PEAK_MPI_OBSERVER_EVENT_START;
    r.phase = PEAK_MPI_CAUSAL_CALL_BEGIN;
    r.operation = PEAK_MPI_CAUSAL_SEND;
    r.op_uid = 10 + i;
    r.peer = 0;
    assert(enqueue(t[1], RECORD, &r));
  }
  peak_mpi_causal_transport_tick(t[1], NULL, NULL, 2, 65536);
  assert(t[1]->send_index == t[1]->count);
  {
    for (unsigned i = 0; i < 6; ++i) {
      PeakMpiObserverRecord r = {0};
      r.incarnation = 102;
      r.thread_uid = r.thread_generation = 1;
      r.sequence = r.op_uid = 1 + i;
      r.kind = PEAK_MPI_OBSERVER_EVENT_START;
      assert(enqueue(t[2], RECORD, &r));
    }
    peak_mpi_causal_transport_tick(t[2], NULL, NULL, 2, 65536);
    assert(t[2]->send_index == t[2]->count);
  }
  rx_calls = rx_success = rx_eagain = idle_eagain = 0;
  busy_accepted = rejected_calls = nonce_calls = feedback_calls = 0;
  reject_rank = 2;
  reject_after = 5;
  blocked_accepted = 0;
  watching = 1;
  uint64_t before = t[0]->stats.sent_bytes + t[0]->stats.received_bytes;
  size_t left = peak_mpi_causal_transport_tick(t[0], NULL, &dormant, 2, 65536);
  unsigned measured_rx = rx_calls, measured_success = rx_success;
  unsigned measured_eagain = rx_eagain, measured_idle_eagain = idle_eagain;
  unsigned measured_nonce = nonce_calls, measured_feedbacks = feedback_calls;
  uint64_t spent = t[0]->stats.sent_bytes + t[0]->stats.received_bytes - before;
  assert(spent <= 65536 && spent + left == 65536 && busy_accepted == 256);
  assert(measured_idle_eagain <= (world - 2) * (256 / SEND_BATCH_FRAMES + 2));
  assert(rejected_calls <= 256 / SEND_BATCH_FRAMES + 2);
  assert(t[0]->accepted[2] == 5 && rejected_calls > 1);
  assert(fresh_sent && fresh_received == 1 && fresh_after <= SEND_BATCH_FRAMES);
  for (unsigned r = 1; r < world; ++r) {
    peak_mpi_causal_transport_tick(t[r], NULL, NULL, 3, 65536);
    assert(t[r]->requested_nonce == 4);
    PeakMpiPolicyFeedback f;
    assert(peak_mpi_causal_transport_feedback(t[r], &f) && f.generation == 77);
    if (r == 2)
      assert(t[r]->count == 1);
  }
  watching = 0;
  printf("world=%u busy_records=%u rejected_attempts=%u owner_recv=%u "
         "recv_success=%u EAGAIN=%u idle_EAGAIN=%u nonce_callbacks=%u "
         "feedback_callbacks=%u charged_bytes=%llu left=%zu accepted_blocked=%u "
         "fresh_after_busy=%u\n",
         world, busy_accepted, rejected_calls, measured_rx, measured_success,
         measured_eagain, measured_idle_eagain, measured_nonce,
         measured_feedbacks, (unsigned long long)spent, left, blocked_accepted,
         fresh_after);

  PeakMpiObserverConfig empty_config = {1, 256, 1, 100};
  PeakMpiObserver *empty_capture = peak_mpi_observer_create(&empty_config);
  assert(empty_capture);
  raw.incarnation = t[0]->c.incarnation;
  rx_eagain = 0;
  watching = 1;
  peak_mpi_causal_transport_tick(t[0], empty_capture, &raw, 4, 65536);
  watching = 0;
  assert(rx_eagain <= world - 1);
  /* Input and ACKs still progress on the next turn with no local capture.
   * Lower the owner's round cap to require the second pass. */
  t[0]->c.queue_frames = 4;
  PeakMpiObserverRecord next = {.incarnation = 101, .thread_uid = 1,
      .thread_generation = 1, .sequence = 257, .op_uid = 266,
      .kind = PEAK_MPI_OBSERVER_EVENT_START};
  for (unsigned i = 0; i < 10; ++i) {
    assert(enqueue(t[1], RECORD, &next));
    ++next.sequence;
    ++next.op_uid;
  }
  peak_mpi_causal_transport_tick(t[1], NULL, NULL, 5, 65536);
  struct pollfd input = {busy_fd, POLLIN, 0};
  assert(poll(&input, 1, 1000) == 1 && (input.revents & POLLIN));
  peak_mpi_causal_transport_tick(t[0], empty_capture, &raw, 5, 65536);
  struct pollfd ack = {t[1]->peers[0].fd, POLLIN, 0};
  assert(poll(&ack, 1, 1000) == 1 && (ack.revents & POLLIN));
  peak_mpi_causal_transport_tick(t[1], NULL, NULL, 5, 65536);
  assert(t[0]->accepted[1] == 266 && !t[1]->count);
  peak_mpi_observer_destroy(empty_capture);

  for (unsigned r = 0; r < world; ++r)
    peak_mpi_causal_transport_destroy(t[r]);
  fresh_client = NULL;
}

static struct {
  uint64_t generation;
  unsigned remote_copies, local_copies, records;
  int available, change_on_raw;
} feedback_probe;
static int probe_feedback(void *p, unsigned rank, double now,
                          PeakMpiPolicyFeedback *f) {
  (void)p;
  if (!feedback_probe.available) return 0;
  if (rank) ++feedback_probe.remote_copies;
  else ++feedback_probe.local_copies;
  memset(f, 0, sizeof *f);
  f->generation = feedback_probe.generation;
  f->member.process = rank;
  f->member.incarnation = 100 + rank;
  f->cohort = (PeakMpiPolicyCohortKey){124, 0, 1};
  f->mode = PEAK_MPI_POLICY_COARSE;
  f->member_count = 2;
  f->g = 2;
  f->q = .1;
  f->age_seconds = now;
  f->recovery.epoch = feedback_probe.generation;
  f->recovery.state = PEAK_MPI_RECOVERY_QUERY;
  return 1;
}
static int probe_feedback_generation(void *p, unsigned rank, uint64_t *out) {
  (void)p; (void)rank;
  if (!feedback_probe.available) return 0;
  *out = feedback_probe.generation;
  return 1;
}
static int probe_raw(void *p, unsigned rank, const PeakMpiCausalRaw *r,
                     double now) {
  (void)p; (void)rank; (void)r; (void)now;
  if (feedback_probe.change_on_raw) {
    ++feedback_probe.generation;
    feedback_probe.change_on_raw = 0;
  }
  return 1;
}
static int probe_record(void *p, unsigned rank, const PeakMpiObserverRecord *r) {
  (void)p; (void)r;
  assert(rank == 1);
  ++feedback_probe.records;
  return 1;
}
static uint64_t probe_nonce(void *p, unsigned rank) {
  (void)p; (void)rank;
  return 9;
}
static void test_feedback_materialization(int use_peek) {
  memset(&feedback_probe, 0, sizeof feedback_probe);
  PeakMpiCausalServiceConfig c = {0};
  c.world_size = 2; c.owner_rank = 0; c.job_context = 124;
  c.address = "127.0.0.1"; c.port = 23673; c.slots = 1; c.queue_frames = 64;
  PeakMpiCausalTransportSink sink = {.feedback = probe_feedback,
      .raw = probe_raw, .record = probe_record, .request_nonce = probe_nonce,
      .feedback_generation = use_peek ? probe_feedback_generation : NULL};
  PeakMpiCausalTransport *t[2];
  for (unsigned rank = 0; rank < 2; ++rank) {
    c.rank = rank; c.incarnation = 100 + rank;
    t[rank] = peak_mpi_causal_transport_create(&c, &sink);
    assert(t[rank]);
  }
  unsigned char members[2] = {1, 1};
  uint64_t prefixes[2] = {0};
  PeakMpiCausalRaw raw[2] = {0};
  for (unsigned rank = 0; rank < 2; ++rank) {
    raw[rank].active = raw[rank].generation = 1;
    raw[rank].incarnation = 100 + rank;
    raw[rank].known_peers = members; raw[rank].peer_count = 2;
    raw[rank].slot_prefixes = &prefixes[rank]; raw[rank].slot_count = 1;
  }
  for (unsigned tries = 0; tries < 1000; ++tries) {
    peak_mpi_causal_transport_tick(t[0], NULL, &raw[0], 1, 65536);
    peak_mpi_causal_transport_tick(t[1], NULL, &raw[1], 1, 65536);
    if ((t[0]->peers[0].ready || t[0]->peers[1].ready) && t[1]->peers[0].ready) break;
    const struct timespec pause = {0, 1000000};
    nanosleep(&pause, NULL);
  }
  assert((t[0]->peers[0].ready || t[0]->peers[1].ready) && t[1]->peers[0].ready);
  Peer *peer = t[0]->peers[0].ready ? &t[0]->peers[0] : &t[0]->peers[1];
  assert(!peer->feedback_sent && !t[1]->have_feedback &&
         !feedback_probe.remote_copies);
  feedback_probe.available = 1;
  for (unsigned i = 0; i < 32; ++i) {
    PeakMpiObserverRecord r = {0};
    r.incarnation = 101; r.thread_uid = r.thread_generation = 1;
    r.sequence = r.op_uid = i + 1; r.kind = PEAK_MPI_OBSERVER_EVENT_START;
    r.operation = PEAK_MPI_CAUSAL_SEND; r.phase = PEAK_MPI_CAUSAL_CALL_BEGIN;
    assert(enqueue(t[1], RECORD, &r));
  }
  peak_mpi_causal_transport_tick(t[1], NULL, NULL, 2, 65536);
  peak_mpi_causal_transport_tick(t[0], NULL, NULL, 2, 65536);
  peak_mpi_causal_transport_tick(t[1], NULL, NULL, 2, 65536);
  PeakMpiPolicyFeedback f;
  assert(feedback_probe.records == 32 && !t[1]->count);
  assert(t[1]->requested_nonce == 9 &&
         peak_mpi_causal_transport_feedback(t[1], &f) && f.generation == 0);
  for (unsigned tick = 0; tick < 3; ++tick)
    peak_mpi_causal_transport_tick(t[0], NULL, NULL, 3 + tick, 65536);
  fprintf(stderr, "stable feedback peek=%d generation0 records=%u remote_copies=%u "
          "owner_local_copies=%u\n", use_peek, feedback_probe.records,
          feedback_probe.remote_copies, feedback_probe.local_copies);
  assert(feedback_probe.local_copies == 4);
  assert(use_peek ? feedback_probe.remote_copies == 1
                  : feedback_probe.remote_copies > 1);
  feedback_probe.change_on_raw = 1;
  ++raw[1].generation;
  peak_mpi_causal_transport_tick(t[1], NULL, &raw[1], 6, 65536);
  peak_mpi_causal_transport_tick(t[0], NULL, NULL, 6, 65536);
  peak_mpi_causal_transport_tick(t[1], NULL, NULL, 6, 65536);
  assert(peak_mpi_causal_transport_feedback(t[1], &f) && f.generation == 1 &&
         f.recovery.epoch == 1 && !feedback_probe.change_on_raw);
  assert(!use_peek || feedback_probe.remote_copies == 2);
  unsigned copies = feedback_probe.remote_copies;
  feedback_probe.available = 0;
  feedback_probe.generation = 2;
  peak_mpi_causal_transport_tick(t[0], NULL, NULL, 7, 65536);
  assert(peer->feedback_sent && peer->feedback_generation == 1 &&
         feedback_probe.remote_copies == copies);
  feedback_probe.available = 1;
  peak_mpi_causal_transport_tick(t[0], NULL, NULL, 8, 65536);
  peak_mpi_causal_transport_tick(t[1], NULL, NULL, 8, 65536);
  assert(peak_mpi_causal_transport_feedback(t[1], &f) && f.generation == 2 &&
         f.recovery.epoch == 2);
  assert(!use_peek || feedback_probe.remote_copies == 3);
  for (unsigned rank = 0; rank < 2; ++rank)
    peak_mpi_causal_transport_destroy(t[rank]);
}

static PeakMpiObserver *pressure_source;
static PeakMpiObserverProducer pressure_writer;
static unsigned pressure_remote, pressure_local, pressure_lost_at, pressure_objects;
static int reject_local;
static PeakMpiObserverSlotState pressure_at_loss;
static int pressure_object(void *context, unsigned rank, uint64_t incarnation,
                            const PeakMpiObserverObject *object) {
  (void)context;
  (void)incarnation;
  (void)object;
  assert(rank == 0);
  ++pressure_objects;
  return 1;
}
static int pressure_record(void *context, unsigned rank,
                            const PeakMpiObserverRecord *record) {
  (void)context;
  (void)record;
  if (!rank) {
    if (reject_local)
      return 0;
    ++pressure_local;
    return 1;
  }
  assert(rank == 1);
  if (++pressure_remote == 2) {
    PeakMpiObserverObject object = {.kind = PEAK_MPI_CAUSAL_OBJECT_COMM, .generation = 1};
    uint64_t uid;
    assert(!peak_mpi_observer_object_add(pressure_source, &object, &uid) && uid == 1);
  }
  if (pressure_remote % 2 == 0) {
    PeakMpiObserverRecord event = {.kind = PEAK_MPI_OBSERVER_EVENT_START, .op_uid = 1};
    int rc = peak_mpi_observer_emit(pressure_source, &pressure_writer, &event);
    if (rc == PEAK_MPI_OBSERVER_OVERFLOW && !pressure_lost_at) {
      pressure_lost_at = pressure_remote;
      assert(peak_mpi_observer_slot_state(pressure_source, 0, &pressure_at_loss));
    }
  }
  return 1;
}
static void test_local_capture_before_receive(void) {
  PeakMpiCausalServiceConfig c = {0};
  c.world_size = 2;
  c.incarnation = 100;
  c.job_context = 90;
  c.address = "127.0.0.1";
  c.port = 39889;
  c.queue_frames = 256;
  c.slots = 1;
  PeakMpiCausalTransportSink sink = {.record = pressure_record, .object = pressure_object};
  PeakMpiCausalTransport *owner = peak_mpi_causal_transport_create(&c, &sink);
  c.rank = 1;
  c.incarnation = 101;
  PeakMpiCausalTransport *client = peak_mpi_causal_transport_create(&c, &sink);
  assert(owner && client);
  unsigned char members[2] = {1, 1};
  uint64_t prefix = 0;
  PeakMpiCausalRaw raw = {.incarnation = 100, .generation = 1, .active = 1,
      .elapsed = 1, .known_peers = members, .peer_count = 2,
      .slot_prefixes = &prefix, .slot_count = 1};
  peak_mpi_causal_transport_tick(owner, NULL, &raw, 1, 65536);
  raw.incarnation = 101;
  for (unsigned i = 0; i < 32; ++i) {
    peak_mpi_causal_transport_tick(client, NULL, &raw, 1, 65536);
    peak_mpi_causal_transport_tick(owner, NULL, NULL, 1, 65536);
  }
  assert(client->peers[0].ready);
  Peer *peer = NULL;
  for (unsigned i = 0; i < owner->peer_n; ++i)
    if (owner->peers[i].ready && owner->peers[i].rank == 1)
      peer = &owner->peers[i];
  assert(peer);
  PeakMpiObserverConfig config = {1, 256, 1, 100};
  pressure_source = peak_mpi_observer_create(&config);
  assert(pressure_source);
  peak_mpi_observer_activate(pressure_source);
  assert(!peak_mpi_observer_claim(pressure_source, 0, 1, 1, &pressure_writer));
  config.incarnation = 101;
  PeakMpiObserver *remote = peak_mpi_observer_create(&config);
  assert(remote);
  peak_mpi_observer_activate(remote);
  PeakMpiObserverProducer remote_writer;
  assert(!peak_mpi_observer_claim(remote, 0, 1, 1, &remote_writer));
  PeakMpiObserverRecord event = {.kind = PEAK_MPI_OBSERVER_EVENT_START, .op_uid = 1};
  for (unsigned i = 0; i < 192; ++i)
    assert(!peak_mpi_observer_emit(pressure_source, &pressure_writer, &event));
  for (unsigned i = 0; i < 254; ++i)
    assert(!peak_mpi_observer_emit(remote, &remote_writer, &event));
  for (unsigned i = 0; i < 16 && client->send_index < 254; ++i)
    peak_mpi_causal_transport_tick(client, remote, NULL, 1, 65536);
  assert(client->count == 254 && client->send_index == 254);
  struct pollfd readable = {peer->fd, POLLIN, 0};
  assert(poll(&readable, 1, 1000) == 1 && (readable.revents & POLLIN));
  /* Remote processing can overlap new producer records. Make local room first,
   * then retain one frozen prefix even when IO publishes a new object/tail. */
  size_t left = peak_mpi_causal_transport_tick(owner, pressure_source, NULL, 1, 65536);
  PeakMpiObserverSlotState after;
  assert(peak_mpi_observer_slot_state(pressure_source, 0, &after));
  printf("local-first remote=%u local=%u first_loss_remote=%u loss_head=%llu "
         "loss_tail=%llu after=%llu/%llu lost=%llu budget_left=%zu\n",
      pressure_remote, pressure_local, pressure_lost_at,
      (unsigned long long)pressure_at_loss.published,
      (unsigned long long)pressure_at_loss.consumed,
      (unsigned long long)after.published, (unsigned long long)after.consumed,
      (unsigned long long)after.lost, left);
  fflush(stdout);
  assert(pressure_remote == 254 && !pressure_lost_at);
  assert(!after.lost && after.published == 319 && after.consumed == 192 && left <= 65536);
  assert(!pressure_objects);
  peak_mpi_causal_transport_tick(owner, pressure_source, NULL, 1, 65536);
  assert(peak_mpi_observer_slot_state(pressure_source, 0, &after));
  assert(pressure_objects == 1 && pressure_local == 319 && after.consumed == 319);
  /* A held local record can be admitted by the late capture too. */
  assert(!peak_mpi_observer_emit(pressure_source, &pressure_writer, &event));
  owner->capture_first = reject_local = 1;
  peak_mpi_causal_transport_tick(owner, pressure_source, NULL, 1, 384);
  assert(owner->held && pressure_local == 319);
  owner->capture_first = 1;
  reject_local = 0;
  peak_mpi_causal_transport_tick(owner, pressure_source, NULL, 1, 384);
  assert(!owner->held && pressure_local == 320);
  /* A definition alone must trigger the second send after an idle first pass. */
  peak_mpi_causal_transport_tick(client, NULL, NULL, 1, 65536);
  PeakMpiObserverObject definition = {.kind = PEAK_MPI_CAUSAL_OBJECT_COMM,
                                       .generation = 1};
  uint64_t definition_uid;
  assert(!peak_mpi_observer_object_add(remote, &definition, &definition_uid));
  client->capture_first = 1;
  peak_mpi_causal_transport_tick(client, remote, NULL, 1, 511);
  assert(client->next_object_uid == definition_uid + 1 &&
         client->count == 1 && client->send_index == 1);
  peak_mpi_causal_transport_destroy(client);
  peak_mpi_causal_transport_destroy(owner);
  peak_mpi_observer_destroy(remote);
  peak_mpi_observer_destroy(pressure_source);
  puts("single frozen object prefix and callback-produced tail deferred to next turn PASS");
}

int main(int argc, char **argv) {
  test_local_capture_before_receive();
  test_feedback_materialization(0);
  test_feedback_materialization(1);
  size_t budget = argc > 1 ? (size_t)strtoul(argv[1], NULL, 10) : 256;
  PeakMpiCausalServiceConfig c = {0};
  c.world_size = 2;
  c.owner_rank = 0;
  c.rank = 0;
  c.job_context = 123;
  c.incarnation = 11;
  c.address = "127.0.0.1";
  c.port = 23541;
  c.queue_frames = 16;
  c.slots = 1;
  PeakMpiCausalTransportSink sink = {.record = record,
                                     .raw = raw_sink,
                                     .feedback = feedback,
                                     .fault = sink_fault,
                                     .request_nonce = nonce};
  PeakMpiCausalTransport *o = peak_mpi_causal_transport_create(&c, &sink);
  assert(o);
  c.rank = 1;
  c.incarnation = 22;
  PeakMpiCausalTransport *t = peak_mpi_causal_transport_create(&c, &sink);
  assert(t);
  unsigned char peers[2] = {1, 1};
  uint64_t prefixes[1] = {0};
  PeakMpiCausalRaw raw = {0};
  raw.active = 1;
  raw.incarnation = 11;
  raw.generation = 1;
  raw.elapsed = 1;
  raw.known_peers = peers;
  raw.peer_count = 2;
  raw.slot_prefixes = prefixes;
  raw.slot_count = 1;
  for (unsigned k = 0; k < 32; ++k) {
    peak_mpi_causal_transport_tick(o, NULL, &raw, 1 + k * .01, 4096);
    raw.incarnation = 22;
    peak_mpi_causal_transport_tick(t, NULL, &raw, 1 + k * .01, 4096);
    raw.incarnation = 11;
  }
  assert(o->peers[0].ready && t->peers[0].ready && !faults);
  PeakMpiObserverConfig oc = {1, 32, 8, 11};
  local_capture = peak_mpi_observer_create(&oc);
  assert(local_capture);
  peak_mpi_observer_activate(local_capture);
  assert(peak_mpi_observer_claim(local_capture, 0, 1, 1, &local_producer) ==
         PEAK_MPI_OBSERVER_OK);
  PeakMpiObserverRecord r = {0};
  r.kind = PEAK_MPI_OBSERVER_EVENT_START;
  r.op_uid = 5;
  assert(peak_mpi_observer_emit(local_capture, &local_producer, &r) ==
         PEAK_MPI_OBSERVER_OK);
  r.incarnation = 22;
  assert(enqueue(t, RECORD, &r));
  size_t send_budget = 4096;
  client_tick(t, 2, &send_budget);
  assert(t->send_index == t->count);
  replenish = control = 1;
  for (unsigned k = 0; k < 64; ++k) {
    unsigned local_before = local_records;
    uint64_t io_before = o->stats.sent_bytes + o->stats.received_bytes;
    peak_mpi_causal_transport_tick(o, local_capture, &raw, 2 + k * .01, budget);
    uint64_t spent = o->stats.sent_bytes + o->stats.received_bytes - io_before +
                     (local_records - local_before) * 256;
    assert(spent <= budget);
    raw.incarnation = 22;
    peak_mpi_causal_transport_tick(t, NULL, &raw, 2 + k * .01, 4096);
    raw.incarnation = 11;
  }
  fprintf(stderr, "local=%u remote=%u nonce=%llu faults=%u\n", local_records,
          remote_records,
          (unsigned long long)peak_mpi_causal_transport_requested_nonce(t),
          faults);
  assert(local_records > 0 && remote_records == 1);
  assert(peak_mpi_causal_transport_requested_nonce(t) == 7);
  PeakMpiPolicyFeedback f;
  assert(peak_mpi_causal_transport_feedback(t, &f) && f.generation == 77);
  assert(!faults);
  puts("sustained local capture plus readable TCP remote and control bounded "
       "progress PASS");
  replenish = control = 0;
  PeakMpiObserverRecord discard;
  while (peak_mpi_observer_consume(local_capture, 0, &discard)) {
  }
  unsigned before = local_records;
  for (unsigned k = 0; k < 4; k++)
    assert(peak_mpi_observer_emit(local_capture, &local_producer, &r) ==
           PEAK_MPI_OBSERVER_OK);
  peak_mpi_causal_transport_tick(o, local_capture, &raw, 4, 1024);
  assert(local_records == before + 4);
  puts("idle remote share borrowed by local capture PASS");
  before = remote_records;
  for (unsigned k = 0; k < 4; k++)
    assert(enqueue(t, RECORD, &r));
  send_budget = 4096;
  client_tick(t, 5, &send_budget);
  peak_mpi_causal_transport_tick(o, local_capture, &raw, 5, 2048);
  assert(remote_records == before + 4);
  puts("idle local share borrowed by readable remote PASS");
  peak_mpi_causal_transport_destroy(t);
  peak_mpi_causal_transport_destroy(o);
  peak_mpi_observer_destroy(local_capture);
  test_many_peer_fairness();
  test_drain_raw();
  test_owner_round_batches();
}
