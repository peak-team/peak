#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <stdio.h>
#ifndef PEAK_TEST_BACKEND_SOURCE
#define PEAK_TEST_BACKEND_SOURCE "../../src/mpi_causal_backend.c"
#endif
#include PEAK_TEST_BACKEND_SOURCE
static unsigned test_profile;
static size_t test_work = 100000;
static int test_buffered_request;
static uint64_t seqs[2];
static PeakMpiCausalService *make(void) {
  PeakMpiCausalServiceConfig c = {0};
  c.rank = c.owner_rank = 0;
  c.world_size = 2;
  c.job_context = 99;
  c.incarnation = 100;
  c.address = "127.0.0.1";
  c.port = 39877;
  c.slots = 2;
  c.queue_frames = 16;
  c.max_operations = 64;
  c.max_context_parts = 32;
  c.max_pending_records = 64;
  c.model = (PeakMpiCostConfig){1024, 4096, 1024, 128, 2, 64};
  c.policy = (PeakMpiPolicyConfig){2, .1, 1, .5, 0, 0, 1};
  PeakMpiCausalService *s = peak_mpi_causal_service_create(&c);
  assert(s);
  memset(seqs, 0, sizeof seqs);
  for (unsigned r = 0; r < 2; r++) {
    PeakMpiObserverObject o = {0};
    o.kind = PEAK_MPI_CAUSAL_OBJECT_COMM;
    o.part_count = 1;
    PeakMpiCausalCommPart p = {0};
    p.local_uid = 1;
    p.creation_kind = PEAK_MPI_CAUSAL_COMM_WORLD;
    p.world_size = p.local_size = p.member_count = 2;
    p.members[1] = 1;
    memcpy(o.payload, &p, sizeof p);
    assert(peak_mpi_causal_service_ingest_object(s, r, 100 + r, &o));
  }
  return s;
}
static void run(PeakMpiCausalService *s) {
  for (int k = 0; k < (test_work < 4 ? 1000 : 4); k++) {
    for (unsigned r = 0; r < 2; r++)
      s->ranks[r].verified_nonce = s->ranks[r].requested_nonce;
    for (size_t z = 0; z < s->operation_count; z++) {
      Operation *o = &s->operations[s->active[z]];
      o->visible = 1;
      o->visible_nonce = 0;
      analysis_wake(s, o);
    }
    matcher_work(s, test_work);
    assert(s->last_work_used <= test_work);
    size_t used;
    PeakMpiCostStatus st;
    do {
      st = peak_mpi_cost_model_run(s->model, 100000, &used);
    } while (st == PEAK_MPI_COST_YIELDED);
    assert(st == PEAK_MPI_COST_OK || st == PEAK_MPI_COST_PENDING);
    s->settle_idle = 0; /* Direct helper starts a new ADVANCE phase. */
    advance_covered(s, 100000);
  }
}
static int observed_tag = 7;
static void observed(PeakMpiCausalService *s, unsigned rank, unsigned phase,
                     uint64_t uid, unsigned op, unsigned flags, double L,
                     int peer, uint64_t prior, uint64_t batch,
                     uint64_t buffer) {
  PeakMpiObserverRecord r = {0};
  r.incarnation = 100 + rank;
  r.thread_uid = 1;
  r.thread_generation = 1;
  r.sequence = ++seqs[rank];
  r.phase = phase;
  r.op_uid = uid;
  r.operation = op;
  r.flags = flags;
  r.L_cut = r.P_cut = (uint64_t)(L * 1e9);
  r.communicator_uid = 1;
  r.peer = r.actual_source = peer;
  r.tag = r.actual_tag = observed_tag;
  r.input_ref = prior;
  r.progress_scope = test_profile == PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC &&
                             phase != PEAK_MPI_CAUSAL_CALL_BEGIN
                         ? PEAK_MPI_OBSERVER_SCOPE_AUTONOMOUS
                         : PEAK_MPI_OBSERVER_SCOPE_INTERVAL;
  r.capability_uid = test_profile;
  if (test_profile == PEAK_MPI_PROGRESS_INTEL_2021_11_OPX_ASYNC0_APPROX)
    r.flags |= PEAK_MPI_OBSERVER_PROGRESS_ESTIMATED;
  if (r.flags & PEAK_MPI_OBSERVER_PROGRESS_ESTIMATED)
    r.flags &= ~PEAK_MPI_OBSERVER_PROGRESS_SEALED;
  r.collective_ordinal = 1;
  r.batch_uid = batch;
  r.progress_begin = buffer;
  if (phase == PEAK_MPI_CAUSAL_CALL_END &&
      (op == PEAK_MPI_CAUSAL_WAIT || op == PEAK_MPI_CAUSAL_TEST))
    r.completion_count = find_op(s, rank, uid)->observed_count;
  assert(parse_record(s, rank, &r));
  s->ranks[rank].sequence[0] = r.sequence;
  s->ranks[rank].last_event++;
}

static void call(PeakMpiCausalService *s, unsigned rank, uint64_t uid,
                 unsigned op, unsigned flags, double cost, int peer,
                 uint64_t prior) {
  observed(s, rank, PEAK_MPI_CAUSAL_CALL_BEGIN, uid, op, flags, cost, peer,
           prior, 0, 42);
  observed(s, rank, PEAK_MPI_CAUSAL_CALL_END, uid, op,
           flags | PEAK_MPI_OBSERVER_PROGRESS_SEALED, cost, peer, 0, 0, 42);
}
static void complete(PeakMpiCausalService *s) {
  observed(s, 0, PEAK_MPI_CAUSAL_CALL_BEGIN, 2, PEAK_MPI_CAUSAL_WAIT, 0, 10, -1,
           1, 0, 0);
  observed(s, 0, PEAK_MPI_CAUSAL_REQUEST_COMPLETE, 1,
           test_buffered_request ? PEAK_MPI_CAUSAL_BSEND : PEAK_MPI_CAUSAL_SEND,
           PEAK_MPI_CAUSAL_NONBLOCKING |
               (test_buffered_request == 2 ? PEAK_MPI_CAUSAL_PERSISTENT
                                           : PEAK_MPI_CAUSAL_RECLAIMED) |
               PEAK_MPI_OBSERVER_PROGRESS_SEALED,
           10, -1, 0, 2, 0);
  observed(s, 0, PEAK_MPI_CAUSAL_CALL_END, 2, PEAK_MPI_CAUSAL_WAIT, 0, 10, -1,
           0, 0, 0);
}
static void delivery(int before, int probe, int absent) {
  PeakMpiCausalService *s = make();
  double d;
  if (!absent)
    call(s, 0, 1,
         test_buffered_request ? PEAK_MPI_CAUSAL_BSEND : PEAK_MPI_CAUSAL_SEND,
         PEAK_MPI_CAUSAL_NONBLOCKING |
             (test_buffered_request == 2 ? PEAK_MPI_CAUSAL_PERSISTENT : 0) |
             (test_buffered_request ? PEAK_MPI_CAUSAL_BUFFERED |
                                          PEAK_MPI_OBSERVER_PROGRESS_ESTIMATED
                                    : 0),
         7, 1, 0);
  if (before)
    complete(s);
  call(s, 1, 1, probe ? PEAK_MPI_CAUSAL_PROBE : PEAK_MPI_CAUSAL_RECV, 0, 0, 0,
       0);
  run(s);
  Operation *r = find_op(s, 1, 1), *send = find_op(s, 0, 1);
  if (probe || absent) {
    assert(peak_mpi_cost_model_delay(s->model, r->exit, &d) ==
           PEAK_MPI_COST_PENDING);
    if (send)
      assert(!send->send_matched);
  } else {
    assert(peak_mpi_cost_model_delay(s->model, r->exit, &d) ==
               PEAK_MPI_COST_OK &&
           d == (before ? 10 : 7));
    assert(send->send_matched);
    if (before)
      assert(
          s->ranks[0].progress_estimate ==
          (test_buffered_request ||
           test_profile == PEAK_MPI_PROGRESS_INTEL_2021_11_OPX_ASYNC0_APPROX));
    else {
      assert(s->ranks[0].progress_estimate && !send->completed &&
             !send->result_defined);
      complete(s);
      run(s);
      double L, P;
      assert(peak_mpi_cost_model_accounted(s->model, 0, &L, &P) ==
                 PEAK_MPI_COST_OK &&
             L == 10 && P == 10);
      assert(peak_mpi_cost_model_delay(s->model, find_op(s, 0, 2)->exit, &d) ==
                 PEAK_MPI_COST_OK &&
             d == 10);
      assert(send->completed && send->retired == (test_buffered_request != 2) &&
             send->result_defined);
    }
  }
  peak_mpi_causal_service_destroy(s);
}
static void buffered_delivery(void) {
  PeakMpiCausalService *s = make();
  double d;
  call(s, 0, 1, PEAK_MPI_CAUSAL_BSEND,
       PEAK_MPI_CAUSAL_BUFFERED |
           (test_profile == PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC
                ? 0
                : PEAK_MPI_OBSERVER_PROGRESS_ESTIMATED),
       7, 1, 0);
  call(s, 1, 1, PEAK_MPI_CAUSAL_RECV, 0, 0, 0, 0);
  run(s);
  PeakMpiCostStatus status =
      peak_mpi_cost_model_delay(s->model, find_op(s, 1, 1)->exit, &d);
  if (status != PEAK_MPI_COST_OK)
    fprintf(stderr, "buffer delivery profile=%u status=%d buffer_nodes=%zu\n",
            test_profile, status, s->buffer_count);
  assert(status == PEAK_MPI_COST_OK && d == 7);
  assert(s->buffer_count == 0);
  assert(s->ranks[0].progress_estimate ==
         (test_profile != PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC));
  peak_mpi_causal_service_destroy(s);
}
static PeakMpiPolicyConfig stale_cfg = {2, .1, .1, .5, 0, 0, 1};
static PeakMpiPolicyCohortKey stale_key = {99, 0, 1};
static PeakMpiPolicyMemberId stale_ids[2] = {{0, 99}, {1, 99}};
static void stale_samples(PeakMpiPolicySample s[2], unsigned gen, double A,
                          double L, double elapsed) {
  for (unsigned i = 0; i < 2; i++) {
    s[i] = (PeakMpiPolicySample){0};
    s[i].member = stale_ids[i];
    s[i].generation = gen;
    s[i].event_cut = 1;
    s[i].L = L;
    s[i].P = s[i].A = s[i].D = A;
    s[i].elapsed = elapsed;
    s[i].app_cost_known = 1;
    s[i].cut =
        (PeakMpiCostCut){(uint64_t)(A * 1e9 + .5), (uint64_t)(A * 1e9 + .5),
                         (uint64_t)(L * 1e9 + .5)};
  }
}
static PeakMpiPolicyCut stale_cut(PeakMpiPolicySample *s, unsigned gen,
                                  double age) {
  return (PeakMpiPolicyCut){stale_key, gen, gen, 1,
                            .01,       age, 0,   PEAK_MPI_POLICY_COMPLETE_CUT,
                            1,         s,   2};
}
static PeakMpiPolicyFeedback receiver_pressure_feedback(PeakMpiPolicy *p,
    unsigned generation, double g) {
  PeakMpiPolicySample samples[2];
  double A = .01 * generation;
  stale_samples(samples, generation, A, A, generation);
  for (unsigned i = 0; i < 2; ++i)
    samples[i].event_cut = generation;
  PeakMpiPolicyCut cut = stale_cut(samples, generation, 0);
  cut.F = A;
  assert(peak_mpi_policy_submit_cut(p, &cut, generation) == PEAK_MPI_POLICY_OK);
  PeakMpiPolicyFeedback f;
  assert(peak_mpi_policy_feedback(p, stale_ids[0], generation, &f) == PEAK_MPI_POLICY_OK);
  f.g = g;
  return f;
}
static void test_resource_failure_advisory_pressure(void) {
  PeakMpiPolicyReceiver r;
  PeakMpiPolicyDecision d;
  assert(peak_mpi_policy_receiver_init(&r, &stale_cfg, stale_key, stale_ids[0], 0) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_rebind(&r, stale_key, 64, 1, 0) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_mark_coarse(&r, PEAK_MPI_POLICY_EVENT_LOSS, 0) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot(&r, 0, 0, 0, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(d.g == 64);
  assert(peak_mpi_policy_receiver_mark_coarse(&r, PEAK_MPI_POLICY_RESOURCE_LIMIT, 0) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot(&r, 0, 0, 0, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(d.mode == PEAK_MPI_POLICY_COARSE && d.g == 1 && d.q_eff == stale_cfg.q);
  assert(!d.global_available && !d.headroom_known && !d.reattach_relaxation && isnan(d.global_headroom));

  assert(peak_mpi_policy_receiver_init(&r, &stale_cfg, stale_key, stale_ids[0], 0) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_rebind(&r, stale_key, 2, 1, 0) == PEAK_MPI_POLICY_OK);
  PeakMpiPolicy *p = peak_mpi_policy_create(&stale_cfg, stale_key, stale_ids, 2, 1, 1, 0);
  assert(p);
  PeakMpiPolicyFeedback f = receiver_pressure_feedback(p, 1, 1.5);
  assert(peak_mpi_policy_receiver_accept(&r, &f, 1) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 1, 1, f.cut, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_mark_coarse(&r, PEAK_MPI_POLICY_RESOURCE_LIMIT, 1) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 1, 1, f.cut, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(d.g == 1.5 && !d.reattach_relaxation);
  PeakMpiCostCut backward = f.cut;
  backward.A_ns--; backward.P_ns--;
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 1, 1, backward, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(d.g == 2 && !d.headroom_known && !d.reattach_relaxation);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 1, .5, f.cut, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(d.g == 2 && !d.headroom_known && !d.reattach_relaxation);

  f.generation++; f.mode = PEAK_MPI_POLICY_COARSE;
  f.reason = PEAK_MPI_POLICY_RESOURCE_LIMIT; f.g = 2; f.risk = .02;
  assert(peak_mpi_policy_receiver_accept(&r, &f, 1) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 1, 1, f.cut, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(d.global_available && d.g == 2 && d.headroom_known);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 2, 2, f.cut, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(!d.global_available && d.g == 1.5 && !d.reattach_relaxation);
  peak_mpi_policy_free(p);
  puts("observer resource failure retains observed pressure without unknown headroom PASS");
}
static void test_receiver_recovered_pressure(void) {
  PeakMpiPolicyReceiver r;
  PeakMpiPolicyDecision d;
  assert(peak_mpi_policy_receiver_init(&r, &stale_cfg, stale_key, stale_ids[0], 0) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_rebind(&r, stale_key, 2, 1, 0) == PEAK_MPI_POLICY_OK);
  PeakMpiPolicy *p = peak_mpi_policy_create(&stale_cfg, stale_key, stale_ids, 2, 1, 1, 0);
  assert(p);
  PeakMpiPolicyFeedback f = receiver_pressure_feedback(p, 1, 1.25);
  assert(peak_mpi_policy_receiver_accept(&r, &f, 1) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 1, 1, f.cut, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(d.g == 1.25 && d.reattach_relaxation);

  assert(peak_mpi_policy_receiver_mark_coarse(&r, PEAK_MPI_POLICY_EVENT_LOSS, 1.1) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 1.1, 1.1, f.cut, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(d.mode == PEAK_MPI_POLICY_COARSE && d.g == 2);
  assert(!d.global_available && !d.headroom_known && !d.reattach_relaxation);

  assert(peak_mpi_policy_receiver_recover(&r, 1.2));
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 1.2, 1.2, f.cut, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(d.g == 1.25 && d.q_eff == stale_cfg.q / 1.25);
  assert(!d.global_available && !d.headroom_known && !d.reattach_relaxation);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 2.8, 2.8, f.cut, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(d.mode == PEAK_MPI_POLICY_STALE && d.g == 1.25);
  assert(!d.global_available && !d.headroom_known && !d.reattach_relaxation);

  f = receiver_pressure_feedback(p, 3, 1.5);
  assert(peak_mpi_policy_receiver_accept(&r, &f, 3) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 3, 3, f.cut, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(d.g == 1.5 && d.reattach_relaxation);

  assert(peak_mpi_policy_receiver_mark_coarse(&r, PEAK_MPI_POLICY_EVENT_LOSS, 3.1) == PEAK_MPI_POLICY_OK);
  f.generation++;
  f.q = 0.2;
  assert(peak_mpi_policy_receiver_accept(&r, &f, 3.2) == PEAK_MPI_POLICY_INVALID);
  assert(peak_mpi_policy_receiver_recover(&r, 3.3));
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 3.3, 3.3, f.cut, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(d.g == 2 && !d.headroom_known && !d.reattach_relaxation);

  assert(peak_mpi_policy_receiver_mark_coarse(&r, PEAK_MPI_POLICY_BAD_CUT, 3.4) == PEAK_MPI_POLICY_OK);
  assert(!peak_mpi_policy_receiver_recover(&r, 3.5));
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 3.5, 3.5, f.cut, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(d.mode == PEAK_MPI_POLICY_COARSE && d.reason == PEAK_MPI_POLICY_BAD_CUT && d.g == 2);
  assert(!d.global_available && !d.headroom_known && !d.reattach_relaxation);
  peak_mpi_policy_free(p);
}
static void test_receiver_pressure_decline(void) {
  PeakMpiPolicyReceiver r;
  PeakMpiPolicyDecision d;
  assert(peak_mpi_policy_receiver_init(&r, &stale_cfg, stale_key, stale_ids[0], 0) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_rebind(&r, stale_key, 2, 1, 0) == PEAK_MPI_POLICY_OK);
  PeakMpiPolicy *p = peak_mpi_policy_create(&stale_cfg, stale_key, stale_ids, 2, 1, 1, 0);
  assert(p);
  PeakMpiPolicyFeedback f = receiver_pressure_feedback(p, 1, 2);
  assert(peak_mpi_policy_receiver_accept(&r, &f, 1) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 1, 1, f.cut, 1, &d) == PEAK_MPI_POLICY_OK && d.g == 2);
  PeakMpiPolicyFeedback high = f;
  f = receiver_pressure_feedback(p, 2, 1.5);
  PeakMpiPolicyFeedback low = f;
  assert(peak_mpi_policy_receiver_accept(&r, &f, 2) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 2, 2, f.cut, 1, &d) == PEAK_MPI_POLICY_OK && d.mode == PEAK_MPI_POLICY_COMPLETE && d.g == 1.5);
  assert(r.conservative_g == 1.5);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 2.4, 2.4, f.cut, 1, &d) == PEAK_MPI_POLICY_OK && d.mode == PEAK_MPI_POLICY_STALE && d.g == 1.5);
  assert(!d.global_available && !d.headroom_known && !d.reattach_relaxation && d.q_eff == stale_cfg.q / 1.5);

  /* An already aged lower COMPLETE tuple cannot lower learned pressure. */
  f = receiver_pressure_feedback(p, 3, 1.25);
  f.age_seconds = 1;
  assert(peak_mpi_policy_receiver_accept(&r, &f, 3) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 3, 3, f.cut, 1, &d) == PEAK_MPI_POLICY_OK && d.mode == PEAK_MPI_POLICY_STALE && d.g == 1.5 && r.conservative_g == 1.5);
  assert(!d.headroom_known && !d.reattach_relaxation);
  f = receiver_pressure_feedback(p, 4, 1.5);
  assert(peak_mpi_policy_receiver_accept(&r, &f, 4) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 4, 4, f.cut, 1, &d) == PEAK_MPI_POLICY_OK && d.g == 1.5);
  PeakMpiCostCut backward = f.cut;
  backward.A_ns--;
  backward.P_ns--;
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 4, 4, backward, 1, &d) == PEAK_MPI_POLICY_OK && d.mode == PEAK_MPI_POLICY_STALE && d.g == 2 && !d.reattach_relaxation);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 4, 3.5, f.cut, 1, &d) == PEAK_MPI_POLICY_OK && d.mode == PEAK_MPI_POLICY_STALE && d.g == 2 && !d.reattach_relaxation);
  PeakMpiPolicyFeedback invalid = f;
  invalid.generation++;
  invalid.g = NAN;
  assert(peak_mpi_policy_receiver_accept(&r, &invalid, 4.5) == PEAK_MPI_POLICY_INVALID);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 4.5, 4.5, f.cut, 1, &d) == PEAK_MPI_POLICY_OK && d.mode == PEAK_MPI_POLICY_STALE && d.g == 2 && !d.reattach_relaxation);

  assert(peak_mpi_policy_receiver_mark_coarse(&r, PEAK_MPI_POLICY_EVENT_LOSS, 4.6) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_recover(&r, 4.7));
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 4.7, 4.7, f.cut, 1, &d) == PEAK_MPI_POLICY_OK && d.g == 2 && !d.headroom_known && !d.reattach_relaxation);
  f = receiver_pressure_feedback(p, 5, 1.5);
  f.mode = PEAK_MPI_POLICY_APPROX;
  f.approximate = 1;
  f.reason = PEAK_MPI_POLICY_PROGRESS_UNPROVEN;
  f.coverage_complete = f.risk_known = f.tails_complete = 0;
  assert(peak_mpi_policy_receiver_accept(&r, &f, 5) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 5, 5, f.cut, 1, &d) == PEAK_MPI_POLICY_OK && d.mode == PEAK_MPI_POLICY_APPROX && d.g == 1.5 && r.conservative_g == 1.5);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 5.4, 5.4, f.cut, 1, &d) == PEAK_MPI_POLICY_OK && d.mode == PEAK_MPI_POLICY_STALE && d.g == 1.5 && !d.headroom_known && !d.reattach_relaxation);

  assert(peak_mpi_policy_receiver_mark_coarse(&r, PEAK_MPI_POLICY_BAD_IDENTITY, 5.5) == PEAK_MPI_POLICY_OK);
  assert(!peak_mpi_policy_receiver_recover(&r, 5.6));
  f = receiver_pressure_feedback(p, 6, 1.25);
  assert(peak_mpi_policy_receiver_accept(&r, &f, 6) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 6, 6, f.cut, 1, &d) == PEAK_MPI_POLICY_OK && d.mode == PEAK_MPI_POLICY_COARSE && d.g == 2 && !d.headroom_known && !d.reattach_relaxation);
  /* A new membership hint makes even a young lower tuple ineligible. */
  PeakMpiPolicyReceiver unknown;
  assert(peak_mpi_policy_receiver_init(&unknown, &stale_cfg, stale_key, stale_ids[0], 0) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_rebind(&unknown, stale_key, 2, 1, 0) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_accept(&unknown, &high, 1) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&unknown, 1, 1, high.cut, 1, &d) == PEAK_MPI_POLICY_OK && d.g == 2);
  assert(peak_mpi_policy_receiver_accept(&unknown, &low, 2) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_note_membership_hint(&unknown, 3, 2) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&unknown, 2, 2, low.cut, 1, &d) == PEAK_MPI_POLICY_OK && unknown.conservative_g == 2 && !d.headroom_known && !d.reattach_relaxation);
  assert(peak_mpi_policy_receiver_snapshot_cut(&unknown, 2.4, 2.4, low.cut, 1, &d) == PEAK_MPI_POLICY_OK && d.mode == PEAK_MPI_POLICY_STALE && d.g == 2 && !d.global_available && !d.headroom_known && !d.reattach_relaxation);
  peak_mpi_policy_free(p);
  puts("receiver fresh pressure decline survives age; aged/invalid/backward/coarse safety PASS");
}

static void test_delayed_receiver_pressure(void) {
  PeakMpiPolicyReceiver r;
  PeakMpiPolicyDecision d;
  assert(peak_mpi_policy_receiver_init(&r, &stale_cfg, stale_key, stale_ids[0], 0) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_rebind(&r, stale_key, 2, 1, 0) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot(&r, 1, 1, 0, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(d.mode == PEAK_MPI_POLICY_STALE && d.g == 1 &&
         !d.global_available && !d.headroom_known && !d.reattach_relaxation);
  PeakMpiPolicySample s[2];
  stale_samples(s, 1, .01, .01, 1);
  PeakMpiPolicy *p = peak_mpi_policy_create(&stale_cfg, stale_key, stale_ids, 2, 1, 1, 0);
  assert(p);
  PeakMpiPolicyCut c = stale_cut(s, 1, 0);
  assert(peak_mpi_policy_submit_cut(p, &c, 1) == PEAK_MPI_POLICY_OK);
  PeakMpiPolicyFeedback f;
  assert(peak_mpi_policy_feedback(p, stale_ids[0], 1, &f) == PEAK_MPI_POLICY_OK);
  f.g = 1.5;
  assert(peak_mpi_policy_receiver_accept(&r, &f, 1) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 2, 2, s[0].cut, 1, &d) == PEAK_MPI_POLICY_OK);
  assert(d.mode == PEAK_MPI_POLICY_STALE && d.g == 1.5 &&
         !d.global_available && !d.headroom_known && !d.reattach_relaxation);
  assert(d.q_eff == stale_cfg.q / 1.5 && d.detach_target == d.q_eff &&
         d.candidate_stop_target == d.q_eff && d.cadence_target == d.q_eff &&
         d.reattach_future_rate_target == d.q_eff);
  PeakMpiPolicyFeedback delayed = f;
  delayed.generation++;
  delayed.mode = PEAK_MPI_POLICY_STALE;
  delayed.reason = PEAK_MPI_POLICY_FEEDBACK_OLD;
  delayed.g = 2;
  assert(peak_mpi_policy_receiver_accept(&r, &delayed, 2) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 2, 2, s[0].cut, 1, &d) == PEAK_MPI_POLICY_OK && d.g == 1.5 && !d.reattach_relaxation);
  /* Backward elapsed and local cost are invalid, rather than harmless age. */
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 2, .5, s[0].cut, 1, &d) == PEAK_MPI_POLICY_OK && d.g == 2);
  PeakMpiCostCut backward = {0};
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 2, 2, backward, 1, &d) == PEAK_MPI_POLICY_OK && d.g == 2);
  f.generation += 2;
  f.age_seconds = 0;
  f.local_elapsed_at_cut = 2;
  f.T_safe = 2;
  assert(peak_mpi_policy_receiver_accept(&r, &f, 2) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 2, 2, s[0].cut, 1, &d) == PEAK_MPI_POLICY_OK && d.mode == PEAK_MPI_POLICY_COMPLETE && d.g == 1.5 && d.global_available);
  PeakMpiPolicyFeedback invalid = f;
  invalid.generation++;
  invalid.g = NAN;
  assert(peak_mpi_policy_receiver_accept(&r, &invalid, 2) == PEAK_MPI_POLICY_INVALID);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 2, 2, s[0].cut, 1, &d) == PEAK_MPI_POLICY_OK && d.mode == PEAK_MPI_POLICY_STALE && d.g == 2 && !d.reattach_relaxation);
  assert(peak_mpi_policy_receiver_mark_coarse(&r, PEAK_MPI_POLICY_EVENT_LOSS, 2) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 2, 2, s[0].cut, 1, &d) == PEAK_MPI_POLICY_OK && d.mode == PEAK_MPI_POLICY_COARSE && d.g == 2 && !d.reattach_relaxation);
  peak_mpi_policy_free(p);
}

static void test_stale_feedback(void) {
  PeakMpiPolicySample s[2], raw[2];
  stale_samples(s, 1, .01, .01, 1);
  PeakMpiPolicy *p =
      peak_mpi_policy_create(&stale_cfg, stale_key, stale_ids, 2, 1, 1, 0);
  assert(p);
  for (unsigned i = 0; i < 2; i++)
    assert(peak_mpi_policy_record_raw(p, stale_key, &s[i], 1) ==
           PEAK_MPI_POLICY_OK);
  PeakMpiPolicyCut c = stale_cut(s, 1, 0);
  assert(peak_mpi_policy_submit_cut(p, &c, 1) == PEAK_MPI_POLICY_OK);
  PeakMpiPolicyFeedback f, old;
  assert(peak_mpi_policy_feedback(p, stale_ids[0], 1, &f) ==
         PEAK_MPI_POLICY_OK);
  PeakMpiPolicyReceiver r;
  PeakMpiPolicyDecision d;
  assert(peak_mpi_policy_receiver_init(&r, &stale_cfg, stale_key, stale_ids[0],
                                       1) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_rebind(&r, stale_key, 2, 1, 1) ==
         PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_accept(&r, &f, 1) == PEAK_MPI_POLICY_OK);
  stale_samples(raw, 2, .01, .011, 2);
  for (unsigned i = 0; i < 2; i++)
    assert(peak_mpi_policy_record_raw(p, stale_key, &raw[i], 2) ==
           PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_feedback(p, stale_ids[0], 2, &old) ==
         PEAK_MPI_POLICY_OK);
  assert(old.mode == PEAK_MPI_POLICY_STALE && old.received_raw_sum == .022 &&
         old.received_app_sum == .02);
  c = stale_cut(s, 2, 1);
  PeakMpiPolicyResult status = peak_mpi_policy_submit_cut(p, &c, 2);
  fprintf(stderr, "repeatSTALE status=%d expectedIGNORED=%d\n", status,
          PEAK_MPI_POLICY_IGNORED);
  assert(status == PEAK_MPI_POLICY_IGNORED);
  assert(peak_mpi_policy_feedback(p, stale_ids[0], 2, &f) ==
             PEAK_MPI_POLICY_OK &&
         f.generation == old.generation && f.event_cut == old.event_cut);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 2, 2, raw[0].cut, 1, &d) ==
             PEAK_MPI_POLICY_OK &&
         d.mode == PEAK_MPI_POLICY_STALE && !d.global_available);
  stale_samples(s, 3, .011, .012, 2.1);
  for (unsigned i = 0; i < 2; i++)
    assert(peak_mpi_policy_record_raw(p, stale_key, &s[i], 2.1) ==
           PEAK_MPI_POLICY_OK);
  c = stale_cut(s, 3, 0);
  c.F = .011;
  assert(peak_mpi_policy_submit_cut(p, &c, 2.1) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_feedback(p, stale_ids[0], 2.1, &f) ==
             PEAK_MPI_POLICY_OK &&
         f.mode == PEAK_MPI_POLICY_COMPLETE && f.event_cut == 1 &&
         f.A_cut == .011);
  assert(peak_mpi_policy_receiver_accept(&r, &f, 2.1) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&r, 2.1, 2.1, s[0].cut, 1, &d) ==
             PEAK_MPI_POLICY_OK &&
         d.global_available);
  peak_mpi_policy_free(p);
  p = peak_mpi_policy_create(&stale_cfg, stale_key, stale_ids, 2, 1, 1, 0);
  assert(p);
  c = stale_cut(s, 1, 1);
  assert(peak_mpi_policy_submit_cut(p, &c, 2.1) == PEAK_MPI_POLICY_OK);
  c.generation = c.cut_id = 2;
  assert(peak_mpi_policy_submit_cut(p, &c, 2.1) == PEAK_MPI_POLICY_IGNORED);
  c.generation = c.cut_id = 3;
  c.policy_generation = 2;
  assert(peak_mpi_policy_submit_cut(p, &c, 2.1) == PEAK_MPI_POLICY_OK);
  c.generation = c.cut_id = 4;
  c.quality = PEAK_MPI_POLICY_OBSERVED_PROGRESS_CUT;
  assert(peak_mpi_policy_submit_cut(p, &c, 2.1) == PEAK_MPI_POLICY_OK);
  c.generation = c.cut_id = 5;
  assert(peak_mpi_policy_submit_cut(p, &c, 2.1) == PEAK_MPI_POLICY_IGNORED);
  peak_mpi_policy_recover(p, 2.1, 1);
  c.generation = c.cut_id = 6;
  assert(peak_mpi_policy_submit_cut(p, &c, 2.1) == PEAK_MPI_POLICY_OK);
  peak_mpi_policy_free(p);
  PeakMpiPolicyCohortKey rightkey = {99, 1, 1}, merged = {99, 0, 2};
  p = peak_mpi_policy_create(&stale_cfg, stale_key, &stale_ids[0], 1, 1, 1, 0);
  PeakMpiPolicy *right =
      peak_mpi_policy_create(&stale_cfg, rightkey, &stale_ids[1], 1, 1, 1, 0);
  assert(p && right);
  c = stale_cut(s, 1, 1);
  c.sample_count = 1;
  assert(peak_mpi_policy_submit_cut(p, &c, 2.1) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_merge(p, right, merged, 2.1) == PEAK_MPI_POLICY_OK);
  c = stale_cut(s, 2, 1);
  c.cohort = merged;
  assert(peak_mpi_policy_submit_cut(p, &c, 2.1) == PEAK_MPI_POLICY_OK);
  peak_mpi_policy_free(right);
  peak_mpi_policy_free(p);
  puts("repeatedSTALE nofeedbackrevision; rawfees retained; cachedSTALE "
       "staysblocked; freshsameevent/recovery policyqualitytransitions publish "
       "PASS");
  return;
}

static void test_feedback_cache_guard(void) {
  PeakMpiCausalService *s = make();
  assert(merge(s, 0, 1));
  PeakMpiPolicy *p = s->ranks[root(s, 0)].policy;
  PeakMpiPolicyCohortKey key = s->ranks[root(s, 0)].key;
  PeakMpiPolicySample samples[2];
  stale_samples(samples, 1, .01, .01, 1);
  for (unsigned i = 0; i < 2; ++i)
    assert(peak_mpi_policy_record_raw(p, key, &samples[i], 1) ==
           PEAK_MPI_POLICY_OK);
  PeakMpiPolicyCut cut = stale_cut(samples, 1, 0);
  cut.cohort = key;
  assert(peak_mpi_policy_submit_cut(p, &cut, 1) == PEAK_MPI_POLICY_OK);
  s->now = 1;
  assert(prepare_feedback_cache(s, 100000) > 0 && !s->cache_dirty);
  PeakMpiPolicyFeedback cached = s->feedback_cache[0], live;
  PeakMpiPolicyReceiver receiver;
  PeakMpiPolicyDecision decision;
  assert(peak_mpi_policy_receiver_init(&receiver, &stale_cfg, key, stale_ids[0],
                                       1) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_rebind(&receiver, key, 2, 1, 1) ==
         PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_accept(&receiver, &cached, 1) ==
         PEAK_MPI_POLICY_OK);
  stale_samples(samples, 2, .01, .011, 2);
  for (unsigned i = 0; i < 2; ++i)
    assert(peak_mpi_policy_record_raw(p, key, &samples[i], 2) ==
           PEAK_MPI_POLICY_OK);
  s->now = 2;
  assert(prepare_feedback_cache(s, 100000) == 0);
  assert(s->feedback_cache[0].generation == cached.generation &&
         s->feedback_cache[0].event_cut == cached.event_cut);
  assert(peak_mpi_policy_feedback(p, stale_ids[0], 2, &live) ==
             PEAK_MPI_POLICY_OK &&
         live.received_raw_sum == .022 && live.received_app_sum == .02);
  assert(peak_mpi_policy_receiver_snapshot_cut(&receiver, 6, 6, samples[0].cut,
                                               1, &decision) ==
             PEAK_MPI_POLICY_OK &&
         decision.mode == PEAK_MPI_POLICY_STALE && !decision.global_available);
  stale_samples(samples, 3, .011, .012, 6);
  for (unsigned i = 0; i < 2; ++i)
    assert(peak_mpi_policy_record_raw(p, key, &samples[i], 6) ==
           PEAK_MPI_POLICY_OK);
  cut = stale_cut(samples, 2, 0);
  cut.cohort = key;
  cut.F = .011;
  assert(peak_mpi_policy_submit_cut(p, &cut, 6) == PEAK_MPI_POLICY_OK);
  s->now = 6;
  s->cache_dirty = 1;
  assert(prepare_feedback_cache(s, 1) == 0 && s->cache_dirty);
  assert(prepare_feedback_cache(s, 100000) > 0 && !s->cache_dirty);
  live = s->feedback_cache[0];
  assert(live.generation > cached.generation && live.A_cut == .011);
  assert(peak_mpi_policy_receiver_accept(&receiver, &live, 6) ==
         PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&receiver, 6, 6, samples[0].cut,
                                               1, &decision) ==
             PEAK_MPI_POLICY_OK &&
         decision.global_available);
  peak_mpi_causal_service_destroy(s);
  puts("clean feedback cache retains tuple/raw fees; aged receiver blocks; "
       "dirty fresh cut recovers PASS");
}
static PeakMpiObserverRecord compact_rec(unsigned phase, unsigned seq,
                                         unsigned uid, unsigned cost) {
  PeakMpiObserverRecord r = {0};
  r.incarnation = 1;
  r.thread_uid = 1;
  r.thread_generation = 1;
  r.sequence = seq;
  r.phase = phase;
  r.op_uid = uid;
  r.batch_uid = 100;
  r.kind = PEAK_MPI_OBSERVER_EVENT_START;
  r.flags = PEAK_MPI_CAUSAL_APP_COST_KNOWN | PEAK_MPI_CAUSAL_ORDERED_CALLER;
  r.L_cut = r.A_cut = cost;
  r.P_cut = cost / 2;
  r.operation = PEAK_MPI_CAUSAL_BCAST;
  return r;
}
static PeakMpiCausalBatchView compact_ready(PeakMpiCausalBatches *b) {
  PeakMpiCausalBatchView v;
  assert(peak_mpi_causal_batches_prepare_next(b, 100, &v) == 1);
  return v;
}
static void test_compact_batches(void) {
  PeakMpiObserverRecord full[4] = {
      compact_rec(PEAK_MPI_CAUSAL_GROUP_BEGIN, 1, 100, 2),
      compact_rec(PEAK_MPI_CAUSAL_CALL_BEGIN, 2, 10, 4),
      compact_rec(PEAK_MPI_CAUSAL_CALL_END, 3, 10, 6),
      compact_rec(PEAK_MPI_CAUSAL_GROUP_END, 4, 100, 8)};
  full[0].progress_end = 1000000000;
  full[3].progress_end = 2000000000;
  full[0].operation = full[3].operation = 0;
  full[3].completion_count = 1;
  full[0].control_generation = 1;
  full[1].control_generation = 2;
  full[2].control_generation = 3;
  full[3].control_generation = 4;
  PeakMpiObserverRecord compact[2] = {full[1], full[2]};
  for (unsigned i = 0; i < 2; i++) {
    compact[i].flags |= PEAK_MPI_CAUSAL_COMPACT_GROUP;
    compact[i].sequence = i + 1;
    compact[i].group_L_cut = compact[i].group_A_cut = i ? 8 : 2;
    compact[i].group_P_cut = i ? 4 : 1;
    compact[i].policy_generation = i ? 4 : 1;
    compact[i].partition = i ? 2000000000 : 1000000000;
    compact[i].progress_end = 0;
  }
  PeakMpiCausalBatches *a = peak_mpi_causal_batches_create_with_slots(1, 2, 32),
                       *b = peak_mpi_causal_batches_create_with_slots(1, 2, 32);
  assert(a && b);
  for (unsigned i = 0; i < 4; i++)
    assert(peak_mpi_causal_batches_ingest(a, 0, &full[i]));
  for (unsigned i = 0; i < 2; i++)
    assert(peak_mpi_causal_batches_ingest(b, 0, &compact[i]));
  PeakMpiCausalBatchView va = compact_ready(a), vb = compact_ready(b);
  assert(va.record_count == 4 && vb.record_count == 4);
  for (unsigned i = 0; i < 4; i++) {
    const PeakMpiObserverRecord *x = peak_mpi_causal_batches_record_at(a, i),
                                *y = peak_mpi_causal_batches_record_at(b, i);
    assert(x->phase == y->phase && x->op_uid == y->op_uid &&
           x->batch_uid == y->batch_uid && x->input_ref == y->input_ref &&
           x->L_cut == y->L_cut && x->A_cut == y->A_cut &&
           x->P_cut == y->P_cut &&
           x->control_generation == y->control_generation);
    if (i == 0 || i == 3)
      assert(x->progress_end == y->progress_end);
  }
  assert(peak_mpi_causal_batches_pending(a) == 4 &&
         peak_mpi_causal_batches_pending(b) == 2);
  PeakMpiObserverRecord saved_begin = *vb.begin, saved_end = *vb.end;
  PeakMpiObserverRecord later[2] = {compact[0], compact[1]};
  for (unsigned i = 0; i < 2; i++) {
    later[i].op_uid = 20;
    later[i].batch_uid = 200;
    later[i].result_ref = 100;
    later[i].input_ref = 10;
    later[i].sequence += 2;
    later[i].L_cut += 10;
    later[i].A_cut += 10;
    later[i].P_cut += 5;
    later[i].group_L_cut += 10;
    later[i].group_A_cut += 10;
    later[i].group_P_cut += 5;
    assert(peak_mpi_causal_batches_ingest(b, 0, &later[i]));
  }
  PeakMpiCausalBatchView same;
  assert(peak_mpi_causal_batches_prepare_next(b, 1, &same) == 1);
  assert(vb.begin == same.begin && vb.end == same.end &&
         !memcmp(vb.begin, &saved_begin, sizeof saved_begin) &&
         !memcmp(vb.end, &saved_end, sizeof saved_end));

  peak_mpi_causal_batches_commit(a);
  peak_mpi_causal_batches_commit(b);
  PeakMpiCausalBatchView v;
  assert(!peak_mpi_causal_batches_prepare_next(a, 100, &v));
  assert(peak_mpi_causal_batches_prepare_next(b, 100, &v) == 1 &&
         v.group_uid == 200);
  peak_mpi_causal_batches_commit(b);
  assert(!peak_mpi_causal_batches_prepare_next(b, 100, &v));
  assert(!peak_mpi_causal_batches_pending(a) &&
         !peak_mpi_causal_batches_pending(b));
  peak_mpi_causal_batches_destroy(a);
  peak_mpi_causal_batches_destroy(b);
  for (unsigned bad = 0; bad < 3; bad++) {
    b = peak_mpi_causal_batches_create_with_slots(1, 2, 32);
    PeakMpiObserverRecord x = compact[0], y = compact[1];
    if (!bad)
      x.flags &= ~PEAK_MPI_CAUSAL_ORDERED_CALLER;
    else if (bad == 1)
      y.thread_uid = 2;
    else
      y.op_uid++;
    assert(peak_mpi_causal_batches_ingest(b, 0, &x));
    assert(peak_mpi_causal_batches_ingest(b, 0, &y));
    assert(peak_mpi_causal_batches_prepare_next(b, 100, &v) == -1);
    peak_mpi_causal_batches_destroy(b);
  }
  b = peak_mpi_causal_batches_create_with_slots(1, 2, 4);
  PeakMpiObserverRecord body[4] = {
      compact[0], compact_rec(PEAK_MPI_CAUSAL_REQUEST_COMPLETE, 2, 30, 5),
      compact_rec(PEAK_MPI_CAUSAL_REQUEST_COMPLETE, 3, 31, 5), compact[1]};
  body[3].sequence = 4;
  body[3].completion_count = 2;
  for (unsigned i = 1; i <= 2; i++) {
    body[i].flags |= PEAK_MPI_CAUSAL_COMPACT_GROUP;
    body[i].batch_uid = 10;
    body[i].progress_begin = 100 + i;
    body[i].progress_end = 200 + i;
  }
  for (unsigned i = 0; i < 4; i++)
    assert(peak_mpi_causal_batches_ingest(b, 0, &body[i]));
  int ready = 0;
  for (unsigned turn = 0; !ready; ++turn) {
    size_t used;
    assert(turn < 100);
    ready = peak_mpi_causal_batches_prepare_next_work(b, 1, &v, &used);
    assert(ready >= 0 && used <= 1);
  }
  assert(v.record_count == 6 && v.native_count == 1 &&
         v.end->completion_count == 1 &&
         peak_mpi_causal_batches_pending(b) == 4);
  assert(peak_mpi_causal_batches_record_at(b, 2)->progress_begin == 101 &&
         peak_mpi_causal_batches_record_at(b, 2)->progress_end == 201 &&
         peak_mpi_causal_batches_record_at(b, 3)->progress_begin == 102 &&
         peak_mpi_causal_batches_record_at(b, 3)->progress_end == 202);
  assert(peak_mpi_causal_batches_record_at(b, 4)->phase ==
             PEAK_MPI_CAUSAL_CALL_END &&
         peak_mpi_causal_batches_record_at(b, 4)->completion_count == 2);
  uint64_t prefix[2] = {4, 0};
  size_t cursor = 0, work = 2;
  assert(!peak_mpi_causal_batches_prefix_clear(b, 0, prefix, 2, &cursor, &work));
  peak_mpi_causal_batches_commit(b);
  for (unsigned turn = 0; peak_mpi_causal_batches_last_group(b, 0) != 100; ++turn) {
    size_t used;
    assert(turn < 100 &&
           !peak_mpi_causal_batches_prepare_next_work(b, 1, &v, &used) && used <= 1);
  }
  cursor = 0; work = 2;
  assert(!peak_mpi_causal_batches_pending(b) &&
         peak_mpi_causal_batches_prefix_clear(b, 0, prefix, 2, &cursor, &work) == 1);
  /* Reuse every physical slot: virtual group markers must not free twice. */
  for (unsigned i = 0; i < 4; ++i) {
    body[i].sequence += 4;
    if (i == 0 || i == 3) {
      body[i].op_uid = 20;
      body[i].batch_uid = 200;
      body[i].result_ref = 100;
    } else
      body[i].batch_uid = 20;
    assert(peak_mpi_causal_batches_ingest(b, 0, &body[i]));
  }
  v = compact_ready(b);
  assert(v.group_uid == 200 && v.record_count == 6);
  peak_mpi_causal_batches_commit(b);
  assert(!peak_mpi_causal_batches_prepare_next(b, 100, &v) &&
         !peak_mpi_causal_batches_pending(b));
  peak_mpi_causal_batches_destroy(b);
  puts("compact exact four cuts/control/identity; physical prefix and retire; "
       "malformed PASS");
}

static PeakMpiCausalService *compact_shape(int compact, int serial) {
  PeakMpiCausalService *s = make();
  for (unsigned rank = 0; rank < 2; rank++) {
    PeakMpiObserverRecord a[4] = {0};
    unsigned phases[4] = {PEAK_MPI_CAUSAL_GROUP_BEGIN,
                          PEAK_MPI_CAUSAL_CALL_BEGIN, PEAK_MPI_CAUSAL_CALL_END,
                          PEAK_MPI_CAUSAL_GROUP_END};
    for (unsigned i = 0; i < 4; i++) {
      a[i].incarnation = 100 + rank;
      a[i].thread_uid = 1;
      a[i].thread_generation = 1;
      a[i].sequence = i + 1;
      a[i].phase = phases[i];
      a[i].op_uid = (i == 0 || i == 3) ? 100 : 10;
      a[i].batch_uid = 100;
      a[i].kind = PEAK_MPI_OBSERVER_EVENT_START;
      a[i].flags =
          PEAK_MPI_CAUSAL_ORDERED_CALLER | PEAK_MPI_CAUSAL_APP_COST_KNOWN;
      a[i].L_cut = a[i].A_cut = (2 + 2 * i + rank) * 1000000000ULL;
      a[i].P_cut = a[i].A_cut / 2;
      a[i].control_generation = i;
      a[i].communicator_uid = 1;
      a[i].capability_uid = PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC;
      a[i].progress_scope = PEAK_MPI_OBSERVER_SCOPE_AUTONOMOUS;
      a[i].operation =
          (i == 0 || i == 3) ? 0
          : serial ? (rank ? PEAK_MPI_CAUSAL_RECV : PEAK_MPI_CAUSAL_SEND)
                   : PEAK_MPI_CAUSAL_ALLREDUCE;
      a[i].peer = a[i].actual_source = 1 - rank;
      a[i].tag = a[i].actual_tag = 7;
      a[i].collective_ordinal = 1;
    }
    a[0].progress_end = 100000000000ULL + rank * 1000000000ULL;
    a[3].progress_end = 200000000000ULL + rank * 1000000000ULL;
    a[3].completion_count = 1;
    a[2].flags |= PEAK_MPI_OBSERVER_PROGRESS_SEALED;
    if (!serial) {
      a[1].flags |= PEAK_MPI_CAUSAL_COLLECTIVE;
      a[2].flags |= PEAK_MPI_CAUSAL_COLLECTIVE;
    }
    if (compact) {
      for (unsigned i = 1; i <= 2; i++) {
        a[i].flags |= PEAK_MPI_CAUSAL_COMPACT_GROUP;
        a[i].sequence = i;
        a[i].group_L_cut = a[i].group_A_cut = a[i == 1 ? 0 : 3].A_cut;
        a[i].group_P_cut = a[i == 1 ? 0 : 3].P_cut;
        a[i].policy_generation = i == 1 ? 0 : 3;
        a[i].partition = (uint32_t)a[i == 1 ? 0 : 3].progress_end;
        a[i].completion_index =
            (uint32_t)(a[i == 1 ? 0 : 3].progress_end >> 32);
        assert(peak_mpi_causal_batches_ingest(s->batches, rank, &a[i]));
      }
    } else
      for (unsigned i = 0; i < 4; i++)
        assert(peak_mpi_causal_batches_ingest(s->batches, rank, &a[i]));
  }
  for (unsigned rank = 0; rank < 2; rank++) {
    unsigned char peers[2] = {1, 1};
    PeakMpiCausalRaw raw = {0};
    raw.generation = 1;
    raw.incarnation = 100 + rank;
    raw.control_generation = 3;
    raw.L = raw.A = 8 + rank;
    raw.P = raw.A / 2;
    raw.app_cost_known = 1;
    raw.cut = (PeakMpiCostCut){(uint64_t)(raw.P * 1e9), (uint64_t)(raw.A * 1e9),
                               (uint64_t)(raw.L * 1e9)};
    raw.elapsed = 200 + rank;
    raw.active = 1;
    raw.known_peers = peers;
    raw.peer_count = 2;
    assert(peak_mpi_causal_service_ingest_raw(s, rank, &raw, 1));
  }
  parse_batches(s, 10000);
  run(s);
  assert(publish_work(s, 100000));
  return s;
}
static void test_compact_backend(void) {
  for (int serial = 0; serial < 2; serial++) {
    PeakMpiCausalService *a = compact_shape(0, serial),
                         *b = compact_shape(1, serial);
    for (unsigned rank = 0; rank < 2; rank++) {
      Operation *x = find_op(a, rank, 10), *y = find_op(b, rank, 10);
      assert(x && y);
      double dx, dy;
      PeakMpiCostStatus sx = peak_mpi_cost_model_delay(a->model, x->exit, &dx),
                        sy = peak_mpi_cost_model_delay(b->model, y->exit, &dy);
      assert(sx == PEAK_MPI_COST_OK && sx == sy && dx == dy);
      assert(a->ranks[rank].last_A_cut == b->ranks[rank].last_A_cut &&
             a->ranks[rank].last_P_cut == b->ranks[rank].last_P_cut &&
             a->ranks[rank].last_raw_L_cut == b->ranks[rank].last_raw_L_cut);
      assert(a->ranks[rank].covered.elapsed == b->ranks[rank].covered.elapsed &&
             a->ranks[rank].covered.elapsed == 200 + rank);
      PeakMpiPolicyFeedback fa, fb;
      assert(peak_mpi_policy_feedback(a->ranks[root(a, rank)].policy,
                                      (PeakMpiPolicyMemberId){rank, 99}, 1,
                                      &fa) == PEAK_MPI_POLICY_OK);
      assert(peak_mpi_policy_feedback(b->ranks[root(b, rank)].policy,
                                      (PeakMpiPolicyMemberId){rank, 99}, 1,
                                      &fb) == PEAK_MPI_POLICY_OK);
      assert(fa.T_safe == fb.T_safe && fa.T_safe > 0 && fa.F == fb.F &&
             fa.risk == fb.risk);
      PeakMpiPolicyReceiver ra, rb;
      PeakMpiPolicyDecision da, db;
      assert(peak_mpi_policy_receiver_init(&ra, &a->config.policy, fa.cohort,
                                           fa.member, 1) == PEAK_MPI_POLICY_OK);
      assert(peak_mpi_policy_receiver_init(&rb, &b->config.policy, fb.cohort,
                                           fb.member, 1) == PEAK_MPI_POLICY_OK);
      assert(
          peak_mpi_policy_receiver_accept(&ra, &fa, 1) == PEAK_MPI_POLICY_OK &&
          peak_mpi_policy_receiver_accept(&rb, &fb, 1) == PEAK_MPI_POLICY_OK);
      assert(peak_mpi_policy_receiver_snapshot_cut(
                 &ra, 1, fa.local_elapsed_at_cut, fa.cut, 100, &da) ==
             PEAK_MPI_POLICY_OK);
      assert(peak_mpi_policy_receiver_snapshot_cut(
                 &rb, 1, fb.local_elapsed_at_cut, fb.cut, 100, &db) ==
             PEAK_MPI_POLICY_OK);
      assert(da.global_available && db.global_available &&
             da.feedback_age_seconds == db.feedback_age_seconds &&
             da.available_headroom == db.available_headroom &&
             da.q_eff == db.q_eff && da.detach_target == db.detach_target &&
             da.candidate_stop_target == db.candidate_stop_target &&
             da.cadence_target == db.cadence_target &&
             da.reattach_future_rate_target == db.reattach_future_rate_target);
      assert(peak_mpi_policy_receiver_snapshot_cut(&ra, 5, fa.local_elapsed_at_cut + 4, fa.cut, 100, &da) == PEAK_MPI_POLICY_OK);
      assert(peak_mpi_policy_receiver_snapshot_cut(&rb, 5, fb.local_elapsed_at_cut + 4, fb.cut, 100, &db) == PEAK_MPI_POLICY_OK);
      assert(da.mode == PEAK_MPI_POLICY_STALE && db.mode == PEAK_MPI_POLICY_STALE &&
             da.feedback_age_seconds == db.feedback_age_seconds &&
             !da.reattach_relaxation && !db.reattach_relaxation && da.q_eff == db.q_eff);
      printf("backend serial=%d rank=%u D=%.17g "
             "exactcosts/elapsed/headroom/targets PASS\n",
             serial, rank, dx);
    }
    peak_mpi_causal_service_destroy(a);
    peak_mpi_causal_service_destroy(b);
  }
}
static PeakMpiObserverRecord queue_test_record(uint64_t seq, uint64_t group,
                                               uint64_t previous, int end) {
  PeakMpiObserverRecord r = {0};
  r.incarnation = 100;
  r.thread_uid = r.thread_generation = 1;
  r.sequence = seq;
  r.op_uid = group + 1000;
  r.batch_uid = group;
  r.result_ref = previous;
  r.kind =
      end ? PEAK_MPI_OBSERVER_EVENT_COMPLETE : PEAK_MPI_OBSERVER_EVENT_START;
  r.phase = end ? PEAK_MPI_CAUSAL_CALL_END : PEAK_MPI_CAUSAL_CALL_BEGIN;
  r.operation = PEAK_MPI_CAUSAL_BSEND;
  r.flags = PEAK_MPI_CAUSAL_ORDERED_CALLER | PEAK_MPI_CAUSAL_COMPACT_GROUP |
            PEAK_MPI_CAUSAL_APP_COST_KNOWN |
            PEAK_MPI_OBSERVER_PROGRESS_ESTIMATED |
            (end ? PEAK_MPI_CAUSAL_SUCCESS : 0);
  r.communicator_uid = 1;
  r.peer = 1;
  r.tag = 7;
  r.capability_uid = PEAK_MPI_PROGRESS_INTEL_2021_11_OPX_ASYNC0_APPROX;
  r.progress_scope = PEAK_MPI_OBSERVER_SCOPE_INTERVAL;
  return r;
}
static void test_unclosed_input_capacity(void) {
  PeakMpiCausalService *s = make();
  peak_mpi_causal_batches_destroy(s->batches);
  s->config.max_pending_records = 2;
  s->batches = peak_mpi_causal_batches_create_with_slots(2, 2, 2);
  PeakMpiObserverRecord r = queue_test_record(1, 20, 0, 0);
  r.flags = 0;
  r.kind = PEAK_MPI_OBSERVER_EVENT_LOCAL_COST;
  r.phase = PEAK_MPI_CAUSAL_GROUP_BEGIN;
  r.op_uid = 20;
  r.result_ref = 0;
  assert(peak_mpi_causal_service_ingest_record(s, 0, &r));
  r = queue_test_record(2, 20, 0, 0);
  r.flags &= ~PEAK_MPI_CAUSAL_COMPACT_GROUP;
  r.result_ref = 0;
  assert(peak_mpi_causal_service_ingest_record(s, 0, &r));
  assert(peak_mpi_causal_batches_pending(s->batches) == 2);
  assert(!peak_mpi_causal_service_runnable(s));
  r = queue_test_record(3, 20, 0, 1);
  r.flags &= ~PEAK_MPI_CAUSAL_COMPACT_GROUP;
  assert(peak_mpi_causal_service_ingest_record(s, 0, &r));
  assert(s->ranks[0].coarse && s->ranks[0].lost == 1);
  peak_mpi_causal_service_destroy(s);
  puts("all-unclosed statefull retains hard resource failure rather than "
       "deadlocking behind missingEND PASS");
}
static void test_recovery_input_capacity(void) {
  for (unsigned reset = 0; reset <= 2; reset += 2) {
    PeakMpiCausalService *s = make();
    peak_mpi_causal_batches_destroy(s->batches);
    s->config.max_pending_records = 4;
    s->batches = peak_mpi_causal_batches_create_with_slots(2, 2, 4);
    assert(s->batches);
    for (unsigned i = 0; i < 4; ++i) {
      PeakMpiObserverRecord r =
          queue_test_record(i + 1, 10 + i / 2, i / 2 ? 10 : 0, i % 2);
      assert(peak_mpi_causal_service_ingest_record(s, 0, &r));
    }
    s->recovery_needed = 1;
    s->recovery.state = PEAK_MPI_RECOVERY_QUERY;
    s->recovery_reset = reset;
    if (reset)
      peak_mpi_causal_batches_reset_begin(s->batches);
    assert(!s->ranks[0].coarse);
    assert(!!peak_mpi_causal_service_runnable(s) == !!reset);
    PeakMpiObserverRecord r = queue_test_record(5, 12, 11, 0);
    assert(peak_mpi_causal_service_ingest_record(s, 0, &r));
    assert(s->ranks[0].coarse &&
           s->ranks[0].reason == PEAK_MPI_POLICY_RESOURCE_LIMIT);
    assert(s->ranks[0].lost == 1 && s->ranks[0].sequence[0] == 5);
    peak_mpi_causal_service_destroy(s);
  }
}
static void test_input_backpressure(void) {
  test_recovery_input_capacity();
  test_unclosed_input_capacity();
  PeakMpiCausalService *s = make();
  peak_mpi_causal_batches_destroy(s->batches);
  s->config.max_pending_records = 4;
  s->batches = peak_mpi_causal_batches_create_with_slots(2, 2, 4);
  assert(s->batches);
  for (unsigned i = 0; i < 4; i++) {
    PeakMpiObserverRecord r =
        queue_test_record(i + 1, 10 + i / 2, i / 2 ? 10 : 0, i % 2);
    assert(peak_mpi_causal_service_ingest_record(s, 0, &r));
  }
  assert(peak_mpi_causal_batches_pending(s->batches) == 4);
  assert(peak_mpi_causal_service_runnable(s));
  s->hard_fault = 1;
  assert(!peak_mpi_causal_service_runnable(s));
  s->hard_fault = 0;
  s->recovery_needed = 1;
  s->recovery.state = PEAK_MPI_RECOVERY_QUERY;
  assert(!peak_mpi_causal_service_runnable(s));
  s->recovery_needed = 0;
  s->recovery.state = PEAK_MPI_RECOVERY_NONE;
  assert(peak_mpi_causal_service_runnable(s));
  PeakMpiObserverRecord blocked = queue_test_record(5, 12, 11, 0);
  assert(!peak_mpi_causal_service_ingest_record(s, 0, &blocked));
  assert(!s->ranks[0].coarse && !s->stats.lost_events &&
         s->ranks[0].sequence[0] == 4 && s->stats.records == 4);
  size_t tries = 0;
  while (peak_mpi_causal_batches_pending(s->batches) == 4) {
    parse_batches(s, 1);
    assert(++tries < 100);
  }
  assert(peak_mpi_causal_service_ingest_record(s, 0, &blocked));
  assert(s->ranks[0].sequence[0] == 5 && s->stats.records == 5 &&
         !s->ranks[0].coarse);
  assert(peak_mpi_causal_service_ingest_record(s, 0, &blocked));
  assert(s->stats.records == 5 && s->stats.duplicates == 1);
  peak_mpi_causal_service_destroy(s);
  puts("full closed pendingqueue backpressures beforeseq/counters; "
       "boundedparse+exactretry+duplicate PASS");
}

static PeakMpiCausalService *fault_make_partial(void) {
  PeakMpiCausalServiceConfig c = {0};
  c.rank = c.owner_rank = 0;
  c.world_size = 4;
  c.job_context = 99;
  c.incarnation = 100;
  c.address = "127.0.0.1";
  c.port = 39878;
  c.slots = 2;
  c.queue_frames = 16;
  c.max_operations = 64;
  c.max_context_parts = 32;
  c.max_pending_records = 64;
  c.model = (PeakMpiCostConfig){1024, 4096, 1024, 128, 4, 64};
  c.policy = (PeakMpiPolicyConfig){4, .1, 1, .5, 0, 0, 1};
  PeakMpiCausalService *s = peak_mpi_causal_service_create(&c);
  assert(s);
  return s;
}

static void idle_feedback_turn(PeakMpiCausalService *s,
                               PeakMpiCausalTransport *const *clients,
                               PeakMpiCausalRaw *raw, double now) {
  for (unsigned r = 0; r < s->config.world_size; ++r) {
    ++raw[r].generation;
    raw[r].elapsed = now;
    if (r) {
      raw[r].watermark_nonce =
          peak_mpi_causal_transport_requested_nonce(clients[r]);
      peak_mpi_causal_transport_tick(clients[r], NULL, &raw[r], now, 65536);
    }
  }
  peak_mpi_causal_service_tick(s, NULL, &raw[0], now, 65536, 16384);
  peak_mpi_causal_service_tick(s, NULL, NULL, now, 65536, 16384);
  for (unsigned r = 1; r < s->config.world_size; ++r)
    peak_mpi_causal_transport_tick(clients[r], NULL, NULL, now, 65536);
}

static void test_idle_feedback_publication(void) {
  PeakMpiCausalService *s = fault_make_partial();
  PeakMpiCausalServiceConfig cfg = s->config;
  PeakMpiCausalTransportSink sink = {0};
  PeakMpiCausalTransport *clients[4] = {0};
  PeakMpiCausalRaw raw[4] = {0};
  unsigned char peers[4] = {1, 1, 1, 1};
  uint64_t prefixes[2] = {0};
  for (unsigned r = 0; r < 4; ++r) {
    raw[r] = (PeakMpiCausalRaw){.incarnation = 100 + r, .active = 1,
        .idle_cut = 1, .known_peers = peers, .peer_count = 4,
        .slot_prefixes = prefixes, .slot_count = 2, .app_cost_known = 1,
        .contract_profile = PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC};
    Rank *rank = &s->ranks[r];
    assert(node(s, r, 0, 0, NULL, &rank->continuation));
    assert(good(s, r, peak_mpi_cost_model_seal(s->model, rank->continuation)));
    rank->have_continuation = 1;
    if (r) {
      cfg.rank = r;
      cfg.incarnation = 100 + r;
      clients[r] = peak_mpi_causal_transport_create(&cfg, &sink);
      assert(clients[r]);
    }
  }
  PeakMpiPolicyFeedback feedback = {0};
  peak_mpi_causal_service_tick(s, NULL, &raw[0], 1, 65536, 16384);
  for (unsigned i = 0; i < 100; ++i) {
    idle_feedback_turn(s, clients, raw, 1);
    if (peak_mpi_causal_transport_feedback(clients[3], &feedback) &&
        feedback.coverage_complete)
      break;
  }
  assert(feedback.coverage_complete && feedback.member_count == 4);
  PeakMpiCausalServiceStats before, after;
  peak_mpi_causal_service_stats(s, &before);
  uint64_t generation = s->generation;
  for (unsigned i = 1; i <= 80; ++i) {
    for (unsigned r = 0; r < 4; ++r) {
      raw[r].cut.L_ns += 1000000;
      raw[r].L = sec(raw[r].cut.L_ns);
    }
    idle_feedback_turn(s, clients, raw, 1 + i * .01);
  }
  peak_mpi_causal_service_stats(s, &after);
  printf("idle TCP feedback: 80 turns, %llu cuts, %llu bytes sent, "
         "%llu received\n", (unsigned long long)(s->generation - generation),
         (unsigned long long)(after.sent_bytes - before.sent_bytes),
         (unsigned long long)(after.received_bytes - before.received_bytes));
  assert(s->generation == generation && after.sent_bytes == before.sent_bytes);
  assert(after.received_bytes > before.received_bytes &&
         s->ranks[3].raw_cut.L_ns == raw[3].cut.L_ns);

  ++s->ranks[3].requested_nonce;
  idle_feedback_turn(s, clients, raw, 1.81);
  idle_feedback_turn(s, clients, raw, 1.82);
  assert(s->ranks[3].verified_nonce == 1 && s->generation == generation);

  raw[2].cut.A_ns = raw[2].cut.P_ns = 2000000;
  raw[2].A = raw[2].P = .002;
  idle_feedback_turn(s, clients, raw, 1.83);
  assert(peak_mpi_causal_transport_feedback(clients[3], &feedback) &&
         feedback.F == .002 && feedback.coverage_complete);
  uint64_t updated = feedback.generation;
  idle_feedback_turn(s, clients, raw, 2.84);
  assert(peak_mpi_causal_transport_feedback(clients[3], &feedback) &&
         feedback.generation > updated && feedback.local_elapsed_at_cut == 2.84);
  peak_mpi_causal_service_tick(s, NULL, NULL, 6, 65536, 16384);
  peak_mpi_causal_service_tick(s, NULL, NULL, 6, 65536, 16384);
  peak_mpi_causal_transport_tick(clients[3], NULL, NULL, 6, 65536);
  assert(peak_mpi_causal_transport_feedback(clients[3], &feedback) &&
         feedback.mode == PEAK_MPI_POLICY_STALE);
  idle_feedback_turn(s, clients, raw, 6.01);
  assert(peak_mpi_causal_transport_feedback(clients[3], &feedback) &&
         feedback.mode == PEAK_MPI_POLICY_COMPLETE);
  raw[3].unknown_members = 1;
  idle_feedback_turn(s, clients, raw, 6.02);
  assert(peak_mpi_causal_transport_feedback(clients[3], &feedback) &&
         !feedback.membership_complete && !feedback.risk_known);
  for (unsigned r = 1; r < 4; ++r)
    peak_mpi_causal_transport_destroy(clients[r]);
  peak_mpi_causal_service_destroy(s);
}

static void test_raw_cohort_membership(void) {
  PeakMpiCausalService *s = fault_make_partial();
  unsigned char peers[4] = {0, 1, 0, 0};
  PeakMpiCausalRaw raw = {0};
  raw.incarnation = 100; raw.generation = 1; raw.L = .1;
  raw.P = .01; raw.elapsed = 1; raw.active = 1;
  raw.known_peers = peers; raw.peer_count = 4;
  assert(peak_mpi_causal_service_ingest_raw(s, 0, &raw, 1));
  assert(root(s, 0) == root(s, 1) && root(s, 0) != root(s, 2) &&
         s->ranks[root(s, 0)].cohort_size == 2);
  uint64_t epoch = s->epoch;
  raw.generation++;
  assert(peak_mpi_causal_service_ingest_raw(s, 0, &raw, 1) && s->epoch == epoch);
  peers[1] = 0; peers[3] = 1;
  raw.incarnation = 102; raw.generation = 1;
  assert(peak_mpi_causal_service_ingest_raw(s, 2, &raw, 1));
  assert(root(s, 2) == root(s, 3) && root(s, 0) != root(s, 2));
  peers[3] = 0; peers[2] = 1;
  raw.incarnation = 101;
  assert(peak_mpi_causal_service_ingest_raw(s, 1, &raw, 1));
  unsigned cohort = root(s, 0);
  assert(s->ranks[cohort].cohort_size == 4 && root(s, 3) == cohort);
  epoch = s->epoch;
  memset(peers, 1, sizeof peers);
  raw.incarnation = 103; raw.L = .2; raw.P = .02; raw.elapsed = 2;
  raw.unknown_members = 1;
  raw.contract_profile = PEAK_MPI_PROGRESS_INTEL_2021_11_OPX_ASYNC0_APPROX;
  assert(peak_mpi_causal_service_ingest_raw(s, 3, &raw, 2));
  assert(s->epoch == epoch && s->ranks[cohort].cohort_size == 4 &&
         s->ranks[3].L == .2 && s->ranks[3].P == .02 &&
         s->ranks[3].elapsed == 2 && s->ranks[3].unknown &&
         s->ranks[cohort].progress_estimate && !s->hard_fault);
  peak_mpi_causal_service_destroy(s);
  puts("RAW membership joins partial cohorts; full cohort preserves costs and uncertainty PASS");
}
static void fault_seed(PeakMpiCausalService *s, int joined) {
  assert(merge(s, 0, 1));
  if (!joined)
    assert(merge(s, 2, 3));
  for (unsigned rank = 0; rank < s->config.world_size; rank += 2) {
    PeakMpiPolicy *p = s->ranks[rank].policy;
    PeakMpiPolicyCohortKey key = s->ranks[rank].key;
    PeakMpiPolicySample samples[2];
    stale_samples(samples, 1, .01, .01, 1);
    for (unsigned j = 0; j < 2; j++) {
      samples[j].member.process = rank + j;
      assert(peak_mpi_policy_record_raw(p, key, &samples[j], 1) ==
             PEAK_MPI_POLICY_OK);
    }
    PeakMpiPolicyCut c = stale_cut(samples, 1, 0);
    c.cohort = key;
    assert(peak_mpi_policy_submit_cut(p, &c, 1) == PEAK_MPI_POLICY_OK);
  }
  s->now = 1;
}
static void check_fault_cache(unsigned why, int partial) {
  PeakMpiCausalService *s = partial ? fault_make_partial() : make();
  fault_seed(s, !partial);
  assert(prepare_feedback_cache(s, 100000) && !s->cache_dirty);
  PeakMpiPolicyFeedback old = s->feedback_cache[0], next;
  assert(old.mode == PEAK_MPI_POLICY_COMPLETE);
  PeakMpiPolicyReceiver receiver;
  assert(!peak_mpi_policy_receiver_init(&receiver, &stale_cfg, s->ranks[0].key,
                                        stale_ids[0], 1));
  assert(!peak_mpi_policy_receiver_rebind(&receiver, s->ranks[0].key, 2, 1, 1));
  assert(!peak_mpi_policy_receiver_accept(&receiver, &old, 1));
  if (partial) {
    s->cache_dirty = 1;
    size_t needed = peak_mpi_policy_feedback_batch_work(s->ranks[0].policy) + 4;
    assert(prepare_feedback_cache(s, needed) > 0);
    assert(s->cache_root == 1 && s->cache_dirty);
  }
  s->now = 1.1;
  if (why == PEAK_MPI_POLICY_EVENT_LOSS) {
    PeakMpiObserverRecord r = {0};
    r.incarnation = 100;
    r.thread_uid = r.thread_generation = 1;
    r.sequence = 2;
    assert(peak_mpi_causal_service_ingest_record(s, 0, &r));
  } else
    sink_fault(s, 0, (PeakMpiPolicyReason)why);
  printf("why%u partial%d dirty%d cursor%u\n", why, partial, s->cache_dirty,
         s->cache_root);
  assert(s->cache_dirty);
  assert(prepare_feedback_cache(s, 100000) > 0 && !s->cache_dirty);
  assert(sink_feedback(s, 0, 1.1, &next));
  printf("new mode%d reason%d g%g gen%llu old%llu\n", next.mode, next.reason,
         next.g, (unsigned long long)next.generation,
         (unsigned long long)old.generation);
  assert(next.mode == PEAK_MPI_POLICY_COARSE && next.reason == why &&
         next.g == 2 && next.generation > old.generation);
  assert(peak_mpi_policy_receiver_accept(&receiver, &next, 1.1) ==
         PEAK_MPI_POLICY_OK);
  double received = receiver.received_at;
  assert(peak_mpi_policy_receiver_accept(&receiver, &next, 1.2) ==
             PEAK_MPI_POLICY_IGNORED &&
         receiver.received_at == received);
  assert(!prepare_feedback_cache(s, 100000) &&
         s->feedback_cache[0].generation == next.generation);
  coarse(s, 0, (PeakMpiPolicyReason)why, 0);
  assert(!s->cache_dirty && !prepare_feedback_cache(s, 100000) &&
         s->feedback_cache[0].generation == next.generation);
  s->now = 1.3;
  coarse(s, 0, (PeakMpiPolicyReason)why, 1);
  assert(s->cache_dirty && prepare_feedback_cache(s, 100000));
  PeakMpiPolicyFeedback lost;
  sink_feedback(s, 0, 1.3, &lost);
  assert(lost.generation > next.generation &&
         lost.lost_events > next.lost_events);
  assert(!peak_mpi_policy_receiver_accept(&receiver, &lost, 1.3));
  if (partial)
    assert(s->feedback_cache[2].mode == PEAK_MPI_POLICY_COMPLETE);
  peak_mpi_causal_service_destroy(s);
}
static void test_cache_publish_cursor(void) {
  PeakMpiCausalService *s = fault_make_partial();
  fault_seed(s, 0);
  assert(prepare_feedback_cache(s, 100000));
  PeakMpiPolicyFeedback old = s->feedback_cache[0];
  s->cache_dirty = 1;
  size_t needed = peak_mpi_policy_feedback_batch_work(s->ranks[0].policy) + 4;
  assert(prepare_feedback_cache(s, needed) > 0 && s->cache_root == 1);
  PeakMpiPolicySample samples[2];
  stale_samples(samples, 2, .02, .02, 1.1);
  PeakMpiPolicy *p = s->ranks[0].policy;
  for (unsigned j = 0; j < 2; j++)
    assert(!peak_mpi_policy_record_raw(p, s->ranks[0].key, &samples[j], 1.1));
  memcpy(s->samples, samples, sizeof samples);
  s->now = s->pub_started = 1.1;
  s->generation = 1;
  s->pub_stage = 4;
  s->pub_root = 0;
  s->pub_count = 2;
  s->pub_complete = 1;
  s->pub_F = .02;
  s->pub_age = s->pub_oldest = 0;
  s->pub_epoch = s->epoch;
  assert(!publish_work(s, 10) && s->pub_root == 1);
  assert(prepare_feedback_cache(s, 100000) > 0);
  PeakMpiPolicyFeedback next;
  sink_feedback(s, 0, 1.1, &next);
  printf("publish afterpartial gen%llu old%llu A%.17g\n",
         (unsigned long long)next.generation,
         (unsigned long long)old.generation, next.A_cut);
  assert(next.generation > old.generation && next.A_cut == .02);
  peak_mpi_causal_service_destroy(s);
}
static void test_cache_recovery_cursor(void) {
  PeakMpiCausalService *s = fault_make_partial();
  uint64_t generation;
  PeakMpiPolicyFeedback f;
  assert(!sink_feedback_generation(s, 0, &generation));
  assert(!sink_feedback_generation(s, s->config.world_size, &generation));
  fault_seed(s, 0);
  assert(prepare_feedback_cache(s, 100000));
  assert(sink_feedback_generation(s, 0, &generation) &&
         sink_feedback(s, 0, 1, &f) && generation == f.generation);
  s->cache_dirty = 1;
  size_t need = peak_mpi_policy_feedback_batch_work(s->ranks[0].policy) + 4;
  assert(prepare_feedback_cache(s, need) > 0 && s->cache_root == 1);
  for (unsigned i = 0; i < s->config.world_size; i++) {
    PeakMpiPolicy *p = s->ranks[i].policy;
    peak_mpi_policy_recover_begin(p, 1.1, 1);
    size_t used;
    while (!peak_mpi_policy_recover_work(p, 100000, &used)) {
    }
  }
  s->now = 1.1;
  s->recovery_needed = 1;
  s->recovery.state = PEAK_MPI_RECOVERY_QUERY;
  s->recovery.epoch = 2;
  assert(sink_feedback_generation(s, 0, &generation) &&
         sink_feedback(s, 0, 1.1, &f) && generation == f.generation &&
         generation == s->feedback_cache[0].generation + 8 + PEAK_MPI_RECOVERY_QUERY);
  s->recovery_reset = 7;
  s->recovery_rank = s->config.world_size;
  size_t used;
  assert(!recovery_work(s, 1, &used));
  assert(s->recovery.state == PEAK_MPI_RECOVERY_ACK);
  assert(sink_feedback_generation(s, 0, &generation) &&
         sink_feedback(s, 0, 1.1, &f) && generation == f.generation &&
         f.recovery.state == PEAK_MPI_RECOVERY_QUERY &&
         generation == s->feedback_cache[0].generation + 8 + PEAK_MPI_RECOVERY_ACK);
  assert(prepare_feedback_cache(s, 100000) > 0);
  printf("recovery partialroot0 feedbackepoch%llu expected2\n",
         (unsigned long long)s->ranks[0].feedback_epoch);
  assert(s->ranks[0].feedback_epoch == 2);
  assert(sink_feedback_generation(s, 0, &generation) &&
         sink_feedback(s, 0, 1.1, &f) && generation == f.generation &&
         f.recovery.state == PEAK_MPI_RECOVERY_ACK);
  peak_mpi_causal_service_destroy(s);
}
static void test_fault_cache(void) {
  test_cache_publish_cursor();
  test_cache_recovery_cursor();
  check_fault_cache(PEAK_MPI_POLICY_EVENT_LOSS, 0);
  check_fault_cache(PEAK_MPI_POLICY_EVENT_LOSS, 1);
  for (unsigned reason = PEAK_MPI_POLICY_MISSING_CONTRACT;
       reason <= PEAK_MPI_POLICY_BAD_CUT; reason++)
    check_fault_cache(reason, 0);
  puts("fault cache publication +partialcohort replay+receiverduplicate PASS");
}

static void test_recovery_reset_runnable(void) {
  PeakMpiCausalService *s = make();
  uint64_t prefixes[2] = {10, 0};
  unsigned char peers[2] = {1, 1};
  PeakMpiCausalRaw raw[2];
  memset(raw, 0, sizeof raw);
  for (unsigned rank = 0; rank < 2; ++rank) {
    raw[rank].generation = 1;
    raw[rank].incarnation = 100 + rank;
    raw[rank].L = .1;
    raw[rank].P = .02;
    raw[rank].elapsed = 1;
    raw[rank].active = 1;
    raw[rank].contract_profile = PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC;
    raw[rank].slot_prefixes = prefixes;
    raw[rank].slot_count = 2;
    raw[rank].known_peers = peers;
    raw[rank].peer_count = 2;
    raw[rank].recovery.comm = raw[rank].recovery.ordinal = 1;
    raw[rank].recovery.operation = PEAK_MPI_CAUSAL_BARRIER;
    raw[rank].recovery.loss = raw[rank].lost_events = rank;
    raw[rank].recovery.fault_mask =
        rank ? UINT64_C(1) << PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW : 0;
    raw[rank].coarse_reason = rank ? PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW : 0;
    assert(peak_mpi_causal_service_ingest_raw(s, rank, &raw[rank], 1));
  }
  peak_mpi_causal_service_tick(s, NULL, &raw[0], 1, 65536, 16384);
  assert(s->recovery.state == PEAK_MPI_RECOVERY_QUERY && !s->recovery_reset);
  assert(!peak_mpi_causal_service_runnable(s));
  for (unsigned rank = 0; rank < 2; ++rank) {
    raw[rank].generation++;
    raw[rank].recovery.epoch = s->recovery.epoch;
    raw[rank].recovery.state = PEAK_MPI_RECOVERY_READY;
    raw[rank].recovery.uid_floor = 1000;
    raw[rank].recovery.L = .1;
    raw[rank].recovery.P = .02;
    raw[rank].recovery.elapsed = 1;
    assert(peak_mpi_causal_service_ingest_raw(s, rank, &raw[rank], 1.1));
    if (!rank) {
      peak_mpi_causal_service_tick(s, NULL, NULL, 1.1, 65536, 16);
      assert(!s->recovery_reset && !peak_mpi_causal_service_runnable(s));
    }
  }
  unsigned turns = 0, productive = 0;
  while (s->recovery.state != PEAK_MPI_RECOVERY_ACK) {
    size_t bytes = 65536, work = 16;
    peak_mpi_causal_service_tick_budget(s, NULL, NULL, 1.2 + turns * .001,
                                        &bytes, &work);
    assert(work <= 16 && ++turns < 1000);
    if (s->recovery_reset) {
      assert(peak_mpi_causal_service_runnable(s));
      if (work < 16)
        productive++;
      s->hard_fault = 1;
      assert(!peak_mpi_causal_service_runnable(s));
      s->hard_fault = 0;
      s->local_fault = PEAK_MPI_POLICY_BAD_IDENTITY;
      assert(!peak_mpi_causal_service_runnable(s));
      s->local_fault = 0;
      s->recovery_restart = 1;
      assert(!peak_mpi_causal_service_runnable(s));
      s->recovery_restart = 0;
    }
  }
  assert(productive >= 2 && !s->recovery_reset &&
         peak_mpi_causal_service_runnable(s));
  peak_mpi_causal_service_tick(s, NULL, NULL, s->now, 65536, 16384);
  PeakMpiPolicyFeedback feedback;
  assert(sink_feedback(s, 0, s->now, &feedback));
  assert(feedback.recovery.state == PEAK_MPI_RECOVERY_ACK);
  peak_mpi_causal_service_destroy(s);
  puts("CPU-only recovery reset drains to ACK; remote waits/faults/idle stay "
       "blocked PASS");
}
/* A resumed rank can record a collective which peers executed while paused. */
typedef struct {
  PeakMpiCausalService *service;
  PeakMpiCausalRaw raw[2];
  uint64_t prefix[2][2], previous_group[2], previous_call[2][2], next_uid;
  uint64_t A[2];
  unsigned char peers[2];
} RecoveryGroups;
static void recovery_raw(RecoveryGroups *c, unsigned rank, double now) {
  PeakMpiCausalRaw *r = &c->raw[rank];
  r->generation++;
  r->L = (double)c->A[rank] / 1e9;
  r->elapsed = now;
  r->watermark_nonce = c->service->ranks[rank].requested_nonce;
  assert(peak_mpi_causal_service_ingest_raw(c->service, rank, r, now));
}
static void recovery_groups_init(RecoveryGroups *c) {
  memset(c, 0, sizeof *c);
  c->service = make();
  c->next_uid = 1000;
  c->peers[0] = c->peers[1] = 1;
  for (unsigned rank = 0; rank < 2; ++rank) {
    c->prefix[rank][0] = 10;
    c->A[rank] = 100000000;
    PeakMpiCausalRaw *r = &c->raw[rank];
    r->incarnation = 100 + rank;
    r->P = .02;
    r->active = 1;
    r->contract_profile = PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC;
    r->slot_prefixes = c->prefix[rank];
    r->slot_count = 2;
    r->known_peers = c->peers;
    r->peer_count = 2;
    r->recovery.comm = r->recovery.ordinal = 1;
    r->recovery.operation = PEAK_MPI_CAUSAL_BARRIER;
    r->recovery.loss = r->lost_events = rank;
    r->recovery.fault_mask = rank ?
        UINT64_C(1) << PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW : 0;
    r->coarse_reason = rank ? PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW : 0;
    recovery_raw(c, rank, 1);
  }
  peak_mpi_causal_service_tick(c->service, NULL, NULL, 1, 65536, 100000);
  assert(c->service->recovery.state == PEAK_MPI_RECOVERY_QUERY);
  for (unsigned rank = 0; rank < 2; ++rank) {
    PeakMpiCausalRecovery *r = &c->raw[rank].recovery;
    r->epoch = c->service->recovery.epoch;
    r->state = PEAK_MPI_RECOVERY_READY;
    r->uid_floor = 1000;
    r->L = .1;
    r->P = .02;
    r->elapsed = 1;
    recovery_raw(c, rank, 1.1);
  }
  for (unsigned turn = 0; turn < 100 &&
      c->service->recovery.state != PEAK_MPI_RECOVERY_ACK; ++turn)
    peak_mpi_causal_service_tick(c->service, NULL, NULL, 1.2, 65536, 100000);
  assert(c->service->recovery.state == PEAK_MPI_RECOVERY_ACK &&
         c->service->approximate && !c->service->hard_fault);
  for (unsigned rank = 0; rank < 2; ++rank) {
    c->raw[rank].recovery.state = PEAK_MPI_RECOVERY_ACK;
    c->raw[rank].recovery.fault_mask = 0;
    c->raw[rank].coarse_reason = 0;
  }
}
static void recovery_scale_init(RecoveryGroups *c, int age, double learned) {
  memset(c, 0, sizeof *c);
  c->service = make();
  fault_seed(c->service, 1);
  PeakMpiPolicySample samples[2];
  stale_samples(samples, 2, .02, .02, 1.1);
  PeakMpiPolicyCut cut = stale_cut(samples, 2, 0);
  cut.cohort = c->service->ranks[0].key;
  cut.F = .01 + learned * .01;
  assert(peak_mpi_policy_submit_cut(c->service->ranks[0].policy, &cut, 1.1) ==
         PEAK_MPI_POLICY_OK);
  c->service->now = 1.1;
  assert(prepare_feedback_cache(c->service, 100000));
  assert(fabs(c->service->feedback_cache[0].g - learned) < 1e-12);
  PeakMpiPolicyReceiver receiver;
  PeakMpiPolicyDecision decision;
  PeakMpiPolicyFeedback before = c->service->feedback_cache[0];
  assert(peak_mpi_policy_receiver_init(&receiver, &stale_cfg, before.cohort,
      before.member, 0) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_accept(&receiver, &before, 1.1) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_snapshot_cut(&receiver, 1.1, 1.1,
      before.cut, 1, &decision) == PEAK_MPI_POLICY_OK);
  assert(peak_mpi_policy_receiver_mark_coarse(&receiver, PEAK_MPI_POLICY_EVENT_LOSS,
      6) == PEAK_MPI_POLICY_OK);
  if (age) {
    assert(peak_mpi_policy_tick(c->service->ranks[0].policy, 5) == PEAK_MPI_POLICY_OK);
    c->service->now = 5;
    c->service->cache_dirty = 1;
    assert(prepare_feedback_cache(c->service, 100000));
    assert(c->service->feedback_cache[0].mode == PEAK_MPI_POLICY_STALE &&
           c->service->feedback_cache[0].g == 2);
  }
  c->next_uid = 1000;
  c->peers[0] = c->peers[1] = 1;
  for (unsigned rank = 0; rank < 2; ++rank) {
    c->prefix[rank][0] = 10;
    c->A[rank] = 100000000;
    PeakMpiCausalRaw *r = &c->raw[rank];
    r->generation = 10;
    r->incarnation = 100 + rank;
    r->P = .02;
    r->active = 1;
    r->contract_profile = PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC;
    r->slot_prefixes = c->prefix[rank];
    r->slot_count = 2;
    r->known_peers = c->peers;
    r->peer_count = 2;
    r->recovery.comm = r->recovery.ordinal = 1;
    r->recovery.operation = PEAK_MPI_CAUSAL_BARRIER;
    r->recovery.loss = r->lost_events = rank;
    r->recovery.fault_mask = rank ?
        UINT64_C(1) << PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW : 0;
    r->coarse_reason = rank ? PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW : 0;
    recovery_raw(c, rank, 6);
  }
  peak_mpi_causal_service_tick(c->service, NULL, NULL, 6, 65536, 100000);
  assert(c->service->recovery.state == PEAK_MPI_RECOVERY_QUERY);
  for (unsigned rank = 0; rank < 2; ++rank) {
    PeakMpiCausalRecovery *r = &c->raw[rank].recovery;
    r->epoch = c->service->recovery.epoch;
    r->state = PEAK_MPI_RECOVERY_READY;
    r->uid_floor = 1000;
    r->L = .1;
    r->P = .02;
    r->elapsed = 1;
    recovery_raw(c, rank, 6.1);
  }
  for (unsigned turn = 0; turn < 100 &&
      c->service->recovery.state != PEAK_MPI_RECOVERY_ACK; ++turn)
    peak_mpi_causal_service_tick(c->service, NULL, NULL, 6.2, 65536, 100000);
  assert(c->service->recovery.state == PEAK_MPI_RECOVERY_ACK &&
         c->service->approximate && !c->service->hard_fault);
  /* The runtime handles the recovery transition and then delivers the same
   * feedback to the policy receiver. The ACK can precede the next cost cut. */
  PeakMpiPolicyFeedback ack;
  assert(peak_mpi_causal_service_feedback(c->service, &ack));
  assert(ack.recovery.state == PEAK_MPI_RECOVERY_ACK && ack.event_cut == 0 &&
         ack.local_elapsed_at_cut == 0 && before.event_cut > 0);
  assert(peak_mpi_policy_receiver_recover(&receiver, 6.2));
  assert(peak_mpi_policy_receiver_accept(&receiver, &ack, 6.2) == PEAK_MPI_POLICY_IGNORED);
  PeakMpiCostCut local = {20000000, 100000000, 100000000};
  assert(peak_mpi_policy_receiver_snapshot_cut(&receiver, 6.2, 6.2, local, 1,
      &decision) == PEAK_MPI_POLICY_OK);
  assert(fabs(decision.g - learned) < 1e-12 && !decision.global_available &&
         !decision.headroom_known && !decision.reattach_relaxation);
  assert(!receiver.have_feedback && receiver.feedback.event_cut == before.event_cut);

  before.generation = ack.generation + 1;
  before.recovery = ack.recovery;
  before.event_cut--;
  /* A real backward cut remains invalid even when it carries the ACK. */
  assert(peak_mpi_policy_receiver_accept(&receiver, &before, 6.3) == PEAK_MPI_POLICY_INVALID);
  assert(peak_mpi_policy_receiver_snapshot_cut(&receiver, 6.3, 6.3, local, 1,
      &decision) == PEAK_MPI_POLICY_OK);
  assert(decision.mode == PEAK_MPI_POLICY_STALE && decision.g == 2 &&
         !decision.global_available && !decision.reattach_relaxation);
  for (unsigned rank = 0; rank < 2; ++rank) {
    c->raw[rank].recovery.state = PEAK_MPI_RECOVERY_ACK;
    c->raw[rank].recovery.fault_mask = 0;
    c->raw[rank].coarse_reason = 0;
  }
}

static void test_stale_recovery_scale(void) {
  for (unsigned scale = 0; scale < 2; ++scale) {
    double learned = scale ? 1.5 : 1;
    for (int aged = 0; aged < 2; ++aged) {
      RecoveryGroups c;
      recovery_scale_init(&c, aged, learned);
      PeakMpiCausalService *s = c.service;
      assert(fabs(s->recovery_g - learned) < 1e-12);
      for (unsigned gen = 100; gen < 103; ++gen) {
        PeakMpiPolicySample samples[2];
        stale_samples(samples, gen, .1, .1, 6.3 + (gen - 100)*.1);
        for (unsigned rank = 0; rank < 2; ++rank) {
          samples[rank].P = .02;
          samples[rank].cut.P_ns = 20000000;
        }
        PeakMpiPolicyCut cut = stale_cut(samples, gen, 0);
        cut.cohort = s->ranks[0].key;
        cut.F = .1;
        cut.quality = PEAK_MPI_POLICY_OBSERVED_PROGRESS_CUT;
        assert(peak_mpi_policy_submit_cut(s->ranks[0].policy, &cut,
            6.3 + (gen - 100)*.1) == PEAK_MPI_POLICY_OK);
      }
      PeakMpiPolicyFeedback feedback;
      assert(peak_mpi_policy_feedback(s->ranks[0].policy, stale_ids[0],
                                     6.5, &feedback) == PEAK_MPI_POLICY_OK);
      assert(feedback.mode == PEAK_MPI_POLICY_APPROX &&
             fabs(feedback.g - learned) < 1e-12);
      peak_mpi_causal_service_destroy(s);
    }
  }
  puts("STALE effective N does not replace learned 1/1.5 across ACK; fresh zero-P-delta cuts preserve learned scale PASS");
}
static uint64_t recovery_group(RecoveryGroups *c, unsigned rank,
    unsigned slot, uint64_t ordinal, uint64_t comm, unsigned flags,
    PeakMpiObserverRecord deferred[4]) {
  uint64_t group = c->next_uid++, uid = c->next_uid++;
  static const unsigned phases[] = {PEAK_MPI_CAUSAL_GROUP_BEGIN,
      PEAK_MPI_CAUSAL_CALL_BEGIN, PEAK_MPI_CAUSAL_CALL_END,
      PEAK_MPI_CAUSAL_GROUP_END};
  for (unsigned i = 0; i < 4; ++i) {
    PeakMpiObserverRecord r = {0};
    r.incarnation = 100 + rank;
    r.thread_uid = slot + 1;
    r.thread_generation = 1;
    r.sequence = ++c->prefix[rank][slot];
    r.phase = phases[i];
    r.op_uid = i == 0 || i == 3 ? group : uid;
    r.batch_uid = group;
    r.input_ref = i == 0 ? c->previous_group[rank]
                       : i == 1 ? c->previous_call[rank][slot] : 0;
    r.kind = PEAK_MPI_OBSERVER_EVENT_START;
    r.flags = PEAK_MPI_CAUSAL_ORDERED_CALLER | PEAK_MPI_CAUSAL_APP_COST_KNOWN;
    r.L_cut = r.A_cut = c->A[rank];
    r.P_cut = 20000000;
    r.communicator_uid = comm;
    r.capability_uid = PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC;
    r.progress_scope = PEAK_MPI_OBSERVER_SCOPE_AUTONOMOUS;
    r.collective_ordinal = ordinal;
    if (i == 1 || i == 2) {
      r.operation = PEAK_MPI_CAUSAL_ALLREDUCE;
      r.flags |= PEAK_MPI_CAUSAL_COLLECTIVE | flags;
    }
    if (i == 2)
      r.flags |= PEAK_MPI_OBSERVER_PROGRESS_SEALED;
    if (i == 0 || i == 3)
      r.progress_end = (uint64_t)(fmax(2 + (double)ordinal / 1000,
          c->raw[rank].elapsed + .001) * 1e9);
    if (i == 3)
      r.completion_count = 1;
    if (deferred)
      deferred[i] = r;
    else
      assert(peak_mpi_causal_service_ingest_record(c->service, rank, &r));
  }
  c->previous_group[rank] = group;
  c->previous_call[rank][slot] = uid;
  return uid;
}
static void recovery_drain(RecoveryGroups *c, unsigned turns, int fresh,
                            size_t budget) {
  for (unsigned turn = 0; turn < turns; ++turn) {
    double now = fmax(10 + (double)c->raw[0].generation / 1000,
        fmax(c->service->now, fmax(c->raw[0].elapsed, c->raw[1].elapsed))) + .001;
    if (fresh)
      for (unsigned rank = 0; rank < 2; ++rank)
        recovery_raw(c, rank, now);
    peak_mpi_causal_service_tick(c->service, NULL, NULL, now, 65536, budget);
    assert(c->service->last_tick_work <= budget);
  }
}
static void test_pending_idle_cut_reuse(void) {
  RecoveryGroups c = {0};
  c.service = make(); c.next_uid = 1000;
  c.peers[0] = c.peers[1] = 1;
  for (unsigned rank = 0; rank < 2; ++rank) {
    c.A[rank] = 100000000;
    PeakMpiCausalRaw *raw = &c.raw[rank];
    raw->incarnation = 100 + rank; raw->P = .02; raw->active = 1;
    raw->contract_profile = PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC;
    raw->slot_prefixes = c.prefix[rank]; raw->slot_count = 2;
    raw->known_peers = c.peers; raw->peer_count = 2;
    raw->app_cost_known = 1; raw->A = .1;
    raw->cut = (PeakMpiCostCut){20000000, 100000000, 100000000};
    recovery_raw(&c, rank, 1);
  }
  recovery_group(&c, 0, 0, 1, 1, 0, NULL);
  c.raw[0].idle_cut = 1;
  c.raw[0].idle_group_uid = c.previous_group[0];
  PeakMpiCostRef ref = {0};
  uint64_t prior_model_generation = 0;
  for (unsigned cycle = 0; cycle < c.service->config.model.max_nodes + 2; ++cycle) {
    PeakMpiCausalRaw *raw = &c.raw[0];
    raw->generation++; raw->elapsed = 3 + cycle * .01;
    raw->L = .1 + cycle * 1e-6;
    raw->cut.L_ns = 100000000 + (uint64_t)cycle * 1000;
    raw->watermark_nonce = c.service->ranks[0].requested_nonce;
    assert(peak_mpi_causal_service_ingest_raw(c.service, 0, raw, raw->elapsed));
    peak_mpi_causal_service_tick(c.service, NULL, NULL, raw->elapsed, 65536, 16384);
    assert(!c.service->hard_fault && c.service->stats.records == 4 &&
           c.service->free_cut_count == c.service->config.model.max_nodes - 1);
    SettledCut *tail = &c.service->cuts[c.service->ranks[0].cut_tail];
    if (!cycle) ref = tail->ref;
    PeakMpiCostStats pending_stats;
    peak_mpi_cost_model_stats(c.service->model, &pending_stats);
    assert(tail->index > c.service->ranks[0].root_index);
    if (cycle) assert(pending_stats.generation == prior_model_generation);
    prior_model_generation = pending_stats.generation;
    assert(tail->ref.slot == ref.slot && tail->ref.generation == ref.generation &&
           tail->sample.cut.L_ns == raw->cut.L_ns &&
           fabs(tail->sample.L - raw->L) < 1e-12 && tail->sample.P == .02 &&
           tail->sample.A == .1 && tail->sample.elapsed == raw->elapsed &&
           tail->received == raw->elapsed && !c.service->ranks[0].covered.generation);
  }
  recovery_group(&c, 1, 0, 1, 1, 0, NULL);
  parse_batches(c.service, 16384);
  assert(c.service->graph_pending); /* The actual late peer changed the graph. */
  c.raw[0].generation++; c.raw[0].elapsed = c.service->now + .1;
  c.raw[0].watermark_nonce = c.service->ranks[0].requested_nonce;
  assert(peak_mpi_causal_service_ingest_raw(c.service, 0, &c.raw[0], c.raw[0].elapsed));
  verify_prefixes(c.service, 16384);
  assert(c.service->graph_pending); /* Reuse must never clear existing work. */
  recovery_drain(&c, 20, 0, 16384);
  double D;
  assert(!c.service->hard_fault &&
         !peak_mpi_cost_model_delay(c.service->model, ref, &D) && fabs(D - .1) < 1e-12 &&
         c.service->free_cut_count == c.service->config.model.max_nodes);
  assert(c.service->ranks[0].covered_ref.slot == ref.slot &&
         c.service->ranks[0].covered_ref.generation == ref.generation &&
         c.service->ranks[0].covered.cut.L_ns == c.raw[0].cut.L_ns &&
         fabs(c.service->ranks[0].covered.L - c.raw[0].L) < 1e-12 &&
         c.service->ranks[0].covered.P == .02 && c.service->ranks[0].covered.A == .1 &&
         c.service->ranks[0].covered.elapsed == c.raw[0].elapsed);
  peak_mpi_causal_service_destroy(c.service);
  /* Eligibility is deliberately narrower than arbitrary monotonic costs. */
  for (unsigned change = 0; change < 12; ++change) {
    PeakMpiCausalService *service = make();
    PeakMpiCostRef first;
    assert(node(service, 0, .1, .02, NULL, &first));
    PeakMpiPolicySample sample = {0};
    sample.member = (PeakMpiPolicyMemberId){0, 99};
    sample.L = sample.A = .1; sample.P = .02; sample.elapsed = 1;
    sample.app_cost_known = 1;
    sample.cut = (PeakMpiCostCut){20000000, 100000000, 100000000};
    if (change == 11) sample.app_cost_known = 0;
    assert(queue_cut(service, 0, first, sample, 1));
    peak_mpi_causal_service_tick(service, NULL, NULL, 1, 65536, 16384);
    assert(service->root_cut_active && !service->graph_pending);
    service->ranks[0].parsed_index = 1; /* Component seam: next cut position. */
    size_t free_before = service->free_cut_count;
    if (change == 0) ++first.generation;
    if (change == 1) ++sample.event_cut;
    if (change == 2) ++sample.control_generation;
    if (change == 3) sample.app_cost_known = 0;
    if (change == 4) { sample.P += 1e-6; sample.cut.P_ns += 1000; }
    if (change == 5) { sample.A += 1e-6; sample.cut.A_ns += 1000; }
    if (change == 6) ++sample.member.incarnation;
    if (change == 7) sample.elapsed = .5;
    if (change == 8) { ++service->ranks[0].last_A_cut; }
    if (change == 9) sample.member.process = 1;
    if (change == 11) { sample.L += 1e-6; sample.cut.L_ns += 1000; }
    assert(queue_cut(service, 0, first, sample, change == 10 ? .5 : 1));
    assert(service->free_cut_count == free_before - 1 && !service->hard_fault &&
           service->cuts[service->ranks[0].cut_tail].index == 1 && service->graph_pending);
    sample.cut.A_ns = 0; service->ranks[0].last_A_cut = 1;
    sample.app_cost_known = 1;
    assert(!queue_cut(service, 0, first, sample, 2) &&
           service->ranks[0].reason == PEAK_MPI_POLICY_BAD_CUT);
    peak_mpi_causal_service_destroy(service);
  }
  puts("pending identical idle cuts reuse unpublished storage; late peer and changed costs retain semantics PASS");
}
static void test_unchanged_idle_root(void) {
  RecoveryGroups c;
  recovery_groups_init(&c);
  PeakMpiObserverRecord records[4];
  uint64_t uid = recovery_group(&c, 1, 0, 2, 1, 0, records);
  for (unsigned i = 0; i < 4; ++i) {
    if (i == 1 || i == 2) {
      records[i].operation = PEAK_MPI_CAUSAL_SEND;
      records[i].flags &= ~(PEAK_MPI_CAUSAL_COLLECTIVE |
                            PEAK_MPI_OBSERVER_PROGRESS_SEALED);
      records[i].flags |= PEAK_MPI_CAUSAL_NONBLOCKING;
      records[i].progress_scope = PEAK_MPI_OBSERVER_SCOPE_INTERVAL;
      records[i].peer = 0;
      records[i].tag = records[i].actual_tag = 7;
      records[i].collective_ordinal = 0;
    }
    assert(peak_mpi_causal_service_ingest_record(c.service, 1, &records[i]));
  }
  recovery_drain(&c, 10, 0, 100000);
  for (unsigned cycle = 0; cycle < 5; ++cycle) {
    double now = c.service->now + .1;
    for (unsigned r = 0; r < 2; ++r) {
      c.raw[r].idle_cut = 1;
      c.raw[r].idle_group_uid = c.previous_group[r];
      recovery_raw(&c, r, now);
    }
    if (cycle) {
      assert(!c.service->graph_pending && !c.service->model_running &&
             !c.service->root_plan_stage);
      verify_prefixes(c.service, 100000);
      assert(!c.service->graph_pending);
    }
    recovery_drain(&c, 10, 0, 100000);
    Operation *send = find_op(c.service, 1, uid);
    assert(send && !send->completed && branch_live_at_root(c.service, send));
    assert(!c.service->hard_fault);
    for (unsigned r = 0; r < 2; ++r)
      assert(c.service->ranks[r].covered.elapsed == c.raw[r].elapsed);
  }
  for (unsigned r = 0; r < 2; ++r) {
    c.A[r] += 10000000;
    c.raw[r].P += .002;
    recovery_raw(&c, r, c.service->now + .1);
  }
  verify_prefixes(c.service, 100000);
  assert(!c.service->graph_pending);
  recovery_drain(&c, 10, 0, 100000);
  for (unsigned r = 0; r < 2; ++r) {
    assert(fabs(c.service->ranks[r].covered.L - .11) < 1e-12);
    assert(fabs(c.service->ranks[r].covered.P - .022) < 1e-12);
    assert(fabs(c.service->ranks[r].covered.D - .21) < 1e-12);
    assert(fabs(c.service->ranks[r].covered_tail - .01) < 1e-12);
  }
  assert(c.service->feedback_cache[0].coverage_complete &&
         c.service->feedback_cache[0].risk_known &&
         fabs(c.service->feedback_cache[0].F - .21) < 1e-12 &&
         fabs(c.service->feedback_cache[0].U_tail) < 1e-12);
  for (unsigned r = 0; r < 2; ++r) {
    recovery_group(&c, r, 0, 3, 1, 0, records);
    for (unsigned i = 0; i < 4; ++i) {
      records[i].P_cut = 22000000;
      assert(peak_mpi_causal_service_ingest_record(c.service, r, &records[i]));
    }
  }
  parse_batches(c.service, 100000);
  assert(c.service->graph_pending);
  recovery_drain(&c, 10, 0, 100000);
  assert(!c.service->hard_fault);
  for (unsigned r = 0; r < 2; ++r) {
    assert(c.service->ranks[r].last_A_cut == 110000000 &&
           c.service->ranks[r].last_P_cut == 22000000);
    recovery_group(&c, r, 0, 4, 1, 0, records);
    for (unsigned i = 0; i < 4; ++i) {
      records[i].P_cut = 22000000;
      assert(peak_mpi_causal_service_ingest_record(c.service, r, &records[i]));
    }
  }
  parse_batches(c.service, 100000);
  assert(c.service->graph_pending);
  recovery_drain(&c, 10, 0, 100000);
  assert(!c.service->hard_fault);
  peak_mpi_causal_service_destroy(c.service);
  puts("unchanged idle cuts preserve live branches and fresh costs without dirtying settled roots PASS");
}
static uint64_t local_observation(RecoveryGroups *c, unsigned slot,
    unsigned ordinal, unsigned operation) {
  PeakMpiObserverRecord records[4];
  uint64_t uid = recovery_group(c, 0, slot, ordinal, 0, 0, records);
  uint64_t A = c->A[0], P = 20000000 + (A - 100000000) / 3;
  for (unsigned i = 0; i < 4; ++i) {
    if (i == 1 || i == 2) {
      records[i].flags &= ~PEAK_MPI_CAUSAL_COLLECTIVE;
      records[i].operation = operation;
    }
    records[i].L_cut = records[i].A_cut = A + i * 10000;
    records[i].P_cut = P + i * 10000 / 3;
    assert(peak_mpi_causal_service_ingest_record(c->service, 0, &records[i]));
  }
  c->A[0] += 30000;
  c->raw[0].P = .02 + (double)(c->A[0] - 100000000) / 3e9;
  return uid;
}
/* These calls reuse the recovery fixture's actual record/RAW path. */
static uint64_t retained_call(RecoveryGroups *c, unsigned rank, unsigned slot,
    unsigned operation, unsigned flags, uint64_t probe) {
  PeakMpiObserverRecord records[4];
  unsigned remote = send_op(operation) || recv_op(operation) || probe_op(operation);
  uint64_t uid = recovery_group(c, rank, slot, 2, remote ? 1 : 0,
      flags & ~PEAK_MPI_CAUSAL_INACTIVE_REQUEST, records);
  uint64_t A = c->A[rank], P = 20000000 + (A - 100000000) / 3;
  for (unsigned i = 0; i < 4; ++i) {
    if (i == 1 || i == 2) {
      records[i].flags &= ~PEAK_MPI_CAUSAL_COLLECTIVE;
      records[i].operation = operation;
      records[i].peer = records[i].actual_source = 1 - rank;
      records[i].tag = records[i].actual_tag = 7;
      if (recv_op(operation)) records[i].flags |= PEAK_MPI_CAUSAL_RECEIVE;
      if (flags & PEAK_MPI_CAUSAL_NONBLOCKING)
        records[i].progress_scope = PEAK_MPI_OBSERVER_SCOPE_INTERVAL;
      if (i == 2) records[i].flags |= flags & PEAK_MPI_CAUSAL_INACTIVE_REQUEST;
      if (operation == PEAK_MPI_CAUSAL_MPROBE && i == 2)
        records[i].request_uid = 9000;
      if (operation == PEAK_MPI_CAUSAL_MRECV) {
        records[i].request_uid = 9000;
        records[i].progress_begin = probe;
      }
    }
    records[i].L_cut = records[i].A_cut = A + i * 10000;
    records[i].P_cut = P + i * 10000 / 3;
    assert(peak_mpi_causal_service_ingest_record(c->service, rank, &records[i]));
  }
  c->A[rank] += 30000;
  c->raw[rank].P = .02 + (double)(c->A[rank] - 100000000) / 3e9;
  return uid;
}
static void retained_request_observation(RecoveryGroups *c, uint64_t send,
                                          int retire) {
  PeakMpiObserverRecord records[4];
  uint64_t wait = recovery_group(c, 0, 0, 3, 0, 0, records);
  uint64_t A = c->A[0], P = 20000000 + (A - 100000000) / 3;
  for (unsigned i = 0; i < 4; ++i) {
    records[i].sequence += i >= 2;
    records[i].L_cut = records[i].A_cut = A + i * 10000;
    records[i].P_cut = P + i * 10000 / 3;
    if (i == 1 || i == 2) {
      records[i].flags &= ~PEAK_MPI_CAUSAL_COLLECTIVE;
      records[i].operation = retire ? PEAK_MPI_CAUSAL_WAIT : PEAK_MPI_CAUSAL_TEST;
    }
  }
  c->prefix[0][0]++;
  records[2].completion_count = !retire;
  assert(peak_mpi_causal_service_ingest_record(c->service, 0, &records[0]));
  assert(peak_mpi_causal_service_ingest_record(c->service, 0, &records[1]));
  PeakMpiObserverRecord done = records[1];
  done.sequence++;
  done.kind = retire ? PEAK_MPI_OBSERVER_EVENT_LOCAL_COST : PEAK_MPI_OBSERVER_EVENT_COMPLETE;
  done.phase = retire ? PEAK_MPI_CAUSAL_REQUEST_RETIRED : PEAK_MPI_CAUSAL_REQUEST_COMPLETE;
  done.op_uid = send;
  done.batch_uid = wait;
  done.operation = PEAK_MPI_CAUSAL_SEND;
  done.flags |= PEAK_MPI_CAUSAL_NONBLOCKING | (retire ? PEAK_MPI_CAUSAL_RECLAIMED :
      PEAK_MPI_CAUSAL_STATUS_ONLY | PEAK_MPI_CAUSAL_STATUS_OBSERVED |
      PEAK_MPI_CAUSAL_SUCCESS | PEAK_MPI_OBSERVER_PROGRESS_SEALED);
  assert(peak_mpi_causal_service_ingest_record(c->service, 0, &done));
  assert(peak_mpi_causal_service_ingest_record(c->service, 0, &records[2]));
  assert(peak_mpi_causal_service_ingest_record(c->service, 0, &records[3]));
  c->A[0] += 30000;
  c->raw[0].P = .02 + (double)(c->A[0] - 100000000) / 3e9;
}
/* A second thread keeps the group open across initiation and status-only
 * completion. The batch plan observes subrecords before initiation CALL_END. */
static void test_concurrent_status_only_retirement(void) {
  RecoveryGroups c = {0};
  c.service = make(); c.next_uid = 1000; c.A[0] = 100000000;
  const unsigned phases[] = {PEAK_MPI_CAUSAL_GROUP_BEGIN,
      PEAK_MPI_CAUSAL_CALL_BEGIN, PEAK_MPI_CAUSAL_CALL_BEGIN,
      PEAK_MPI_CAUSAL_CALL_END, PEAK_MPI_CAUSAL_CALL_BEGIN,
      PEAK_MPI_CAUSAL_REQUEST_COMPLETE, PEAK_MPI_CAUSAL_CALL_END,
      PEAK_MPI_CAUSAL_CALL_END, PEAK_MPI_CAUSAL_GROUP_END};
  const unsigned slots[] = {1,1,0,0,0,0,0,1,1};
  const uint64_t uids[] = {100,101,102,102,103,102,103,101,100};
  for (unsigned i = 0; i < 9; ++i) {
    PeakMpiObserverRecord r = {0};
    r.incarnation = 100; r.thread_uid = slots[i] + 1;
    r.thread_generation = 1; r.sequence = ++c.prefix[0][slots[i]];
    r.phase = phases[i]; r.op_uid = uids[i]; r.batch_uid = i == 5 ? 103 : 100;
    r.kind = PEAK_MPI_OBSERVER_EVENT_START;
    r.flags = PEAK_MPI_CAUSAL_APP_COST_KNOWN;
    r.L_cut = r.A_cut = c.A[0]; r.P_cut = 20000000;
    r.capability_uid = PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC;
    r.progress_scope = PEAK_MPI_OBSERVER_SCOPE_AUTONOMOUS;
    if (i == 2 || i == 3 || i == 5) {
      r.operation = PEAK_MPI_CAUSAL_SEND;
      r.flags |= PEAK_MPI_CAUSAL_NONBLOCKING;
      r.communicator_uid = 1; r.peer = 1; r.tag = 7;
    } else if (i == 4 || i == 6) r.operation = PEAK_MPI_CAUSAL_TEST;
    else if (i == 1 || i == 7) r.operation = PEAK_MPI_CAUSAL_WAIT;
    if (i == 3 || i == 5 || i == 6 || i == 7)
      r.flags |= PEAK_MPI_CAUSAL_SUCCESS | PEAK_MPI_OBSERVER_PROGRESS_SEALED;
    if (i == 4) r.input_ref = 102;
    if (i == 5) r.flags |= PEAK_MPI_CAUSAL_STATUS_ONLY | PEAK_MPI_CAUSAL_STATUS_OBSERVED;
    if (i == 6) r.completion_count = 1;
    if (i == 8) {
      r.flags |= PEAK_MPI_CAUSAL_CONCURRENT;
      r.completion_count = 3;
    }
    assert(peak_mpi_causal_service_ingest_record(c.service, 0, &r));
  }
  parse_batches(c.service, 100000);
  Operation *send = find_op(c.service, 0, 102);
  assert(!c.service->hard_fault && send && send->ended && send->completed &&
         !send->retired && operation_metadata_pinned(c.service, send));
  /* A later Wait reaps the same retained identity, without another completion. */
  c.previous_group[0] = 100; c.previous_call[0][0] = 103;
  retained_request_observation(&c, 102, 1);
  parse_batches(c.service, 100000);
  send = find_op(c.service, 0, 102);
  assert(!c.service->hard_fault && send && send->retired);
  peak_mpi_causal_service_destroy(c.service);
  puts("concurrent status-only completion retains request metadata until reap PASS");
}
static void test_idle_cut_dependency_index(void) {
  RecoveryGroups c = {0};
  c.service = make(); c.next_uid = 1000;
  c.peers[0] = c.peers[1] = 1;
  for (unsigned rank = 0; rank < 2; ++rank) {
    c.A[rank] = 100000000;
    PeakMpiCausalRaw *raw = &c.raw[rank];
    raw->incarnation = 100 + rank; raw->P = .02; raw->active = 1;
    raw->contract_profile = PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC;
    raw->slot_prefixes = c.prefix[rank]; raw->slot_count = 2;
    raw->known_peers = c.peers; raw->peer_count = 2;
    recovery_raw(&c, rank, 1);
  }
  uint64_t send = retained_call(&c, 0, 0, PEAK_MPI_CAUSAL_SEND,
                                 PEAK_MPI_CAUSAL_NONBLOCKING, 0);
  retained_request_observation(&c, send, 0);
  retained_request_observation(&c, send, 1);
  c.raw[0].idle_cut = 1;
  c.raw[0].idle_group_uid = c.previous_group[0];
  uint64_t index = 0, generation = 0;
  PeakMpiCostRef ref = {0};
  for (unsigned cycle = 0; cycle < 6; ++cycle) {
    recovery_raw(&c, 0, 3 + cycle * .01);
    peak_mpi_causal_service_tick(c.service, NULL, NULL, c.raw[0].elapsed,
                                  65536, 16384);
    SettledCut *tail = &c.service->cuts[c.service->ranks[0].cut_tail];
    PeakMpiCostStats stats;
    peak_mpi_cost_model_stats(c.service->model, &stats);
    if (!cycle) { index = tail->index; ref = tail->ref; generation = stats.generation; }
    double D;
    assert(!c.service->hard_fault && c.service->stats.records == 14 &&
           tail->index == index && c.service->ranks[0].root_index == index &&
           stats.generation == generation && stats.active_nodes == 7 &&
           stats.active_edges == 8 &&
           peak_mpi_cost_model_delay(c.service->model, ref, &D) == PEAK_MPI_COST_PENDING &&
           tail->sample.elapsed == c.raw[0].elapsed && tail->received == c.raw[0].elapsed);
  }
  retained_call(&c, 1, 0, PEAK_MPI_CAUSAL_RECV, 0, 0);
  recovery_drain(&c, 20, 0, 16384);
  double D;
  PeakMpiCostStats stats;
  peak_mpi_cost_model_stats(c.service->model, &stats);
  assert(!c.service->hard_fault && stats.generation > generation &&
         !peak_mpi_cost_model_delay(c.service->model, ref, &D) && fabs(D - .10008) < 1e-12 &&
         c.service->ranks[0].covered_ref.slot == ref.slot &&
         c.service->ranks[0].covered_ref.generation == ref.generation &&
         c.service->ranks[0].covered.L == c.raw[0].L &&
         c.service->ranks[0].covered.P == c.raw[0].P &&
         c.service->ranks[0].covered.A == c.raw[0].L &&
         c.service->free_cut_count == c.service->config.model.max_nodes);
  peak_mpi_causal_service_destroy(c.service);
  puts("reused idle cut keeps dependency index; pending roots avoid redundant solves and late peer still settles PASS");
}
static void retained_actual_cost(RecoveryGroups *c, unsigned rank, unsigned calls) {
  double A, P;
  assert(!peak_mpi_cost_model_accounted(c->service->model, rank, &A, &P));
  assert(fabs(A - sec(100000000 + calls * 30000 - 10000)) < 1e-12 &&
         fabs(P - sec(20000000 + (calls - 1) * 10000 + 20000 / 3)) < 1e-12 &&
         c->service->ranks[rank].A == sec(c->A[rank]) &&
         c->service->ranks[rank].P == c->raw[rank].P);
}
static void test_retained_send_inputs(void) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    RecoveryGroups c;
    recovery_groups_init(&c);
    unsigned flags = mode ? PEAK_MPI_CAUSAL_NONBLOCKING : 0;
    if (mode == 1) flags |= PEAK_MPI_CAUSAL_INACTIVE_REQUEST;
    uint64_t send = retained_call(&c, 0, 0, PEAK_MPI_CAUSAL_SEND, flags, 0);
    if (mode == 2) {
      retained_request_observation(&c, send, 0);
      local_observation(&c, 0, 4, PEAK_MPI_CAUSAL_TEST);
    }
    recovery_drain(&c, 32, 1, 100000);
    Operation *o = find_op(c.service, 0, send);
    assert(o && o->legacy_resolved && !o->send_matched && !operation_live(c.service, o));
    assert(mode != 2 || (!o->retired && c.service->ranks[0].anchor[0] != send));
    PeakMpiCostRef entry = o->entry, scope = progress_input(c.service, o);
    double entry_D, scope_D, D;
    assert(!peak_mpi_cost_model_delay(c.service->model, entry, &entry_D) &&
           !peak_mpi_cost_model_delay(c.service->model, scope, &scope_D));
    /* A newer local tail cannot replace either immutable send input. */
    c.A[0] += 70000000;
    c.raw[0].P = .02 + (double)(c.A[0] - 100000000) / 3e9;
    recovery_drain(&c, 8, 1, 100000);
    assert(!peak_mpi_cost_model_delay(c.service->model, entry, &D) && D == entry_D);
    assert(!peak_mpi_cost_model_delay(c.service->model, scope, &D) && D == scope_D);
    uint64_t receive = retained_call(&c, 1, 0, PEAK_MPI_CAUSAL_RECV, 0, 0);
    recovery_drain(&c, 32, 1, 100000);
    Operation *r = find_op(c.service, 1, receive);
    assert(r && r->matched && !r->legacy_resolved && !c.service->hard_fault &&
           !peak_mpi_cost_model_delay(c.service->model, output(r), &D));
    assert(fabs(D - (mode == 0 ? .20004 : mode == 1 ? .20003 : .20010)) < 1e-12);
    retained_actual_cost(&c, 0, mode == 2 ? 3 : 1);
    retained_actual_cost(&c, 1, 1);
    if (mode == 2) retained_request_observation(&c, send, 1);
    else local_observation(&c, 0, 4, PEAK_MPI_CAUSAL_WAIT);
    recovery_drain(&c, 32, 1, 100000);
    assert(!find_op(c.service, 0, send) && !c.service->hard_fault &&
           peak_mpi_cost_model_delay(c.service->model, entry, &D) == PEAK_MPI_COST_INVALID &&
           peak_mpi_cost_model_delay(c.service->model, scope, &D) == PEAK_MPI_COST_INVALID);
    peak_mpi_causal_service_destroy(c.service);
  }
  puts("retained send identities keep immutable inputs through late receive; raw costs and reclaim unchanged PASS");
}
static void test_reserved_message_metadata(void) {
  RecoveryGroups c;
  recovery_groups_init(&c);
  uint64_t send = retained_call(&c, 0, 0, PEAK_MPI_CAUSAL_SEND, 0, 0);
  recovery_drain(&c, 32, 1, 100000);
  uint64_t probe = retained_call(&c, 1, 0, PEAK_MPI_CAUSAL_MPROBE, 0, 0);
  recovery_drain(&c, 32, 1, 100000);
  Operation *o = find_op(c.service, 0, send), *p = find_op(c.service, 1, probe);
  assert(o && p && p->matched && !p->legacy_resolved);
  size_t send_slot = (size_t)(o - c.service->operations);
  assert(o->reserved_by == (size_t)(p - c.service->operations) && p->peer_op == send_slot);
  PeakMpiCostRef entry = o->entry, scope = progress_input(c.service, o);
  local_observation(&c, 0, 4, PEAK_MPI_CAUSAL_WAIT);
  recovery_drain(&c, 32, 1, 100000);
  double D;
  assert(find_op(c.service, 0, send) && !c.service->hard_fault &&
         !peak_mpi_cost_model_delay(c.service->model, entry, &D) &&
         !peak_mpi_cost_model_delay(c.service->model, scope, &D));
  uint64_t receive = retained_call(&c, 1, 1, PEAK_MPI_CAUSAL_MRECV, 0, probe);
  recovery_drain(&c, 32, 1, 100000);
  o = find_op(c.service, 0, send); p = find_op(c.service, 1, probe);
  Operation *r = find_op(c.service, 1, receive);
  assert(o && p && r && r->matched && !r->legacy_resolved && r->peer_op == send_slot &&
         !c.service->hard_fault && !peak_mpi_cost_model_delay(c.service->model, output(r), &D));
  assert(fabs(D - .20007) < 1e-12 && !branch_live_at_cut(c.service, o) &&
         !branch_live_at_cut(c.service, p) && c.service->ranks[1].anchor[0] == probe &&
         fabs(c.service->pub_F - fmax(c.service->ranks[0].covered.D,
                                     c.service->ranks[1].covered.D)) < 1e-12);
  retained_actual_cost(&c, 0, 2); retained_actual_cost(&c, 1, 2);
  assert(peak_mpi_cost_model_delay(c.service->model, entry, &D) == PEAK_MPI_COST_INVALID &&
         peak_mpi_cost_model_delay(c.service->model, scope, &D) == PEAK_MPI_COST_INVALID);
  uint64_t other = local_observation(&c, 1, 5, PEAK_MPI_CAUSAL_TEST);
  recovery_drain(&c, 32, 1, 100000);
  r = find_op(c.service, 0, other); o = find_op(c.service, 0, send);
  p = find_op(c.service, 1, probe);
  assert(r && o && p && (size_t)(r - c.service->operations) != send_slot &&
         p->peer_op == send_slot && !branch_live_at_cut(c.service, p));
  retained_call(&c, 1, 0, PEAK_MPI_CAUSAL_WAIT, 0, 0);
  recovery_drain(&c, 32, 1, 100000);
  assert(!find_op(c.service, 0, send) && !find_op(c.service, 1, probe) && !c.service->hard_fault);
  peak_mpi_causal_service_destroy(c.service);
  puts("Mprobe reservation pins identity until Probe metadata retires, without retaining consumed graph pressure PASS");
}

static void test_observation_metadata_retirement(void) {
  RecoveryGroups c;
  recovery_groups_init(&c);
  PeakMpiObserverRecord records[4];
  uint64_t receive = recovery_group(&c, 0, 0, 2, 1, 0, records);
  for (unsigned i = 1; i <= 2; ++i) {
    records[i].flags &= ~PEAK_MPI_CAUSAL_COLLECTIVE;
    records[i].flags |= PEAK_MPI_OBSERVER_PROGRESS_ESTIMATED;
    records[i].operation = PEAK_MPI_CAUSAL_RECV;
    records[i].peer = records[i].actual_source = 1;
    records[i].tag = records[i].actual_tag = 7;
    records[i].capability_uid = PEAK_MPI_PROGRESS_INTEL_2021_11_OPX_ASYNC0_APPROX;
  }
  for (unsigned i = 0; i < 4; ++i)
    assert(peak_mpi_causal_service_ingest_record(c.service, 0, &records[i]));
  recovery_drain(&c, 4, 0, 100000);
  uint64_t anchors[2] = {0}, first = 0;
  for (unsigned k = 0; k < 100; ++k) {
    unsigned slot = k % 2;
    if (anchors[slot])
      assert(find_op(c.service, 0, anchors[slot]));
    anchors[slot] = local_observation(&c, slot, k + 3,
        k % 3 ? PEAK_MPI_CAUSAL_WAIT : PEAK_MPI_CAUSAL_TEST);
    if (!k)
      first = anchors[slot];
    recovery_drain(&c, 4, 0, 100000);
    assert(!c.service->hard_fault && c.service->operation_count <= 3);
  }
  assert(find_op(c.service, 0, receive) && !find_op(c.service, 0, first) &&
         !c.service->ranks[0].covered_index);
  /* Both slots' next calls must still find their current input_ref anchors. */
  for (unsigned slot = 0; slot < 2; ++slot) {
    assert(find_op(c.service, 0, anchors[slot]));
    anchors[slot] = local_observation(&c, slot, 103 + slot, PEAK_MPI_CAUSAL_WAIT);
  }
  recovery_drain(&c, 32, 1, 100000);
  double A, P, D;
  assert(!c.service->hard_fault && c.service->ranks[0].covered_index == 103 &&
         peak_mpi_cost_model_accounted(c.service->model, 0, &A, &P) == PEAK_MPI_COST_OK &&
         peak_mpi_cost_model_delay(c.service->model,
             c.service->ranks[0].covered_ref, &D) == PEAK_MPI_COST_OK);
  assert(fabs(A - sec(c.A[0] - 10000)) < 1e-12 &&
         fabs(P - sec(20000000 + (c.A[0] - 100000000 - 30000) / 3 + 20000 / 3)) < 1e-12 &&
         fabs(D - (sec(c.A[0]) + A)) < 1e-12 &&
         c.service->ranks[0].A == sec(c.A[0]) &&
         c.service->ranks[0].P == c.raw[0].P);
  for (unsigned slot = 0; slot < 2; ++slot)
    assert(find_op(c.service, 0, anchors[slot]));
  peak_mpi_causal_service_destroy(c.service);
  puts("unsettled Wait/Test graph costs survive metadata retirement; both "
       "thread order anchors and covered accounting preserved PASS");
}

static void test_recovery_collective_boundary(void) {
  for (unsigned asynchronous = 0; asynchronous < 2; ++asynchronous) {
    RecoveryGroups c;
    recovery_groups_init(&c);
    for (unsigned ordinal = 2; ordinal < 102; ++ordinal) {
      for (unsigned rank = 0; rank < 2; ++rank)
        if (!asynchronous || rank == 0 || ordinal != 2)
          recovery_group(&c, rank, 0, ordinal, 1, 0, NULL);
      recovery_drain(&c, 32, 1, 100000);
      assert(!c.service->hard_fault && c.service->operation_count <= 2);
    }
    assert(c.service->ranks[0].covered_index == 100 &&
           c.service->ranks[1].covered_index == 100 - asynchronous);
    peak_mpi_causal_service_destroy(c.service);
  }
  /* Later input from another slot cannot jump its missing predecessor group. */
  RecoveryGroups c;
  recovery_groups_init(&c);
  uint64_t first = recovery_group(&c, 0, 0, 2, 1, 0, NULL);
  recovery_group(&c, 0, 1, 3, 1, 0, NULL);
  PeakMpiObserverRecord delayed[4];
  recovery_group(&c, 1, 0, 2, 1, 0, delayed);
  recovery_group(&c, 1, 1, 3, 1, 0, NULL);
  recovery_drain(&c, 32, 1, 100000);
  Operation *o = find_op(c.service, 0, first);
  assert(o && !o->legacy_resolved && !o->collective_built);
  for (unsigned i = 0; i < 4; ++i)
    assert(peak_mpi_causal_service_ingest_record(c.service, 1, &delayed[i]));
  recovery_drain(&c, 32, 1, 100000);
  o = find_op(c.service, 0, first);
  assert(o && o->collective_built && !o->legacy_resolved &&
         c.service->ranks[0].covered_index == 2 &&
         c.service->ranks[1].covered_index == 2 && !c.service->hard_fault);
  peak_mpi_causal_service_destroy(c.service);
  /* Same members are not sufficient: a different context supplies no proof. */
  recovery_groups_init(&c);
  for (unsigned rank = 0; rank < 2; ++rank) {
    PeakMpiObserverObject object = {0};
    object.kind = PEAK_MPI_CAUSAL_OBJECT_COMM;
    object.part_count = 1;
    PeakMpiCausalCommPart part = {0};
    part.local_uid = 2;
    part.parent_uid = part.creation_ordinal = 1;
    part.creation_kind = PEAK_MPI_CAUSAL_COMM_DUPLICATED;
    part.world_size = part.local_size = part.member_count = 2;
    part.members[1] = 1;
    memcpy(object.payload, &part, sizeof part);
    assert(peak_mpi_causal_service_ingest_object(c.service, rank, 100 + rank,
                                                &object));
  }
  first = recovery_group(&c, 0, 0, 2, 1, 0, NULL);
  recovery_group(&c, 0, 1, 3, 2, 0, NULL);
  recovery_group(&c, 1, 1, 3, 2, 0, NULL);
  recovery_drain(&c, 32, 1, 100000);
  o = find_op(c.service, 0, first);
  assert(o && !o->legacy_resolved && !o->collective_built &&
         !c.service->contexts[o->context - 1].built_collective_ordinal &&
         c.service->contexts[context_for(c.service, 0, 2) - 1].built_collective_ordinal == 3 &&
         !c.service->hard_fault);
  peak_mpi_causal_service_destroy(c.service);
  for (unsigned excluded = 0; excluded < 3; ++excluded) {
    recovery_groups_init(&c);
    if (!excluded)
      c.service->approximate = 0;
    unsigned flags = excluded == 1 ? PEAK_MPI_CAUSAL_NONBLOCKING
        : excluded == 2 ? PEAK_MPI_CAUSAL_INTERCOMM : 0;
    first = recovery_group(&c, 0, 0, 2, 1, flags, NULL);
    recovery_group(&c, 0, 1, 3, 1, 0, NULL);
    recovery_group(&c, 1, 1, 3, 1, 0, NULL);
    recovery_drain(&c, 32, 1, 100000);
    o = find_op(c.service, 0, first);
    assert(o && !o->legacy_resolved && !o->collective_built &&
           !c.service->hard_fault);
    if (excluded == 1)
      assert(!o->completed && !o->result_defined);
    peak_mpi_causal_service_destroy(c.service);
  }
  /* Error returns were already matched/sealed by the existing completion
   * path; a later complete group must not add dependencies to that output. */
  recovery_groups_init(&c);
  first = recovery_group(&c, 0, 0, 2, 1, 0, delayed);
  delayed[2].mpi_error = 1;
  for (unsigned i = 0; i < 4; ++i)
    assert(peak_mpi_causal_service_ingest_record(c.service, 0, &delayed[i]));
  recovery_group(&c, 0, 1, 3, 1, 0, NULL);
  recovery_group(&c, 1, 1, 3, 1, 0, NULL);
  recovery_drain(&c, 32, 1, 100000);
  o = find_op(c.service, 0, first);
  assert(o && o->matched && o->sealed && !o->legacy_resolved &&
         !c.service->hard_fault);
  peak_mpi_causal_service_destroy(c.service);
  /* Parsed immutable costs can lead RAW; the estimate must not use old RAW
   * as if it covered those costs, nor overwrite either raw accounting lane. */
  recovery_groups_init(&c);
  c.A[0] = 200000000;
  first = recovery_group(&c, 0, 0, 2, 1, 0, NULL);
  recovery_group(&c, 0, 1, 3, 1, 0, NULL);
  c.A[1] = 300000000;
  recovery_group(&c, 1, 1, 3, 1, 0, NULL);
  recovery_drain(&c, 3000, 0, 16);
  o = find_op(c.service, 0, first);
  assert(o && o->legacy_resolved && o->matched && !o->collective_built);
  double D;
  assert(peak_mpi_cost_model_delay(c.service->model, o->exit, &D) ==
             PEAK_MPI_COST_OK && fabs(D - .5) < 1e-12);
  for (unsigned rank = 0; rank < 2; ++rank) {
    double A, P;
    assert(peak_mpi_cost_model_accounted(c.service->model, rank, &A, &P) ==
           PEAK_MPI_COST_OK);
    assert(fabs(A - (rank ? .3 : .2)) < 1e-12 && fabs(P - .02) < 1e-12);
    assert(c.service->ranks[rank].A == .1 && c.service->ranks[rank].P == .02);
  }
  /* The next complete group uses MAX for parallel increments, rather than
   * extending the missing-boundary estimate into permanent sum accounting. */
  c.A[0] = 300000000;
  first = recovery_group(&c, 0, 1, 4, 1, 0, NULL);
  c.A[1] = 400000000;
  recovery_group(&c, 1, 1, 4, 1, 0, NULL);
  recovery_drain(&c, 32, 1, 100000);
  o = find_op(c.service, 0, first);
  assert(o && o->collective_built && !o->legacy_resolved &&
         peak_mpi_cost_model_delay(c.service->model, o->exit, &D) ==
             PEAK_MPI_COST_OK && fabs(D - .6) < 1e-12 &&
         !c.service->hard_fault);
  peak_mpi_causal_service_destroy(c.service);
  puts("asynchronous recovery collective gap: bounded retirement, delayed/cross-"
       "slot/context/nonapprox/NBC guards and immutable costs PASS");
}

static void test_recovered_prefix_reply(void) {
  for (unsigned nonblocking = 0; nonblocking < 2; ++nonblocking) {
    RecoveryGroups c;
    recovery_groups_init(&c);
    PeakMpiObserverRecord records[4];
    uint64_t uid = recovery_group(&c, 0, 0, 2, 1,
        nonblocking ? PEAK_MPI_CAUSAL_NONBLOCKING : 0, records);
    for (unsigned i = 0; i < 4; ++i) {
      if (nonblocking)
        records[i].flags &= ~PEAK_MPI_CAUSAL_ORDERED_CALLER;
      if (i == 1 || i == 2) {
        records[i].operation = PEAK_MPI_CAUSAL_SEND;
        records[i].flags &= ~PEAK_MPI_CAUSAL_COLLECTIVE;
        records[i].peer = 1;
        records[i].tag = 7;
        records[i].collective_ordinal = 0;
      }
      assert(peak_mpi_causal_service_ingest_record(c.service, 0, &records[i]));
    }
    uint64_t output_uid = uid;
    if (nonblocking) {
      recovery_drain(&c, 8, 1, 100000);
      Operation *open = find_op(c.service, 0, uid);
      assert(open && !open->completed && !open->result_defined &&
             !open->legacy_resolved);
      output_uid = recovery_group(&c, 0, 1, 0, 1, 0, records);
      for (unsigned i = 0; i < 4; ++i) {
        records[i].flags &= ~(PEAK_MPI_CAUSAL_COLLECTIVE |
                              PEAK_MPI_CAUSAL_ORDERED_CALLER);
        if (i == 1 || i == 2)
          records[i].operation = PEAK_MPI_CAUSAL_WAIT;
      }
      PeakMpiObserverRecord done = records[1];
      done.sequence++;
      done.phase = PEAK_MPI_CAUSAL_REQUEST_COMPLETE;
      done.kind = PEAK_MPI_OBSERVER_EVENT_COMPLETE;
      done.operation = PEAK_MPI_CAUSAL_SEND;
      done.op_uid = uid;
      done.batch_uid = output_uid;
      done.flags |= PEAK_MPI_CAUSAL_NONBLOCKING | PEAK_MPI_CAUSAL_RECLAIMED |
                    PEAK_MPI_CAUSAL_STATUS_OBSERVED | PEAK_MPI_CAUSAL_SUCCESS |
                    PEAK_MPI_OBSERVER_PROGRESS_SEALED;
      records[2].sequence++;
      records[2].completion_count = 1;
      records[3].sequence++;
      c.prefix[0][1]++;
      assert(peak_mpi_causal_service_ingest_record(c.service, 0, &records[0]));
      assert(peak_mpi_causal_service_ingest_record(c.service, 0, &records[1]));
      assert(peak_mpi_causal_service_ingest_record(c.service, 0, &done));
      assert(peak_mpi_causal_service_ingest_record(c.service, 0, &records[2]));
      assert(peak_mpi_causal_service_ingest_record(c.service, 0, &records[3]));
    }
    recovery_drain(&c, 32, 0, 100000);
    Operation *o = find_op(c.service, 0, uid);
    assert(o && o->analysis_waiting && !o->legacy_wait);
    for (unsigned rank = 0; rank < 2; ++rank)
      recovery_raw(&c, rank, 10.1);
    recovery_drain(&c, 32, 0, 100000);
    assert(o->legacy_wait && o->analysis_waiting && !o->legacy_resolved);
    uint64_t required[2] = {o->self_nonce, o->match_nonce};
    uint64_t finite_prefix[2][2];
    memcpy(finite_prefix, c.prefix, sizeof finite_prefix);
    /* Old READY metadata with the old echo, or even a high echo from an
     * earlier recovery epoch, cannot acknowledge this new nonce. */
    for (unsigned rank = 0; rank < 2; ++rank) {
      PeakMpiCausalRaw stale = c.raw[rank];
      stale.generation = ++c.raw[rank].generation;
      stale.recovery.state = PEAK_MPI_RECOVERY_READY;
      assert(peak_mpi_causal_service_ingest_raw(c.service, rank, &stale, 10.1));
      stale.generation = ++c.raw[rank].generation;
      stale.recovery.epoch--;
      stale.watermark_nonce = required[rank];
      assert(peak_mpi_causal_service_ingest_raw(c.service, rank, &stale, 10.1));
    }
    recovery_drain(&c, 8, 0, 100000);
    for (unsigned rank = 0; rank < 2; ++rank)
      assert(c.service->ranks[rank].verified_nonce < required[rank]);
    assert(!o->legacy_resolved);
    /* The retained early-reply guards also apply after recovery. */
    for (unsigned guarded = 0; guarded < 3; ++guarded) {
      c.service->ranks[0].coarse = guarded == 0;
      c.service->recovery_reset = guarded == 1;
      c.service->recovery_needed = guarded == 2;
      recovery_raw(&c, 0, 10.2);
      assert(!c.service->ranks[0].verify_active);
      c.service->ranks[0].coarse = c.service->recovery_reset =
          c.service->recovery_needed = 0;
    }
    for (unsigned rank = 0; rank < 2; ++rank)
      recovery_raw(&c, rank, 10.2);
    /* New publications are legitimately beyond the older finite reply but
     * remain in transit. They must not replace or falsely complete it. */
    PeakMpiObserverRecord pending[2][4];
    for (unsigned rank = 0; rank < 2; ++rank) {
      recovery_group(&c, rank, 0, 3, 1, 0, pending[rank]);
      recovery_raw(&c, rank, 10.3);
    }
    recovery_drain(&c, 32, 0, 100000);
    o = find_op(c.service, 0, uid);
    assert(o && o->legacy_resolved && !c.service->hard_fault &&
           c.service->ranks[0].covered_index != 0);
    for (unsigned rank = 0; rank < 2; ++rank) {
      Rank *r = &c.service->ranks[rank];
      assert(r->verified_nonce >= required[rank] &&
             r->sequence[0] < c.prefix[rank][0] && r->A == .1 && r->P == .02);
      assert(!memcmp(r->verified_prefix, finite_prefix[rank],
                     sizeof finite_prefix[rank]));
    }
    double D;
    Operation *output = find_op(c.service, 0, output_uid);
    assert(output && peak_mpi_cost_model_delay(c.service->model, output->exit,
                &D) == PEAK_MPI_COST_OK && fabs(D - .2) < 1e-12);
    peak_mpi_causal_service_destroy(c.service);
  }
  puts("recovered finite nonce replies survive newer in-transit RAW tails; "
       "old epochs/echoes and open requests cannot complete them PASS");
}

static void recovery_groups_again(RecoveryGroups *c, uint64_t ordinal) {
  double now = fmax(c->service->now,
      fmax(c->raw[0].elapsed, c->raw[1].elapsed)) + .001;
  c->raw[0].lost_events++;
  c->raw[0].coarse_reason = PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW;
  c->raw[0].recovery.fault_mask =
      UINT64_C(1) << PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW;
  for (unsigned rank = 0; rank < 2; ++rank) {
    PeakMpiCausalRecovery *r = &c->raw[rank].recovery;
    r->state = PEAK_MPI_RECOVERY_NONE;
    r->comm = 1;
    r->ordinal = ordinal;
    r->operation = PEAK_MPI_CAUSAL_BARRIER;
    r->loss = c->raw[rank].lost_events;
    recovery_raw(c, rank, now);
  }
  peak_mpi_causal_service_tick(c->service, NULL, NULL, now, 65536, 100000);
  assert(c->service->recovery.state == PEAK_MPI_RECOVERY_QUERY);
  for (unsigned rank = 0; rank < 2; ++rank) {
    PeakMpiCausalRecovery *r = &c->raw[rank].recovery;
    r->state = PEAK_MPI_RECOVERY_READY;
    r->epoch = c->service->recovery.epoch;
    r->uid_floor = c->next_uid;
    r->L = (double)c->A[rank] / 1e9;
    r->P = c->raw[rank].P;
    r->elapsed = now + .001;
    recovery_raw(c, rank, now + .001);
  }
  for (unsigned turn = 0; turn < 100 &&
      c->service->recovery.state != PEAK_MPI_RECOVERY_ACK; ++turn)
    peak_mpi_causal_service_tick(c->service, NULL, NULL,
        now + .002 + turn * .001, 65536, 100000);
  assert(c->service->recovery.state == PEAK_MPI_RECOVERY_ACK &&
         !c->service->hard_fault);
  for (unsigned rank = 0; rank < 2; ++rank) {
    c->raw[rank].recovery.state = PEAK_MPI_RECOVERY_ACK;
    c->raw[rank].recovery.fault_mask = 0;
    c->raw[rank].coarse_reason = 0;
  }
  /* The frontend resets only capture-group/call anchors, not MPI ordinals. */
  memset(c->previous_group, 0, sizeof c->previous_group);
  memset(c->previous_call, 0, sizeof c->previous_call);
}
static void test_collective_proof_reuse(void) {
  RecoveryGroups c;
  recovery_groups_init(&c);
  for (unsigned k = 0; k < 8; ++k)
    recovery_group(&c, 0, 0, k + 2, 1, 0, NULL);
  for (unsigned k = 0; k < 40; ++k) {
    PeakMpiObserverRecord records[4];
    recovery_group(&c, 0, 0, k + 10, 1, 0, records);
    for (unsigned i = 0; i < 4; ++i) {
      if (i == 1 || i == 2) {
        records[i].flags &= ~PEAK_MPI_CAUSAL_COLLECTIVE;
        records[i].operation = PEAK_MPI_CAUSAL_SEND;
        records[i].peer = 1;
        records[i].tag = k;
        records[i].collective_ordinal = 0;
      }
      assert(peak_mpi_causal_service_ingest_record(c.service, 0, &records[i]));
    }
    recovery_drain(&c, 4, 1, 100000);
  }
  recovery_drain(&c, 32, 1, 100000);
  assert(!c.service->contexts[0].built_collective_ordinal &&
         c.service->ranks[0].covered_index == 0 && !c.service->hard_fault);
  size_t legacy = 0;
  for (size_t i = 0; i < c.service->operation_count; ++i)
    legacy += c.service->operations[c.service->active[i]].legacy_resolved != 0;
  assert(legacy); /* Ordinary approximate paid anchors are not group proofs. */
  recovery_drain(&c, 20, 1, 100000);
  assert(!c.service->contexts[0].built_collective_ordinal &&
         c.service->ranks[0].covered_index == 0 && !c.service->hard_fault);
  recovery_group(&c, 0, 1, 10, 1, 0, NULL);
  recovery_group(&c, 1, 1, 10, 1, 0, NULL);
  recovery_drain(&c, 64, 1, 100000);
  assert(c.service->contexts[0].built_collective_ordinal == 10 &&
         c.service->operation_count <= 3 && !c.service->hard_fault);
  double D;
  assert(peak_mpi_cost_model_delay(c.service->model,
      c.service->ranks[0].covered_ref, &D) == PEAK_MPI_COST_OK &&
      fabs(D - .2) < 1e-12 && c.service->ranks[0].A == .1 &&
      c.service->ranks[0].P == .02);
  peak_mpi_causal_service_destroy(c.service);
  recovery_groups_init(&c);
  uint64_t old = recovery_group(&c, 0, 0, 2, 1, 0, NULL);
  recovery_group(&c, 1, 0, 2, 1, 0, NULL);
  recovery_drain(&c, 32, 1, 100000);
  assert(c.service->contexts[0].built_collective_ordinal == 2);
  recovery_group(&c, 0, 0, 3, 1, 0, NULL);
  recovery_group(&c, 1, 0, 3, 1, 0, NULL);
  recovery_drain(&c, 32, 1, 100000);
  assert(!find_op(c.service, 0, old) &&
         c.service->contexts[0].built_collective_ordinal == 3);
  recovery_groups_again(&c, 4);
  assert(c.service->contexts[0].built_collective_ordinal == 3);
  uint64_t gap = recovery_group(&c, 0, 0, 5, 1, 0, NULL);
  recovery_drain(&c, 32, 1, 100000);
  Operation *o = find_op(c.service, 0, gap);
  assert(o && !o->legacy_resolved &&
         c.service->contexts[0].built_collective_ordinal == 3);
  uint64_t nbc = recovery_group(&c, 0, 1, 6, 1,
      PEAK_MPI_CAUSAL_NONBLOCKING, NULL);
  recovery_group(&c, 1, 1, 6, 1, PEAK_MPI_CAUSAL_NONBLOCKING, NULL);
  recovery_drain(&c, 64, 1, 100000);
  o = find_op(c.service, 0, gap);
  Operation *witness = find_op(c.service, 0, nbc);
  assert(o && o->legacy_resolved && !o->collective_built && witness &&
         witness->collective_built && !witness->completed &&
         !witness->result_defined &&
         c.service->contexts[0].built_collective_ordinal == 6 &&
         !c.service->hard_fault);
  peak_mpi_causal_service_destroy(c.service);
  puts("collective group proofs reused across unrelated sends, operation GC "
       "and recovery; old/legacy proofs cannot close newer gaps PASS");
}

/* A recovery may take over a quota-yielded in-place GC compaction. */
static void test_recovery_interrupted_operation_gc(void) {
  for (unsigned duplicate = 0; duplicate < 2; ++duplicate) {
    RecoveryGroups c;
    recovery_groups_init(&c);
    local_observation(&c, 0, 2, PEAK_MPI_CAUSAL_WAIT);
    local_observation(&c, duplicate ? 1 : 0, 3, PEAK_MPI_CAUSAL_WAIT);
    local_observation(&c, 0, 4, PEAK_MPI_CAUSAL_WAIT);
    parse_batches(c.service, 10000);
    for (unsigned rank = 0; rank < 2; ++rank)
      recovery_raw(&c, rank, 3);
    assert(c.service->operation_count == 3);
    size_t first_slot = c.service->active[0];
    for (unsigned turn = 0; turn < 10000; ++turn) {
      checkpoint(c.service, 1);
      if (c.service->gc_stage == 4 &&
          c.service->gc_cursor == (duplicate ? 2 : 1))
        break;
      assert(turn + 1 < 10000);
    }
    assert(!c.service->operations[first_slot].used);
    if (duplicate)
      assert(c.service->active[0] == c.service->active[1] &&
             c.service->operations[c.service->active[0]].used);
    recovery_groups_again(&c, 5);
    assert(c.service->free_count == c.service->config.max_operations &&
           !c.service->operation_count);
    unsigned char seen[64] = {0};
    assert(c.service->config.max_operations == sizeof seen);
    for (size_t i = 0; i < c.service->free_count; ++i) {
      size_t slot = c.service->free_ops[i];
      assert(slot < sizeof seen && !seen[slot]);
      seen[slot] = 1;
    }
    for (unsigned rank = 0; rank < 2; ++rank) {
      PeakMpiObserverRecord records[4];
      recovery_group(&c, rank, 0, 6, 1, 0, records);
      for (unsigned i = 0; i < 4; ++i) {
        records[i].P_cut = (uint64_t)(c.raw[rank].P * 1e9 + .5);
        assert(peak_mpi_causal_service_ingest_record(c.service, rank, &records[i]));
      }
    }
    recovery_drain(&c, 32, 1, 100000);
    assert(!c.service->hard_fault &&
           c.service->recovery.state == PEAK_MPI_RECOVERY_ACK);
    peak_mpi_causal_service_destroy(c.service);
  }
  puts("recovery safely takes over yielded operation GC with freed and duplicate retained slots PASS");
}

static void test_immediate_ordered_send(void) {
  for (unsigned immediate = 0; immediate < 2; ++immediate) {
    RecoveryGroups c = {0};
    c.service = make();
    c.next_uid = 1000;
    c.peers[0] = c.peers[1] = 1;
    for (unsigned rank = 0; rank < 2; ++rank) {
      c.A[rank] = 100000000 * (rank + 1);
      c.raw[rank].incarnation = 100 + rank;
      c.raw[rank].P = .02;
      c.raw[rank].active = 1;
      c.raw[rank].contract_profile = PEAK_MPI_PROGRESS_INTEL_2021_11_OPX_ASYNC0_APPROX;
      c.raw[rank].slot_prefixes = c.prefix[rank];
      c.raw[rank].slot_count = 2;
      c.raw[rank].known_peers = c.peers;
      c.raw[rank].peer_count = 2;
      recovery_raw(&c, rank, 1);
    }
    uint64_t uids[2];
    for (unsigned rank = 0; rank < 2; ++rank) {
      PeakMpiObserverRecord records[4];
      uids[rank] = recovery_group(&c, rank, 0, 0, 1,
          rank ? 0 : PEAK_MPI_CAUSAL_NONBLOCKING, records);
      for (unsigned i = 0; i < 4; ++i) {
        records[i].capability_uid = PEAK_MPI_PROGRESS_INTEL_2021_11_OPX_ASYNC0_APPROX;
        records[i].progress_scope = PEAK_MPI_OBSERVER_SCOPE_INTERVAL;
        records[i].flags |= PEAK_MPI_OBSERVER_PROGRESS_ESTIMATED;
        records[i].flags &= ~PEAK_MPI_OBSERVER_PROGRESS_SEALED;
        if (i == 1 || i == 2) {
          records[i].flags &= ~PEAK_MPI_CAUSAL_COLLECTIVE;
          records[i].operation = rank ? PEAK_MPI_CAUSAL_RECV : PEAK_MPI_CAUSAL_SEND;
          records[i].peer = records[i].actual_source = 1 - rank;
          records[i].tag = records[i].actual_tag = 13;
        }
        if (!rank && immediate && i == 2)
          records[i].flags |= PEAK_MPI_CAUSAL_INACTIVE_REQUEST;
        assert(peak_mpi_causal_service_ingest_record(c.service, rank, &records[i]));
      }
    }
    /* No new RAW/nonce replies: an immediate request needs only its return
     * sequence as completion evidence, without closing its progress interval. */
    recovery_drain(&c, 8, 0, 16384);
    Operation *send = find_op(c.service, 0, uids[0]);
    Operation *recv = find_op(c.service, 1, uids[1]);
    assert(send && recv && !c.service->hard_fault && !c.service->approximate);
    assert(!send->ordered_completion_sequence &&
           !c.service->ranks[0].verified_nonce && !c.service->ranks[1].verified_nonce);
    if (immediate) {
      double A, P, D;
      assert(send->immediate && send->completed && send->retired &&
             send->send_matched && recv->matched && send->sealed && recv->sealed);
      assert(!c.service->ranks[0].requested_nonce && !c.service->ranks[1].requested_nonce);
      /* Preserve the standard-send reverse dependency: the later peer's
       * entry affects the send output even though initiation completed. */
      assert(peak_mpi_cost_model_delay(c.service->model, send->exit, &D) ==
             PEAK_MPI_COST_OK && fabs(D - .2) < 1e-12);
      assert(peak_mpi_cost_model_delay(c.service->model, recv->exit, &D) ==
             PEAK_MPI_COST_OK && fabs(D - .2) < 1e-12);
      for (unsigned rank = 0; rank < 2; ++rank)
        assert(peak_mpi_cost_model_accounted(c.service->model, rank, &A, &P) ==
               PEAK_MPI_COST_OK && fabs(A - .1 * (rank + 1)) < 1e-12 &&
               fabs(P - .02) < 1e-12);
      for (unsigned rank = 0; rank < 2; ++rank)
        recovery_group(&c, rank, 0, 1, 1, 0, NULL);
      recovery_drain(&c, 8, 0, 16384);
      assert(!find_op(c.service, 0, uids[0]) &&
             !find_op(c.service, 1, uids[1]) && c.service->operation_count == 2 &&
             !c.service->hard_fault);
    } else {
      assert(!send->immediate && !send->completed && !recv->matched &&
             c.service->ranks[0].requested_nonce && c.service->ranks[1].requested_nonce);
    }
    peak_mpi_causal_service_destroy(c.service);
  }
  puts("immediate ordered send uses return completion; open request still waits; "
       "reverse dependency, lifetime and original costs preserved PASS");
}

static void test_model_cpu_continuation(void) {
  const unsigned budgets[] = {16, 64, 66, 67, 68, 93, 128, 256};
  int saw_model_only = 0, saw_root_only = 0;
  int saw_publish_any = 0, saw_gc_any = 0;
  unsigned saw_phase_boundary = 0;
  for (size_t b = 0; b < sizeof budgets / sizeof budgets[0]; ++b) {
    RecoveryGroups c;
    recovery_groups_init(&c);
    for (unsigned k = 0; k < 6; ++k)
      for (unsigned rank = 0; rank < 2; ++rank)
        recovery_group(&c, rank, 0, k + 2, 1, 0, NULL);
    int witnessed = 0, saw_publish = 0, saw_gc = 0, continuing = 0;
    unsigned turns = 0;
    for (; turns < 2000; ++turns) {
      size_t bytes = 65536, work = budgets[b];
      peak_mpi_causal_service_tick_budget(c.service, NULL, NULL, 2, &bytes, &work);
      assert(work <= budgets[b] && !c.service->hard_fault);
      assert(!continuing || work < budgets[b]);
      if (!peak_mpi_causal_batches_runnable(c.service->batches) &&
          (c.service->model_running || c.service->root_plan_stage ||
           c.service->pub_stage || c.service->gc_stage ||
           c.service->engine_phase != ENGINE_PARSE ||
           c.service->graph_pending)) {
        if (!c.service->model_running && !c.service->root_plan_stage &&
            !c.service->pub_stage && !c.service->gc_stage) {
          saw_phase_boundary |= 1u << c.service->engine_phase;
        }
        saw_model_only |= c.service->model_running && !c.service->root_plan_stage;
        saw_root_only |= !c.service->model_running && c.service->root_plan_stage;
        saw_publish |= c.service->pub_stage != 0;
        saw_gc |= c.service->gc_stage != 0;
        assert(peak_mpi_causal_service_runnable(c.service));
        witnessed = 1;
      }
      continuing = peak_mpi_causal_service_runnable(c.service);
      if (!continuing && c.service->parsed_records == 48)
        break;
    }
    for (unsigned rank = 0; rank < 2; ++rank) {
      const PeakMpiPolicySample *covered = &c.service->ranks[rank].covered;
      assert(covered->event_cut == c.prefix[rank][0]);
      assert(fabs(covered->A - .1) < 1e-12 &&
             fabs(covered->P - .02) < 1e-12 &&
             fabs(covered->D - .2) < 1e-12);
    }
    saw_publish_any |= saw_publish;
    saw_gc_any |= saw_gc;
    if (budgets[b] == 16) {
      PeakMpiPolicyFeedback feedback;
      assert(peak_mpi_causal_service_feedback(c.service, &feedback));
      assert(feedback.event_cut == c.prefix[0][0]);
    }
    assert(turns < 2000);
    if (budgets[b] == 16 || budgets[b] == 64 || budgets[b] == 66 ||
        budgets[b] == 256)
      assert(witnessed);
    peak_mpi_causal_service_destroy(c.service);
  }
  assert(saw_model_only && saw_root_only && saw_publish_any && saw_gc_any);
  assert((saw_phase_boundary & (1u << ENGINE_VERIFY)) &&
         (saw_phase_boundary & (1u << ENGINE_MATCH)) &&
         (saw_phase_boundary & (1u << ENGINE_ADVANCE)));
  PeakMpiCausalService *empty = make();
  assert(advance_covered(empty, 1) == 1 && !empty->settle_done);
  assert(advance_covered(empty, 1) == 1 && empty->settle_done);
  peak_mpi_causal_service_destroy(empty);
  /* A RAW arriving during an empty GC continuation may wait until the next
   * scheduled service call; its coherent prefix must remain available. */
  RecoveryGroups late;
  recovery_groups_init(&late);
  for (unsigned k = 0; k < 6; ++k)
    for (unsigned rank = 0; rank < 2; ++rank)
      recovery_group(&late, rank, 0, k + 2, 1, 0, NULL);
  unsigned late_turn;
  for (late_turn = 0; late_turn < 2000; ++late_turn) {
    size_t bytes = 65536, work = 16;
    peak_mpi_causal_service_tick_budget(late.service, NULL, NULL, 2,
                                        &bytes, &work);
    if (late.service->gc_stage &&
        !peak_mpi_causal_batches_pending(late.service->batches) &&
        late.service->parsed_records == late.service->cycle_records &&
        !late.service->cycle_idle_progress)
      break;
  }
  assert(late_turn < 2000);
  uint64_t parsed = late.service->parsed_records;
  double now = late.service->now + .1;
  for (unsigned rank = 0; rank < 2; ++rank)
    recovery_raw(&late, rank, now);
  for (late_turn = 0; late_turn < 2000 && late.service->gc_stage; ++late_turn) {
    size_t bytes = 65536, work = 16;
    peak_mpi_causal_service_tick_budget(late.service, NULL, NULL, now,
                                        &bytes, &work);
    assert(work <= 16 && !late.service->hard_fault);
  }
  assert(!late.service->gc_stage && late_turn < 2000);
  peak_mpi_causal_service_tick(late.service, NULL, NULL, now, 65536, 16384);
  for (unsigned rank = 0; rank < 2; ++rank)
    assert(late.service->ranks[rank].last_verify_generation ==
           late.raw[rank].generation);
  assert(late.service->parsed_records == parsed && !late.service->hard_fault);
  peak_mpi_causal_service_destroy(late.service);
  /* An atomic cohort commit may exceed a configured tiny quota. Failed
   * admission must not report work and cause immediate zero-progress retries. */
  RecoveryGroups tiny;
  recovery_groups_init(&tiny);
  for (unsigned rank = 0; rank < 2; ++rank)
    recovery_group(&tiny, rank, 0, 2, 1, 0, NULL);
  unsigned tiny_turn;
  for (tiny_turn = 0; tiny_turn < 2000; ++tiny_turn) {
    size_t bytes = 65536, work = 8;
    peak_mpi_causal_service_tick_budget(tiny.service, NULL, NULL, 2, &bytes, &work);
    assert(work <= 8 && !tiny.service->hard_fault);
    if (tiny.service->pub_stage == 4 && tiny.service->pub_complete &&
        tiny.service->pub_count == 2)
      break;
  }
  assert(tiny_turn < 2000);
  PeakMpiPolicyFeedback before, after;
  assert(peak_mpi_causal_service_feedback(tiny.service, &before));
  for (unsigned retry = 0; retry < 3; ++retry) {
    size_t bytes = 65536, work = 8;
    peak_mpi_causal_service_tick_budget(tiny.service, NULL, NULL, 2, &bytes, &work);
    assert(work == 8 && !tiny.service->last_tick_work &&
           tiny.service->pub_stage == 4 && !tiny.service->hard_fault);
    assert(peak_mpi_causal_service_feedback(tiny.service, &after));
    assert(after.event_cut == before.event_cut);
  }
  peak_mpi_causal_service_destroy(tiny.service);
  /* An incomplete remote collective needs input, not repeated CPU service. */
  RecoveryGroups c;
  recovery_groups_init(&c);
  recovery_group(&c, 0, 0, 2, 1, 0, NULL);
  for (unsigned turn = 0; turn < 100; ++turn) {
    size_t bytes = 65536, work = 256;
    peak_mpi_causal_service_tick_budget(c.service, NULL, NULL, 2, &bytes, &work);
    assert(work <= 256 && !c.service->hard_fault);
  }
  assert(c.service->operation_count && !c.service->model_running &&
         !c.service->root_plan_stage &&
         !peak_mpi_causal_service_runnable(c.service));
  peak_mpi_causal_service_destroy(c.service);
  puts("YIELDED model, root, publish and GC work resume within quota; remote dependency waits without spin PASS");
}

int main(void) {
  test_idle_feedback_publication();
  test_recovery_interrupted_operation_gc();
  test_model_cpu_continuation();
  test_immediate_ordered_send();
  test_stale_recovery_scale();
  test_pending_idle_cut_reuse();
  test_unchanged_idle_root();
  test_concurrent_status_only_retirement();
  test_idle_cut_dependency_index();
  test_observation_metadata_retirement();
  test_retained_send_inputs();
  test_reserved_message_metadata();
  test_collective_proof_reuse();
  test_recovered_prefix_reply();
  test_recovery_collective_boundary();
  test_recovery_reset_runnable();
  test_fault_cache();
  test_input_backpressure();
  test_raw_cohort_membership();
  test_compact_batches();
  test_compact_backend();
  test_delayed_receiver_pressure();
  test_resource_failure_advisory_pressure();
  test_receiver_recovered_pressure();
  test_receiver_pressure_decline();
  test_stale_feedback();
  test_feedback_cache_guard();
  test_profile = PEAK_MPI_PROGRESS_INTEL_2021_11_ASYNC;
  buffered_delivery();
  test_profile = PEAK_MPI_PROGRESS_OMPI_5_0_5_UCX_1_17_RC_PROTO2_HOST_OBSERVED;
  buffered_delivery();
  test_profile = PEAK_MPI_PROGRESS_INTEL_2021_11_OPX_ASYNC0_APPROX;
  buffered_delivery();
  for (size_t work = 1; work <= 3; ++work) {
    test_work = work;
    test_profile =
        PEAK_MPI_PROGRESS_OMPI_5_0_5_UCX_1_17_RC_PROTO2_HOST_OBSERVED;
    delivery(0, 0, 0);
    delivery(1, 0, 0);
    delivery(0, 1, 0);
    delivery(0, 0, 1);
    test_profile = PEAK_MPI_PROGRESS_INTEL_2021_11_OPX_ASYNC0_APPROX;
    delivery(0, 0, 0);
    delivery(1, 0, 0);
    test_buffered_request = 1;
    delivery(0, 0, 0);
    delivery(1, 0, 0);
    test_profile =
        PEAK_MPI_PROGRESS_OMPI_5_0_5_UCX_1_17_RC_PROTO2_HOST_OBSERVED;
    delivery(0, 0, 0);
    delivery(1, 0, 0);
    test_buffered_request = 2; /* Activated persistent Bsend record. */
    delivery(0, 0, 0);
    delivery(1, 0, 0);
    test_buffered_request = 0;
  }
  puts("buffered delivery and late completion retain uncertainty and costs "
       "PASS");
}
