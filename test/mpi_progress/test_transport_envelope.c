#define _POSIX_C_SOURCE 200809L
#include "../../src/mpi_causal_transport.c"
#include <assert.h>
#include <stdio.h>
static unsigned faults, records;
static void bad(void *v, unsigned r, PeakMpiPolicyReason why) {
  (void)v;
  (void)r;
  (void)why;
  faults++;
}
static int got(void *v, unsigned r, const PeakMpiObserverRecord *e) {
  (void)v;
  assert(r == 1 && e->incarnation == 22);
  records++;
  return 1;
}
static int connection(PeakMpiCausalTransport *o) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(!connect(fd, (struct sockaddr *)&o->addr, o->addrlen));
  return fd;
}
static void pump(PeakMpiCausalTransport *o) {
  for (unsigned k = 0; k < 8; k++) {
    size_t budget = 65536;
    owner_tick(o, 1 + k * .01, &budget);
  }
}
static void send_frame(int fd, PeakMpiCausalTransport *sender, unsigned type,
                       uint64_t seq) {
  unsigned char b[4096];
  size_t n = 0;
  if (type == RECORD) {
    PeakMpiObserverRecord r = {0};
    r.incarnation = sender->c.incarnation;
    r.sequence = seq;
    n = record_encode(b + HDR, &r);
  }
  size_t size = frame(sender, b, type, n, seq);
  assert(write(fd, b, size) == (ssize_t)size);
}
static uint64_t random_word(void) {
  static uint64_t v = 123456789;
  v ^= v << 13;
  v ^= v >> 7;
  v ^= v << 17;
  return v;
}
int main(void) {
  _Static_assert(sizeof(PeakMpiObserverRecord) == 256, "fixed producer record");
  unsigned char b[4096], original[64];
  PeakMpiObserverRecord r = {0}, decoded;
  r.incarnation = 22; r.thread_uid = r.thread_generation = r.sequence = 1;
  r.flags = PEAK_MPI_CAUSAL_COMPACT_GROUP | PEAK_MPI_CAUSAL_ORDERED_CALLER;
  r.group_L_cut = UINT64_MAX; r.group_A_cut = UINT64_MAX - 1;
  r.group_P_cut = UINT64_C(0x123456789abcdef0);
  r.result_ref = UINT64_MAX; r.policy_generation = UINT64_MAX;
  size_t rn = record_encode(b, &r);
  assert(rn == 256);
  record_decode(b, &decoded);
  assert(!memcmp(&r, &decoded, sizeof r));
  rn = packed_record(b, &r, 22);
  assert(rn < 256 && unpack_record(b, rn, 22, &decoded));
  assert(!memcmp(&r, &decoded, sizeof r));
  PeakMpiCausalTransport old_receiver = {0};
  old_receiver.cap = sizeof b;
  FrameHeader old_header;
  unsigned char *old = b;
  p32(&old, UINT32_C(0x504d4336));
  assert(frame_header(&old_receiver, b, 4, &old_header) == -1);
  old = b; p32(&old, UINT32_C(0x504d4336) ^ UINT32_C(0x80000000));
  assert(frame_header(&old_receiver, b, 4, &old_header) == -1);
  PeakMpiCausalTransport t = {0}, sender_header = {0};
  t.cap = sizeof b;
  FrameHeader h;
  unsigned dense_headers = 0;
  for (unsigned k = 0; k < 2048; k++) {
    sender_header.c.rank = k % 2 ? (uint32_t)random_word() : k % 256;
    sender_header.c.job_context = k % 2 ? random_word() : 113294603;
    sender_header.c.incarnation =
        k % 2 ? random_word() : UINT64_C(0x0012c93b14158000) + k;
    uint64_t seq = k % 2 ? random_word() : k;
    size_t n = k % 64;
    for (size_t i = 0; i < n; i++)
      original[i] = b[HDR + i] = (unsigned char)random_word();
    b[HDR + n] = 0x7a;
    size_t total = frame(&sender_header, b, RECORD, n, seq);
    assert(frame_header(&t, b, total, &h) == 1);
    for (size_t cut = 0; cut < h.bytes; cut++)
      assert(frame_header(&t, b, cut, &h) == 0);
    assert(frame_header(&t, b, total, &h) == 1);
    assert(h.length == n && h.type == RECORD &&
           h.rank == sender_header.c.rank &&
           h.job == sender_header.c.job_context &&
           h.incarnation == sender_header.c.incarnation && h.sequence == seq);
    assert(total == h.bytes + n && total <= HDR + n &&
           !memcmp(b + h.bytes, original, n) && b[HDR + n] == 0x7a);
    const unsigned char *m = b;
    if (g32(&m) == DENSE_MAGIC)
      dense_headers++;
  }
  assert(dense_headers && dense_headers < 2048);
  unsigned char *p = b;
  p32(&p, DENSE_MAGIC);
  p32(&p, RECORD);
  p32(&p, UINT32_MAX - HDR);
  p32(&p, UINT32_MAX);
  p64(&p, UINT64_MAX);
  p64(&p, UINT64_MAX);
  p64(&p, UINT64_MAX);
  t.cap = UINT32_MAX;
  assert(frame_header(&t, b, HDR, &h) == 1 && h.bytes == HDR &&
         h.length == UINT32_MAX - HDR && h.rank == UINT32_MAX &&
         h.job == UINT64_MAX && h.incarnation == UINT64_MAX &&
         h.sequence == UINT64_MAX);
  for (size_t cut = 0; cut < HDR; cut++)
    assert(frame_header(&t, b, cut, &h) == 0);
  t.cap = sizeof b;
  p = b;
  p32(&p, MAGIC);
  *p++ = RECORD;
  memset(p, 255, 5);
  assert(frame_header(&t, b, 10, &h) == -1);
  p = b;
  p32(&p, MAGIC);
  *p++ = RECORD;
  PackedOut w = {p, b + 128, 1};
  pack_uint(&w, 0);
  pack_uint(&w, 0);
  pack_uint(&w, 0);
  pack_uint(&w, 1);
  memset(w.p, 255, 10);
  assert(frame_header(&t, b, (size_t)(w.p - b) + 10, &h) == -1);
  p = b;
  p32(&p, UINT32_C(0x504d4335));
  assert(frame_header(&t, b, 4, &h) == -1);
  PeakMpiCausalServiceConfig c = {0};
  c.rank = c.owner_rank = 0;
  c.world_size = 2;
  c.job_context = 123;
  c.incarnation = 11;
  c.address = "127.0.0.1";
  c.port = 23573;
  c.queue_frames = 8;
  c.slots = 1;
  PeakMpiCausalTransportSink sink = {.record = got, .fault = bad};
  for (unsigned scenario = 0; scenario < 9; scenario++) {
    faults = records = 0;
    PeakMpiCausalTransport *o = peak_mpi_causal_transport_create(&c, &sink);
    assert(o);
    pump(o);
    int fd = connection(o);
    PeakMpiCausalTransport sender = {0};
    sender.c = c;
    sender.c.rank = 1;
    sender.c.incarnation = 22;
    if (scenario == 0)
      send_frame(fd, &sender, RECORD, 1); /* before HELLO */
    else if (scenario == 1) {
      sender.c.job_context = 124;
      send_frame(fd, &sender, HELLO, 0);
    } else if (scenario == 2) {
      sender.c.rank = 2;
      send_frame(fd, &sender, HELLO, 0);
    } else if (scenario == 3) {
      sender.c.incarnation = 0;
      send_frame(fd, &sender, HELLO, 0);
    } else {
      send_frame(fd, &sender, HELLO, 0);
      pump(o);
      assert(!faults && o->peers[0].ready);
      if (scenario == 4) {
        sender.c.job_context = 124;
        send_frame(fd, &sender, HELLO, 0);
      }
      if (scenario == 5) {
        sender.c.incarnation = 23;
        send_frame(fd, &sender, RECORD, 1);
      }
      if (scenario == 6)
        send_frame(fd, &sender, RECORD, 2);
      if (scenario == 7) {
        unsigned char wire[4096];
        PeakMpiObserverRecord r = {0};
        r.incarnation = 23;
        assert(packed_record(wire + HDR, &r, 22) == 256);
        size_t n = record_encode(wire + HDR, &r);
        size_t size = frame(&sender, wire, RECORD, n, 1);
        assert(write(fd, wire, size) == (ssize_t)size);
      }
      if (scenario == 8) {
        unsigned char wire[4096], peers[2] = {1, 1};
        PeakMpiCausalRaw r = {0};
        r.incarnation = 23;
        r.peer_count = 2;
        r.known_peers = peers;
        size_t full = 140 + RECOVERY_BYTES + 2;
        assert(packed_raw(wire + HDR, &r, 22) == full);
        size_t n = raw_encode(wire + HDR, &r);
        size_t size = frame(&sender, wire, RAW, n, 0);
        assert(write(fd, wire, size) == (ssize_t)size);
      }
    }
    pump(o);
    assert(faults == 1 && records == 0);
    close(fd);
    peak_mpi_causal_transport_destroy(o);
  }
  /* Same-incarnation reconnect replays the original sequence without
   * redelivery. */
  faults = records = 0;
  PeakMpiCausalTransport *o = peak_mpi_causal_transport_create(&c, &sink);
  assert(o);
  pump(o);
  PeakMpiCausalTransport sender = {0};
  sender.c = c;
  sender.c.rank = 1;
  sender.c.incarnation = 22;
  int fd = connection(o);
  unsigned char hello[128];
  size_t hello_size = frame(&sender, hello, HELLO, 0, 0);
  for (size_t i = 0; i < hello_size; i++) {
    assert(write(fd, hello + i, 1) == 1);
    size_t one = 1;
    owner_tick(o, 1, &one);
  }
  send_frame(fd, &sender, HELLO, 0);
  send_frame(fd, &sender, HELLO, 0);
  pump(o);
  assert(!faults);
  send_frame(fd, &sender, RECORD, 1);
  pump(o);
  assert(records == 1 && !faults);
  send_frame(fd, &sender, HELLO, 0);
  send_frame(fd, &sender, HELLO, 0);
  pump(o);
  assert(records == 1 && !faults && o->accepted[1] == 1);
  close(fd);
  pump(o);
  fd = connection(o);
  send_frame(fd, &sender, HELLO, 0);
  pump(o);
  send_frame(fd, &sender, RECORD, 1);
  pump(o);
  assert(records == 1 && !faults && o->stats.duplicates == 1);
  close(fd);
  peak_mpi_causal_transport_destroy(o);
  /* Malformed feedback must not partially overwrite an already published tuple.
   */
  {
    int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    assert(nonblock(pair[1]));
    unsigned char in[4096], out[4096], wire[4096];
    uint64_t incarnations[2] = {11, 22};
    Peer p = {.fd = pair[1], .ready = 1, .in = in, .out = out, .need = HDR};
    PeakMpiCausalTransport receiver = {0}, server = {0};
    receiver.c = c;
    receiver.c.rank = 1;
    receiver.cap = sizeof in;
    receiver.peers = &p;
    receiver.incarnations = incarnations;
    receiver.sink.fault = bad;
    receiver.have_feedback = 1;
    receiver.feedback.generation = 77;
    receiver.feedback.L_cut = 3.5;
    server.c = c;
    memset(wire + HDR, 0, FEEDBACK_MASK_BYTES);
    size_t size = frame(&server, wire, PACKED_FEEDBACK, FEEDBACK_MASK_BYTES, 0);
    assert(write(pair[0], wire, size) == (ssize_t)size);
    size_t budget = 4096;
    client_tick(&receiver, 1, &budget);
    assert(receiver.have_feedback && receiver.feedback.generation == 77 &&
           receiver.feedback.L_cut == 3.5);
    close(pair[0]);
  }
  /* The first server control must carry the expected owner rank AND job. */
  for (unsigned wrong = 0; wrong < 2; wrong++) {
    int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    assert(nonblock(pair[1]));
    unsigned char in[4096], out[4096], wire[4096];
    uint64_t incarnations[2] = {0, 22};
    Peer p = {.fd = pair[1], .ready = 1, .in = in, .out = out, .need = HDR};
    PeakMpiCausalTransport receiver = {0}, server = {0};
    receiver.c = c;
    receiver.c.rank = 1;
    receiver.cap = sizeof in;
    receiver.peers = &p;
    receiver.incarnations = incarnations;
    receiver.sink.fault = bad;
    receiver.have_feedback = 1;
    receiver.feedback.generation = 77;
    server.c = c;
    if (wrong)
      server.c.rank = 1;
    else
      server.c.job_context = 124;
    PeakMpiPolicyFeedback f = {0};
    f.generation = 99;
    size_t payload = feedback_encode(wire + HDR, &f);
    size_t total = frame(&server, wire, FEEDBACK, payload, 0);
    assert(write(pair[0], wire, total) == (ssize_t)total);
    size_t budget = 4096;
    client_tick(&receiver, 1, &budget);
    assert(p.fd < 0 && receiver.feedback.generation == 77);
    close(pair[0]);
  }
  puts("all-ID compact/dense40/everypartial/overflow/version headers; "
       "preHELLO/job/rank/inc/conflictingHELLO/gapseq rejection; "
       "identicalHELLO idempotent and reconnect dedup PASS");
  return 0;
}
