#include "internal/mpi_policy_adapter.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    PeakMpiPolicyMemberId id;
    PeakMpiPolicySample raw, covered;
    double raw_received_at, baseline_P;
    uint64_t baseline_P_ns;
    int baseline_cut_known;
    uint64_t baseline_control_generation, seen;
    int have_raw, have_covered;
} Member;

struct PeakMpiPolicy {
    PeakMpiPolicyConfig config;
    PeakMpiPolicyCohortKey cohort;
    Member *members;
    size_t *index, *scratch;
    size_t count, count_hint, index_capacity;
    size_t recovery_cursor;
    PeakMpiPolicyMode mode;
    PeakMpiPolicyReason reason;
    double g, F, created_at, last_now, cut_received_at, cut_age;
    double oldest_unresolved_age, baseline_F, baseline_at;
    /* Coverage may remain fresh while mixed cuts provide no usable pressure. */
    double pressure_received_at, pressure_age;
    uint64_t revision, last_input_generation, last_cut_id, policy_generation;
    uint64_t lost_events, seen;
    int active, membership_complete, have_input_generation, have_cut;
    int baseline_valid, generation_exhausted, membership_unavailable;
};

static int nonnegative(double v) { return isfinite(v) && v >= 0; }
static int member_equal(PeakMpiPolicyMemberId a, PeakMpiPolicyMemberId b) {
    return a.process == b.process && a.incarnation == b.incarnation;
}
static int cohort_equal(PeakMpiPolicyCohortKey a, PeakMpiPolicyCohortKey b) {
    return a.job_incarnation == b.job_incarnation &&
        a.cohort_id == b.cohort_id && a.epoch == b.epoch;
}
static double maximum(double a, double b) { return a > b ? a : b; }
static double minimum(double a, double b) { return a < b ? a : b; }
static double age_add(double a, double b) { return a > DBL_MAX-b ? DBL_MAX : a+b; }
static double clamp_g(double g, size_t n) {
    return minimum((double)(n ? n : 1), maximum(1, g));
}
static uint64_t mix(uint64_t v) {
    v ^= v >> 30; v *= UINT64_C(0xbf58476d1ce4e5b9);
    v ^= v >> 27; v *= UINT64_C(0x94d049bb133111eb);
    return v ^ (v >> 31);
}
static size_t hash_member(PeakMpiPolicyMemberId id) {
    return (size_t)mix(id.process ^ mix(id.incarnation));
}
static size_t find_member(const PeakMpiPolicy *p, PeakMpiPolicyMemberId id) {
    size_t slot = hash_member(id) & (p->index_capacity - 1);
    for (size_t i = 0; i < p->index_capacity; ++i) {
        size_t entry = p->index[slot];
        if (!entry) return SIZE_MAX;
        if (member_equal(p->members[entry-1].id, id)) return entry-1;
        slot = (slot + 1) & (p->index_capacity - 1);
    }
    return SIZE_MAX;
}
static void insert_member(PeakMpiPolicy *p, size_t index) {
    size_t slot = hash_member(p->members[index].id) & (p->index_capacity - 1);
    while (p->index[slot]) slot = (slot + 1) & (p->index_capacity - 1);
    p->index[slot] = index + 1;
}
static int config_valid(const PeakMpiPolicyConfig *c) {
    return c && c->member_capacity && c->member_capacity <= SIZE_MAX/4 &&
        isfinite(c->q) && c->q > 0 && c->q < 1 &&
        isfinite(c->heartbeat_max_seconds) && c->heartbeat_max_seconds > 0 &&
        c->heartbeat_max_seconds <= DBL_MAX/3 &&
        isfinite(c->ema_alpha) && c->ema_alpha > 0 && c->ema_alpha <= 1 &&
        nonnegative(c->min_observation_seconds) &&
        nonnegative(c->initial_multiplier) &&
        (c->initial_multiplier == 0 || c->initial_multiplier >= 1);
}
static int clock_valid(double now, double previous) {
    return nonnegative(now) && now >= previous;
}
static double sample_app(const PeakMpiPolicySample *s) {
    return s->app_cost_known ? peak_mpi_cost_cut_seconds(s->cut.A_ns) : s->L;
}
static double feedback_app_cut(const PeakMpiPolicyFeedback *f) {
    return f->app_cost_known ? f->A_cut : f->L_cut;
}
static double feedback_latest_app(const PeakMpiPolicyFeedback *f) {
    return f->app_cost_known ? f->local_latest_A : f->local_latest_L;
}
static int sample_valid(const PeakMpiPolicySample *s) {
    return s && nonnegative(s->L) && nonnegative(s->P) && s->P <= s->L && (!s->app_cost_known ||
        peak_mpi_cost_cut_valid(s->cut, s->P, s->A, s->L)) &&
        nonnegative(s->D) && nonnegative(s->elapsed) && nonnegative(s->age_seconds);
}
static int raw_equal(const PeakMpiPolicySample *a, const PeakMpiPolicySample *b) {
    return a->event_cut == b->event_cut && a->L == b->L && sample_app(a) == sample_app(b) &&
        (!(a->app_cost_known && b->app_cost_known) || a->A == b->A) &&
        (!(a->app_cost_known && b->app_cost_known) ||
         (a->cut.P_ns == b->cut.P_ns && a->cut.A_ns == b->cut.A_ns && a->cut.L_ns == b->cut.L_ns)) && a->P == b->P &&
        a->elapsed == b->elapsed && a->control_generation == b->control_generation;
}
static int follows(const PeakMpiPolicySample *a, const PeakMpiPolicySample *b) {
    int cost_follows = a->app_cost_known && b->app_cost_known
        ? a->cut.P_ns >= b->cut.P_ns && a->cut.A_ns >= b->cut.A_ns &&
          a->cut.L_ns >= b->cut.L_ns &&
          a->cut.P_ns-b->cut.P_ns <= a->cut.A_ns-b->cut.A_ns
        : a->L >= b->L && a->P >= b->P;
    return cost_follows && a->event_cut >= b->event_cut &&
        a->elapsed >= b->elapsed && a->control_generation >= b->control_generation;
}
static int raw_follows(const PeakMpiPolicySample *a, const PeakMpiPolicySample *b) {
    return a->L >= b->L && a->P >= b->P &&
        (!(a->app_cost_known && b->app_cost_known) || a->A >= b->A) && follows(a,b);
}

static int bump(PeakMpiPolicy *p) {
    if (p->revision == UINT64_MAX) {
        p->generation_exhausted = 1; p->membership_complete = 0;
        p->membership_unavailable = 1;
        p->mode = PEAK_MPI_POLICY_COARSE; p->reason = PEAK_MPI_POLICY_RESOURCE_LIMIT;
        p->have_cut = p->baseline_valid = 0;
        return 0;
    }
    ++p->revision;
    return 1;
}
static void coarse(PeakMpiPolicy *p, PeakMpiPolicyReason reason) {
    if (p->mode != PEAK_MPI_POLICY_COARSE ||
        (p->reason == PEAK_MPI_POLICY_EVENT_LOSS &&
         reason != PEAK_MPI_POLICY_EVENT_LOSS))
        p->reason = reason;
    p->mode = PEAK_MPI_POLICY_COARSE;
    p->g = p->membership_complete ? (double)(p->count ? p->count : 1) :
        maximum(p->g, (double)(p->count_hint ? p->count_hint : 1));
    p->baseline_valid = 0;
}
static PeakMpiPolicyResult bad_cut(PeakMpiPolicy *p, PeakMpiPolicyReason reason) {
    if (reason == PEAK_MPI_POLICY_BAD_IDENTITY) {
        p->membership_complete = 0; p->membership_unavailable = 1;
    }
    coarse(p, reason); p->have_cut = 0;
    (void)bump(p); return PEAK_MPI_POLICY_INVALID;
}
static void make_stale(PeakMpiPolicy *p) {
    if (p->mode == PEAK_MPI_POLICY_COARSE || !p->active) return;
    p->mode = PEAK_MPI_POLICY_STALE; p->reason = PEAK_MPI_POLICY_FEEDBACK_OLD;
    /* Stale pressure is temporary; it must not replace the learned rate
     * estimate, especially when no new profiling is available after recovery. */
    p->baseline_valid = 0;
}
static int raw_stream_compatible(const Member *m, const PeakMpiPolicySample *s) {
    if (!m->have_raw) return 1;
    if (s->generation == m->raw.generation) return raw_equal(s, &m->raw);
    if (s->generation > m->raw.generation) return raw_follows(s, &m->raw);
    return raw_follows(&m->raw, s);
}
static int apply_raw(Member *m, const PeakMpiPolicySample *s, double now) {
    if (m->have_raw && s->generation <= m->raw.generation) return 0;
    m->raw = *s; m->raw_received_at = now; m->have_raw = 1; return 1;
}
static double pressure_age(const PeakMpiPolicy *p, double now) {
    return p->active && p->mode != PEAK_MPI_POLICY_COARSE ?
        age_add(p->pressure_age, now-p->pressure_received_at) : 0;
}
static double coverage_age(const PeakMpiPolicy *p, double now) {
    double age = p->have_cut ? age_add(p->cut_age, now-p->cut_received_at) : now-p->created_at;
    age = maximum(age, pressure_age(p, now));
    if (!p->have_cut) return age;
    for (size_t i=0; i<p->count; ++i) {
        const Member *m = &p->members[i];
        if (m->have_raw && m->have_covered && m->raw.elapsed > m->covered.elapsed) {
            double member_age = age_add(m->raw.age_seconds, now-m->raw_received_at);
            member_age = age_add(member_age, m->raw.elapsed-m->covered.elapsed);
            age = maximum(age, member_age);
        }
    }
    return age;
}

PeakMpiPolicy *peak_mpi_policy_create(const PeakMpiPolicyConfig *c,
    PeakMpiPolicyCohortKey cohort, const PeakMpiPolicyMemberId *ids, size_t count,
    int actual, int membership_complete, double now) {
    if (!config_valid(c) || !nonnegative(now) || count > c->member_capacity ||
        (count && !ids) || (actual && !count) ||
        (membership_complete && c->initial_multiplier > (double)(count ? count : 1)))
        return NULL;
    size_t table_size = 2;
    while (table_size < c->member_capacity * 2) {
        if (table_size > SIZE_MAX/2) return NULL;
        table_size *= 2;
    }
    PeakMpiPolicy *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->members = calloc(c->member_capacity, sizeof(*p->members));
    p->scratch = calloc(c->member_capacity, sizeof(*p->scratch));
    p->index = calloc(table_size, sizeof(*p->index));
    if (!p->members || !p->scratch || !p->index) { peak_mpi_policy_free(p); return NULL; }
    p->config = *c; p->cohort = cohort; p->count = p->count_hint = count;
    p->index_capacity = table_size;
    p->active = !!actual && (count > 1 || !membership_complete);
    p->membership_complete = !!membership_complete;
    p->mode = p->active ? PEAK_MPI_POLICY_COMPLETE : PEAK_MPI_POLICY_LOCAL;
    p->g = c->initial_multiplier ? c->initial_multiplier : 1;
    p->created_at = p->last_now = p->pressure_received_at = now; p->revision = 1;
    p->policy_generation = c->policy_generation;
    for (size_t i=0; i<count; ++i) {
        if (find_member(p, ids[i]) != SIZE_MAX) { peak_mpi_policy_free(p); return NULL; }
        p->members[i].id = ids[i]; insert_member(p, i);
    }
    return p;
}
void peak_mpi_policy_recover_begin(PeakMpiPolicy *p, double now, double g) {
    if (!p || (p->mode == PEAK_MPI_POLICY_COARSE &&
               p->reason != PEAK_MPI_POLICY_EVENT_LOSS))
        return;
    p->mode = PEAK_MPI_POLICY_COMPLETE;
    p->reason = PEAK_MPI_POLICY_NO_REASON;
    p->have_cut = p->baseline_valid = 0;
    p->last_now = now;
    p->g = clamp_g(g, p->count);
    p->cut_received_at = p->pressure_received_at = now;
    p->cut_age = p->pressure_age = 0;
    p->recovery_cursor = 0;
    (void)bump(p);
}
int peak_mpi_policy_recover_work(PeakMpiPolicy *p, size_t budget,
                                 size_t *used) {
    *used = 0;
    while (p->recovery_cursor < p->count && *used < budget) {
        p->members[p->recovery_cursor++].have_covered = 0;
        ++*used;
    }
    return p->recovery_cursor == p->count;
}
void peak_mpi_policy_recover(PeakMpiPolicy *p, double now, double g) {
    if (!p)
        return;
    peak_mpi_policy_recover_begin(p, now, g);
    size_t used = 0;
    (void)peak_mpi_policy_recover_work(p, SIZE_MAX, &used);
}
void peak_mpi_policy_free(PeakMpiPolicy *p) {
    if (!p) return;
    free(p->index); free(p->scratch); free(p->members); free(p);
}

PeakMpiPolicyResult peak_mpi_policy_record_raw(PeakMpiPolicy *p,
    PeakMpiPolicyCohortKey cohort, const PeakMpiPolicySample *s, double now) {
    if (!p || !s || !clock_valid(now, p->last_now)) return PEAK_MPI_POLICY_INVALID;
    p->last_now = now;
    if (!cohort_equal(p->cohort, cohort)) return PEAK_MPI_POLICY_IGNORED;
    size_t index = find_member(p, s->member);
    if (index == SIZE_MAX) return bad_cut(p, PEAK_MPI_POLICY_BAD_IDENTITY);
    Member *m = &p->members[index];
    if (m->have_raw && s->generation < m->raw.generation) return PEAK_MPI_POLICY_IGNORED;
    if (!sample_valid(s) || !raw_stream_compatible(m, s)) return bad_cut(p, PEAK_MPI_POLICY_BAD_CUT);
    if (!apply_raw(m, s, now)) return PEAK_MPI_POLICY_IGNORED;
    return bump(p) ? PEAK_MPI_POLICY_OK : PEAK_MPI_POLICY_CAPACITY;
}

PeakMpiPolicyResult peak_mpi_policy_submit_cut(PeakMpiPolicy *p,
    const PeakMpiPolicyCut *c, double now) {
    if (!p || !c || !clock_valid(now, p->last_now)) return PEAK_MPI_POLICY_INVALID;
    p->last_now = now;
    if (!cohort_equal(p->cohort, c->cohort) ||
        (p->have_input_generation && c->generation <= p->last_input_generation) ||
        c->policy_generation < p->policy_generation) return PEAK_MPI_POLICY_IGNORED;
    if (!nonnegative(c->F) || !nonnegative(c->age_seconds) ||
        !nonnegative(c->oldest_unresolved_age) ||
        (c->sample_count && !c->samples) ||
        (c->quality & ~PEAK_MPI_POLICY_COMPLETE_CUT) ||
        (p->have_cut && c->cut_id <= p->last_cut_id))
        return bad_cut(p, PEAK_MPI_POLICY_BAD_CUT);
    if (c->sample_count > p->config.member_capacity) {
        p->membership_complete = 0; p->membership_unavailable = 1;
        p->count_hint = p->count_hint > c->sample_count ? p->count_hint : c->sample_count;
        coarse(p, PEAK_MPI_POLICY_RESOURCE_LIMIT);
        (void)bump(p); return PEAK_MPI_POLICY_CAPACITY;
    }
    if (++p->seen == 0) {
        for (size_t i=0; i<p->count; ++i) p->members[i].seen = 0;
        p->seen = 1;
    }
    double age = c->age_seconds;
    int mixed = !c->stable;
    for (size_t i=0; i<c->sample_count; ++i) {
        const PeakMpiPolicySample *s = &c->samples[i];
        size_t index = find_member(p, s->member);
        if (index == SIZE_MAX) return bad_cut(p, PEAK_MPI_POLICY_BAD_IDENTITY);
        Member *m = &p->members[index];
        if (m->seen == p->seen || !sample_valid(s) ||
            (m->have_covered && !follows(s, &m->covered)))
            return bad_cut(p, PEAK_MPI_POLICY_BAD_CUT);
        m->seen = p->seen; p->scratch[i] = index;
        age = maximum(age, s->age_seconds);
        mixed |= !!s->mixed_control;
        if (p->baseline_valid && s->control_generation != m->baseline_control_generation)
            mixed = 1;
    }
    p->last_input_generation = c->generation; p->have_input_generation = 1;
    int policy_change = c->policy_generation != p->policy_generation;
    p->policy_generation = c->policy_generation;
    if (policy_change) p->baseline_valid = 0;
    int full = p->membership_complete && c->sample_count == p->count;
    int fine_complete = (c->quality & PEAK_MPI_POLICY_COMPLETE_CUT) == PEAK_MPI_POLICY_COMPLETE_CUT;
    int fresh = age <= 3*p->config.heartbeat_max_seconds;
    if (!full || !fresh || (p->mode != PEAK_MPI_POLICY_COARSE && !fine_complete)) {
        make_stale(p); p->baseline_valid = 0;
        return bump(p) ? PEAK_MPI_POLICY_OK : PEAK_MPI_POLICY_CAPACITY;
    }
    /* Retain a valid local cut's actual history without inventing an actual
     * communication edge or activating global policy. */
    if (p->mode == PEAK_MPI_POLICY_STALE && !mixed && !policy_change) {
        p->mode = PEAK_MPI_POLICY_COMPLETE; p->reason = PEAK_MPI_POLICY_NO_REASON;
        p->baseline_valid = 0;
    }
    if (!mixed && !policy_change) {
        p->pressure_received_at = now; p->pressure_age = age;
    }
    if (!p->active) p->baseline_valid = 0;
    else if (p->mode != PEAK_MPI_POLICY_COARSE) {
        if (mixed || policy_change) p->baseline_valid = 0;
        else if (!p->baseline_valid || now-p->baseline_at >= p->config.min_observation_seconds) {
            if (p->baseline_valid) {
                double amplitude = 0;
                for (size_t i=0; i<c->sample_count; ++i) {
                    const Member *m = &p->members[p->scratch[i]];
                    double delta = c->samples[i].app_cost_known && m->baseline_cut_known
                        ? peak_mpi_cost_cut_seconds(c->samples[i].cut.P_ns-m->baseline_P_ns)
                        : c->samples[i].P-m->baseline_P;
                    amplitude = maximum(amplitude, delta);
                }
                if (amplitude > 0) {
                    double sample = clamp_g(maximum(0, c->F-p->baseline_F)/amplitude, p->count);
                    p->g = sample >= p->g ? sample :
                        p->config.ema_alpha*sample + (1-p->config.ema_alpha)*p->g;
                }
            }
            p->baseline_F = c->F; p->baseline_at = now; p->baseline_valid = 1;
            for (size_t i=0; i<c->sample_count; ++i) {
                Member *m = &p->members[p->scratch[i]];
                m->baseline_P = c->samples[i].P;
                m->baseline_P_ns = c->samples[i].cut.P_ns;
                m->baseline_cut_known = c->samples[i].app_cost_known;
                m->baseline_control_generation = c->samples[i].control_generation;
            }
        }
    } else p->g = (double)(p->count ? p->count : 1);
    for (size_t i=0; i<c->sample_count; ++i) {
        Member *m = &p->members[p->scratch[i]];
        m->covered = c->samples[i]; m->have_covered = 1;
    }
    p->F = c->F; p->cut_age = age; p->oldest_unresolved_age = c->oldest_unresolved_age;
    p->cut_received_at = now; p->last_cut_id = c->cut_id; p->have_cut = 1;
    return bump(p) ? PEAK_MPI_POLICY_OK : PEAK_MPI_POLICY_CAPACITY;
}

PeakMpiPolicyResult peak_mpi_policy_merge(PeakMpiPolicy *p, const PeakMpiPolicy *s,
    PeakMpiPolicyCohortKey merged, double now) {
    if (!p || !s || p == s || !clock_valid(now, p->last_now) || !clock_valid(now, s->last_now) ||
        p->cohort.job_incarnation != s->cohort.job_incarnation ||
        merged.job_incarnation != p->cohort.job_incarnation ||
        merged.epoch <= p->cohort.epoch || merged.epoch <= s->cohort.epoch)
        return PEAK_MPI_POLICY_INVALID;
    p->last_now = now;
    for (size_t i=0; i<s->count; ++i)
        if (find_member(p, s->members[i].id) != SIZE_MAX)
            return bad_cut(p, PEAK_MPI_POLICY_BAD_IDENTITY);
    if (s->count > p->config.member_capacity-p->count) {
        p->membership_complete = 0; p->membership_unavailable = 1;
        p->count_hint = SIZE_MAX-p->count_hint < s->count_hint ? SIZE_MAX :
            p->count_hint+s->count_hint;
        coarse(p, PEAK_MPI_POLICY_RESOURCE_LIMIT);
        (void)bump(p); return PEAK_MPI_POLICY_CAPACITY;
    }
    if (p->config.q != s->config.q ||
        p->config.heartbeat_max_seconds != s->config.heartbeat_max_seconds ||
        p->config.ema_alpha != s->config.ema_alpha ||
        p->config.min_observation_seconds != s->config.min_observation_seconds) {
        p->membership_complete = 0; p->membership_unavailable = 1;
        return bad_cut(p, PEAK_MPI_POLICY_BAD_CUT);
    }
    for (size_t i=0; i<s->count; ++i) {
        p->members[p->count] = s->members[i];
        p->members[p->count].seen = 0;
        insert_member(p, p->count);
        ++p->count;
    }
    p->g = maximum(p->g, s->g); p->F = maximum(p->F, s->F);
    p->count_hint = SIZE_MAX-p->count_hint < s->count_hint ? SIZE_MAX :
        p->count_hint+s->count_hint;
    p->cohort = merged; p->active = 1;
    p->membership_complete &= s->membership_complete;
    p->membership_unavailable |= s->membership_unavailable;
    p->policy_generation = p->policy_generation > s->policy_generation ?
        p->policy_generation : s->policy_generation;
    p->lost_events = UINT64_MAX-p->lost_events < s->lost_events ? UINT64_MAX :
        p->lost_events+s->lost_events;
    if (p->mode == PEAK_MPI_POLICY_COARSE || s->mode == PEAK_MPI_POLICY_COARSE) {
        if (p->mode != PEAK_MPI_POLICY_COARSE) p->reason = s->reason;
        coarse(p, p->reason);
    } else if (p->mode == PEAK_MPI_POLICY_STALE || s->mode == PEAK_MPI_POLICY_STALE) make_stale(p);
    else p->mode = PEAK_MPI_POLICY_COMPLETE;
    p->have_cut = p->baseline_valid = p->have_input_generation = 0;
    p->created_at = p->pressure_received_at = now; p->pressure_age = 0;
    return bump(p) ? PEAK_MPI_POLICY_OK : PEAK_MPI_POLICY_CAPACITY;
}
PeakMpiPolicyResult peak_mpi_policy_mark_coarse(PeakMpiPolicy *p,
    PeakMpiPolicyReason reason, uint64_t lost_events, double now) {
    if (!p || !clock_valid(now, p->last_now) ||
        reason < PEAK_MPI_POLICY_MISSING_CONTRACT || reason > PEAK_MPI_POLICY_BAD_CUT)
        return PEAK_MPI_POLICY_INVALID;
    p->last_now = now; coarse(p, reason);
    p->lost_events = UINT64_MAX-p->lost_events < lost_events ? UINT64_MAX : p->lost_events+lost_events;
    return bump(p) ? PEAK_MPI_POLICY_OK : PEAK_MPI_POLICY_CAPACITY;
}
PeakMpiPolicyResult peak_mpi_policy_set_membership_known(PeakMpiPolicy *p, int complete, double now) {
    if (!p || !clock_valid(now, p->last_now)) return PEAK_MPI_POLICY_INVALID;
    p->last_now = now;
    if (complete && p->membership_unavailable) return PEAK_MPI_POLICY_INVALID;
    if (p->membership_complete == !!complete) return PEAK_MPI_POLICY_IGNORED;
    p->membership_complete = !!complete; p->have_cut = p->baseline_valid = 0;
    if (complete) p->g = clamp_g(p->g, p->count);
    return bump(p) ? PEAK_MPI_POLICY_OK : PEAK_MPI_POLICY_CAPACITY;
}
PeakMpiPolicyResult peak_mpi_policy_tick(PeakMpiPolicy *p, double now) {
    if (!p || !clock_valid(now, p->last_now)) return PEAK_MPI_POLICY_INVALID;
    p->last_now = now;
    double age = coverage_age(p, now);
    if (p->active && p->mode != PEAK_MPI_POLICY_COARSE && p->mode != PEAK_MPI_POLICY_STALE &&
        age > 3*p->config.heartbeat_max_seconds) {
        make_stale(p); return bump(p) ? PEAK_MPI_POLICY_OK : PEAK_MPI_POLICY_CAPACITY;
    }
    return PEAK_MPI_POLICY_OK;
}

/* One cohort scan. Age, risk, actual-cost basis and tails are independent
 * accumulations in the same member order as the single-member API. */
static PeakMpiPolicyResult feedback_common(PeakMpiPolicy *p, double now,
    PeakMpiPolicyFeedback *f) {
    if (!clock_valid(now, p->last_now)) return PEAK_MPI_POLICY_INVALID;
    p->last_now = now;
    memset(f, 0, sizeof(*f));
    f->membership_complete = p->membership_complete; f->lost_events = p->lost_events;
    f->oldest_unresolved_age = p->oldest_unresolved_age;
    f->coverage_complete = p->have_cut && p->membership_complete;
    f->risk_known = f->coverage_complete;
    f->received_raw_complete = p->membership_complete;
    f->tails_complete = f->coverage_complete;
    f->age_seconds = p->have_cut ? age_add(p->cut_age, now-p->cut_received_at) : now-p->created_at;
    f->age_seconds = maximum(f->age_seconds, pressure_age(p, now));
    f->T_safe = INFINITY;
    double coarse_sum = 0;
    f->app_cost_known = 1;
    for (size_t i=0; i<p->count; ++i) {
        const Member *m = &p->members[i];
        if (p->have_cut && m->have_raw && m->have_covered && m->raw.elapsed > m->covered.elapsed) {
            double member_age = age_add(m->raw.age_seconds, now-m->raw_received_at);
            member_age = age_add(member_age, m->raw.elapsed-m->covered.elapsed);
            f->age_seconds = maximum(f->age_seconds, member_age);
        }
        if (m->have_raw || m->have_covered)
            f->received_raw_sum += maximum(m->have_raw ? m->raw.L : 0,
                m->have_covered ? m->covered.L : 0);
        else f->received_raw_complete = f->tails_complete = 0;
        if (m->have_raw || m->have_covered)
            f->received_app_sum += maximum(m->have_raw ? sample_app(&m->raw) : 0,
                m->have_covered ? sample_app(&m->covered) : 0);
        if ((m->have_raw && !m->raw.app_cost_known) ||
            (m->have_covered && !m->covered.app_cost_known)) f->app_cost_known = 0;
        if (!m->have_covered) { f->risk_known = f->tails_complete = 0; continue; }
        coarse_sum += sample_app(&m->covered); f->T_safe = minimum(f->T_safe, m->covered.elapsed);
        if (m->have_raw) {
            if (m->raw.app_cost_known && m->covered.app_cost_known) {
                if (m->raw.cut.A_ns > m->covered.cut.A_ns)
                    f->U_tail += peak_mpi_cost_cut_seconds(m->raw.cut.A_ns-m->covered.cut.A_ns);
            } else if (sample_app(&m->raw) > sample_app(&m->covered))
                f->U_tail += sample_app(&m->raw)-sample_app(&m->covered);
        }
    }
    if (!isfinite(f->T_safe)) { f->T_safe = 0; f->risk_known = 0; }
    /* Fold tick's age transition into this scan; batch admission therefore
     * charges no hidden coverage_age(n) pass. */
    if (p->active && p->mode != PEAK_MPI_POLICY_COARSE && p->mode != PEAK_MPI_POLICY_STALE &&
        f->age_seconds > 3*p->config.heartbeat_max_seconds) {
        make_stale(p);
        if (!bump(p)) return PEAK_MPI_POLICY_CAPACITY;
    }
    f->cohort = p->cohort; f->generation = p->revision;
    f->policy_generation = p->policy_generation; f->mode = p->mode; f->reason = p->reason;
    f->member_count = p->count_hint; f->q = p->config.q; f->g = p->g; f->F = p->F;
    if (p->mode == PEAK_MPI_POLICY_STALE)
        f->g = p->membership_complete ? (double)(p->count ? p->count : 1) :
            maximum(p->g, (double)(p->count_hint ? p->count_hint : 1));
    f->rate_baseline_valid = p->baseline_valid;
    f->risk = p->mode == PEAK_MPI_POLICY_COARSE ? coarse_sum : p->F;
    if (!isfinite(f->received_app_sum) || !isfinite(f->received_raw_sum) || !isfinite(coarse_sum) || !isfinite(f->U_tail)) {
        coarse(p, PEAK_MPI_POLICY_RESOURCE_LIMIT); (void)bump(p);
        f->mode = p->mode; f->reason = p->reason; f->g = p->g; f->generation = p->revision;
        f->risk_known = f->tails_complete = f->received_raw_complete = 0;
        f->received_raw_sum = f->risk = f->U_tail = 0;
    }
    return PEAK_MPI_POLICY_OK;
}

/* Constant work: no cohort lookup, scan, policy mutation or retained pointer. */
static void feedback_member(const Member *m, const PeakMpiPolicyFeedback *common,
    PeakMpiPolicyFeedback *f) {
    memcpy(f, common, sizeof(*f));
    f->member = m->id;
    if (m->have_covered) {
        f->event_cut = m->covered.event_cut; f->control_generation = m->covered.control_generation;
        f->cut = m->covered.cut;
        f->L_cut = m->covered.app_cost_known ? peak_mpi_cost_cut_seconds(m->covered.cut.L_ns) : m->covered.L;
        f->A_cut = sample_app(&m->covered);
        f->P_cut = m->covered.app_cost_known ? peak_mpi_cost_cut_seconds(m->covered.cut.P_ns) : m->covered.P; f->D_cut = m->covered.D;
        f->local_elapsed_at_cut = m->covered.elapsed;
    }
    f->local_latest_L = maximum(m->have_raw ? m->raw.L : 0, f->L_cut);
    f->local_latest_A = maximum(m->have_raw ? sample_app(&m->raw) : 0, f->A_cut);
    f->local_latest_A_ns = m->have_raw && m->raw.cut.A_ns > f->cut.A_ns
        ? m->raw.cut.A_ns : f->cut.A_ns;
    f->local_latest_L_ns = m->have_raw && m->raw.cut.L_ns > f->cut.L_ns
        ? m->raw.cut.L_ns : f->cut.L_ns;
}

PeakMpiPolicyResult peak_mpi_policy_feedback(PeakMpiPolicy *p,
    PeakMpiPolicyMemberId id, double now, PeakMpiPolicyFeedback *f) {
    if (!p || !f) return PEAK_MPI_POLICY_INVALID;
    PeakMpiPolicyResult result = peak_mpi_policy_tick(p, now);
    if (result != PEAK_MPI_POLICY_OK) return result;
    size_t index = find_member(p, id);
    if (index == SIZE_MAX) return PEAK_MPI_POLICY_NOT_MEMBER;
    PeakMpiPolicyFeedback common;
    result = feedback_common(p, now, &common);
    if (result == PEAK_MPI_POLICY_OK) feedback_member(&p->members[index], &common, f);
    return result;
}

size_t peak_mpi_policy_feedback_batch_work(const PeakMpiPolicy *p) {
    /* create bounds capacity by SIZE_MAX/4, so this cannot overflow. */
    return p ? 1+2*p->count : 0;
}
PeakMpiPolicyResult peak_mpi_policy_feedback_batch(PeakMpiPolicy *p,
    double now, PeakMpiPolicyFeedback *out, size_t capacity, size_t allowance,
    size_t *written, size_t *used) {
    if (written) *written = 0;
    if (used) *used = 0;
    if (!p || !written || !used || !clock_valid(now, p->last_now))
        return PEAK_MPI_POLICY_INVALID;
    if (capacity < p->count) return PEAK_MPI_POLICY_CAPACITY;
    if (p->count && !out) return PEAK_MPI_POLICY_INVALID;
    size_t required = peak_mpi_policy_feedback_batch_work(p);
    if (allowance < required) return PEAK_MPI_POLICY_BUDGET;
    PeakMpiPolicyFeedback common;
    PeakMpiPolicyResult result = feedback_common(p, now, &common);
    *used = 1+p->count;
    if (result != PEAK_MPI_POLICY_OK) return result;
    for (size_t i=0; i<p->count; ++i) feedback_member(&p->members[i], &common, &out[i]);
    *written = p->count; *used = required;
    return PEAK_MPI_POLICY_OK;
}

static int feedback_valid(const PeakMpiPolicyFeedback *f) {
    if (f->mode == PEAK_MPI_POLICY_APPROX &&
        (!f->approximate || !f->recovery.epoch || f->coverage_complete ||
         f->risk_known || f->tails_complete))
        return 0;
    if (!(f->mode >= PEAK_MPI_POLICY_LOCAL &&
          f->mode <= PEAK_MPI_POLICY_APPROX &&
          f->reason >= PEAK_MPI_POLICY_NO_REASON &&
          f->reason <= PEAK_MPI_POLICY_BAD_CUT && f->member_count &&
          isfinite(f->q) && f->q > 0 && f->q < 1 &&
          (!f->membership_complete || f->member_count > 1 ||
           f->mode == PEAK_MPI_POLICY_LOCAL) &&
          isfinite(f->g) && f->g >= 1 &&
          (!f->membership_complete || f->g <= (double)f->member_count) &&
          (!f->membership_complete || f->mode != PEAK_MPI_POLICY_COARSE ||
           f->g == (double)f->member_count) &&
          (!f->membership_complete || f->mode != PEAK_MPI_POLICY_STALE ||
           f->g == (double)f->member_count) &&
          nonnegative(f->L_cut) && nonnegative(f->P_cut) &&
          f->P_cut <= f->L_cut && (!f->app_cost_known ||
          (peak_mpi_cost_cut_valid(f->cut, f->P_cut, f->A_cut, f->L_cut) &&
           f->local_latest_A_ns >= f->cut.A_ns &&
           f->local_latest_L_ns >= f->cut.L_ns && f->local_latest_A_ns <= f->local_latest_L_ns &&
           f->local_latest_A == peak_mpi_cost_cut_seconds(f->local_latest_A_ns) &&
           nonnegative(f->local_latest_A) && f->local_latest_A >= f->A_cut &&
           nonnegative(f->received_app_sum))) && nonnegative(f->D_cut) &&
          nonnegative(f->local_elapsed_at_cut) && nonnegative(f->F) &&
          nonnegative(f->risk) && nonnegative(f->U_tail) &&
          nonnegative(f->T_safe) && nonnegative(f->local_latest_L) &&
          f->local_latest_L >= f->L_cut && nonnegative(f->received_raw_sum) &&
          nonnegative(f->age_seconds) &&
          nonnegative(f->oldest_unresolved_age) &&
          (!f->coverage_complete ||
           (f->membership_complete && f->T_safe <= f->local_elapsed_at_cut)) &&
          (!f->received_raw_complete || f->membership_complete) &&
          (!f->risk_known || f->coverage_complete) &&
          (!f->tails_complete ||
           (f->coverage_complete && f->received_raw_complete))))
        return 0;
    /* These are canonical same-value invariants, not comparisons between
     * separately accumulated floating-point totals. A finite fixed epsilon
     * cannot bound F versus raw history without the model's path-operation
     * error bound; S+tail versus received sum also has size-dependent error.
     * Keep those quantities unchanged and independently visible. Complete
     * membership/coverage flags, not a numerical reconciliation, establish
     * whether the raw history is known. */
    if (f->risk_known && f->mode == PEAK_MPI_POLICY_COMPLETE && f->risk != f->F) return 0;
    if (f->coverage_complete && f->mode == PEAK_MPI_POLICY_COMPLETE &&
        f->D_cut > f->F) return 0;
    return 1;
}
PeakMpiPolicyResult peak_mpi_policy_receiver_init(PeakMpiPolicyReceiver *r,
    const PeakMpiPolicyConfig *c, PeakMpiPolicyCohortKey cohort,
    PeakMpiPolicyMemberId member, double now) {
    if (!r || !config_valid(c) || !nonnegative(now)) return PEAK_MPI_POLICY_INVALID;
    memset(r, 0, sizeof(*r)); r->config = *c; r->cohort = cohort; r->member = member;
    r->created_at = r->last_now = now; r->conservative_g = 1;
    return PEAK_MPI_POLICY_OK;
}
PeakMpiPolicyResult peak_mpi_policy_receiver_rebind(PeakMpiPolicyReceiver *r,
    PeakMpiPolicyCohortKey cohort, size_t count, int membership_complete, double now) {
    if (!r || !clock_valid(now, r->last_now) || !count ||
        cohort.job_incarnation != r->cohort.job_incarnation ||
        (r->active && count < r->known_member_count) ||
        (!cohort_equal(cohort, r->cohort) && cohort.epoch <= r->cohort.epoch))
        return PEAK_MPI_POLICY_INVALID;
    if (r->active && cohort_equal(cohort, r->cohort)) return PEAK_MPI_POLICY_IGNORED;
    r->last_now = now; r->cohort = cohort; r->known_member_count = count;
    r->membership_complete = !!membership_complete; r->active = 1;
    r->have_feedback = r->feedback_invalid = 0; r->last_generation = 0; r->created_at = now;
    r->membership_hint_pending = 0;
    if (r->permanent_coarse) r->conservative_g = maximum(r->conservative_g, (double)count);
    return PEAK_MPI_POLICY_OK;
}
PeakMpiPolicyResult peak_mpi_policy_receiver_note_membership_hint(PeakMpiPolicyReceiver *r,
    size_t count, double now) {
    if (!r || !clock_valid(now, r->last_now) || !count) return PEAK_MPI_POLICY_INVALID;
    r->last_now = now;
    if (count <= r->known_member_count) return PEAK_MPI_POLICY_IGNORED;
    r->membership_hint_pending |= r->have_feedback;
    r->known_member_count = count; r->membership_complete = 0; r->active = 1;
    return PEAK_MPI_POLICY_OK;
}
int peak_mpi_policy_receiver_recover(PeakMpiPolicyReceiver *r, double now) {
    if (!r ||
        (r->permanent_coarse && r->coarse_reason != PEAK_MPI_POLICY_EVENT_LOSS))
        return 0;
    r->permanent_coarse = r->feedback_invalid = 0;
    r->coarse_reason = PEAK_MPI_POLICY_NO_REASON;
    r->have_feedback = 0;
    r->created_at = r->last_now = now;
    return 1;
}
PeakMpiPolicyResult peak_mpi_policy_receiver_accept(PeakMpiPolicyReceiver *r,
    const PeakMpiPolicyFeedback *f, double now) {
    if (!r || !f || !clock_valid(now, r->last_now)) return PEAK_MPI_POLICY_INVALID;
    r->last_now = now;
    if (!cohort_equal(f->cohort, r->cohort) || !member_equal(f->member, r->member) ||
        (r->have_feedback && f->generation <= r->last_generation)) return PEAK_MPI_POLICY_IGNORED;
    /* Local reports are not cohort feedback and cannot activate a receiver. */
    if (f->mode == PEAK_MPI_POLICY_LOCAL) return PEAK_MPI_POLICY_IGNORED;
    if (!feedback_valid(f) || f->q != r->config.q ||
        f->policy_generation < r->config.policy_generation ||
        (r->have_history && (f->event_cut < r->feedback.event_cut ||
         (f->app_cost_known && r->feedback.app_cost_known &&
          (f->cut.P_ns < r->feedback.cut.P_ns || f->cut.A_ns < r->feedback.cut.A_ns ||
           f->cut.L_ns < r->feedback.cut.L_ns || f->local_latest_A_ns < r->feedback.local_latest_A_ns ||
           f->local_latest_L_ns < r->feedback.local_latest_L_ns ||
           f->cut.P_ns-r->feedback.cut.P_ns > f->cut.A_ns-r->feedback.cut.A_ns)) ||
         (!(f->app_cost_known && r->feedback.app_cost_known) &&
          (f->L_cut < r->feedback.L_cut || f->P_cut < r->feedback.P_cut ||
           f->local_latest_L < r->feedback.local_latest_L)) ||
         f->local_elapsed_at_cut < r->feedback.local_elapsed_at_cut ||
         f->control_generation < r->feedback.control_generation ||
         f->policy_generation < r->feedback.policy_generation))) {
        r->active = r->feedback_invalid = 1;
        return PEAK_MPI_POLICY_INVALID;
    }
    if (r->membership_hint_pending && f->membership_complete) {
        /* Obsolete membership is not confirmation, but a validated sticky
         * coarse signal must survive discovery of additional participants. */
        if (f->mode == PEAK_MPI_POLICY_COARSE) {
            r->permanent_coarse = 1;
            if (!r->coarse_reason ||
                (r->coarse_reason == PEAK_MPI_POLICY_EVENT_LOSS &&
                 f->reason != PEAK_MPI_POLICY_EVENT_LOSS))
                r->coarse_reason = f->reason;
        }
        return PEAK_MPI_POLICY_IGNORED;
    }
    if (f->membership_complete && (f->member_count < r->known_member_count ||
        (r->have_feedback && r->feedback.membership_complete &&
         f->member_count != r->feedback.member_count))) {
        r->active = r->feedback_invalid = 1;
        return PEAK_MPI_POLICY_INVALID;
    }
    r->feedback = *f; r->last_generation = f->generation; r->received_at = now;
    r->have_history = r->have_feedback = r->active = 1; r->feedback_invalid = 0;
    if (r->known_member_count < f->member_count) r->known_member_count = f->member_count;
    r->membership_complete = !!f->membership_complete;
    if (f->mode == PEAK_MPI_POLICY_COARSE) {
        r->permanent_coarse = 1;
        if (!r->coarse_reason ||
            (r->coarse_reason == PEAK_MPI_POLICY_EVENT_LOSS &&
             f->reason != PEAK_MPI_POLICY_EVENT_LOSS))
            r->coarse_reason = f->reason;
    }
    if (f->mode != PEAK_MPI_POLICY_STALE)
        r->conservative_g = maximum(r->conservative_g, f->g);
    return PEAK_MPI_POLICY_OK;
}
PeakMpiPolicyResult peak_mpi_policy_receiver_mark_coarse(PeakMpiPolicyReceiver *r,
    PeakMpiPolicyReason reason, double now) {
    if (!r || !clock_valid(now, r->last_now) ||
        reason < PEAK_MPI_POLICY_MISSING_CONTRACT || reason > PEAK_MPI_POLICY_BAD_CUT)
        return PEAK_MPI_POLICY_INVALID;
    r->last_now = now; r->active = 1; r->permanent_coarse = 1;
    if (!r->coarse_reason || (r->coarse_reason == PEAK_MPI_POLICY_EVENT_LOSS &&
                              reason != PEAK_MPI_POLICY_EVENT_LOSS))
        r->coarse_reason = reason;
    r->conservative_g = maximum(r->conservative_g,
        (double)(r->known_member_count ? r->known_member_count : 1));
    return PEAK_MPI_POLICY_OK;
}

static PeakMpiPolicyResult receiver_snapshot(PeakMpiPolicyReceiver *r,
    double now, double elapsed, double A, const PeakMpiCostCut *cut,
    double local_headroom, PeakMpiPolicyDecision *d) {
    if (!r || !d || !clock_valid(now, r->last_now) || !nonnegative(elapsed) ||
        !nonnegative(A) || !isfinite(local_headroom)) return PEAK_MPI_POLICY_INVALID;
    r->last_now = now; memset(d, 0, sizeof(*d));
    d->g = 1; d->mode = PEAK_MPI_POLICY_LOCAL; d->global_headroom = NAN;
    d->available_headroom = local_headroom;
    if (!r->active) {
        d->headroom_known = d->reattach_relaxation = local_headroom > 0;
    } else {
        const PeakMpiPolicyFeedback *f = &r->feedback;
        double local_cut_age = r->have_feedback && f->coverage_complete ?
            elapsed-f->local_elapsed_at_cut : 0;
        double age = r->have_feedback ? maximum(age_add(f->age_seconds,now-r->received_at),
            local_cut_age) : now-r->created_at;
        d->feedback_age_seconds = maximum(0, age);
        int cut_ordered = cut && f->app_cost_known
            ? cut->A_ns >= f->cut.A_ns : A >= feedback_app_cut(f);
        int latest_ordered = cut && f->app_cost_known
            ? cut->A_ns >= f->local_latest_A_ns : A >= feedback_latest_app(f);
        int fresh = r->have_feedback && !r->feedback_invalid &&
            elapsed >= f->local_elapsed_at_cut && cut_ordered &&
            age <= 3*r->config.heartbeat_max_seconds;
        d->mode = r->have_feedback ? f->mode : PEAK_MPI_POLICY_COMPLETE;
        d->reason = r->have_feedback ? f->reason : PEAK_MPI_POLICY_UNKNOWN_MEMBERS;
        if (r->permanent_coarse) {
            d->mode = PEAK_MPI_POLICY_COARSE; d->reason = r->coarse_reason;
        } else if (!fresh && (r->feedback_invalid ||
            (r->have_feedback && (elapsed < f->local_elapsed_at_cut || !cut_ordered)) ||
            age > 3*r->config.heartbeat_max_seconds)) {
            d->mode = PEAK_MPI_POLICY_STALE; d->reason = PEAK_MPI_POLICY_FEEDBACK_OLD;
        }
        d->g = r->have_feedback ? f->g : r->conservative_g;
        /* A membership bound is not a pressure observation. Unknown members
         * still forbid global headroom, without tightening local control. */
        if (d->mode == PEAK_MPI_POLICY_STALE || d->mode == PEAK_MPI_POLICY_COARSE) {
            d->g = r->membership_complete ? (double)(r->known_member_count ? r->known_member_count : 1) :
                maximum(r->conservative_g, (double)(r->known_member_count ? r->known_member_count : 1));
        }
        d->g = maximum(1, d->g);
        if (d->mode != PEAK_MPI_POLICY_STALE)
            r->conservative_g = maximum(r->conservative_g, d->g);
        int matching_mode = !r->permanent_coarse || f->mode == PEAK_MPI_POLICY_COARSE;
        d->global_available = fresh && matching_mode &&
            d->mode != PEAK_MPI_POLICY_STALE && r->membership_complete &&
            f->coverage_complete && f->risk_known && f->tails_complete;
        if (r->have_feedback && (f->coverage_complete || f->approximate) &&
            d->mode != PEAK_MPI_POLICY_COARSE && cut_ordered)
            d->local_D_estimate = f->D_cut + (cut && f->app_cost_known
                ? peak_mpi_cost_cut_seconds(cut->A_ns-f->cut.A_ns) : A-feedback_app_cut(f));
        else d->local_D_estimate = NAN;
        if (!isfinite(d->local_D_estimate)) d->local_D_estimate = NAN;
        if (fresh && matching_mode && !r->permanent_coarse &&
            d->mode == PEAK_MPI_POLICY_APPROX && f->approximate &&
            r->membership_complete) {
            d->available_headroom = local_headroom;
            d->headroom_known = 1;
            d->reattach_relaxation = local_headroom > 0;
        }
        /* A finer tuple received after permanent coarse cannot establish a
         * coarse raw-sum risk; wait for another explicitly coarse tuple. */
        if (d->global_available && matching_mode && f->risk_known && f->tails_complete &&
            latest_ordered && elapsed >= f->local_elapsed_at_cut) {
            double extra = cut && f->app_cost_known
                ? peak_mpi_cost_cut_seconds(cut->A_ns-f->local_latest_A_ns) : A-feedback_latest_app(f);
            double h = r->config.q*f->T_safe-f->risk-f->U_tail-extra;
            if (isfinite(h)) {
                d->global_headroom = h; d->available_headroom = minimum(local_headroom, h);
                d->headroom_known = 1;
                d->reattach_relaxation = d->available_headroom > 0;
            }
        }
    }
    d->q_eff = r->config.q/d->g;
    d->detach_target = d->candidate_stop_target = d->cadence_target =
        d->reattach_future_rate_target = d->q_eff;
    return PEAK_MPI_POLICY_OK;
}

PeakMpiPolicyResult peak_mpi_policy_receiver_snapshot(PeakMpiPolicyReceiver *r,
    double now, double elapsed, double A, double headroom, PeakMpiPolicyDecision *d) {
    return receiver_snapshot(r, now, elapsed, A, NULL, headroom, d);
}
PeakMpiPolicyResult peak_mpi_policy_receiver_snapshot_cut(PeakMpiPolicyReceiver *r,
    double now, double elapsed, PeakMpiCostCut cut, double headroom, PeakMpiPolicyDecision *d) {
    if (cut.P_ns > cut.A_ns || cut.A_ns > cut.L_ns) return PEAK_MPI_POLICY_INVALID;
    return receiver_snapshot(r, now, elapsed, peak_mpi_cost_cut_seconds(cut.A_ns), &cut, headroom, d);
}
