#define _POSIX_C_SOURCE 200809L
#include "../../src/mpi_causal_transport.c"
#include <assert.h>
#include <stdio.h>
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
int main(int argc, char **argv) {
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
}
