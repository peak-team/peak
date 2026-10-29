#include "internal/global_budget_causal.h"

#include <stdlib.h>
#include <string.h>

typedef struct CausalEvent CausalEvent;
typedef struct CausalRequest CausalRequest;
typedef struct CausalContext CausalContext;
typedef struct CausalCollective CausalCollective;
typedef struct { size_t rank, slot_plus_one; } CausalMemberSlot;
struct CausalEvent {
    PeakGlobalBudgetCausalEvent event;
    uint64_t *requests;
    uint64_t previous_cost;
    bool ordered;
    CausalEvent *next;
};
struct CausalRequest {
    size_t rank, peer;
    uint32_t context_owner;
    uint64_t id, context, round, tag, post, wait;
    PeakGlobalBudgetCausalOperation operation;
    bool waited, done, unresolved_match;
    CausalCollective *collective;
    CausalRequest *next, *previous, *request_next, *match_next;
};
struct CausalContext {
    uint32_t owner;
    uint64_t id;
    size_t count, *members, slot_capacity;
    CausalMemberSlot *slots;
    CausalCollective *rounds;
    CausalContext *next, *index_next;
};
struct CausalCollective {
    uint64_t round, dependency;
    size_t waited, completed;
    CausalRequest **members;
    CausalContext *context;
    CausalCollective *next;
};
typedef struct {
    uint64_t height, observed, ordered_cost, covered_floor;
    uint64_t ordered_sequence, processed_sequence, last_mpi_sequence;
    size_t mpi_depth;
    CausalEvent *queue;
    bool ready, blocked;
} CausalRank;
typedef struct { size_t rank, next_request; } CausalWalk;
struct PeakGlobalBudgetCausal {
    size_t world_size;
    CausalRank *ranks;
    CausalRequest *requests;
    CausalRequest **request_index, **match_index;
    size_t index_size, request_count;
    CausalContext *contexts;
    CausalContext **context_index;
    size_t context_index_size, context_count;
    unsigned char *walk_state;
    CausalWalk *walk;
    size_t *ready_ranks, ready_head, ready_count, blocked_count;
    bool cycle_dirty;
    uint64_t raw, causal_raw, fallback_raw, unresolved, serialized, confirmed;
    bool sum_mode, resource_failed, saturated, permanent_sum;
};

static uint64_t maximum(uint64_t a, uint64_t b) { return a > b ? a : b; }
static uint64_t add(PeakGlobalBudgetCausal *m, uint64_t a, uint64_t b) {
    if (b > UINT64_MAX - a) { m->saturated = true; return UINT64_MAX; }
    return a + b;
}
static void confirm(PeakGlobalBudgetCausal *m) {
    for (size_t r = 0; r < m->world_size; ++r)
        m->confirmed = maximum(m->confirmed, m->ranks[r].height);
}
void peak_global_budget_causal_fallback(PeakGlobalBudgetCausal *m) {
    /* SUM observations already add each raw delta to S and keep U at zero.
     * Heights/snapshots need rebasing only at the eventual closed checkpoint. */
    if (m->sum_mode) return;
    confirm(m);
    m->confirmed = add(m, m->confirmed, m->unresolved);
    m->fallback_raw = add(m, m->fallback_raw, m->unresolved);
    m->unresolved = 0;
    m->sum_mode = true;
    for (size_t r = 0; r < m->world_size; ++r) {
        m->ranks[r].height = m->confirmed;
        m->ranks[r].covered_floor = m->ranks[r].observed;
    }
}
static void resource_failure(PeakGlobalBudgetCausal *m) {
    peak_global_budget_causal_fallback(m);
    m->resource_failed = m->permanent_sum = true;
}
PeakGlobalBudgetCausal *peak_global_budget_causal_new(size_t world_size) {
    if (!world_size || world_size > SIZE_MAX / sizeof(CausalRank) ||
        world_size > SIZE_MAX / sizeof(CausalWalk)) return NULL;
    PeakGlobalBudgetCausal *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->world_size = world_size;
    m->index_size = 16;
    m->context_index_size = 16;
    m->context_index = calloc(m->context_index_size, sizeof(*m->context_index));
    m->request_index = calloc(m->index_size, sizeof(*m->request_index));
    m->match_index = calloc(m->index_size, sizeof(*m->match_index));
    m->ranks = calloc(world_size, sizeof(*m->ranks));
    m->walk_state = calloc(world_size, 1);
    m->walk = calloc(world_size, sizeof(*m->walk));
    m->ready_ranks = calloc(world_size, sizeof(*m->ready_ranks));
    if (!m->ranks || !m->walk_state || !m->walk || !m->ready_ranks ||
        !m->request_index || !m->match_index || !m->context_index) {
        free(m->ranks); free(m->walk_state); free(m->walk); free(m->ready_ranks);
        free(m->request_index); free(m->match_index); free(m->context_index); free(m); return NULL;
    }
    return m;
}
static void free_requests(PeakGlobalBudgetCausal *m) {
    while (m->requests) {
        CausalRequest *next = m->requests->next;
        free(m->requests); m->requests = next;
    }
    m->request_count = 0;
    memset(m->request_index, 0, m->index_size * sizeof(*m->request_index));
    memset(m->match_index, 0, m->index_size * sizeof(*m->match_index));
}
static void free_rounds(CausalContext *c) {
    while (c->rounds) {
        CausalCollective *next = c->rounds->next;
        free(c->rounds->members); free(c->rounds); c->rounds = next;
    }
}
void peak_global_budget_causal_free(PeakGlobalBudgetCausal *m) {
    if (!m) return;
    for (size_t r = 0; r < m->world_size; ++r) {
        CausalEvent *p = m->ranks[r].queue;
        while (p) { CausalEvent *next = p->next; free(p->requests); free(p); p = next; }
    }
    free_requests(m);
    while (m->contexts) {
        CausalContext *next = m->contexts->next;
        free_rounds(m->contexts);
        free(m->contexts->members); free(m->contexts->slots); free(m->contexts); m->contexts = next;
    }
    free(m->ranks); free(m->walk_state); free(m->walk); free(m->ready_ranks);
        free(m->request_index); free(m->match_index); free(m->context_index); free(m);
}
static uint64_t mix(uint64_t x);
static size_t context_bucket(uint32_t owner, uint64_t id, size_t size) {
    return (size_t)mix(mix((uint64_t)owner) ^ id) & (size - 1);
}
static CausalContext *context(PeakGlobalBudgetCausal *m, uint32_t owner, uint64_t id) {
    size_t slot = context_bucket(owner, id, m->context_index_size);
    for (CausalContext *p = m->context_index[slot]; p; p = p->index_next)
        if (p->owner == owner && p->id == id) return p;
    return NULL;
}
static bool grow_context_index(PeakGlobalBudgetCausal *m) {
    if (m->context_count < m->context_index_size) return true;
    if (m->context_index_size > SIZE_MAX / (2 * sizeof(CausalContext *))) {
        resource_failure(m); return false;
    }
    size_t size = m->context_index_size * 2;
    CausalContext **index = calloc(size, sizeof(*index));
    if (!index) { resource_failure(m); return false; }
    for (CausalContext *p = m->contexts; p; p = p->next) {
        size_t slot = context_bucket(p->owner, p->id, size);
        p->index_next = index[slot]; index[slot] = p;
    }
    free(m->context_index); m->context_index = index; m->context_index_size = size;
    return true;
}
bool peak_global_budget_causal_context(PeakGlobalBudgetCausal *m, uint32_t owner, uint64_t id,
                                      const size_t *members, size_t count) {
    if (!count || count > m->world_size || !members) {
        peak_global_budget_causal_fallback(m); return false;
    }
    CausalContext *old = context(m, owner, id);
    if (old) {
        bool same = old->count == count &&
            memcmp(old->members, members, count * sizeof(*members)) == 0;
        if (!same) peak_global_budget_causal_fallback(m);
        return same;
    }
    if (!grow_context_index(m)) return false;
    CausalContext *p = calloc(1, sizeof(*p));
    if (p) {
        p->members = malloc(count * sizeof(*members));
        p->slot_capacity = 2;
        while (p->slot_capacity < count * 2) p->slot_capacity *= 2;
        p->slots = calloc(p->slot_capacity, sizeof(*p->slots));
    }
    if (!p || !p->members || !p->slots) {
        if (p) { free(p->members); free(p->slots); free(p); }
        resource_failure(m); return false;
    }
    for (size_t i = 0; i < count; ++i) {
        size_t slot = (size_t)mix((uint64_t)members[i]) & (p->slot_capacity - 1);
        while (p->slots[slot].slot_plus_one && p->slots[slot].rank != members[i])
            slot = (slot + 1) & (p->slot_capacity - 1);
        if (members[i] >= m->world_size || p->slots[slot].slot_plus_one) {
            free(p->members); free(p->slots); free(p);
            peak_global_budget_causal_fallback(m); return false;
        }
        p->slots[slot] = (CausalMemberSlot){ members[i], i + 1 };
    }
    memcpy(p->members, members, count * sizeof(*members));
    p->owner = owner; p->id = id; p->count = count; p->next = m->contexts; m->contexts = p;
    size_t slot = context_bucket(owner, id, m->context_index_size);
    p->index_next = m->context_index[slot]; m->context_index[slot] = p;
    ++m->context_count;
    return true;
}
static void register_collective(PeakGlobalBudgetCausal *m, CausalRequest *a) {
    CausalContext *c = context(m, a->context_owner, a->context);
    if (!c) { peak_global_budget_causal_fallback(m); return; }
    size_t key = (size_t)mix((uint64_t)a->rank) & (c->slot_capacity - 1);
    while (c->slots[key].slot_plus_one && c->slots[key].rank != a->rank)
        key = (key + 1) & (c->slot_capacity - 1);
    if (!c->slots[key].slot_plus_one) { peak_global_budget_causal_fallback(m); return; }
    size_t slot = c->slots[key].slot_plus_one - 1;
    CausalCollective *g = c->rounds;
    while (g && g->round != a->round) g = g->next;
    if (!g) {
        g = calloc(1, sizeof(*g));
        if (g) g->members = calloc(c->count, sizeof(*g->members));
        if (!g || !g->members) { free(g); resource_failure(m); return; }
        g->context = c; g->round = a->round; g->next = c->rounds; c->rounds = g;
    }
    if (g->members[slot]) { peak_global_budget_causal_fallback(m); return; }
    g->members[slot] = a; a->collective = g;
}
static uint64_t mix(uint64_t x) {
    x ^= x >> 30; x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27; x *= UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}
static size_t request_bucket(size_t rank, uint64_t id, size_t size) {
    return (size_t)mix(id ^ mix((uint64_t)rank)) & (size - 1);
}
static size_t match_bucket(const CausalRequest *a, size_t size) {
    size_t source = a->operation == PEAK_CAUSAL_RECV ? a->peer : a->rank;
    size_t destination = a->operation == PEAK_CAUSAL_RECV ? a->rank : a->peer;
    uint64_t h = mix(mix((uint64_t)source) ^ (uint64_t)destination);
    h = mix(h ^ a->context); h = mix(h ^ a->context_owner);
    h = mix(h ^ a->tag); h = mix(h ^ a->round);
    return (size_t)h & (size - 1);
}
static bool point_to_point(const CausalRequest *a) {
    return a->operation == PEAK_CAUSAL_SEND || a->operation == PEAK_CAUSAL_RECV ||
           a->operation == PEAK_CAUSAL_AMBIGUOUS_SEND;
}
static void index_request(PeakGlobalBudgetCausal *m, CausalRequest *a) {
    size_t slot = request_bucket(a->rank, a->id, m->index_size);
    a->request_next = m->request_index[slot]; m->request_index[slot] = a;
    if (point_to_point(a)) {
        slot = match_bucket(a, m->index_size);
        a->match_next = m->match_index[slot]; m->match_index[slot] = a;
    }
}
static bool grow_indexes(PeakGlobalBudgetCausal *m) {
    if (m->request_count < m->index_size) return true;
    if (m->index_size > SIZE_MAX / (2 * sizeof(CausalRequest *))) {
        resource_failure(m); return false;
    }
    size_t size = m->index_size * 2;
    CausalRequest **requests = calloc(size, sizeof(*requests));
    CausalRequest **matches = calloc(size, sizeof(*matches));
    if (!requests || !matches) {
        free(requests); free(matches); resource_failure(m); return false;
    }
    free(m->request_index); free(m->match_index);
    m->request_index = requests; m->match_index = matches; m->index_size = size;
    for (CausalRequest *a = m->requests; a; a = a->next) index_request(m, a);
    return true;
}
static CausalRequest *request(PeakGlobalBudgetCausal *m, size_t rank, uint64_t id) {
    size_t slot = request_bucket(rank, id, m->index_size);
    for (CausalRequest *p = m->request_index[slot]; p; p = p->request_next)
        if (p->rank == rank && p->id == id) return p;
    return NULL;
}
static CausalRequest *partner(PeakGlobalBudgetCausal *m, CausalRequest *a) {
    if (!point_to_point(a)) return NULL;
    CausalRequest *found = NULL;
    size_t slot = match_bucket(a, m->index_size);
    for (CausalRequest *b = m->match_index[slot]; b; b = b->match_next) {
        if (a == b || (b->operation == PEAK_CAUSAL_RECV) == (a->operation == PEAK_CAUSAL_RECV) ||
            a->rank != b->peer || a->peer != b->rank || a->context != b->context ||
            a->context_owner != b->context_owner || a->tag != b->tag || a->round != b->round) continue;
        if (found) { peak_global_budget_causal_fallback(m); return NULL; }
        found = b;
    }
    return found;
}

static void observe(PeakGlobalBudgetCausal *m, CausalRank *r, uint64_t cost) {
    if (cost <= r->observed) return;
    uint64_t delta = cost - r->observed;
    r->observed = cost;
    m->raw = add(m, m->raw, delta);
    if (m->sum_mode) {
        m->fallback_raw = add(m, m->fallback_raw, delta);
        m->confirmed = add(m, m->confirmed, delta);
        r->covered_floor = cost;
    } else m->unresolved = add(m, m->unresolved, delta);
}
bool peak_global_budget_causal_reserve_outside(PeakGlobalBudgetCausal *m,
                                              size_t rank, uint64_t cumulative) {
    if (rank >= m->world_size) return false;
    observe(m, &m->ranks[rank], cumulative);
    return true;
}
static bool order(PeakGlobalBudgetCausal *m, CausalRank *r) {
    for (CausalEvent *p = r->queue; p; p = p->next) {
        if (p->ordered) continue;
        if (p->event.sequence != r->ordered_sequence + 1) break;
        if (p->event.cumulative_cost_ns < r->ordered_cost ||
            (p->event.kind == PEAK_CAUSAL_MPI_EXIT && !r->mpi_depth)) {
            peak_global_budget_causal_fallback(m); m->permanent_sum = true; return false;
        }
        p->previous_cost = r->ordered_cost;
        p->ordered = true;
        if (r->blocked && p->event.kind == PEAK_CAUSAL_WAIT) m->cycle_dirty = true;
        r->ordered_cost = p->event.cumulative_cost_ns;
        r->ordered_sequence = p->event.sequence;
        if (p->event.kind == PEAK_CAUSAL_MPI_ENTER) ++r->mpi_depth;
        if (p->event.kind == PEAK_CAUSAL_MPI_EXIT) --r->mpi_depth;
    }
    return true;
}
static bool ready(PeakGlobalBudgetCausal *m, CausalEvent *p, uint64_t *dependency) {
    *dependency = 0;
    for (size_t i = 0; i < p->event.request_count; ++i) {
        CausalRequest *a = request(m, p->event.rank, p->requests[i]);
        if (!a || !a->waited) { peak_global_budget_causal_fallback(m); return true; }
        if (a->operation == PEAK_CAUSAL_COLLECTIVE) {
            CausalCollective *g = a->collective;
            if (!g) { peak_global_budget_causal_fallback(m); return true; }
            if (g->waited != g->context->count) return false;
            *dependency = maximum(*dependency, g->dependency);
        } else {
            CausalRequest *b = partner(m, a);
            if (!b || !b->waited) return m->sum_mode;
            *dependency = maximum(*dependency, maximum(b->post, b->wait));
        }
    }
    return true;
}
static void charge(PeakGlobalBudgetCausal *m, CausalEvent *p) {
    CausalRank *r = &m->ranks[p->event.rank];
    uint64_t floor = maximum(p->previous_cost, r->covered_floor);
    if (p->event.cumulative_cost_ns <= floor) return;
    uint64_t delta = p->event.cumulative_cost_ns - floor;
    /* On saturation the published risk is UINT64_MAX; retain saturated totals. */
    if (delta > m->unresolved) { m->saturated = true; m->unresolved = 0; }
    else m->unresolved -= delta;
    m->causal_raw = add(m, m->causal_raw, delta);
    r->height = add(m, r->height, delta);
}
static void remove_request(PeakGlobalBudgetCausal *m, CausalRequest *a) {
    if (a->previous) a->previous->next = a->next; else m->requests = a->next;
    if (a->next) a->next->previous = a->previous;
    size_t slot = request_bucket(a->rank, a->id, m->index_size);
    CausalRequest **at = &m->request_index[slot];
    while (*at != a) at = &(*at)->request_next;
    *at = a->request_next;
    if (point_to_point(a)) {
        slot = match_bucket(a, m->index_size);
        at = &m->match_index[slot];
        while (*at != a) at = &(*at)->match_next;
        *at = a->match_next;
    }
    --m->request_count;
    free(a);
}
static void retire_request(PeakGlobalBudgetCausal *m, CausalRequest *a) {
    CausalCollective *g = a->collective;
    if (g && g->completed == g->context->count) {
        for (size_t i = 0; i < g->context->count; ++i)
            if (g->members[i]) remove_request(m, g->members[i]);
        CausalCollective **at = &g->context->rounds;
        while (*at != g) at = &(*at)->next;
        *at = g->next; free(g->members); free(g);
    } else if (!g) {
        CausalRequest *b = partner(m, a);
        if (b && b->done) { remove_request(m, b); remove_request(m, a); }
    }
}
static void retire_completed(PeakGlobalBudgetCausal *m, CausalEvent *p) {
    if (p->event.kind == PEAK_CAUSAL_COMPLETE) {
        for (size_t i = 0; i < p->event.request_count; ++i) {
            CausalRequest *a = request(m, p->event.rank, p->requests[i]);
            if (a && a->done) retire_request(m, a);
        }
    }

}

static void enqueue(PeakGlobalBudgetCausal *m, size_t rank) {
    CausalRank *r = &m->ranks[rank];
    if (r->ready) return;
    size_t tail = (m->ready_head + m->ready_count) % m->world_size;
    m->ready_ranks[tail] = rank;
    ++m->ready_count;
    r->ready = true;
}
static void wait_ready(PeakGlobalBudgetCausal *m, CausalRequest *a) {
    if (m->blocked_count) m->cycle_dirty = true;
    if (a->collective) {
        CausalCollective *g = a->collective;
        if (g->waited == g->context->count)
            for (size_t i = 0; i < g->context->count; ++i) enqueue(m, g->context->members[i]);
    } else {
        CausalRequest *b = partner(m, a);
        if (b && b->waited) enqueue(m, b->rank);
    }
}
static void resolve_receive(PeakGlobalBudgetCausal *m, CausalEvent *p) {
    PeakGlobalBudgetCausalEvent *e = &p->event;
    if (e->kind != PEAK_CAUSAL_COMPLETE || e->operation != PEAK_CAUSAL_RECV ||
        e->request_count != 1) return;
    CausalRequest *a = request(m, e->rank, p->requests[0]);
    if (!a || a->operation != PEAK_CAUSAL_RECV || !a->unresolved_match) return;
    if (e->peer >= m->world_size || e->tag == UINT64_MAX || !e->round) {
        peak_global_budget_causal_fallback(m); return;
    }
    size_t slot = match_bucket(a, m->index_size);
    CausalRequest **at = &m->match_index[slot];
    while (*at != a) at = &(*at)->match_next;
    *at = a->match_next;
    a->peer = e->peer; a->tag = e->tag; a->round = e->round;
    a->unresolved_match = false;
    slot = match_bucket(a, m->index_size);
    a->match_next = m->match_index[slot]; m->match_index[slot] = a;
    wait_ready(m, a);
}
static bool process(PeakGlobalBudgetCausal *m, CausalEvent *p) {
    PeakGlobalBudgetCausalEvent *e = &p->event;
    CausalRank *r = &m->ranks[e->rank];
    uint64_t dependency = 0;
    resolve_receive(m, p);
    if (e->kind == PEAK_CAUSAL_FRONTIER && e->last_mpi_sequence != r->last_mpi_sequence)
        peak_global_budget_causal_fallback(m);
    if (!m->sum_mode && e->kind == PEAK_CAUSAL_COMPLETE && !ready(m, p, &dependency)) return false;
    if (e->kind == PEAK_CAUSAL_UNKNOWN ||
        (e->kind == PEAK_CAUSAL_POST && e->operation == PEAK_CAUSAL_AMBIGUOUS_SEND))
        peak_global_budget_causal_fallback(m);
    charge(m, p);
    if (e->kind == PEAK_CAUSAL_POST) {
        if (request(m, e->rank, e->request)) peak_global_budget_causal_fallback(m);
        else {
            if (!grow_indexes(m)) return false;
            CausalRequest *a = calloc(1, sizeof(*a));
            if (!a) { resource_failure(m); return false; }
            *a = (CausalRequest){ .rank=e->rank, .peer=e->peer, .id=e->request,
                .context_owner=e->context_owner, .context=e->context, .round=e->round, .tag=e->tag,
                .operation=e->operation, .post=r->height, .next=m->requests,
                .unresolved_match=e->operation == PEAK_CAUSAL_RECV &&
                    (e->peer >= m->world_size || e->tag == UINT64_MAX || !e->round) };
            if (m->requests) m->requests->previous = a;
            m->requests = a; ++m->request_count; index_request(m, a);
            if (a->operation == PEAK_CAUSAL_COLLECTIVE) register_collective(m, a);
            if (m->permanent_sum) return false;
            if (m->blocked_count) m->cycle_dirty = true;
        }
    }
    if (e->kind == PEAK_CAUSAL_WAIT) {
        for (size_t i = 0; i < e->request_count; ++i) {
            CausalRequest *a = request(m, e->rank, p->requests[i]);
            if (!a) peak_global_budget_causal_fallback(m);
            else if (!a->waited) {
                a->wait = r->height; a->waited = true;
                if (a->collective) {
                    ++a->collective->waited;
                    a->collective->dependency = maximum(a->collective->dependency,
                                                        maximum(a->post, a->wait));
                }
                wait_ready(m, a);
            }
        }
    }
    if (e->kind == PEAK_CAUSAL_COMPLETE) {
        if (!m->sum_mode) r->height = maximum(r->height, dependency);
        for (size_t i = 0; i < e->request_count; ++i) {
            CausalRequest *a = request(m, e->rank, p->requests[i]);
            if (a) {
                if (!a->done && a->collective) ++a->collective->completed;
                a->done = true;
            } else peak_global_budget_causal_fallback(m);
        }
    }
    if (e->kind != PEAK_CAUSAL_FRONTIER) r->last_mpi_sequence = e->sequence;
    m->confirmed = maximum(m->confirmed, r->height);
    if (e->kind == PEAK_CAUSAL_COMPLETE) retire_completed(m, p);
    return true;
}
/* Iterative DFS over real queued waits, rather than recursive rank-depth calls.
 * A missing peer is not a cycle: it remains unresolved until it arrives. */
static bool future_wait(CausalRank *r, uint64_t id) {
    for (CausalEvent *p = r->queue; p; p = p->next)
        if (p->ordered && p->event.kind == PEAK_CAUSAL_WAIT)
            for (size_t i = 0; i < p->event.request_count; ++i)
                if (p->requests[i] == id) return true;
    return false;
}
static bool cycle(PeakGlobalBudgetCausal *m) {
    memset(m->walk_state, 0, m->world_size);
    for (size_t start = 0; start < m->world_size; ++start) {
        if (m->walk_state[start]) continue;
        size_t depth = 1;
        m->walk[0] = (CausalWalk){ .rank = start };
        m->walk_state[start] = 1;
        while (depth) {
            CausalWalk *frame = &m->walk[depth - 1];
            CausalEvent *p = m->ranks[frame->rank].queue;
            bool descended = false;
            if (p && p->ordered && p->event.kind == PEAK_CAUSAL_COMPLETE) {
                while (frame->next_request < p->event.request_count) {
                    size_t i = frame->next_request++;
                    CausalRequest *a = request(m, frame->rank, p->requests[i]);
                    CausalRequest *b = a ? partner(m, a) : NULL;
                    if (m->sum_mode) return true;
                    if (!b || b->waited || !future_wait(&m->ranks[b->rank], b->id)) continue;
                    if (m->walk_state[b->rank] == 1) return true;
                    if (m->walk_state[b->rank] == 2) continue;
                    m->walk_state[b->rank] = 1;
                    m->walk[depth++] = (CausalWalk){ .rank = b->rank };
                    descended = true;
                    break;
                }
            }
            if (!descended) { m->walk_state[frame->rank] = 2; --depth; }
        }
    }
    return false;
}
static void wake_blocked(PeakGlobalBudgetCausal *m) {
    for (size_t i = 0; i < m->world_size; ++i)
        if (m->ranks[i].blocked) enqueue(m, i);
}
static void drain(PeakGlobalBudgetCausal *m) {
    for (;;) {
        while (m->ready_count) {
            size_t rank = m->ready_ranks[m->ready_head];
            m->ready_head = (m->ready_head + 1) % m->world_size;
            --m->ready_count;
            CausalRank *r = &m->ranks[rank];
            r->ready = false;
            CausalEvent *p;
            while ((p = r->queue) && p->ordered) {
                if (!process(m, p)) {
                    if (!r->blocked) {
                        r->blocked = true; ++m->blocked_count; m->cycle_dirty = true;
                    }
                    break;
                }
                if (r->blocked) { r->blocked = false; --m->blocked_count; }
                r->queue = p->next;
                r->processed_sequence = p->event.sequence;
                free(p->requests); free(p);
                if (m->permanent_sum) return;
            }
            if (m->permanent_sum) return;
        }
        if (m->sum_mode) {
            if (!m->blocked_count) return;
            wake_blocked(m);
            continue;
        }
        if (!m->cycle_dirty || !m->blocked_count) return;
        m->cycle_dirty = false;
        if (!cycle(m)) return;
        peak_global_budget_causal_fallback(m);
        wake_blocked(m);
    }
}

bool peak_global_budget_causal_submit(PeakGlobalBudgetCausal *m,
                                     const PeakGlobalBudgetCausalEvent *e) {
    if (e->rank >= m->world_size || !e->sequence) return false;
    CausalRank *r = &m->ranks[e->rank];
    observe(m, r, e->cumulative_cost_ns);
    if (m->permanent_sum) return false;
    if (e->sequence <= r->processed_sequence) return false;
    CausalEvent **at = &r->queue;
    while (*at && (*at)->event.sequence < e->sequence) at = &(*at)->next;
    if (*at && (*at)->event.sequence == e->sequence) return false;
    if ((e->request_count && !e->requests) ||
        e->request_count > SIZE_MAX / sizeof(uint64_t)) {
        peak_global_budget_causal_fallback(m); m->permanent_sum = true; return false;
    }
    CausalEvent *p = calloc(1, sizeof(*p));
    if (p && e->request_count) p->requests = malloc(e->request_count * sizeof(uint64_t));
    if (!p || (e->request_count && !p->requests)) {
        free(p); resource_failure(m); return false;
    }
    p->event = *e;
    if (e->request_count) memcpy(p->requests, e->requests, e->request_count * sizeof(uint64_t));
    p->event.requests = p->requests;
    p->next = *at; *at = p;
    if (!order(m, r)) return false;
    if (r->blocked && e->kind == PEAK_CAUSAL_WAIT) m->cycle_dirty = true;
    enqueue(m, e->rank);
    drain(m);
    return !m->resource_failed;
}
void peak_global_budget_causal_serialized(PeakGlobalBudgetCausal *m, uint64_t cost) {
    m->raw = add(m, m->raw, cost);
    m->serialized = add(m, m->serialized, cost);
}
static bool checkpoint(PeakGlobalBudgetCausal *m, bool entry_fence) {
    /* This public boundary is not called from process()/partner(). Draining
     * here is safe; draining inside fallback() would free their live records. */
    if (m->sum_mode && !m->permanent_sum) { wake_blocked(m); drain(m); }
    if (m->permanent_sum || m->saturated || m->unresolved) return false;
    for (size_t i = 0; i < m->world_size; ++i)
        if (m->ranks[i].queue || m->ranks[i].mpi_depth) return false;
    for (CausalRequest *p = m->requests; p; p = p->next) {
        /* Without a true all-world entry fence, even a completed standard
         * send may depend on a held post-cut receive (rendezvous). */
        if (!entry_fence || !p->done || p->operation == PEAK_CAUSAL_RECV ||
            p->operation == PEAK_CAUSAL_COLLECTIVE) return false;
    }
    for (CausalContext *c = m->contexts; c; c = c->next)
        if (c->rounds) return false;
    confirm(m);
    for (size_t i = 0; i < m->world_size; ++i) m->ranks[i].height = m->confirmed;
    for (CausalRequest *p = m->requests; p; p = p->next)
        p->post = p->wait = m->confirmed;
    m->sum_mode = false;
    return true;
}
bool peak_global_budget_causal_checkpoint(PeakGlobalBudgetCausal *m) {
    return checkpoint(m, false);
}
bool peak_global_budget_causal_checkpoint_after_entry_fence(PeakGlobalBudgetCausal *m) {
    return checkpoint(m, true);
}
PeakGlobalBudgetCausalView peak_global_budget_causal_view(const PeakGlobalBudgetCausal *m) {
    PeakGlobalBudgetCausalView v = { .raw_cost_ns=m->raw, .causal_raw_ns=m->causal_raw,
        .fallback_raw_ns=m->fallback_raw, .confirmed_ns=m->confirmed,
        .unresolved_ns=m->unresolved, .serialized_ns=m->serialized,
        .sum_mode=m->sum_mode, .resource_failed=m->resource_failed, .saturated=m->saturated };
    for (CausalRequest *p = m->requests; p; p = p->next) v.outstanding_requests += !p->done;
    if (m->saturated || m->unresolved > UINT64_MAX - m->confirmed ||
        m->serialized > UINT64_MAX - (m->confirmed + m->unresolved)) {
        v.effective_ns = UINT64_MAX; v.saturated = true;
    } else v.effective_ns = m->confirmed + m->unresolved + m->serialized;
    return v;
}
