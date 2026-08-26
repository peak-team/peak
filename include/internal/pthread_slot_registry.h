#ifndef PEAK_PTHREAD_SLOT_REGISTRY_H
#define PEAK_PTHREAD_SLOT_REGISTRY_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define PEAK_PTHREAD_SLOT_REGISTRY_MAX_CAPACITY 4096U

typedef struct {
    size_t slot;
    uint64_t generation;
} PeakPthreadSlotToken;

typedef enum {
    PEAK_PTHREAD_START_PENDING = 0,
    PEAK_PTHREAD_START_READY = 1,
    PEAK_PTHREAD_START_ABANDONED = 2,
} PeakPthreadStartHandshakeState;

typedef struct {
    pthread_t tid;
    PeakPthreadSlotToken token;
    bool occupied;
} PeakPthreadSlotRegistryEntry;

/* The registry is deliberately allocation-free. It is used only from the
 * pthread create/join lifecycle, and its fixed maximum matches the configured
 * PEAK thread limit. */
typedef struct {
    pthread_mutex_t mutex;
    PeakPthreadSlotRegistryEntry entries[PEAK_PTHREAD_SLOT_REGISTRY_MAX_CAPACITY];
    size_t reusable[PEAK_PTHREAD_SLOT_REGISTRY_MAX_CAPACITY];
    size_t reusable_head;
    size_t reusable_count;
    size_t next_slot;
    size_t capacity;
    uint64_t next_generation;
    bool initialized;
} PeakPthreadSlotRegistry;

bool peak_pthread_slot_registry_init(PeakPthreadSlotRegistry* registry,
                                     size_t capacity);
void peak_pthread_slot_registry_destroy(PeakPthreadSlotRegistry* registry);
bool peak_pthread_slot_registry_reserve_insert(
    PeakPthreadSlotRegistry* registry, pthread_t tid, PeakPthreadSlotToken* token);
bool peak_pthread_slot_registry_capture(PeakPthreadSlotRegistry* registry,
                                        pthread_t tid,
                                        PeakPthreadSlotToken* token);
bool peak_pthread_slot_registry_compare_remove(PeakPthreadSlotRegistry* registry,
                                               pthread_t tid,
                                               uint64_t generation,
                                               bool reusable);
bool peak_pthread_slot_registry_quarantine(PeakPthreadSlotRegistry* registry,
                                           pthread_t tid);
bool peak_pthread_slot_registry_contains(PeakPthreadSlotRegistry* registry,
                                         pthread_t tid);
size_t peak_pthread_slot_registry_snapshot(PeakPthreadSlotRegistry* registry,
                                           pthread_t* tids,
                                           size_t* slots,
                                           size_t capacity,
                                           bool* complete);
bool peak_pthread_slot_registry_publish_ready(_Atomic int* state);
PeakPthreadStartHandshakeState peak_pthread_slot_registry_wait_ready(
    _Atomic int* state, unsigned int timeout_ms);
#ifdef PEAK_ENABLE_TEST_HOOKS
unsigned int peak_pthread_slot_registry_test_wake_waiters(void);
#endif

#endif /* PEAK_PTHREAD_SLOT_REGISTRY_H */
