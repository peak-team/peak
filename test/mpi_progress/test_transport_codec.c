#define _POSIX_C_SOURCE 200809L
#include "../../src/mpi_causal_transport.c"
#include <assert.h>
#include <stdio.h>
static uint64_t rng = 1234567;
static uint64_t next(void) {
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return rng;
}
static double bits(uint64_t v) {
  double d;
  memcpy(&d, &v, 8);
  return d;
}
int main(void) {
  /* PMC7 carries the three previously reserved group-cut words. Exercise all
   * 256 dense bytes and place the sentinel after that exact fixed bound. */
  _Static_assert(sizeof(PeakMpiObserverRecord) == 256, "fixed producer record");
  unsigned char dense[2828], packed[2829], again[2828], peers[256],
      outpeers[256];
  uint64_t prefixes[288], outprefix[288];
  PeakMpiCausalRaw a = {0}, b;
  PeakMpiObserverRecord e, f;
  unsigned dense_records = 0, dense_raws = 0;
  for (unsigned k = 0; k < 2048; k++) {
    for (unsigned i = 0; i < 256; i++)
      dense[i] = (k % 2) ? (unsigned char)next() : 0;
    record_decode(dense, &e);
    if (k % 4 == 1) {
#define S64(f) e.f = (next() % 4) ? 0 : next() % 100000;
#define S32(f)                                                                 \
  e.f = (next() % 4) ? 0 : (next() % 2 ? UINT32_MAX : (uint32_t)(next() % 100));
      R64(S64)
      R32(S32)
#undef S64
#undef S32
          record_encode(dense, &e);
    }
    packed[256] = 0xab;
    size_t n = packed_record(packed, &e, e.incarnation);
    assert(n <= 256);
    if (n < 256) {
      assert(unpack_record(packed, n, e.incarnation, &f));
      for (size_t cut = 0; cut < n; cut++)
        assert(!unpack_record(packed, cut, e.incarnation, &f));
      assert(unpack_record(packed, n, e.incarnation, &f));
    } else {
      dense_records++;
      record_encode(packed, &e);
      record_decode(packed, &f);
    }
    record_encode(again, &f);
    assert(!memcmp(dense, again, 256));
    assert(packed[256] == 0xab);
    memset(&a, 0, sizeof a);
    a.incarnation = next();
    a.generation = k + 1;
    a.peer_count = 256;
    a.known_peers = peers;
    a.slot_count = 288;
    a.slot_prefixes = prefixes;
    a.L = bits(k % 2 ? next() : UINT64_C(0x8000000000000000));
    a.A = bits(k % 2 ? next() : UINT64_C(0x7ff8000000012345));
    a.P = bits(next());
    a.elapsed = bits(next());
    a.cut.P_ns = next();
    a.cut.A_ns = next();
    a.cut.L_ns = next();
    a.unknown_members = k % 2;
    a.recovery.epoch = 1;
    a.recovery.state = 0;
    a.recovery.L = bits(next());
    a.recovery.A = bits(next());
    a.recovery.P = bits(next());
    a.recovery.cut.P_ns = next();
    for (unsigned i = 0; i < 256; i++)
      peers[i] =
          (k % 5 == 0)
              ? 1
              : (k % 5 == 1 ? 0
                            : (k % 5 == 2 ? i != k % 256
                                          : (k % 5 == 3 ? (i == 1 || i == 255)
                                                        : (next() & 1))));
    for (unsigned i = 0; i < 288; i++)
      prefixes[i] = k % 2 ? next() : (i == 0 ? k : 0);
    size_t full = raw_encode(dense, &a);
    assert(full == 2828);
    packed[2828] = 0xcd;
    n = packed_raw(packed, &a, a.incarnation);
    assert(n <= full);
    if (n < full) {
      assert(unpack_raw(packed, n, a.incarnation, &b, outpeers, 256, outprefix,
                        288));
      for (size_t cut = 0; cut < n; cut++)
        assert(!unpack_raw(packed, cut, a.incarnation, &b, outpeers, 256,
                           outprefix, 288));
      assert(unpack_raw(packed, n, a.incarnation, &b, outpeers, 256, outprefix,
                        288));
    } else {
      dense_raws++;
      raw_encode(packed, &a);
      assert(raw_decode(packed, full, &b, outpeers, 256, outprefix, 288));
    }
    assert(raw_encode(again, &b) == full && !memcmp(dense, again, full));
    assert(packed[2828] == 0xcd);
  }
  for (unsigned k = 0; k < 2048; k++) {
    PeakMpiPolicyFeedback x = {0}, y;
#define V64(f) x.f = (k % 2) ? next() : next() % 3;
#define VD(f)                                                                  \
  x.f = bits((k % 2) ? next()                                                  \
                     : (next() % 3 ? 0 : UINT64_C(0x8000000000000000)));
#define VI(f) x.f = (k % 2) ? (uint32_t)next() : (next() % 3);
    F64(V64)
    FD(VD) FI(VI)
#undef V64
#undef VD
#undef VI
        x.member_count = next();
    x.approximate = (uint32_t)next();
    x.recovery.epoch = 1;
    x.recovery.L = bits(next());
    x.recovery.P = bits(next());
    x.recovery.app_cost_known = next();
    x.recovery.cut.L_ns = next();
    size_t full = feedback_encode(dense, &x);
    assert(full == 432);
    packed[432] = 0xef;
    size_t n = packed_feedback(packed, &x);
    assert(n <= full);
    if (n < full) {
      assert(unpack_feedback(packed, n, &y));
      for (size_t cut = 0; cut < n; cut++)
        assert(!unpack_feedback(packed, cut, &y));
      assert(unpack_feedback(packed, n, &y));
    } else {
      feedback_encode(packed, &x);
      feedback_decode(packed, &y);
    }
    assert(feedback_encode(again, &y) == full && !memcmp(dense, again, full));
    assert(packed[432] == 0xef);
  }
  puts("2048 FEEDBACK bitexact "
       "alltruncations/densefallback/floatbits/generations/knownness PASS");
  {
    PeakMpiObserverRecord x = {0}, y;
    x.incarnation = 22;
    x.sequence = 1;
    size_t n = packed_record(packed, &x, 22);
    assert(n < 256);
    packed[0] |= 1;
    assert(!unpack_record(packed, n, 22, &y));
    assert(packed_record(packed, &x, 23) == 256);
    a.incarnation = 22;
    assert(packed_raw(packed, &a, 23) == 2828);
  }
  puts("same-frame incarnation dedup reservedbit/mismatch fallback PASS");
  /* A mismatched private version is rejected before payload dispatch. */
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  assert(nonblock(sockets[1]));
  unsigned char h[HDR] = {0}, *hp = h;
  p32(&hp, UINT32_C(0x504d4335));
  p32(&hp, HELLO);
  Peer peer = {.fd = sockets[1], .in = packed, .need = HDR};
  PeakMpiCausalTransport transport = {.cap = sizeof packed};
  assert(write(sockets[0], h, HDR) == HDR);
  size_t budget = HDR;
  assert(read_frame(&transport, &peer, &budget) == -1);
  close(sockets[0]);
  close(sockets[1]);
  unsigned char bad[10];
  memset(bad, 255, sizeof bad);
  PackedIn v = {bad, bad + 10, 1};
  (void)unpack_uint(&v);
  assert(!v.ok);
  puts("2048 bitexact record/RAW roundtrips, alltruncations, "
       "NaN/-0/recoveryNONE/capacity/varintoverflow PASS");
  printf("densefallback records=%u RAW=%u\n", dense_records, dense_raws);
  return 0;
}
