#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#include "internal/mpi_observer.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(PeakMpiObserverRecord) == PEAK_MPI_OBSERVER_RECORD_BYTES,
               "capture record ABI changed");

typedef struct {
    _Atomic uint64_t head;
    unsigned char head_padding[64 - sizeof(_Atomic uint64_t)];
    _Atomic uint64_t tail;
    unsigned char tail_padding[64 - sizeof(_Atomic uint64_t)];
    _Atomic uint64_t generation, thread_uid;
    _Atomic uint64_t last_attempt, first_lost, lost;
    _Atomic uint64_t P, other_cost;
    _Atomic unsigned owner; /* 0: free, 1: writer, 2: installing writer */
    PeakMpiObserverRecord *records;
    unsigned char slot_padding[56];
} PeakMpiObserverSlot;

_Static_assert(sizeof(PeakMpiObserverSlot) == 256,
               "review capture slot cache-line layout on this target");

struct PeakMpiObserver {
    PeakMpiObserverConfig config;
    PeakMpiObserverSlot *slots;
    PeakMpiObserverRecord *records;
    PeakMpiObserverObject *objects;
    _Atomic size_t object_count;
    pthread_mutex_t object_mutex;
    pthread_mutex_t feedback_mutex;
    bool has_feedback;
    PeakMpiObserverFeedback feedback;
    _Atomic bool active;
    _Atomic unsigned fault_state; /* 0: none, 1: publishing, 2: immutable */
    PeakMpiObserverFault fault;
};

static bool product_fits(size_t a, size_t b) {
    return b == 0 || a <= SIZE_MAX / b;
}

static void *allocate_aligned(size_t size) {
    void *storage = NULL;
    return posix_memalign(&storage, 64, size) == 0 ? storage : NULL;
}

static void fail_at(PeakMpiObserver *o, PeakMpiObserverReason reason,
                    size_t slot, uint64_t generation, uint64_t sequence) {
    unsigned expected = 0;
    if (atomic_compare_exchange_strong_explicit(&o->fault_state, &expected, 1,
            memory_order_acq_rel, memory_order_relaxed)) {
        o->fault = (PeakMpiObserverFault){reason, slot, generation, sequence};
        atomic_store_explicit(&o->fault_state, 2, memory_order_release);
    }
}

static void lose_record(PeakMpiObserverSlot *s, uint64_t sequence) {
    uint64_t lost = atomic_load_explicit(&s->lost, memory_order_relaxed);
    if (!lost) atomic_store_explicit(&s->first_lost, sequence, memory_order_relaxed);
    if (lost != UINT64_MAX)
        atomic_store_explicit(&s->lost, lost + 1, memory_order_release);
}

static PeakMpiObserverSlot *owned(PeakMpiObserver *o,
                                  const PeakMpiObserverProducer *p) {
    if (!o || !p || p->slot >= o->config.slots) return NULL;
    PeakMpiObserverSlot *s = &o->slots[p->slot];
    if (atomic_load_explicit(&s->owner, memory_order_relaxed) != 1 ||
        atomic_load_explicit(&s->generation, memory_order_relaxed) != p->generation ||
        atomic_load_explicit(&s->thread_uid, memory_order_relaxed) != p->thread_uid)
        return NULL;
    return s;
}

PeakMpiObserver *peak_mpi_observer_create(const PeakMpiObserverConfig *c) {
    if (!c || !c->slots || !c->records_per_slot || !c->object_capacity ||
        !c->incarnation || c->slots > UINT32_MAX ||
        c->records_per_slot > UINT64_MAX / 2 ||
        !product_fits(c->slots, sizeof(PeakMpiObserverSlot)) ||
        !product_fits(c->slots, c->records_per_slot) ||
        !product_fits(c->slots * c->records_per_slot, sizeof(PeakMpiObserverRecord)) ||
        !product_fits(c->object_capacity, sizeof(PeakMpiObserverObject))) return NULL;
    PeakMpiObserver *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->config = *c;
    o->slots = allocate_aligned(c->slots * sizeof(*o->slots));
    o->records = allocate_aligned(c->slots * c->records_per_slot * sizeof(*o->records));
    o->objects = calloc(c->object_capacity, sizeof(*o->objects));
    if (!o->slots || !o->records || !o->objects) goto fail;
    memset(o->slots, 0, c->slots * sizeof(*o->slots));
    /* Record storage is first touched by the producer's complete copy before
     * release publication; inactive slots need no eager page initialization. */
    if (pthread_mutex_init(&o->object_mutex, NULL)) goto fail;
    if (pthread_mutex_init(&o->feedback_mutex, NULL)) {
        pthread_mutex_destroy(&o->object_mutex);
        goto fail;
    }
    atomic_init(&o->object_count, 0);
    atomic_init(&o->active, false);
    atomic_init(&o->fault_state, 0);
    for (size_t i = 0; i < c->slots; ++i) {
        PeakMpiObserverSlot *s = &o->slots[i];
        s->records = o->records + i * c->records_per_slot;
        atomic_init(&s->head, 0); atomic_init(&s->tail, 0);
        atomic_init(&s->generation, 0); atomic_init(&s->thread_uid, 0);
        atomic_init(&s->last_attempt, 0); atomic_init(&s->first_lost, 0);
        atomic_init(&s->lost, 0); atomic_init(&s->other_cost, 0);
        atomic_init(&s->P, 0); atomic_init(&s->owner, 0);
    }
    /* Hot paths rely on lock-free scalar atomics, never emulated atomic locks. */
    if (!atomic_is_lock_free(&o->slots[0].head) ||
        !atomic_is_lock_free(&o->slots[0].owner) ||
        !atomic_is_lock_free(&o->active)) {
        peak_mpi_observer_destroy(o);
        return NULL;
    }
    return o;
fail:
    free(o->objects); free(o->records); free(o->slots); free(o);
    return NULL;
}

void peak_mpi_observer_destroy(PeakMpiObserver *o) {
    if (!o) return;
    pthread_mutex_destroy(&o->feedback_mutex);
    pthread_mutex_destroy(&o->object_mutex);
    free(o->objects); free(o->records); free(o->slots); free(o);
}

void peak_mpi_observer_activate(PeakMpiObserver *o) {
    if (o) atomic_store_explicit(&o->active, true, memory_order_release);
}

bool peak_mpi_observer_active(const PeakMpiObserver *o) {
    return o && atomic_load_explicit(&o->active, memory_order_acquire);
}

void peak_mpi_observer_fail(PeakMpiObserver *o, PeakMpiObserverReason reason) {
    if (o && reason > PEAK_MPI_OBSERVER_REASON_NONE &&
        reason < PEAK_MPI_OBSERVER_REASON_PENDING_FAULT)
        fail_at(o, reason, SIZE_MAX, 0, 0);
}

PeakMpiObserverReason peak_mpi_observer_coarse_reason(const PeakMpiObserver *o) {
    if (!o) return PEAK_MPI_OBSERVER_REASON_METADATA;
    unsigned state = atomic_load_explicit(&o->fault_state, memory_order_acquire);
    return state == 0 ? PEAK_MPI_OBSERVER_REASON_NONE :
           state == 1 ? PEAK_MPI_OBSERVER_REASON_PENDING_FAULT : o->fault.reason;
}

bool peak_mpi_observer_fault(const PeakMpiObserver *o, PeakMpiObserverFault *f) {
    if (!o || !f || atomic_load_explicit(&o->fault_state, memory_order_acquire) != 2)
        return false;
    *f = o->fault;
    return true;
}

PeakMpiObserverStatus peak_mpi_observer_claim(PeakMpiObserver *o, size_t slot,
    uint64_t generation, uint64_t thread_uid, PeakMpiObserverProducer *p) {
    if (!o || !p || slot >= o->config.slots || !generation || !thread_uid)
        return PEAK_MPI_OBSERVER_BAD_ARGUMENT;
    PeakMpiObserverSlot *s = &o->slots[slot];
    unsigned expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&s->owner, &expected, 2,
            memory_order_acquire, memory_order_relaxed)) return PEAK_MPI_OBSERVER_BUSY;
    if (generation <= atomic_load_explicit(&s->generation, memory_order_relaxed)) {
        atomic_store_explicit(&s->owner, 0, memory_order_release);
        return PEAK_MPI_OBSERVER_BAD_PRODUCER;
    }
    atomic_store_explicit(&s->generation, generation, memory_order_relaxed);
    atomic_store_explicit(&s->thread_uid, thread_uid, memory_order_relaxed);
    *p = (PeakMpiObserverProducer){slot, generation, thread_uid};
    atomic_store_explicit(&s->owner, 1, memory_order_release);
    return PEAK_MPI_OBSERVER_OK;
}

PeakMpiObserverStatus peak_mpi_observer_release(PeakMpiObserver *o,
    const PeakMpiObserverProducer *p) {
    PeakMpiObserverSlot *s = owned(o, p);
    if (!s) return PEAK_MPI_OBSERVER_BAD_PRODUCER;
    atomic_store_explicit(&s->owner, 0, memory_order_release);
    return PEAK_MPI_OBSERVER_OK;
}

PeakMpiObserverStatus peak_mpi_observer_set_cost(PeakMpiObserver *o,
    const PeakMpiObserverProducer *p, uint64_t L, uint64_t P) {
    PeakMpiObserverSlot *s = owned(o, p);
    if (!s) return PEAK_MPI_OBSERVER_BAD_PRODUCER;
    if (P > L || P < atomic_load_explicit(&s->P, memory_order_relaxed) ||
        L - P < atomic_load_explicit(&s->other_cost, memory_order_relaxed)) {
        fail_at(o, PEAK_MPI_OBSERVER_REASON_INVALID_COST, p->slot, p->generation,
                atomic_load_explicit(&s->last_attempt, memory_order_relaxed));
        return PEAK_MPI_OBSERVER_BAD_ARGUMENT;
    }
    /* Separate monotonic lanes avoid a per-callback atomic RMW, version fence
     * or reader retry. Concurrent boundaries conservatively sample intervals. */
    atomic_store_explicit(&s->P, P, memory_order_relaxed);
    atomic_store_explicit(&s->other_cost, L - P, memory_order_relaxed);
    return PEAK_MPI_OBSERVER_OK;
}

PeakMpiObserverStatus peak_mpi_observer_emit(PeakMpiObserver *o,
    const PeakMpiObserverProducer *p, const PeakMpiObserverRecord *r) {
    PeakMpiObserverSlot *s = owned(o, p);
    if (!s) return PEAK_MPI_OBSERVER_BAD_PRODUCER;
    if (!r) return PEAK_MPI_OBSERVER_BAD_ARGUMENT;
    if (!atomic_load_explicit(&o->active, memory_order_acquire))
        return PEAK_MPI_OBSERVER_INACTIVE;
    if (atomic_load_explicit(&o->fault_state, memory_order_acquire))
        return PEAK_MPI_OBSERVER_COARSE;
    uint64_t seq = atomic_load_explicit(&s->last_attempt, memory_order_relaxed);
    if (seq == UINT64_MAX) {
        fail_at(o, PEAK_MPI_OBSERVER_REASON_SEQUENCE_LIMIT, p->slot, p->generation, seq);
        return PEAK_MPI_OBSERVER_CAPACITY;
    }
    ++seq;
    atomic_store_explicit(&s->last_attempt, seq, memory_order_relaxed);
    if (r->P_cut > r->L_cut || r->kind < PEAK_MPI_OBSERVER_EVENT_START ||
        r->kind > PEAK_MPI_OBSERVER_EVENT_BATCH_END ||
        ((r->kind == PEAK_MPI_OBSERVER_EVENT_START ||
          r->kind == PEAK_MPI_OBSERVER_EVENT_COMPLETE ||
          r->kind == PEAK_MPI_OBSERVER_EVENT_PROGRESS) && !r->op_uid)) {
        lose_record(s, seq);
        fail_at(o, r->P_cut > r->L_cut ? PEAK_MPI_OBSERVER_REASON_INVALID_COST :
                PEAK_MPI_OBSERVER_REASON_IDENTITY, p->slot, p->generation, seq);
        return PEAK_MPI_OBSERVER_BAD_ARGUMENT;
    }
    uint64_t head = atomic_load_explicit(&s->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&s->tail, memory_order_acquire);
    if (head - tail >= o->config.records_per_slot) {
        lose_record(s, seq);
        fail_at(o, PEAK_MPI_OBSERVER_REASON_RING_OVERFLOW, p->slot, p->generation, seq);
        return PEAK_MPI_OBSERVER_OVERFLOW;
    }
    PeakMpiObserverRecord copy = *r;
    copy.incarnation = o->config.incarnation;
    copy.thread_uid = p->thread_uid;
    copy.thread_generation = p->generation;
    copy.sequence = seq;
    memset(copy.reserved, 0, sizeof(copy.reserved));
    s->records[head % o->config.records_per_slot] = copy;
    atomic_store_explicit(&s->head, head + 1, memory_order_release);
    return PEAK_MPI_OBSERVER_OK;
}

bool peak_mpi_observer_consume(PeakMpiObserver *o, size_t slot,
                               PeakMpiObserverRecord *r) {
    if (!o || !r || slot >= o->config.slots) return false;
    PeakMpiObserverSlot *s = &o->slots[slot];
    uint64_t tail = atomic_load_explicit(&s->tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&s->head, memory_order_acquire)) return false;
    *r = s->records[tail % o->config.records_per_slot];
    atomic_store_explicit(&s->tail, tail + 1, memory_order_release);
    return true;
}

bool peak_mpi_observer_slot_state(const PeakMpiObserver *o, size_t slot,
                                  PeakMpiObserverSlotState *out) {
    if (!o || !out || slot >= o->config.slots) return false;
    const PeakMpiObserverSlot *s = &o->slots[slot];
    uint64_t P = atomic_load_explicit(&s->P, memory_order_relaxed);
    uint64_t other = atomic_load_explicit(&s->other_cost, memory_order_relaxed);
    out->L = P + other; out->P = P;
    out->generation = atomic_load_explicit(&s->generation, memory_order_acquire);
    out->lost = atomic_load_explicit(&s->lost, memory_order_acquire);
    out->last_attempt_sequence = atomic_load_explicit(&s->last_attempt, memory_order_relaxed);
    out->first_lost_sequence = atomic_load_explicit(&s->first_lost, memory_order_relaxed);
    out->consumed = atomic_load_explicit(&s->tail, memory_order_acquire);
    out->published = atomic_load_explicit(&s->head, memory_order_acquire);
    out->claimed = atomic_load_explicit(&s->owner, memory_order_acquire) != 0;
    return true;
}

bool peak_mpi_observer_cost_snapshot(PeakMpiObserver *o, uint64_t *L, uint64_t *P) {
    if (!o || !L || !P) return false;
    uint64_t profile = 0, other = 0;
    for (size_t i = 0; i < o->config.slots; ++i) {
        uint64_t p = atomic_load_explicit(&o->slots[i].P, memory_order_relaxed);
        uint64_t c = atomic_load_explicit(&o->slots[i].other_cost, memory_order_relaxed);
        if (p > UINT64_MAX - profile || c > UINT64_MAX - other) goto overflow;
        profile += p;
        other += c;
    }
    if (other > UINT64_MAX - profile) goto overflow;
    *P = profile;
    *L = profile + other;
    return true;
overflow:
    fail_at(o, PEAK_MPI_OBSERVER_REASON_INVALID_COST, SIZE_MAX, 0, 0);
    return false;
}

PeakMpiObserverStatus peak_mpi_observer_object_add(PeakMpiObserver *o,
    const PeakMpiObserverObject *d, uint64_t *uid) {
    if (!o || !d || !uid || !d->generation) return PEAK_MPI_OBSERVER_BAD_ARGUMENT;
    if (atomic_load_explicit(&o->fault_state, memory_order_acquire))
        return PEAK_MPI_OBSERVER_COARSE;
    if (pthread_mutex_lock(&o->object_mutex)) {
        fail_at(o, PEAK_MPI_OBSERVER_REASON_METADATA, SIZE_MAX, 0, 0);
        return PEAK_MPI_OBSERVER_BAD_ARGUMENT;
    }
    size_t count = atomic_load_explicit(&o->object_count, memory_order_relaxed);
    if (count == o->config.object_capacity) {
        pthread_mutex_unlock(&o->object_mutex);
        fail_at(o, PEAK_MPI_OBSERVER_REASON_OBJECT_CAPACITY, SIZE_MAX, 0, 0);
        return PEAK_MPI_OBSERVER_CAPACITY;
    }
    o->objects[count] = *d;
    o->objects[count].uid = (uint64_t)count + 1;
    *uid = o->objects[count].uid;
    atomic_store_explicit(&o->object_count, count + 1, memory_order_release);
    pthread_mutex_unlock(&o->object_mutex);
    return PEAK_MPI_OBSERVER_OK;
}

const PeakMpiObserverObject *peak_mpi_observer_object_get(const PeakMpiObserver *o,
                                                         uint64_t uid) {
    if (!o || !uid || uid > atomic_load_explicit(&o->object_count, memory_order_acquire))
        return NULL;
    return &o->objects[(size_t)(uid - 1)];
}

PeakMpiObserverStatus peak_mpi_observer_feedback_publish(PeakMpiObserver *o,
    uint64_t generation, const void *payload, size_t size) {
    if (!o || !payload || !size || size > PEAK_MPI_OBSERVER_FEEDBACK_BYTES)
        return PEAK_MPI_OBSERVER_BAD_ARGUMENT;
    int lock_result = pthread_mutex_trylock(&o->feedback_mutex);
    if (lock_result) {
        if (lock_result == EBUSY) return PEAK_MPI_OBSERVER_BUSY;
        fail_at(o, PEAK_MPI_OBSERVER_REASON_METADATA, SIZE_MAX, 0, 0);
        return PEAK_MPI_OBSERVER_BAD_ARGUMENT;
    }
    if (o->has_feedback && generation <= o->feedback.generation) {
        pthread_mutex_unlock(&o->feedback_mutex);
        return PEAK_MPI_OBSERVER_OLD_GENERATION;
    }
    o->feedback.generation = generation;
    o->feedback.size = size;
    memcpy(o->feedback.payload, payload, size);
    memset(o->feedback.payload + size, 0, sizeof(o->feedback.payload) - size);
    o->has_feedback = true;
    pthread_mutex_unlock(&o->feedback_mutex);
    return PEAK_MPI_OBSERVER_OK;
}

PeakMpiObserverStatus peak_mpi_observer_feedback_read(PeakMpiObserver *o,
    PeakMpiObserverFeedback *feedback) {
    if (!o || !feedback) return PEAK_MPI_OBSERVER_BAD_ARGUMENT;
    int lock_result = pthread_mutex_trylock(&o->feedback_mutex);
    if (lock_result) {
        if (lock_result == EBUSY) return PEAK_MPI_OBSERVER_BUSY;
        fail_at(o, PEAK_MPI_OBSERVER_REASON_METADATA, SIZE_MAX, 0, 0);
        return PEAK_MPI_OBSERVER_BAD_ARGUMENT;
    }
    PeakMpiObserverStatus result = PEAK_MPI_OBSERVER_EMPTY;
    if (o->has_feedback) { *feedback = o->feedback; result = PEAK_MPI_OBSERVER_OK; }
    pthread_mutex_unlock(&o->feedback_mutex);
    return result;
}
