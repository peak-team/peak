#ifndef PEAK_PTHREAD_SLOT_REGISTRY_H
#define PEAK_PTHREAD_SLOT_REGISTRY_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/types.h>

#define PEAK_PTHREAD_SLOT_REGISTRY_MAX_CAPACITY 4096U

typedef struct {
    size_t slot;
    uint64_t generation;
} PeakPthreadSlotToken;

typedef struct {
    PeakPthreadSlotToken token;
    pid_t kernel_tid;
} PeakPthreadDetachedCandidate;

typedef enum {
    PEAK_PTHREAD_START_PENDING = 0,
    PEAK_PTHREAD_START_READY = 1,
    PEAK_PTHREAD_START_ABANDONED = 2,
} PeakPthreadStartHandshakeState;

typedef struct {
    pthread_t tid;
    PeakPthreadSlotToken token;
    pid_t kernel_tid;
    bool occupied;
    bool detached;
    bool final_destructor_pass;
    bool exit_pending;
    bool retiring;
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
    size_t reclaim_cursor;
    size_t exit_pending_count;
    size_t max_exit_pending_count;
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
bool peak_pthread_slot_registry_compare_remove_token(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadSlotToken token,
    bool reusable);
bool peak_pthread_slot_registry_begin_retire(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadSlotToken token);
bool peak_pthread_slot_registry_complete_retire(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadSlotToken token,
    bool reusable);
bool peak_pthread_slot_registry_defer_retire(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadSlotToken token);
bool peak_pthread_slot_registry_mark_kernel_tid(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadSlotToken token,
    pid_t kernel_tid);
bool peak_pthread_slot_registry_mark_detached(
    PeakPthreadSlotRegistry* registry,
    pthread_t tid,
    uint64_t generation,
    bool* became_exit_pending);
bool peak_pthread_slot_registry_mark_final_destructor(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadSlotToken token,
    bool* became_exit_pending);
size_t peak_pthread_slot_registry_snapshot_exit_pending(
    PeakPthreadSlotRegistry* registry,
    PeakPthreadDetachedCandidate* candidates,
    size_t budget,
    size_t* entries_examined);
size_t peak_pthread_slot_registry_exit_pending_count(
    PeakPthreadSlotRegistry* registry,
    size_t* max_count);
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
