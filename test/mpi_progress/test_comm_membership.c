#define _POSIX_C_SOURCE 200809L
#include "../../src/mpi_causal_backend.c"
#include <assert.h>
#include <stdio.h>
static PeakMpiCausalService *make(unsigned world) {
  PeakMpiCausalServiceConfig c = {0};
  c.world_size = world;
  c.rank = c.owner_rank = 0;
  c.job_context = 99;
  c.incarnation = 100;
  c.address = "127.0.0.1";
  c.port = 39877;
  c.slots = 1;
  c.queue_frames = 16;
  c.max_operations = 64;
  c.max_context_parts = 4096;
  c.max_pending_records = 64;
  c.model = (PeakMpiCostConfig){1024, 4096, 1024, 128, world, 64};
  c.policy = (PeakMpiPolicyConfig){world, .1, 1, .5, 0, 0, 1};
  PeakMpiCausalService *s = peak_mpi_causal_service_create(&c);
  assert(s);
  return s;
}
static void part(PeakMpiCausalService *s, unsigned rank, uint64_t uid,
                 uint64_t parent, unsigned kind, uint64_t ordinal, unsigned off,
                 unsigned size) {
  PeakMpiObserverObject o = {0};
  o.kind = PEAK_MPI_CAUSAL_OBJECT_COMM;
  o.part_count = (size + 15) / 16;
  PeakMpiCausalCommPart p = {0};
  p.local_uid = uid;
  p.parent_uid = parent;
  p.creation_kind = kind;
  p.creation_ordinal = ordinal;
  p.world_size = s->config.world_size;
  p.local_size = size;
  p.member_offset = off;
  p.member_count = size - off < 16 ? size - off : 16;
  for (unsigned j = 0; j < p.member_count; j++)
    p.members[j] = (int)(off + j);
  memcpy(o.payload, &p, sizeof p);
  assert(peak_mpi_causal_service_ingest_object(s, rank, 100 + rank, &o));
}
static void listpart(PeakMpiCausalService *s, unsigned rank, uint64_t uid,
                     uint64_t ordinal, unsigned off, unsigned count,
                     const int *values) {
  PeakMpiObserverObject o = {0};
  o.kind = PEAK_MPI_CAUSAL_OBJECT_COMM;
  o.part_count = 1;
  PeakMpiCausalCommPart p = {0};
  p.local_uid = uid;
  p.parent_uid = 1;
  p.creation_kind = PEAK_MPI_CAUSAL_COMM_SPLIT;
  p.creation_ordinal = ordinal;
  p.world_size = s->config.world_size;
  p.local_size = 4;
  p.member_offset = off;
  p.member_count = count;
  for (unsigned j = 0; j < count; j++)
    p.members[j] = values[j];
  memcpy(o.payload, &p, sizeof p);
  assert(peak_mpi_causal_service_ingest_object(s, rank, 100 + rank, &o));
}
static void immutable_tests(void) {
  PeakMpiCausalService *s = make(8);
  for (unsigned r = 0; r < 8; r++)
    part(s, r, 1, 0, PEAK_MPI_CAUSAL_COMM_WORLD, 0, 0, 8);
  int a[] = {0, 2, 4, 6}, b[] = {1, 3, 5, 7};
  listpart(s, 0, 10, 1, 2, 2, a + 2);
  assert(!context_for(s, 0, 10));
  listpart(s, 0, 10, 1, 2, 2, a + 2);
  assert(s->comms[8].got == 2);
  listpart(s, 1, 10, 1, 0, 4, a);
  assert(context_for(s, 1, 10));
  assert(!context_for(s, 0, 10));
  listpart(s, 0, 10, 1, 0, 2, a);
  assert(context_for(s, 0, 10) == context_for(s, 1, 10));
  listpart(s, 0, 11, 2, 0, 4, a);
  assert(context_for(s, 0, 11) != context_for(s, 0, 10));
  assert(member_root(s, 10) == member_root(s, 8));
  size_t spare = s->comms[10].member_offset;
  assert(s->comms[10].member_spare);
  listpart(s, 0, 12, 3, 0, 4, b);
  assert(s->comms[11].member_offset == spare);
  assert(member_root(s, 11) != member_root(s, 8));
  for (unsigned i = 0; i < 4; i++) {
    assert(member_value(s, 8, i) == a[i]);
    assert(member_value(s, 10, i) == a[i]);
    assert(member_value(s, 11, i) == b[i]);
  }
  /* A local handle generation has a distinct uid and context; replay cannot
   * alter old lineage. */
  listpart(s, 0, 13, 4, 0, 4, b);
  assert(context_for(s, 0, 13) != context_for(s, 0, 12));
  listpart(s, 0, 13, 5, 0, 4, b);
  assert(s->ranks[0].coarse);
  for (unsigned i = 0; i < 4; i++)
    assert(member_value(s, 8, i) == a[i]);
  peak_mpi_causal_service_destroy(s);
  puts("out-of-order private receipts, immutable sharing/reuse and distinct "
       "lineage PASS");
}
int main(void) {
  PeakMpiCausalService *s = make(256);
  for (unsigned r = 0; r < 256; r++)
    for (unsigned j = 0; j < 256; j += 16)
      part(s, r, 1, 0, PEAK_MPI_CAUSAL_COMM_WORLD, 0, j, 256);
  for (unsigned r = 0; r < 256; r++) {
    for (unsigned j = 0; j < 256; j += 16)
      part(s, r, 3, 1, PEAK_MPI_CAUSAL_COMM_DUPLICATED, 1, j, 256);
    if (s->ranks[r].coarse)
      fprintf(
          stderr, "RED worlddup rank=%u comms=%zu contexts=%zu membercap=%zu\n",
          r, s->comm_count, s->context_count, s->config.max_context_parts * 16);
    assert(!s->ranks[r].coarse && context_for(s, r, 3));
  }
  assert(context_for(s, 0, 3) != context_for(s, 0, 1));
  puts("256 WORLDdup fixedbudget distinctcontext PASS");
  peak_mpi_causal_service_destroy(s);
  immutable_tests();
  return 0;
}
