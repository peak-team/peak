#include "internal/mpi_cost_model.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NONE UINT32_MAX
#define TOMBSTONE (UINT32_MAX - 1u)

typedef struct {
    uint64_t generation, search_stamp;
    uint32_t free_next, active_index, out, in, pins, component, input_count;
    bool active, defined, sealed, settled, retire, visited, group_identity_valid, root_selected;
    PeakMpiCostNodeSpec spec;
    double delay;
} CostNode;

typedef struct {
    uint64_t generation;
    uint32_t free_next, active_index, rank, references, component, root_references;
    bool active, defined, frozen, conflict;
    double L, P;
} CostOwner;

typedef struct {
    uint32_t source, target, out_next, out_previous, in_next, in_previous;
    uint32_t component_next, free_next, active_index, hash_slot;
    PeakMpiCostEdgeKind kind;
    bool active, retained;
    double wait;
} CostEdge;

typedef struct {
    uint64_t generation;
    uint32_t free_next, active_index;
    bool active;
    PeakMpiCostRef node;
    PeakMpiCostBranchKind kind;
} CostBranch;

typedef struct {
    bool continuation_known, raw_known;
    PeakMpiCostRef continuation;
    PeakMpiCostRawCut raw;
    double accounted_L, accounted_P;
} CostRank;

typedef struct {
    uint32_t nodes, indegree, out, group;
    bool ready, cyclic, all_slack, entries_qualified, counted;
    double seed, weight, value, max_input, min_wait;
} CostComponent;

typedef struct { uint32_t node, edge; } CostFrame;
typedef enum {
    IDLE, RESET_NODES, RESET_EDGES, RESET_OWNERS, GROUP_SCAN, GROUP_INPUTS,
    FILTER_SCAN, FILTER_SEARCH, ORDER_SAVE, ORDER_RESET,
    ORDER_BUILD, ORDER_QUEUE, ORDER_DEPTH,
    DFS_SCAN, DFS_WALK, SCC_WALK, AGG_NODES,
    BLOCK_OWNERS, AGG_EDGES, CHECK_GROUPS, QUEUE_COMPONENTS, TOPOLOGY,
    COMMIT_NODES, FRONTIER_RANKS, FRONTIER_BRANCHES, FINISH
} CostPhase;

typedef enum {
    ROOT_NONE, ROOT_CLEAR_OWNERS, ROOT_CLEAR_NODES, ROOT_SEEDS,
    ROOT_CLOSURE, ROOT_PROJECT_NODES, ROOT_PROJECT_OWNERS,
    ROOT_PROJECT_EDGES, ROOT_SOLVE, ROOT_RESTORE
} RootPhase;

struct PeakMpiCostModel {
    PeakMpiCostConfig config;
    CostNode *nodes;
    CostOwner *owners;
    CostEdge *edges;
    CostBranch *branches;
    CostRank *ranks;
    CostComponent *components;
    CostFrame *stack;
    uint32_t *finish, *queue, *edge_hash;
    uint32_t *node_ids, *owner_ids, *edge_ids, *branch_ids, *continuation_ids;
    uint64_t *rank_seen;
    uint32_t hash_capacity, free_node, free_owner, free_edge, free_branch;
    uint32_t unused_node, unused_owner, unused_edge, unused_branch;
    uint32_t cursor, stack_size, finish_count, component_count, reverse_index;
    uint32_t scc_current, queue_head, queue_tail, search_edge, search_candidate;
    uint32_t topology_current, topology_edge, topology_done;
    uint32_t gc_phase, gc_cursor, gc_incident;
    uint32_t continuation_count;
    uint32_t group_node, group_edge, group_inputs;
    bool group_unknown, ordering, has_reverse, has_owner_conflict;
    bool requires_qualified_g_build, final_dag;
    uint64_t search_stamp, group_stamp;
    unsigned reset_stage;
    uint32_t reset_cursor;
    CostPhase phase;
    bool graph_dirty, frontier_dirty, frontier_valid, pending, all_closed;
    double frontier;
    PeakMpiCostStatus last_result;
    PeakMpiCostStats stats;
    uint32_t *root_nodes, *root_edges, *root_owners;
    RootPhase root_stage;
    uint32_t root_cursor, root_seed_count, root_head, root_tail, root_edge;
    uint32_t root_owner, root_n, root_e, root_o;
    size_t root_full_n, root_full_e, root_full_o;
    bool root_projected, root_swapped, root_reset, root_global_pending, root_global_closed;
    PeakMpiCostStatus root_result;
};

static bool nonnegative(double x) { return isfinite(x) && x >= 0.0; }
static bool no_owner(PeakMpiCostOwnerRef r) { return r.slot == NONE && r.generation == 0; }
static bool same_ref(PeakMpiCostRef a, PeakMpiCostRef b)
{ return a.slot == b.slot && a.generation == b.generation; }
static bool same_owner(PeakMpiCostOwnerRef a, PeakMpiCostOwnerRef b)
{ return a.slot == b.slot && a.generation == b.generation; }

static bool valid_node(const PeakMpiCostModel *m, PeakMpiCostRef r)
{
    return r.slot < m->config.max_nodes && m->nodes[r.slot].active &&
           m->nodes[r.slot].generation == r.generation;
}

static bool valid_owner(const PeakMpiCostModel *m, PeakMpiCostOwnerRef r)
{
    return r.slot < m->config.max_owners && m->owners[r.slot].active &&
           m->owners[r.slot].generation == r.generation;
}

static bool valid_branch(const PeakMpiCostModel *m, PeakMpiCostBranchRef r)
{
    return r.slot < m->config.max_branches && m->branches[r.slot].active &&
           m->branches[r.slot].generation == r.generation;
}

void peak_mpi_cost_model_fail(PeakMpiCostModel *m, PeakMpiCostReason reason,
                            uint64_t lost_events)
{
    if (!m) return;
    if (!m->stats.coarse) {
        m->stats.coarse = true;
        m->stats.reason = reason == PEAK_MPI_COST_REASON_NONE ?
                          PEAK_MPI_COST_INTERNAL_ERROR : reason;
    }
    if (UINT64_MAX - m->stats.lost_events < lost_events)
        m->stats.lost_events = UINT64_MAX;
    else
        m->stats.lost_events += lost_events;
    m->frontier_valid = false;
    /* Retain storage: callers can still hold native-operation references. */
}

static PeakMpiCostStatus fail(PeakMpiCostModel *m, PeakMpiCostReason reason)
{
    peak_mpi_cost_model_fail(m, reason, 0);
    return PEAK_MPI_COST_COARSE;
}

static PeakMpiCostStatus writable(PeakMpiCostModel *m)
{
    if (!m) return PEAK_MPI_COST_INVALID;
    if (m->stats.coarse) return PEAK_MPI_COST_COARSE;
    if (m->phase != IDLE || m->root_stage) return PEAK_MPI_COST_BUSY;
    return PEAK_MPI_COST_OK;
}

static void touch_graph(PeakMpiCostModel *m)
{
    m->graph_dirty = true;
    m->frontier_dirty = true;
    m->frontier_valid = false;
    m->gc_phase = m->gc_cursor = 0;
}

static void touch_frontier(PeakMpiCostModel *m)
{
    m->frontier_dirty = true;
    m->frontier_valid = false;
    m->gc_phase = m->gc_cursor = 0;
}

PeakMpiCostModel *peak_mpi_cost_model_create(const PeakMpiCostConfig *config)
{
    if (!config || !config->max_nodes || !config->max_edges ||
        !config->max_owners || !config->max_branches || !config->max_ranks ||
        !config->max_hash_probes || config->max_hash_probes > 64 ||
        config->max_nodes >= TOMBSTONE ||
        config->max_edges > UINT32_MAX / 8u ||
        config->max_owners >= TOMBSTONE || config->max_branches >= TOMBSTONE)
        return NULL;
    PeakMpiCostModel *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->config = *config;
    m->hash_capacity = 1;
    while (m->hash_capacity < config->max_edges * 4u) m->hash_capacity *= 2u;
    m->root_nodes = calloc(config->max_nodes, sizeof(uint32_t));
    m->root_edges = calloc(config->max_edges, sizeof(uint32_t));
    m->root_owners = calloc(config->max_owners, sizeof(uint32_t));
    m->nodes = calloc(config->max_nodes, sizeof(*m->nodes));
    m->owners = calloc(config->max_owners, sizeof(*m->owners));
    m->edges = calloc(config->max_edges, sizeof(*m->edges));
    m->branches = calloc(config->max_branches, sizeof(*m->branches));
    m->ranks = calloc(config->max_ranks, sizeof(*m->ranks));
    m->components = calloc(config->max_nodes, sizeof(*m->components));
    m->stack = calloc(config->max_nodes, sizeof(*m->stack));
    m->finish = calloc(config->max_nodes, sizeof(*m->finish));
    m->queue = calloc(config->max_nodes, sizeof(*m->queue));
    m->edge_hash = calloc(m->hash_capacity, sizeof(*m->edge_hash));
    m->node_ids = calloc(config->max_nodes, sizeof(*m->node_ids));
    m->owner_ids = calloc(config->max_owners, sizeof(*m->owner_ids));
    m->edge_ids = calloc(config->max_edges, sizeof(*m->edge_ids));
    m->branch_ids = calloc(config->max_branches, sizeof(*m->branch_ids));
    m->continuation_ids = calloc(config->max_ranks, sizeof(*m->continuation_ids));
    m->rank_seen = calloc(config->max_ranks, sizeof(*m->rank_seen));
    if (!m->root_nodes || !m->root_edges || !m->root_owners || !m->nodes || !m->owners || !m->edges || !m->branches || !m->ranks ||
        !m->components || !m->stack || !m->finish || !m->queue || !m->edge_hash ||
        !m->node_ids || !m->owner_ids || !m->edge_ids || !m->branch_ids ||
        !m->continuation_ids || !m->rank_seen) {
        peak_mpi_cost_model_destroy(m);
        return NULL;
    }
    /* Virgin slots are already zeroed. Build links only when an allocated
     * slot is reclaimed; untouched capacity needs no eager list sweep. */
    m->free_node = m->free_owner = m->free_edge = m->free_branch = NONE;
    for (uint32_t i = 0; i < m->hash_capacity; ++i) m->edge_hash[i] = NONE;
    m->frontier_valid = true;
    m->last_result = PEAK_MPI_COST_OK;
    return m;
}

void peak_mpi_cost_model_destroy(PeakMpiCostModel *m)
{
    if (!m) return;
    free(m->root_nodes); free(m->root_edges); free(m->root_owners);
    free(m->nodes); free(m->owners); free(m->edges); free(m->branches);
    free(m->ranks); free(m->components); free(m->stack); free(m->finish);
    free(m->queue); free(m->edge_hash); free(m->node_ids); free(m->owner_ids);
    free(m->edge_ids); free(m->branch_ids); free(m->continuation_ids);
    free(m->rank_seen); free(m);
}

PeakMpiCostStatus peak_mpi_cost_model_reserve_owner(PeakMpiCostModel *m,
                                                 PeakMpiCostOwnerRef *out)
{
    PeakMpiCostStatus status = writable(m);
    if (status != PEAK_MPI_COST_OK) return status;
    if (!out) return PEAK_MPI_COST_INVALID;
    if (m->free_owner == NONE && m->unused_owner == m->config.max_owners)
        return fail(m, PEAK_MPI_COST_OWNER_CAPACITY);
    uint32_t i = m->free_owner != NONE ? m->free_owner : m->unused_owner;
    CostOwner *o = &m->owners[i];
    if (o->generation == UINT64_MAX) return fail(m, PEAK_MPI_COST_GENERATION_OVERFLOW);
    uint64_t generation = o->generation + 1;
    if (m->free_owner != NONE) m->free_owner = o->free_next;
    else ++m->unused_owner;
    memset(o, 0, sizeof(*o));
    o->active = true;
    o->generation = generation;
    o->component = NONE;
    o->active_index = m->stats.active_owners;
    m->owner_ids[o->active_index] = i;
    *out = (PeakMpiCostOwnerRef){i, generation};
    ++m->stats.active_owners;
    touch_graph(m);
    return PEAK_MPI_COST_OK;
}

PeakMpiCostStatus peak_mpi_cost_model_define_owner(PeakMpiCostModel *m,
                                                PeakMpiCostOwnerRef ref,
                                                uint32_t rank, double L, double P)
{
    PeakMpiCostStatus status = writable(m);
    if (status != PEAK_MPI_COST_OK) return status;
    if (!valid_owner(m, ref)) return fail(m, PEAK_MPI_COST_INVALID_REFERENCE);
    if (rank >= m->config.max_ranks || !nonnegative(L) || !nonnegative(P) || P > L)
        return fail(m, PEAK_MPI_COST_INVALID_COST);
    CostOwner *o = &m->owners[ref.slot];
    if (o->defined)
        return o->rank == rank && o->L == L && o->P == P ? PEAK_MPI_COST_OK :
               fail(m, PEAK_MPI_COST_CONFLICTING_DEFINITION);
    CostRank *r = &m->ranks[rank];
    if (!isfinite(r->accounted_L + L) || !isfinite(r->accounted_P + P))
        return fail(m, PEAK_MPI_COST_INVALID_COST);
    o->defined = true; o->rank = rank; o->L = L; o->P = P;
    r->accounted_L += L; r->accounted_P += P;
    touch_graph(m);
    return PEAK_MPI_COST_OK;
}

PeakMpiCostStatus peak_mpi_cost_model_reserve_node(PeakMpiCostModel *m,
                                                PeakMpiCostRef *out)
{
    PeakMpiCostStatus status = writable(m);
    if (status != PEAK_MPI_COST_OK) return status;
    if (!out) return PEAK_MPI_COST_INVALID;
    if (m->free_node == NONE && m->unused_node == m->config.max_nodes)
        return fail(m, PEAK_MPI_COST_NODE_CAPACITY);
    uint32_t i = m->free_node != NONE ? m->free_node : m->unused_node;
    CostNode *n = &m->nodes[i];
    if (n->generation == UINT64_MAX) return fail(m, PEAK_MPI_COST_GENERATION_OVERFLOW);
    uint64_t generation = n->generation + 1;
    if (m->free_node != NONE) m->free_node = n->free_next;
    else ++m->unused_node;
    memset(n, 0, sizeof(*n));
    n->active = true; n->generation = generation;
    n->in = n->out = n->component = NONE;
    n->active_index = m->stats.active_nodes;
    m->node_ids[n->active_index] = i;
    n->spec.owner = PEAK_MPI_COST_NO_OWNER;
    *out = (PeakMpiCostRef){i, generation};
    ++m->stats.active_nodes;
    uint32_t pending = m->stats.active_nodes - m->stats.settled_nodes;
    if (pending > m->stats.peak_pending_nodes) m->stats.peak_pending_nodes = pending;
    touch_graph(m);
    return PEAK_MPI_COST_OK;
}

static bool same_spec(const PeakMpiCostNodeSpec *a, const PeakMpiCostNodeSpec *b)
{
    return a->kind == b->kind && a->rank == b->rank && same_owner(a->owner, b->owner) &&
           a->seed_delay == b->seed_delay && a->participants == b->participants &&
           a->single_execution_flow == b->single_execution_flow &&
           a->progress_clear == b->progress_clear && a->s1 == b->s1;
}

PeakMpiCostStatus peak_mpi_cost_model_define_node(PeakMpiCostModel *m,
                                               PeakMpiCostRef ref,
                                               const PeakMpiCostNodeSpec *spec)
{
    PeakMpiCostStatus status = writable(m);
    if (status != PEAK_MPI_COST_OK) return status;
    if (!spec) return PEAK_MPI_COST_INVALID;
    if (!valid_node(m, ref)) return fail(m, PEAK_MPI_COST_INVALID_REFERENCE);
    CostNode *n = &m->nodes[ref.slot];
    if (n->defined)
        return same_spec(&n->spec, spec) ? PEAK_MPI_COST_OK :
               fail(m, PEAK_MPI_COST_CONFLICTING_DEFINITION);
    if ((spec->kind != PEAK_MPI_COST_EVENT && spec->kind != PEAK_MPI_COST_COLLECTIVE) ||
        (spec->rank >= m->config.max_ranks && spec->rank != PEAK_MPI_COST_NO_RANK) ||
        !nonnegative(spec->seed_delay)) return fail(m, PEAK_MPI_COST_INVALID_COST);
    if (spec->kind == PEAK_MPI_COST_COLLECTIVE &&
        (!no_owner(spec->owner) || spec->seed_delay != 0 || !spec->participants))
        return fail(m, PEAK_MPI_COST_CONFLICTING_DEFINITION);
    if (!no_owner(spec->owner)) {
        if (!valid_owner(m, spec->owner)) return fail(m, PEAK_MPI_COST_INVALID_REFERENCE);
        CostOwner *o = &m->owners[spec->owner.slot];
        if (o->frozen) return fail(m, PEAK_MPI_COST_CLOSED_DEPENDENCIES);
        if (o->defined && o->rank != spec->rank)
            return fail(m, PEAK_MPI_COST_CONFLICTING_DEFINITION);
        ++o->references;
    }
    n->spec = *spec; n->defined = true;
    touch_graph(m);
    return PEAK_MPI_COST_OK;
}

static uint64_t mix(uint64_t x)
{
    x ^= x >> 30; x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27; x *= UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

PeakMpiCostStatus peak_mpi_cost_model_add_edge(PeakMpiCostModel *m,
                                            PeakMpiCostRef source,
                                            PeakMpiCostRef target,
                                            PeakMpiCostEdgeKind kind, double wait)
{
    PeakMpiCostStatus status = writable(m);
    if (status != PEAK_MPI_COST_OK) return status;
    if (!valid_node(m, source) || !valid_node(m, target))
        return fail(m, PEAK_MPI_COST_INVALID_REFERENCE);
    if (kind < PEAK_MPI_COST_GUARANTEED || kind > PEAK_MPI_COST_COLLECTIVE_CANDIDATE ||
        !nonnegative(wait) || (kind != PEAK_MPI_COST_SLACK_INPUT && wait != 0))
        return fail(m, PEAK_MPI_COST_INVALID_COST);
    uint64_t hash = mix(source.generation ^ ((uint64_t)source.slot << 32)) ^
                    mix(target.generation ^ target.slot) ^ mix((uint64_t)kind);
    uint32_t slot = (uint32_t)hash & (m->hash_capacity - 1u), insertion = NONE;
    uint32_t probes = m->config.max_hash_probes < m->hash_capacity ?
                      m->config.max_hash_probes : m->hash_capacity;
    for (uint32_t p = 0; p < probes; ++p, slot = (slot + 1u) & (m->hash_capacity - 1u)) {
        uint32_t i = m->edge_hash[slot];
        if (i == TOMBSTONE) { if (insertion == NONE) insertion = slot; continue; }
        if (i == NONE) { if (insertion == NONE) insertion = slot; break; }
        CostEdge *e = &m->edges[i];
        if (e->source == source.slot && e->target == target.slot && e->kind == kind)
            return e->wait == wait ? PEAK_MPI_COST_OK :
                   fail(m, PEAK_MPI_COST_CONFLICTING_DEFINITION);
    }
    if (insertion == NONE) return fail(m, PEAK_MPI_COST_INDEX_CAPACITY);
    if (m->free_edge == NONE && m->unused_edge == m->config.max_edges)
        return fail(m, PEAK_MPI_COST_EDGE_CAPACITY);
    CostNode *a = &m->nodes[source.slot], *b = &m->nodes[target.slot];
    if (b->sealed || b->settled || b->retire || a->retire)
        return fail(m, PEAK_MPI_COST_CLOSED_DEPENDENCIES);
    uint32_t i = m->free_edge != NONE ? m->free_edge : m->unused_edge;
    CostEdge *e = &m->edges[i];
    if (m->free_edge != NONE) m->free_edge = e->free_next;
    else ++m->unused_edge;
    memset(e, 0, sizeof(*e));
    e->active = true; e->source = source.slot; e->target = target.slot;
    e->active_index = m->stats.active_edges;
    m->edge_ids[e->active_index] = i;
    e->kind = kind; e->wait = wait; e->hash_slot = insertion;
    e->out_previous = e->in_previous = NONE;
    e->out_next = a->out; e->in_next = b->in;
    if (a->out != NONE) m->edges[a->out].out_previous = i;
    if (b->in != NONE) m->edges[b->in].in_previous = i;
    a->out = b->in = i;
    m->edge_hash[insertion] = i;
    ++m->stats.active_edges;
    touch_graph(m);
    return PEAK_MPI_COST_OK;
}

PeakMpiCostStatus peak_mpi_cost_model_seal(PeakMpiCostModel *m, PeakMpiCostRef ref)
{
    PeakMpiCostStatus status = writable(m);
    if (status != PEAK_MPI_COST_OK) return status;
    if (!valid_node(m, ref)) return fail(m, PEAK_MPI_COST_INVALID_REFERENCE);
    if (!m->nodes[ref.slot].sealed) {
        m->nodes[ref.slot].sealed = true;
        touch_graph(m);
    }
    return PEAK_MPI_COST_OK;
}

/* Historical committed nodes are immutable summary anchors. Their old
 * incoming graph is no longer needed for settlement of a new prefix. */
static bool selected(const PeakMpiCostModel *m, const CostEdge *e)
{
    if (m->root_projected &&
        (!m->nodes[e->source].root_selected || !m->nodes[e->target].root_selected))
        return false;
    /* Use precisely the guarantee predicate of the fallback path search. */
    if (m->ordering)
        return e->active && (e->kind == PEAK_MPI_COST_GUARANTEED ||
               (e->kind == PEAK_MPI_COST_SLACK_INPUT &&
                m->nodes[e->target].defined &&
                m->nodes[e->target].spec.kind == PEAK_MPI_COST_COLLECTIVE &&
                m->nodes[e->target].group_identity_valid));
    return e->active && e->retained && !m->nodes[e->target].settled;
}

static void search_start(PeakMpiCostModel *m, uint32_t i)
{
    CostEdge *e = &m->edges[i];
    if (m->search_stamp == UINT64_MAX) {
        fail(m, PEAK_MPI_COST_GENERATION_OVERFLOW);
        return;
    }
    ++m->search_stamp;
    m->final_dag = false;
    m->search_candidate = i;
    m->search_edge = NONE;
    m->queue_head = 0; m->queue_tail = 1;
    /* G has target -> source exactly when reversed G has source -> target.
     * Searching predecessors avoids unrelated already-parsed future branches. */
    m->queue[0] = e->source;
    m->nodes[e->source].search_stamp = m->search_stamp;
    if (e->source == e->target) {
        e->retained = false; ++m->stats.excluded_candidates;
    } else {
        m->phase = FILTER_SEARCH;
    }
}

static void search_step(PeakMpiCostModel *m)
{
    CostEdge *candidate = &m->edges[m->search_candidate];
    if (m->search_edge != NONE) {
        CostEdge *e = &m->edges[m->search_edge];
        m->search_edge = e->in_next;
        bool guaranteed = e->kind == PEAK_MPI_COST_GUARANTEED ||
                          (e->kind == PEAK_MPI_COST_SLACK_INPUT &&
                           m->nodes[e->target].defined &&
                           m->nodes[e->target].spec.kind == PEAK_MPI_COST_COLLECTIVE &&
                           m->nodes[e->target].group_identity_valid);
        if (e->active && guaranteed) {
            CostNode *n = &m->nodes[e->source];
            /* Reaching the target's guaranteed SCC proves the path. A
             * predecessor below its level cannot lie on such a path. */
            if (n->component == m->nodes[candidate->target].component) {
                candidate->retained = false; ++m->stats.excluded_candidates;
                m->phase = FILTER_SCAN;
            } else if (n->input_count >= m->nodes[candidate->target].input_count &&
                       n->search_stamp != m->search_stamp) {
                n->search_stamp = m->search_stamp;
                m->queue[m->queue_tail++] = e->source;
            }
        }
    } else if (m->queue_head < m->queue_tail) {
        uint32_t i = m->queue[m->queue_head++];
        m->search_edge = m->nodes[i].in;
    } else {
        candidate->retained = true; ++m->stats.retained_candidates;
        m->phase = FILTER_SCAN;
    }
}

/* Fixed scratch is shared with later ordering/topology phases. During DFS,
 * queue holds discovery indices and finish is the active Tarjan stack. */
static void dfs_push(PeakMpiCostModel *m, uint32_t i)
{
    CostNode *n = &m->nodes[i];
    n->visited = true;
    m->queue[i] = ++m->reverse_index;
    n->input_count = m->queue[i];
    m->finish[m->finish_count++] = i;
    m->stack[m->stack_size++] = (CostFrame){i, n->out};
}
static void dfs_return(PeakMpiCostModel *m)
{
    uint32_t child = m->stack[--m->stack_size].node;
    if (m->stack_size && m->nodes[child].component == NONE) {
        CostNode *parent = &m->nodes[m->stack[m->stack_size - 1u].node];
        if (m->nodes[child].input_count < parent->input_count)
            parent->input_count = m->nodes[child].input_count;
    }
    m->phase = m->stack_size ? DFS_WALK : DFS_SCAN;
}
static void aggregate_node(PeakMpiCostModel *m, uint32_t i)
{
    CostNode *n = &m->nodes[i];
    if (!n->active) return;
    CostComponent *c = &m->components[n->component];
    if (n->settled) {
        if (n->delay > c->seed) c->seed = n->delay;
        return;
    }
    if (!n->defined || !n->sealed) { c->ready = false; m->all_closed = false; }
    if (!n->defined) return;
    if (n->spec.seed_delay > c->seed) c->seed = n->spec.seed_delay;
    if (n->spec.kind == PEAK_MPI_COST_COLLECTIVE) c->group = i;
    if (!no_owner(n->spec.owner)) {
        if (!valid_owner(m, n->spec.owner)) { fail(m, PEAK_MPI_COST_INVALID_REFERENCE); return; }
        CostOwner *o = &m->owners[n->spec.owner.slot];
        if (!o->defined) { c->ready = false; m->all_closed = false; return; }
        if (o->rank != n->spec.rank) { fail(m, PEAK_MPI_COST_CONFLICTING_DEFINITION); return; }
        if (o->component == NONE) {
            o->component = n->component;
            c->weight += o->L;
            if (!isfinite(c->weight)) fail(m, PEAK_MPI_COST_INVALID_COST);
        } else if (o->component != n->component) {
            o->conflict = true;
            m->has_owner_conflict = true;
        }
    }
}

static void aggregate_edge(PeakMpiCostModel *m, uint32_t i)
{
    CostEdge *e = &m->edges[i];
    if (!selected(m, e)) return;
    CostNode *a = &m->nodes[e->source], *b = &m->nodes[e->target];
    if (e->kind == PEAK_MPI_COST_SLACK_INPUT && b->defined &&
        b->spec.kind != PEAK_MPI_COST_COLLECTIVE) {
        fail(m, PEAK_MPI_COST_CONFLICTING_DEFINITION);
        return;
    }
    if (b->defined && b->spec.kind == PEAK_MPI_COST_COLLECTIVE &&
        (e->kind == PEAK_MPI_COST_SLACK_INPUT ||
         e->kind == PEAK_MPI_COST_COLLECTIVE_CANDIDATE)) ++b->input_count;
    CostComponent *source = &m->components[a->component];
    CostComponent *target = &m->components[b->component];
    if (a->component == b->component) {
        if (e->source == e->target) source->cyclic = true;
        return;
    }
    ++target->indegree;
    e->component_next = source->out;
    source->out = i;
    if (e->kind != PEAK_MPI_COST_SLACK_INPUT) target->all_slack = false;
    if (!a->defined || !a->spec.single_execution_flow || !a->spec.progress_clear)
        target->entries_qualified = false;
}

static void topology_step(PeakMpiCostModel *m)
{
    if (m->topology_current == NONE) {
        if (m->queue_head == m->queue_tail) {
            if (m->topology_done != m->component_count) { fail(m, PEAK_MPI_COST_INTERNAL_ERROR); return; }
            m->phase = COMMIT_NODES; m->cursor = 0;
            return;
        }
        uint32_t i = m->queue[m->queue_head++];
        CostComponent *c = &m->components[i];
        double merged = c->max_input > c->seed ? c->max_input : c->seed;
        if (c->group != NONE && c->nodes == 1 && !c->cyclic && c->ready &&
            c->all_slack && c->entries_qualified &&
            m->nodes[c->group].group_identity_valid &&
            c->min_wait < merged) merged = c->min_wait;
        c->value = merged + c->weight;
        if (!isfinite(c->value)) { fail(m, PEAK_MPI_COST_INVALID_COST); return; }
        ++m->topology_done;
        m->topology_current = i; m->topology_edge = c->out;
    } else if (m->topology_edge != NONE) {
        CostEdge *e = &m->edges[m->topology_edge];
        m->topology_edge = e->component_next;
        CostComponent *a = &m->components[m->topology_current];
        CostComponent *b = &m->components[m->nodes[e->target].component];
        if (a->value > b->max_input) b->max_input = a->value;
        if (!a->ready) b->ready = false;
        if (e->kind == PEAK_MPI_COST_SLACK_INPUT) {
            double upper = a->value + e->wait;
            if (upper < b->min_wait) b->min_wait = upper;
        }
        if (--b->indegree == 0 && !m->final_dag)
            m->queue[m->queue_tail++] = m->nodes[e->target].component;
    } else {
        m->topology_current = NONE;
    }
}

static void frontier_ref(PeakMpiCostModel *m, PeakMpiCostRef ref)
{
    if (!valid_node(m, ref)) { fail(m, PEAK_MPI_COST_INVALID_REFERENCE); return; }
    CostNode *n = &m->nodes[ref.slot];
    if (!n->settled) m->frontier_valid = false;
    else if (n->delay > m->frontier) m->frontier = n->delay;
}

static void run_step(PeakMpiCostModel *m)
{
    uint32_t i;
    switch (m->phase) {
    case RESET_NODES:
        if (m->cursor < m->stats.active_nodes) {
            uint32_t component = m->cursor;
            i = m->node_ids[m->cursor++];
            m->nodes[i].visited = false; m->nodes[i].component = NONE;
            m->nodes[i].input_count = 0;
            if (!m->nodes[i].settled) m->nodes[i].group_identity_valid = false;
            memset(&m->components[component], 0, sizeof(m->components[component]));
            m->components[component].out = m->components[component].group = NONE;
            m->components[component].ready = m->components[component].all_slack = true;
            m->components[component].entries_qualified = true;
            m->components[component].min_wait = INFINITY;
        } else { m->phase = RESET_EDGES; m->cursor = 0; }
        break;
    case RESET_EDGES:
        if (m->cursor < m->stats.active_edges) {
            uint32_t at = m->edge_ids[m->cursor++];
            CostEdge *e = &m->edges[at];
            e->retained = e->kind != PEAK_MPI_COST_SEND_REVERSE;
            if (e->kind == PEAK_MPI_COST_SEND_REVERSE) m->has_reverse = true;
            if (e->kind == PEAK_MPI_COST_SLACK_INPUT) m->requires_qualified_g_build = true;
            e->component_next = NONE;
            /* Singleton G adjacency can be prepared while edges are hot.
             * SLACK_INPUT qualification is not known until GROUP_SCAN. */
            if (e->kind == PEAK_MPI_COST_GUARANTEED && e->source != e->target) {
                uint32_t source = m->nodes[e->source].active_index;
                uint32_t target = m->nodes[e->target].active_index;
                e->component_next = m->components[source].out;
                m->components[source].out = at;
                ++m->components[target].indegree;
            }
        } else { m->phase = RESET_OWNERS; m->cursor = 0; }
        break;
    case RESET_OWNERS:
        if (m->cursor < m->stats.active_owners) {
            CostOwner *o = &m->owners[m->owner_ids[m->cursor++]];
            o->component = NONE; o->conflict = false;
            /* Definitions/references cannot change while a run is busy. */
            if (!o->defined || !o->references) {
                m->pending = true;
                m->all_closed = false;
            }
        } else { m->phase = GROUP_SCAN; m->cursor = 0; }
        break;
    case GROUP_SCAN:
        if (m->cursor < m->stats.active_nodes) {
            i = m->node_ids[m->cursor++];
            CostNode *n = &m->nodes[i];
            if (!m->has_reverse || m->requires_qualified_g_build) {
                m->components[n->active_index].out = NONE;
                m->components[n->active_index].indegree = 0;
            }
            if (m->has_reverse) {
                n->component = n->active_index;
                m->components[n->active_index].nodes = 1;
            }
            if (!n->settled && n->defined && n->spec.kind == PEAK_MPI_COST_COLLECTIVE && n->spec.s1) {
                if (m->group_stamp == UINT64_MAX) { fail(m, PEAK_MPI_COST_GENERATION_OVERFLOW); break; }
                ++m->group_stamp;
                m->group_node = i; m->group_edge = n->in; m->group_inputs = 0;
                m->group_unknown = false;
                m->phase = GROUP_INPUTS;
            }
        } else {
            m->ordering = m->has_reverse;
            m->component_count = m->ordering ? m->stats.active_nodes : 0;
            m->phase = m->ordering
                ? (m->requires_qualified_g_build ? ORDER_BUILD : ORDER_QUEUE)
                : DFS_SCAN;
            m->queue_head = m->queue_tail = m->topology_done = 0;
            m->cursor = m->reverse_index = 0;
        }
        break;
    case GROUP_INPUTS:
        if (m->group_edge != NONE) {
            CostEdge *e = &m->edges[m->group_edge];
            m->group_edge = e->in_next;
            if (e->kind == PEAK_MPI_COST_SLACK_INPUT || e->kind == PEAK_MPI_COST_COLLECTIVE_CANDIDATE) {
                ++m->group_inputs;
                CostNode *entry = &m->nodes[e->source];
                if (!entry->defined) m->group_unknown = true;
                else if (entry->spec.rank >= m->config.max_ranks ||
                         m->rank_seen[entry->spec.rank] == m->group_stamp)
                    fail(m, PEAK_MPI_COST_CONFLICTING_DEFINITION);
                else m->rank_seen[entry->spec.rank] = m->group_stamp;
            }
        } else {
            CostNode *n = &m->nodes[m->group_node];
            if (n->sealed && m->group_inputs != n->spec.participants)
                fail(m, PEAK_MPI_COST_CONFLICTING_DEFINITION);
            else n->group_identity_valid = n->sealed && !m->group_unknown &&
                                          m->group_inputs == n->spec.participants;
            m->phase = GROUP_SCAN;
        }
        break;
    case FILTER_SCAN:
        if (m->cursor < m->stats.active_edges) {
            i = m->edge_ids[m->cursor++];
            if (m->edges[i].active && m->edges[i].kind == PEAK_MPI_COST_SEND_REVERSE) {
                CostEdge *e = &m->edges[i];
                /* A path target -> source strictly increases SCC depth. */
                uint32_t source = m->nodes[e->source].input_count;
                uint32_t target = m->nodes[e->target].input_count;
                if (m->nodes[e->target].component == m->nodes[e->source].component) {
                    e->retained = false;
                    ++m->stats.excluded_candidates;
                } else if (target >= source) {
                    e->retained = true;
                    ++m->stats.retained_candidates;
                } else search_start(m, i);
            }
            /* Strict G levels order every effective final edge, proving no
             * final SCC can contain more than one node or a self-loop. */
            CostEdge *edge = &m->edges[i];
            if (m->final_dag && selected(m, edge) &&
                m->nodes[edge->source].input_count >= m->nodes[edge->target].input_count)
                m->final_dag = false;
        } else { m->phase = ORDER_RESET; m->cursor = 0; }
        break;
    case FILTER_SEARCH: search_step(m); break;
    case DFS_SCAN:
        if (m->cursor < m->stats.active_nodes) {
            i = m->node_ids[m->cursor++];
            if (m->nodes[i].active && !m->nodes[i].visited) {
                dfs_push(m, i); m->phase = DFS_WALK;
            }
        } else {
            m->phase = m->ordering ? ORDER_BUILD : AGG_NODES;
            m->cursor = 0;
        }
        break;
    case DFS_WALK: {
        CostFrame *f = &m->stack[m->stack_size - 1u];
        CostNode *n = &m->nodes[f->node];
        if (f->edge != NONE) {
            CostEdge *e = &m->edges[f->edge]; f->edge = e->out_next;
            if (selected(m, e)) {
                CostNode *target = &m->nodes[e->target];
                if (!target->visited) dfs_push(m, e->target);
                else if (target->component == NONE &&
                         m->queue[e->target] < n->input_count)
                    n->input_count = m->queue[e->target];
            }
        } else if (n->input_count == m->queue[f->node]) {
            m->scc_current = m->component_count++;
            m->phase = SCC_WALK;
        } else dfs_return(m);
        break;
    }
    /* The guaranteed-only SCC DAG gives an exact necessary path condition.
     * Its longest-path levels do not depend on arbitrary DFS visitation order.
     * Reuse component scratch and node input_count until filtering finishes;
     * ORDER_RESET restores them before the ordinary settlement SCC pass. */
    case ORDER_BUILD:
        if (m->cursor < m->stats.active_edges) {
            uint32_t at = m->edge_ids[m->cursor++];
            CostEdge *e = &m->edges[at];
            if (selected(m, e)) {
                uint32_t source = m->nodes[e->source].component;
                uint32_t target = m->nodes[e->target].component;
                if (source != target) {
                    e->component_next = m->components[source].out;
                    m->components[source].out = at;
                    m->components[target].indegree++;
                }
            }
        } else {
            m->phase = ORDER_QUEUE;
            m->cursor = 0;
            m->queue_head = m->queue_tail = m->topology_done = 0;
        }
        break;
    case ORDER_QUEUE:
        if (m->cursor < m->component_count) {
            uint32_t at = m->cursor++;
            if (!m->components[at].indegree) m->queue[m->queue_tail++] = at;
        } else { m->phase = ORDER_DEPTH; m->topology_current = NONE; }
        break;
    case ORDER_DEPTH:
        if (m->topology_current != NONE) {
            if (m->topology_edge != NONE) {
                CostEdge *e = &m->edges[m->topology_edge];
                m->topology_edge = e->component_next;
                uint32_t target = m->nodes[e->target].component;
                CostComponent *c = &m->components[target];
                c->value = fmax(c->value,
                                m->components[m->topology_current].value + 1);
                if (!--c->indegree) m->queue[m->queue_tail++] = target;
            } else m->topology_current = NONE;
        } else if (m->queue_head < m->queue_tail) {
            m->topology_current = m->queue[m->queue_head++];
            ++m->topology_done;
            m->topology_edge = m->components[m->topology_current].out;
        } else {
            /* A partial Kahn traversal means G has a multi-node cycle.
             * Discard its scratch and compute exact SCCs before filtering. */
            if (m->topology_done != m->component_count) {
                m->component_count = 0;
                m->phase = ORDER_RESET;
            } else m->phase = ORDER_SAVE;
            m->cursor = 0;
        }
        break;
    case ORDER_SAVE:
        if (m->cursor < m->stats.active_nodes) {
            CostNode *n = &m->nodes[m->node_ids[m->cursor++]];
            n->input_count = (uint32_t)m->components[n->component].value;
            n->visited = false;
        } else {
            m->final_dag = m->component_count == m->stats.active_nodes;
            m->ordering = false;
            m->component_count = 0;
            m->phase = FILTER_SCAN;
            m->cursor = m->finish_count = 0;
        }
        break;
    case ORDER_RESET:
        if (m->cursor < m->stats.active_nodes) {
            uint32_t at = m->cursor++;
            CostNode *n = &m->nodes[m->node_ids[at]];
            n->input_count = 0; n->component = m->final_dag ? at : NONE; n->visited = false;
            memset(&m->components[at], 0, sizeof(m->components[at]));
            m->components[at].out = m->components[at].group = NONE;
            m->components[at].ready = m->components[at].all_slack = true;
            m->components[at].entries_qualified = true;
            m->components[at].min_wait = INFINITY;
            if (m->final_dag) m->components[at].nodes = 1;
        } else {
            m->phase = m->final_dag ? AGG_NODES : DFS_SCAN;
            if (m->final_dag) m->component_count = m->stats.active_nodes;
            m->cursor = m->reverse_index = 0;
        }
        break;
    case SCC_WALK: {
        /* Pop one member per work unit, including very large components. */
        i = m->finish[--m->finish_count];
        m->nodes[i].component = m->scc_current;
        m->nodes[i].input_count = 0;
        ++m->components[m->scc_current].nodes;
        if (i == m->stack[m->stack_size - 1u].node) dfs_return(m);
        break;
    }
    case AGG_NODES:
        if (m->cursor < m->stats.active_nodes) aggregate_node(m, m->node_ids[m->cursor++]);
        else {
            m->phase = m->has_owner_conflict ? BLOCK_OWNERS : AGG_EDGES;
            m->cursor = 0;
        }
        break;
    case BLOCK_OWNERS:
        if (m->cursor < m->stats.active_nodes) {
            CostNode *n = &m->nodes[m->node_ids[m->cursor++]];
            if (n->active && n->defined && !n->settled && !no_owner(n->spec.owner) &&
                m->owners[n->spec.owner.slot].conflict) {
                if (m->all_closed) fail(m, PEAK_MPI_COST_UNGROUPED_OWNER);
                else m->components[n->component].ready = false;
            }
        } else { m->phase = AGG_EDGES; m->cursor = 0; }
        break;
    case AGG_EDGES:
        if (m->cursor < m->stats.active_edges) aggregate_edge(m, m->edge_ids[m->cursor++]);
        else { m->phase = CHECK_GROUPS; m->cursor = 0; }
        break;
    case CHECK_GROUPS:
        if (m->cursor < m->stats.active_nodes) {
            CostNode *n = &m->nodes[m->node_ids[m->cursor++]];
            if (n->active && !n->settled && n->defined && n->sealed &&
                n->spec.kind == PEAK_MPI_COST_COLLECTIVE && n->input_count != n->spec.participants)
                fail(m, PEAK_MPI_COST_CONFLICTING_DEFINITION);
        } else {
            m->cursor = 0;
            m->queue_head = 0;
            if (m->final_dag) {
                /* The preserved guaranteed Kahn queue is nondecreasing in G
                 * depth; every retained final edge increases that depth. */
                m->queue_tail = m->component_count;
                m->phase = TOPOLOGY;
                m->topology_current = NONE;
                m->topology_done = 0;
            } else {
                m->phase = QUEUE_COMPONENTS;
                m->queue_tail = 0;
            }
        }
        break;
    case QUEUE_COMPONENTS:
        if (m->cursor < m->component_count) {
            i = m->cursor++;
            CostComponent *c = &m->components[i];
            if (c->nodes > 1) { c->cyclic = true; ++m->stats.scc_count; }
            if (c->indegree == 0) m->queue[m->queue_tail++] = i;
        } else { m->phase = TOPOLOGY; m->topology_current = NONE; m->topology_done = 0; }
        break;
    case TOPOLOGY: topology_step(m); break;
    case COMMIT_NODES:
        if (m->cursor < m->stats.active_nodes) {
            CostNode *n = &m->nodes[m->node_ids[m->cursor++]];
            if (!n->active) break;
            CostComponent *c = &m->components[n->component];
            if (!n->settled && c->ready) {
                if (c->nodes > 1 && !c->counted) {
                    if (m->stats.scc_settlements == UINT64_MAX) {
                        fail(m, PEAK_MPI_COST_GENERATION_OVERFLOW);
                        break;
                    }
                    ++m->stats.scc_settlements;
                    c->counted = true;
                }
                n->settled = true; n->delay = c->value;
                ++m->stats.settled_nodes;
                if (!no_owner(n->spec.owner)) m->owners[n->spec.owner.slot].frozen = true;
            }
            if (!n->settled) m->pending = true;
        } else {
            m->phase = FRONTIER_RANKS; m->cursor = 0;
            m->frontier = 0; m->frontier_valid = true;
        }
        break;
    case FRONTIER_RANKS:
        if (m->cursor < m->continuation_count) {
            CostRank *r = &m->ranks[m->continuation_ids[m->cursor++]];
            frontier_ref(m, r->continuation);
        } else { m->phase = FRONTIER_BRANCHES; m->cursor = 0; }
        break;
    case FRONTIER_BRANCHES:
        if (m->cursor < m->stats.active_branches) {
            CostBranch *b = &m->branches[m->branch_ids[m->cursor++]];
            if (b->active) frontier_ref(m, b->node);
        } else m->phase = FINISH;
        break;
    case FINISH:
        if (m->stats.generation == UINT64_MAX) { fail(m, PEAK_MPI_COST_GENERATION_OVERFLOW); break; }
        ++m->stats.generation;
        m->last_result = m->pending ? PEAK_MPI_COST_PENDING : PEAK_MPI_COST_OK;
        m->graph_dirty = m->frontier_dirty = false;
        m->phase = IDLE;
        break;
    case IDLE: break;
    }
}

static PeakMpiCostStatus full_model_run(PeakMpiCostModel *m, size_t budget,
                                       size_t *used)
{
    if (used) *used = 0;
    if (!m) return PEAK_MPI_COST_INVALID;
    if (m->stats.coarse) return PEAK_MPI_COST_COARSE;
    if (m->phase == IDLE && !m->graph_dirty && !m->frontier_dirty) return m->last_result;
    if (!budget) return PEAK_MPI_COST_YIELDED;
    if (m->phase == IDLE) {
        m->cursor = 0;
        if (m->graph_dirty) {
            m->phase = RESET_NODES;
            m->has_reverse = false; m->requires_qualified_g_build = false;
            m->final_dag = false;
            m->finish_count = m->component_count = m->stack_size = 0;
            m->pending = m->root_projected && m->root_global_pending; m->all_closed = !m->root_projected || m->root_global_closed; m->has_owner_conflict = false;
            m->stats.scc_count = m->stats.excluded_candidates = m->stats.retained_candidates = 0;
        } else {
            m->phase = FRONTIER_RANKS; m->frontier = 0; m->frontier_valid = true;
        }
    }
    size_t work = 0;
    while (work < budget && m->phase != IDLE && !m->stats.coarse) { run_step(m); ++work; }
    if (used) *used = work;
    if (m->stats.coarse) return PEAK_MPI_COST_COARSE;
    return m->phase == IDLE ? m->last_result : PEAK_MPI_COST_YIELDED;
}

/* Optional rooted solve: begin/add roots while idle, then bounded run.
 * Three fixed work vectors preserve original active-ID vectors and iteration
 * order. Selection follows every incoming edge (even an excluded candidate):
 * any guaranteed path target -> source for a selected reverse edge therefore
 * lies in the same backward closure. Shared owners include every reference.
 * Previously settled omitted nodes retain valid immutable values. The global
 * frontier remains unknown; latest SCC/candidate statistics describe this
 * processed subset, while capacity/settled counts remain global. */
PeakMpiCostStatus peak_mpi_cost_model_roots_begin(PeakMpiCostModel *m) {
    PeakMpiCostStatus st = writable(m);
    if (st != PEAK_MPI_COST_OK)
        return st;
    if (m->search_stamp == UINT64_MAX)
        return fail(m, PEAK_MPI_COST_GENERATION_OVERFLOW);
    ++m->search_stamp;
    m->root_stage = ROOT_CLEAR_OWNERS;
    m->root_cursor = m->root_seed_count = 0;
    m->root_full_n = m->stats.active_nodes;
    m->root_full_e = m->stats.active_edges;
    m->root_full_o = m->stats.active_owners;
    m->root_head = m->root_tail = 0;
    m->root_n = m->root_e = m->root_o = 0;
    m->root_owner = NONE;
    m->root_projected = m->root_swapped = m->root_reset = false;
    m->root_global_pending = false;
    m->root_global_closed = true;
    m->root_result = PEAK_MPI_COST_PENDING;
    m->root_edge = NONE;
    return PEAK_MPI_COST_OK;
}
PeakMpiCostStatus peak_mpi_cost_model_roots_add(PeakMpiCostModel *m, PeakMpiCostRef ref) {
    if (!m || m->root_stage != ROOT_CLEAR_OWNERS || m->root_cursor)
        return PEAK_MPI_COST_BUSY;
    if (!valid_node(m, ref))
        return PEAK_MPI_COST_INVALID;
    if (m->nodes[ref.slot].search_stamp == m->search_stamp)
        return PEAK_MPI_COST_OK;
    if (m->root_seed_count == m->config.max_nodes)
        return PEAK_MPI_COST_PENDING;
    m->nodes[ref.slot].search_stamp = m->search_stamp;
    /* Reuse the search stamp to deduplicate seeds. Later G searches take a new
     * stamp. queue holds seeds until selection flags/index heads are ready. */
    m->queue[m->root_seed_count++] = ref.slot;
    return PEAK_MPI_COST_OK;
}
int peak_mpi_cost_model_closed_hint(const PeakMpiCostModel *m, PeakMpiCostRef ref) {
    return m && valid_node(m, ref) && m->nodes[ref.slot].defined && m->nodes[ref.slot].sealed;
}
/* Selection-only scratch: finish is the closure queue; stack[node slot].edge
 * links owner references and owner.component is its head. No SCC work runs
 * until closure is complete, so the later Tarjan/component uses cannot overlap. */
static void root_include(PeakMpiCostModel *m, uint32_t i) {
    CostNode *n = &m->nodes[i];
    if (n->root_selected)
        return;
    n->root_selected = true;
    m->finish[m->root_tail++] = i;
}
static void root_restore_begin(PeakMpiCostModel *m) {
    m->phase = IDLE;
    m->ordering = false;
    m->root_stage = ROOT_RESTORE;
    m->root_cursor = 0;
}
static void root_step(PeakMpiCostModel *m) {
    uint32_t i;
    switch (m->root_stage) {
    case ROOT_NONE:
    case ROOT_SOLVE:
        break;
    case ROOT_CLEAR_NODES: /* Clear one node and index its owner reference. */
        if (m->root_cursor < m->root_full_n) {
            i = m->node_ids[m->root_cursor++];
            m->nodes[i].root_selected = false;
            CostNode *n = &m->nodes[i];
            if (!n->settled && (!n->defined || !n->sealed)) {
                m->root_global_pending = true;
                m->root_global_closed = false;
            }
            if (n->defined && !no_owner(n->spec.owner)) {
                if (!valid_owner(m, n->spec.owner)) {
                    fail(m, PEAK_MPI_COST_INVALID_REFERENCE);
                    break;
                }
                CostOwner *o = &m->owners[n->spec.owner.slot];
                if (o->defined && o->rank != n->spec.rank) {
                    fail(m, PEAK_MPI_COST_CONFLICTING_DEFINITION);
                    break;
                }
                m->stack[i].edge = o->component;
                o->component = i;
                if (o->references > 1)
                    root_include(m, i);
            }
        } else {
            m->root_stage = ROOT_SEEDS;
            m->root_cursor = 0;
        }
        break;
    case ROOT_CLEAR_OWNERS:
        if (m->root_cursor < m->root_full_o) {
            i = m->owner_ids[m->root_cursor++];
            m->owners[i].root_references = 0;
            m->owners[i].component = NONE;
            if (!m->owners[i].defined || !m->owners[i].references) {
                m->root_global_pending = true;
                m->root_global_closed = false;
            }
        } else {
            m->root_stage = ROOT_CLEAR_NODES;
            m->root_cursor = 0;
        }
        break;
    case ROOT_SEEDS:
        if (m->root_cursor < m->root_seed_count)
            root_include(m, m->queue[m->root_cursor++]);
        else {
            m->root_stage = ROOT_CLOSURE;
            m->root_cursor = 0;
        }
        break;
    case ROOT_CLOSURE:
        if (m->root_edge != NONE) {
            CostEdge *e = &m->edges[m->root_edge];
            m->root_edge = e->in_next;
            if (e->active)
                root_include(m, e->source);
        } else if (m->root_owner != NONE) {
            i = m->root_owner;
            m->root_owner = m->stack[i].edge;
            root_include(m, i);
        } else if (m->root_head < m->root_tail) {
            i = m->finish[m->root_head++];
            CostNode *n = &m->nodes[i];
            m->root_edge = n->in;
            if (n->defined && !no_owner(n->spec.owner)) {
                CostOwner *o = &m->owners[n->spec.owner.slot];
                if (!o->root_references) {
                    o->root_references = 1;
                    m->root_owner = o->component;
                }
            }
        } else {
            m->root_stage = ROOT_PROJECT_NODES;
            m->root_cursor = 0;
            m->root_projected = true;
        }
        break;
    case ROOT_PROJECT_NODES:
        if (m->root_cursor < m->root_full_n) {
            i = m->node_ids[m->root_cursor++];
            if (m->nodes[i].root_selected) { /* Active nodes never read free_next. Retirement/reset
                                              * writes the free-list link before a node becomes
                                              * inactive. Restore this saved index one selected node
                                              * per work unit, including pre-swap cancellation. */
                m->nodes[i].free_next = m->nodes[i].active_index;
                m->nodes[i].active_index = m->root_n;
                m->root_nodes[m->root_n++] = i;
            }
        } else {
            m->root_stage = ROOT_PROJECT_OWNERS;
            m->root_cursor = 0;
        }
        break;
    case ROOT_PROJECT_OWNERS:
        if (m->root_cursor < m->root_full_o) {
            i = m->owner_ids[m->root_cursor++];
            if (m->owners[i].root_references)
                m->root_owners[m->root_o++] = i;
        } else {
            m->root_stage = ROOT_PROJECT_EDGES;
            m->root_cursor = 0;
        }
        break;
    case ROOT_PROJECT_EDGES:
        if (m->root_cursor < m->root_full_e) {
            i = m->edge_ids[m->root_cursor++];
            CostEdge *e = &m->edges[i];
            if (m->nodes[e->source].root_selected && m->nodes[e->target].root_selected)
                m->root_edges[m->root_e++] = i;
        } else {
            uint32_t *ids = m->node_ids;
            m->node_ids = m->root_nodes;
            m->root_nodes = ids;
            ids = m->owner_ids;
            m->owner_ids = m->root_owners;
            m->root_owners = ids;
            ids = m->edge_ids;
            m->edge_ids = m->root_edges;
            m->root_edges = ids;
            m->root_swapped = true;
            m->stats.active_nodes = m->root_n;
            m->stats.active_edges = m->root_e;
            m->stats.active_owners = m->root_o;
            m->graph_dirty = m->frontier_dirty = true;
            m->root_stage = ROOT_SOLVE;
        }
        break;
    case ROOT_RESTORE:
        if (m->root_cursor < m->root_n) {
            i = (m->root_swapped ? m->node_ids : m->root_nodes)[m->root_cursor++];
            m->nodes[i].active_index = m->nodes[i].free_next;
        } else {
            if (m->root_swapped) {
                uint32_t *ids = m->node_ids;
                m->node_ids = m->root_nodes;
                m->root_nodes = ids;
                ids = m->owner_ids;
                m->owner_ids = m->root_owners;
                m->root_owners = ids;
                ids = m->edge_ids;
                m->edge_ids = m->root_edges;
                m->root_edges = ids;
            }
            m->stats.active_nodes = m->root_full_n;
            m->stats.active_edges = m->root_full_e;
            m->stats.active_owners = m->root_full_o;
            m->root_projected = m->root_swapped = false;
            m->root_stage = ROOT_NONE;
            m->graph_dirty = m->frontier_dirty = true;
            m->frontier_valid = false;
            if (m->root_reset) {
                m->reset_stage = 1;
                m->reset_cursor = 0;
                m->root_reset = false;
            }
        }
        break;
    }
}
PeakMpiCostStatus peak_mpi_cost_model_run(PeakMpiCostModel *m, size_t budget, size_t *used) {
    if (!m || !m->root_stage)
        return full_model_run(m, budget, used);
    size_t work = 0;
    while (work < budget && m->root_stage) {
        if (m->stats.coarse && m->root_stage < ROOT_RESTORE) {
            m->root_result = PEAK_MPI_COST_COARSE;
            if (m->root_projected)
                root_restore_begin(m);
            else {
                m->root_stage = ROOT_NONE;
                break;
            }
        }
        if (m->root_stage == ROOT_SOLVE) {
            size_t spent = 0;
            PeakMpiCostStatus st = full_model_run(m, budget - work, &spent);
            work += spent;
            if (st == PEAK_MPI_COST_YIELDED)
                break;
            m->root_result = st;
            root_restore_begin(m);
        } else {
            root_step(m);
            ++work;
        }
    }
    if (used)
        *used = work;
    return m->root_stage ? PEAK_MPI_COST_YIELDED : m->root_result;
}
PeakMpiCostStatus peak_mpi_cost_model_delay(const PeakMpiCostModel *m,
                                         PeakMpiCostRef ref, double *out)
{
    if (!m || !out || !valid_node(m, ref)) return PEAK_MPI_COST_INVALID;
    if (m->stats.coarse) return PEAK_MPI_COST_COARSE;
    if (m->phase != IDLE || m->root_stage) return PEAK_MPI_COST_BUSY;
    if (!m->nodes[ref.slot].settled) return PEAK_MPI_COST_PENDING;
    *out = m->nodes[ref.slot].delay;
    return PEAK_MPI_COST_OK;
}

PeakMpiCostStatus peak_mpi_cost_model_hold_branch(PeakMpiCostModel *m,
                                               PeakMpiCostRef ref,
                                               PeakMpiCostBranchKind kind,
                                               PeakMpiCostBranchRef *out)
{
    PeakMpiCostStatus status = writable(m);
    if (status != PEAK_MPI_COST_OK) return status;
    if (!out) return PEAK_MPI_COST_INVALID;
    if (!valid_node(m, ref)) return fail(m, PEAK_MPI_COST_INVALID_REFERENCE);
    if (kind < PEAK_MPI_COST_SEND_BRANCH || kind > PEAK_MPI_COST_PROGRESS_BRANCH)
        return fail(m, PEAK_MPI_COST_CONFLICTING_DEFINITION);
    if (m->nodes[ref.slot].pins == UINT32_MAX)
        return fail(m, PEAK_MPI_COST_BRANCH_CAPACITY);
    if (m->free_branch == NONE && m->unused_branch == m->config.max_branches)
        return fail(m, PEAK_MPI_COST_BRANCH_CAPACITY);
    uint32_t i = m->free_branch != NONE ? m->free_branch : m->unused_branch;
    CostBranch *b = &m->branches[i];
    if (b->generation == UINT64_MAX) return fail(m, PEAK_MPI_COST_GENERATION_OVERFLOW);
    if (m->free_branch != NONE) m->free_branch = b->free_next;
    else ++m->unused_branch;
    b->active = true; ++b->generation; b->node = ref; b->kind = kind;
    b->active_index = m->stats.active_branches;
    m->branch_ids[b->active_index] = i;
    ++m->nodes[ref.slot].pins;
    ++m->stats.active_branches;
    *out = (PeakMpiCostBranchRef){i, b->generation};
    touch_frontier(m);
    return PEAK_MPI_COST_OK;
}

static void release_branch(PeakMpiCostModel *m, uint32_t i)
{
    CostBranch *b = &m->branches[i];
    --m->nodes[b->node.slot].pins;
    uint32_t last = m->branch_ids[m->stats.active_branches - 1u];
    m->branch_ids[b->active_index] = last;
    m->branches[last].active_index = b->active_index;
    b->active = false; b->free_next = m->free_branch; m->free_branch = i;
    --m->stats.active_branches;
}

PeakMpiCostStatus peak_mpi_cost_model_release_branch(PeakMpiCostModel *m,
                                                  PeakMpiCostBranchRef ref,
                                                  bool closed)
{
    PeakMpiCostStatus status = writable(m);
    if (status != PEAK_MPI_COST_OK) return status;
    if (!valid_branch(m, ref)) return fail(m, PEAK_MPI_COST_INVALID_REFERENCE);
    if (!closed) return fail(m, PEAK_MPI_COST_INVALID_CLOSURE);
    release_branch(m, ref.slot); touch_frontier(m);
    return PEAK_MPI_COST_OK;
}

static void set_continuation(PeakMpiCostModel *m, uint32_t rank, PeakMpiCostRef ref)
{
    CostRank *r = &m->ranks[rank];
    if (r->continuation_known && same_ref(r->continuation, ref)) return;
    ++m->nodes[ref.slot].pins;
    if (r->continuation_known) --m->nodes[r->continuation.slot].pins;
    else m->continuation_ids[m->continuation_count++] = rank;
    r->continuation = ref; r->continuation_known = true;
}

PeakMpiCostStatus peak_mpi_cost_model_set_continuation(PeakMpiCostModel *m,
                                                    uint32_t rank,
                                                    PeakMpiCostRef ref)
{
    PeakMpiCostStatus status = writable(m);
    if (status != PEAK_MPI_COST_OK) return status;
    if (rank >= m->config.max_ranks || !valid_node(m, ref))
        return fail(m, PEAK_MPI_COST_INVALID_REFERENCE);
    if ((!m->ranks[rank].continuation_known ||
         !same_ref(m->ranks[rank].continuation, ref)) &&
        m->nodes[ref.slot].pins == UINT32_MAX)
        return fail(m, PEAK_MPI_COST_BRANCH_CAPACITY);
    set_continuation(m, rank, ref); touch_frontier(m);
    return PEAK_MPI_COST_OK;
}

PeakMpiCostStatus peak_mpi_cost_model_replace_branch(PeakMpiCostModel *m,
                                                  PeakMpiCostBranchRef branch,
                                                  uint32_t rank,
                                                  PeakMpiCostRef continuation)
{
    PeakMpiCostStatus status = writable(m);
    if (status != PEAK_MPI_COST_OK) return status;
    if (!valid_branch(m, branch) || rank >= m->config.max_ranks || !valid_node(m, continuation))
        return fail(m, PEAK_MPI_COST_INVALID_REFERENCE);
    if ((!m->ranks[rank].continuation_known ||
         !same_ref(m->ranks[rank].continuation, continuation)) &&
        m->nodes[continuation.slot].pins == UINT32_MAX)
        return fail(m, PEAK_MPI_COST_BRANCH_CAPACITY);
    set_continuation(m, rank, continuation);
    release_branch(m, branch.slot);
    touch_frontier(m);
    return PEAK_MPI_COST_OK;
}

PeakMpiCostStatus peak_mpi_cost_model_frontier(const PeakMpiCostModel *m, double *out)
{
    if (!m || !out) return PEAK_MPI_COST_INVALID;
    if (m->stats.coarse) return PEAK_MPI_COST_COARSE;
    if (m->root_stage || m->phase != IDLE || m->frontier_dirty || !m->frontier_valid) return PEAK_MPI_COST_PENDING;
    *out = m->frontier;
    return PEAK_MPI_COST_OK;
}

PeakMpiCostStatus peak_mpi_cost_model_retire(PeakMpiCostModel *m,
                                          PeakMpiCostRef ref, bool closed)
{
    PeakMpiCostStatus status = writable(m);
    if (status != PEAK_MPI_COST_OK) return status;
    if (!valid_node(m, ref)) return fail(m, PEAK_MPI_COST_INVALID_REFERENCE);
    if (!closed) return fail(m, PEAK_MPI_COST_INVALID_CLOSURE);
    if (!m->nodes[ref.slot].retire) {
        m->nodes[ref.slot].retire = true;
        m->gc_phase = m->gc_cursor = 0;
    }
    return PEAK_MPI_COST_OK;
}

static void remove_edge(PeakMpiCostModel *m, uint32_t i)
{
    CostEdge *e = &m->edges[i];
    if (e->out_previous == NONE) m->nodes[e->source].out = e->out_next;
    else m->edges[e->out_previous].out_next = e->out_next;
    if (e->out_next != NONE) m->edges[e->out_next].out_previous = e->out_previous;
    if (e->in_previous == NONE) m->nodes[e->target].in = e->in_next;
    else m->edges[e->in_previous].in_next = e->in_next;
    if (e->in_next != NONE) m->edges[e->in_next].in_previous = e->in_previous;
    m->edge_hash[e->hash_slot] = TOMBSTONE;
    uint32_t last = m->edge_ids[m->stats.active_edges - 1u];
    m->edge_ids[e->active_index] = last;
    m->edges[last].active_index = e->active_index;
    e->active = false; e->free_next = m->free_edge; m->free_edge = i;
    --m->stats.active_edges;
}

static void remove_node(PeakMpiCostModel *m, uint32_t i)
{
    CostNode *n = &m->nodes[i];
    if (!no_owner(n->spec.owner)) {
        CostOwner *o = &m->owners[n->spec.owner.slot];
        if (--o->references == 0) {
            uint32_t last = m->owner_ids[m->stats.active_owners - 1u];
            m->owner_ids[o->active_index] = last;
            m->owners[last].active_index = o->active_index;
            o->active = false; o->free_next = m->free_owner;
            m->free_owner = n->spec.owner.slot;
            --m->stats.active_owners;
        }
    }
    uint32_t last = m->node_ids[m->stats.active_nodes - 1u];
    m->node_ids[n->active_index] = last;
    m->nodes[last].active_index = n->active_index;
    n->active = false; n->free_next = m->free_node; m->free_node = i;
    --m->stats.active_nodes; --m->stats.settled_nodes;
}

PeakMpiCostStatus peak_mpi_cost_model_collect(PeakMpiCostModel *m, size_t budget,
                                           size_t *used)
{
    if (used) *used = 0;
    PeakMpiCostStatus status = writable(m);
    if (status != PEAK_MPI_COST_OK) return status;
    size_t work = 0;
    while (work < budget) {
        if (m->gc_cursor >= m->stats.active_nodes) {
            m->gc_phase = m->gc_cursor = 0;
            if (used) *used = work;
            return PEAK_MPI_COST_OK;
        }
        uint32_t i = m->node_ids[m->gc_cursor];
        CostNode *n = &m->nodes[i];
        if (!m->gc_phase) {
            ++work;
            if (!n->retire || !n->settled || n->pins) { ++m->gc_cursor; continue; }
            m->gc_incident = n->out;
            m->gc_phase = 1;
        }
        if (m->gc_phase == 1 && m->gc_incident == NONE) {
            m->gc_incident = n->in;
            m->gc_phase = 2;
        }
        if (m->gc_phase == 2 && m->gc_incident == NONE) {
            if (n->in == NONE && n->out == NONE) remove_node(m, i);
            else ++m->gc_cursor;
            m->gc_phase = 0;
            continue;
        }
        if (work == budget) break;
        CostEdge *e = &m->edges[m->gc_incident];
        uint32_t edge = m->gc_incident;
        m->gc_incident = m->gc_phase == 1 ? e->out_next : e->in_next;
        if (m->nodes[e->source].settled && m->nodes[e->target].settled)
            remove_edge(m, edge);
        ++work;
    }
    if (used) *used = work;
    return PEAK_MPI_COST_YIELDED;
}

PeakMpiCostStatus peak_mpi_cost_model_accounted(const PeakMpiCostModel *m,
                                             uint32_t rank, double *L, double *P)
{
    if (!m || rank >= m->config.max_ranks || !L || !P) return PEAK_MPI_COST_INVALID;
    *L = m->ranks[rank].accounted_L; *P = m->ranks[rank].accounted_P;
    return PEAK_MPI_COST_OK;
}

PeakMpiCostStatus peak_mpi_cost_model_raw_cut(PeakMpiCostModel *m, uint32_t rank,
                                           const PeakMpiCostRawCut *cut)
{
    if (!m || !cut) return PEAK_MPI_COST_INVALID;
    if (rank >= m->config.max_ranks) return fail(m, PEAK_MPI_COST_INVALID_REFERENCE);
    if (!nonnegative(cut->L) || !nonnegative(cut->P) || cut->P > cut->L || !nonnegative(cut->elapsed))
        return fail(m, PEAK_MPI_COST_INVALID_COST);
    CostRank *r = &m->ranks[rank];
    if (r->raw_known) {
        if (cut->generation < r->raw.generation) return PEAK_MPI_COST_DUPLICATE;
        if (cut->generation == r->raw.generation)
            return cut->L == r->raw.L && cut->P == r->raw.P && cut->elapsed == r->raw.elapsed &&
                   cut->control_generation == r->raw.control_generation ? PEAK_MPI_COST_OK :
                   fail(m, PEAK_MPI_COST_CONFLICTING_DEFINITION);
        if (cut->L < r->raw.L || cut->P < r->raw.P || cut->elapsed < r->raw.elapsed ||
            cut->control_generation < r->raw.control_generation) return fail(m, PEAK_MPI_COST_INVALID_COST);
    }
    r->raw = *cut; r->raw_known = true;
    return PEAK_MPI_COST_OK;
}

PeakMpiCostStatus peak_mpi_cost_model_raw(const PeakMpiCostModel *m, uint32_t rank,
                                       PeakMpiCostRawCut *out)
{
    if (!m || !out || rank >= m->config.max_ranks) return PEAK_MPI_COST_INVALID;
    if (!m->ranks[rank].raw_known) return PEAK_MPI_COST_PENDING;
    *out = m->ranks[rank].raw;
    return PEAK_MPI_COST_OK;
}

void peak_mpi_cost_model_stats(const PeakMpiCostModel *m, PeakMpiCostStats *out)
{
    if (m && out) {
        *out = m->stats;
        if (m->root_stage) {
            out->active_nodes = m->root_full_n;
            out->active_edges = m->root_full_e;
            out->active_owners = m->root_full_o;
        }
    }
}

/* Epoch reset reuses active-ID/free lists: no allocation or capacity sweep.
 * Virgin cursors stay at their high-water marks; only reclaimed slots return
 * to the lists, preserving generations and the untouched suffix. */
void peak_mpi_cost_model_reset_begin(PeakMpiCostModel *m) {
    if (!m)
        return;
    if (m->root_stage) {
        if (m->root_projected) {
            m->root_reset = true;
            root_restore_begin(m);
            return;
        }
        m->root_stage = ROOT_NONE;
    }
    m->phase = IDLE;
    m->ordering = false;
    m->reset_stage = 1;
    m->reset_cursor = 0;
}
int peak_mpi_cost_model_reset_work(PeakMpiCostModel *m, size_t budget,
                                   size_t *used) {
    *used = 0;
    if (m->root_stage) {
        peak_mpi_cost_model_run(m, budget, used);
        return 0;
    }
    while (m->reset_stage && *used < budget) {
        ++*used;
        if (m->reset_stage == 1) {
            if (m->stats.active_branches)
                release_branch(m, m->branch_ids[m->stats.active_branches - 1]);
            else
                m->reset_stage = 2;
        } else if (m->reset_stage == 2) {
            if (m->stats.active_edges)
                remove_edge(m, m->edge_ids[m->stats.active_edges - 1]);
            else
                m->reset_stage = 3;
        } else if (m->reset_stage == 3) {
            if (m->stats.active_nodes) {
                uint32_t i = m->node_ids[--m->stats.active_nodes];
                CostNode *n = &m->nodes[i];
                if (n->settled)
                    --m->stats.settled_nodes;
                n->active = false;
                n->free_next = m->free_node;
                m->free_node = i;
            } else
                m->reset_stage = 4;
        } else if (m->reset_stage == 4) {
            if (m->stats.active_owners) {
                uint32_t i = m->owner_ids[--m->stats.active_owners];
                CostOwner *o = &m->owners[i];
                o->active = false;
                o->free_next = m->free_owner;
                m->free_owner = i;
            } else
                m->reset_stage = 5;
        } else if (m->reset_cursor < m->config.max_ranks) {
            CostRank *r = &m->ranks[m->reset_cursor++];
            r->continuation_known = r->raw_known = false;
        } else {
            m->stats.settled_nodes = 0;
            m->continuation_count = 0;
            m->stats.coarse = false;
            m->stats.reason = PEAK_MPI_COST_REASON_NONE;
            m->graph_dirty = m->frontier_dirty = true;
            m->frontier_valid = false;
            m->pending = false;
            m->gc_phase = m->gc_cursor = 0;
            m->reset_stage = 0;
            m->last_result = PEAK_MPI_COST_PENDING;
        }
    }
    return !m->reset_stage;
}
void peak_mpi_cost_model_reset_base(PeakMpiCostModel *m, uint32_t rank,
                                    double L, double P) {
    if (m && rank < m->config.max_ranks) {
        m->ranks[rank].accounted_L = L;
        m->ranks[rank].accounted_P = P;
    }
}
