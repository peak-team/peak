#define _GNU_SOURCE

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "frida-gum.h"

static atomic_bool normal_release;
static atomic_bool exit_release;
static atomic_uint parked_workers;
static atomic_ulong target_sink;
static _Thread_local jmp_buf unwind_target;
static void (*release_thread_state)(void);

__attribute__((noinline, noclone, used, externally_visible,
               visibility("default")))
uintptr_t
peak_fastpath_lifetime_handoff_target(uintptr_t mode)
{
    if (mode != 0) {
        atomic_fetch_add_explicit(&parked_workers, 1, memory_order_release);
        atomic_bool* release = mode == 2 ? &exit_release : &normal_release;
        while (!atomic_load_explicit(release, memory_order_acquire)) {
            sched_yield();
        }
    }
    if (mode == 2) {
        longjmp(unwind_target, 1);
    }
    atomic_fetch_add_explicit(&target_sink, mode + 1, memory_order_relaxed);
    return mode + 1;
}

static void*
required_symbol(const char* name)
{
    void* symbol = dlsym(RTLD_DEFAULT, name);
    if (symbol == NULL) {
        fprintf(stderr, "missing symbol %s: %s\n", name, dlerror());
    }
    return symbol;
}

static void*
worker_main(void* argument)
{
    (void)peak_fastpath_lifetime_handoff_target(0);
    uintptr_t mode = (uintptr_t)argument;
    if (mode == 2) {
        if (setjmp(unwind_target) == 0) {
            (void)peak_fastpath_lifetime_handoff_target(mode);
        } else {
            release_thread_state();
        }
    } else {
        (void)peak_fastpath_lifetime_handoff_target(mode);
    }
    return NULL;
}

typedef struct {
    GumInterceptor* interceptor;
    gpointer address;
    GumInvocationListener* listener;
    gboolean (*prepare)(GumInterceptor*, gpointer, GumInvocationListener*);
    gboolean result;
} PrepareArgs;

static void*
prepare_main(void* data)
{
    PrepareArgs* args = data;
    args->result =
        args->prepare(args->interceptor, args->address, args->listener);
    return NULL;
}

static gboolean
flush_until_done(GumInterceptor* interceptor,
                 gboolean (*flush)(GumInterceptor*))
{
    for (unsigned int attempt = 0; attempt < 10000; attempt++) {
        if (flush(interceptor)) {
            return TRUE;
        }
        sched_yield();
    }
    return FALSE;
}

int
main(void)
{
    typedef void (*TransactionFunc)(GumInterceptor*);
    typedef void (*DetachFunc)(GumInterceptor*, GumInvocationListener*);
    typedef gboolean (*FlushFunc)(GumInterceptor*);
    typedef gboolean (*PrepareFunc)(
        GumInterceptor*, gpointer, GumInvocationListener*);
    typedef GumAttachReturn (*PeakAttachFunc)(
        GumInterceptor*, gpointer, GumInvocationListener*, const void*);
    typedef void (*PauseEnableFunc)(void);
    typedef int (*PauseHeldFunc)(void);
    typedef void (*PauseReleaseFunc)(void);

    for (uintptr_t i = 0; i < 8; i++) {
        (void)peak_fastpath_lifetime_handoff_target(0);
    }

    GumInterceptor** interceptor =
        (GumInterceptor**)required_symbol("interceptor");
    GumInvocationListener*** listeners =
        (GumInvocationListener***)required_symbol("array_listener");
    gpointer** addresses = (gpointer**)required_symbol("hook_address");
    TransactionFunc begin = (TransactionFunc)required_symbol(
        "gum_interceptor_begin_transaction");
    TransactionFunc end = (TransactionFunc)required_symbol(
        "gum_interceptor_end_transaction");
    DetachFunc detach =
        (DetachFunc)required_symbol("gum_interceptor_detach");
    FlushFunc flush = (FlushFunc)required_symbol("gum_interceptor_flush");
    PrepareFunc prepare = (PrepareFunc)required_symbol(
        "gum_interceptor_peak_prepare_fast_detach");
    PeakAttachFunc attach = (PeakAttachFunc)required_symbol(
        "peak_general_listener_test_gum_attach_target");
    PauseEnableFunc close_pause_enable = (PauseEnableFunc)required_symbol(
        "peak_general_listener_test_fast_close_pause_enable");
    PauseHeldFunc close_is_held = (PauseHeldFunc)required_symbol(
        "peak_general_listener_test_fast_close_is_held");
    PauseReleaseFunc close_release = (PauseReleaseFunc)required_symbol(
        "peak_general_listener_test_fast_close_release");
    PauseEnableFunc abandon_pause_enable = (PauseEnableFunc)required_symbol(
        "peak_general_listener_test_fast_abandon_pause_enable");
    PauseHeldFunc abandon_is_held = (PauseHeldFunc)required_symbol(
        "peak_general_listener_test_fast_abandon_is_held");
    PauseReleaseFunc abandon_release = (PauseReleaseFunc)required_symbol(
        "peak_general_listener_test_fast_abandon_release");
    PauseHeldFunc abandon_waiting_count = (PauseHeldFunc)required_symbol(
        "peak_general_listener_test_fast_abandon_waiting_count");
    PauseEnableFunc preclose_pause_enable = (PauseEnableFunc)required_symbol(
        "peak_general_listener_test_fast_preclose_pause_enable");
    PauseHeldFunc preclose_is_held = (PauseHeldFunc)required_symbol(
        "peak_general_listener_test_fast_preclose_is_held");
    PauseReleaseFunc preclose_release = (PauseReleaseFunc)required_symbol(
        "peak_general_listener_test_fast_preclose_release");
    PauseHeldFunc close_is_waiting = (PauseHeldFunc)required_symbol(
        "peak_general_listener_test_fast_close_is_waiting");
    release_thread_state = (void (*)(void))required_symbol(
        "peak_general_listener_test_release_current_thread_state");
    if (interceptor == NULL || listeners == NULL || addresses == NULL ||
        begin == NULL || end == NULL || detach == NULL || flush == NULL ||
        prepare == NULL || attach == NULL || close_pause_enable == NULL ||
        close_is_held == NULL || close_release == NULL ||
        abandon_pause_enable == NULL || abandon_is_held == NULL ||
        abandon_release == NULL || abandon_waiting_count == NULL ||
        preclose_pause_enable == NULL || preclose_is_held == NULL ||
        preclose_release == NULL || close_is_waiting == NULL ||
        release_thread_state == NULL || *interceptor == NULL ||
        *listeners == NULL || *addresses == NULL ||
        (*listeners)[0] == NULL || (*addresses)[0] == NULL) {
        return 1;
    }

    pthread_t seed_worker;
    atomic_store_explicit(&exit_release, false, memory_order_relaxed);
    atomic_store_explicit(&parked_workers, 0, memory_order_relaxed);
    if (pthread_create(&seed_worker, NULL, worker_main, (void*)2) != 0) {
        fputs("seed worker create failed\n", stderr);
        return 1;
    }
    while (atomic_load_explicit(&parked_workers, memory_order_acquire) != 1) {
        sched_yield();
    }
    PrepareArgs prepare_args = {
        .interceptor = *interceptor,
        .address = (*addresses)[0],
        .listener = (*listeners)[0],
        .prepare = prepare,
        .result = FALSE,
    };
    /*
     * Hold prepare after it has closed the abandon handoff but before Gum's
     * usage seed is published. The non-local-unwind worker must wait on the
     * post-seed side and then release exactly one counted frame.
     */
    close_pause_enable();
    pthread_t prepare_thread;
    if (pthread_create(&prepare_thread, NULL, prepare_main, &prepare_args) !=
        0) {
        fputs("prepare thread create failed\n", stderr);
        return 1;
    }
    while (!close_is_held()) {
        sched_yield();
    }
    atomic_store_explicit(&exit_release, true, memory_order_release);
    while (abandon_waiting_count() != 1) {
        sched_yield();
    }
    close_release();
    if (pthread_join(prepare_thread, NULL) != 0 || !prepare_args.result) {
        fputs("seed handoff prepare failed\n", stderr);
        return 1;
    }
    if (pthread_join(seed_worker, NULL) != 0) {
        fputs("seed worker join failed\n", stderr);
        return 1;
    }
    begin(*interceptor);
    detach(*interceptor, (*listeners)[0]);
    end(*interceptor);
    if (!flush_until_done(*interceptor, flush)) {
        fputs("seed handoff did not flush\n", stderr);
        return 1;
    }
    begin(*interceptor);
    GumAttachReturn status =
        attach(*interceptor, (*addresses)[0], (*listeners)[0], NULL);
    end(*interceptor);
    if (status != GUM_ATTACH_OK) {
        fprintf(stderr, "first restore failed: %d\n", status);
        return 1;
    }

    atomic_store_explicit(&normal_release, false, memory_order_relaxed);
    atomic_store_explicit(&exit_release, false, memory_order_relaxed);
    atomic_store_explicit(&parked_workers, 0, memory_order_relaxed);
    pthread_t exiting_worker;
    pthread_t normal_worker;
    if (pthread_create(&exiting_worker, NULL, worker_main, (void*)2) != 0 ||
        pthread_create(&normal_worker, NULL, worker_main, (void*)1) != 0) {
        fputs("abandon worker create failed\n", stderr);
        return 1;
    }
    while (atomic_load_explicit(&parked_workers, memory_order_acquire) != 2) {
        sched_yield();
    }
    if (!prepare(*interceptor, (*addresses)[0], (*listeners)[0])) {
        fputs("abandon handoff prepare failed\n", stderr);
        return 1;
    }
    /*
     * Pause one abandon after active removal. The normal leave removes the
     * last active frame and clears the descriptor before the abandon resumes;
     * its stable token must still decrement Gum's seeded usage counter.
     */
    abandon_pause_enable();
    atomic_store_explicit(&exit_release, true, memory_order_release);
    while (!abandon_is_held()) {
        sched_yield();
    }
    atomic_store_explicit(&normal_release, true, memory_order_release);
    if (pthread_join(normal_worker, NULL) != 0) {
        fputs("normal worker join failed\n", stderr);
        return 1;
    }
    abandon_release();
    if (pthread_join(exiting_worker, NULL) != 0) {
        fputs("exiting worker join failed\n", stderr);
        return 1;
    }
    begin(*interceptor);
    detach(*interceptor, (*listeners)[0]);
    end(*interceptor);
    if (!flush_until_done(*interceptor, flush)) {
        fputs("abandon handoff did not flush\n", stderr);
        return 1;
    }

    begin(*interceptor);
    status = attach(*interceptor, (*addresses)[0], (*listeners)[0], NULL);
    end(*interceptor);
    if (status != GUM_ATTACH_OK) {
        fprintf(stderr, "final restore failed: %d\n", status);
        return 1;
    }

    /*
     * Cover the other side of the close handoff: an abandoner that registered
     * while open removes its active frame, and close must wait for that reader
     * before taking a snapshot. No Gum usage decrement belongs to this frame.
     */
    atomic_store_explicit(&exit_release, false, memory_order_relaxed);
    atomic_store_explicit(&parked_workers, 0, memory_order_relaxed);
    pthread_t preclose_worker;
    if (pthread_create(&preclose_worker, NULL, worker_main, (void*)2) != 0) {
        fputs("preclose worker create failed\n", stderr);
        return 1;
    }
    while (atomic_load_explicit(&parked_workers, memory_order_acquire) != 1) {
        sched_yield();
    }
    preclose_pause_enable();
    atomic_store_explicit(&exit_release, true, memory_order_release);
    while (!preclose_is_held()) {
        sched_yield();
    }
    close_pause_enable();
    prepare_args.result = FALSE;
    if (pthread_create(&prepare_thread, NULL, prepare_main, &prepare_args) !=
        0) {
        fputs("preclose prepare thread create failed\n", stderr);
        return 1;
    }
    while (!close_is_waiting()) {
        sched_yield();
    }
    preclose_release();
    while (!close_is_held()) {
        sched_yield();
    }
    close_release();
    if (pthread_join(prepare_thread, NULL) != 0 || !prepare_args.result) {
        fputs("preclose handoff prepare failed\n", stderr);
        return 1;
    }
    if (pthread_join(preclose_worker, NULL) != 0) {
        fputs("preclose worker join failed\n", stderr);
        return 1;
    }
    begin(*interceptor);
    detach(*interceptor, (*listeners)[0]);
    end(*interceptor);
    if (!flush_until_done(*interceptor, flush)) {
        fputs("preclose handoff did not flush\n", stderr);
        return 1;
    }
    begin(*interceptor);
    status = attach(*interceptor, (*addresses)[0], (*listeners)[0], NULL);
    end(*interceptor);
    if (status != GUM_ATTACH_OK) {
        fprintf(stderr, "preclose restore failed: %d\n", status);
        return 1;
    }

    puts("fastpath_lifetime_handoff_ok");
    return 0;
}
