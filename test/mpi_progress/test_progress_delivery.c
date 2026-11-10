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
  b = peak_mpi_causal_batches_create_with_slots(1, 2, 32);
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
  v = compact_ready(b);
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
  fault_seed(s, 0);
  assert(prepare_feedback_cache(s, 100000));
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
  s->recovery_reset = 7;
  s->recovery_rank = s->config.world_size;
  size_t used;
  assert(!recovery_work(s, 1, &used));
  assert(s->recovery.state == PEAK_MPI_RECOVERY_ACK);
  assert(prepare_feedback_cache(s, 100000) > 0);
  printf("recovery partialroot0 feedbackepoch%llu expected2\n",
         (unsigned long long)s->ranks[0].feedback_epoch);
  assert(s->ranks[0].feedback_epoch == 2);
  PeakMpiPolicyFeedback f;
  assert(sink_feedback(s, 0, 1.1, &f) &&
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
         !peak_mpi_causal_service_runnable(s));
  peak_mpi_causal_service_tick(s, NULL, NULL, s->now, 65536, 16384);
  PeakMpiPolicyFeedback feedback;
  assert(sink_feedback(s, 0, s->now, &feedback));
  assert(feedback.recovery.state == PEAK_MPI_RECOVERY_ACK);
  peak_mpi_causal_service_destroy(s);
  puts("CPU-only recovery reset drains to ACK; remote waits/faults/idle stay "
       "blocked PASS");
}
int main(void) {
  test_recovery_reset_runnable();
  test_fault_cache();
  test_input_backpressure();
  test_compact_batches();
  test_compact_backend();
  test_delayed_receiver_pressure();
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
