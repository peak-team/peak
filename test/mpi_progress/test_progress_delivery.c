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
int main(void) {
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
