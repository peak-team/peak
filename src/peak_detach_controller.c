#define _GNU_SOURCE
#include "peak_detach_controller.h"
#include "peak_detach_helper_protocol.h"
#include "peak_logging.h"
#include "peak_signal_policy_internal.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PEAK_DETACH_CONTROLLER_IO_TIMEOUT_MS_ENV \
    "PEAK_DETACH_CONTROLLER_IO_TIMEOUT_MS"
#define PEAK_DETACH_HELPER_SPAWN_ENV "PEAK_DETACH_HELPER_SPAWN"
#define PEAK_TEST_DETACH_HELPER_FORCE_CLONE_SPAWN_FAIL_ENV \
    "PEAK_TEST_DETACH_HELPER_FORCE_CLONE_SPAWN_FAIL"
#define PEAK_TEST_DETACH_SIGNAL_INJECT_STALE_BEFORE_STOP_ENV \
    "PEAK_TEST_DETACH_SIGNAL_INJECT_STALE_BEFORE_STOP"
#define PEAK_TEST_DETACH_SIGNAL_INJECT_FUTURE_BEFORE_STOP_ENV \
    "PEAK_TEST_DETACH_SIGNAL_INJECT_FUTURE_BEFORE_STOP"
#define PEAK_DETACH_CONTROLLER_DEFAULT_IO_TIMEOUT_MS 10000u
#define PEAK_DETACH_HELPER_CLOSE_GRACE_MS 250u
#define PEAK_DETACH_HELPER_CLOSE_POLL_US 1000u
#define PEAK_DETACH_MAX_PHYSICAL_PATCH_RECORDS 8192u
#define PEAK_DETACH_HELPER_PROTOCOL_FD 3
#define PEAK_AUTO_HELPER_PERF_FALLBACK_STOP_WINDOW_US_ENV \
    "PEAK_AUTO_HELPER_PERF_FALLBACK_STOP_WINDOW_US"
#define PEAK_AUTO_HELPER_PERF_FALLBACK_DEFAULT_STOP_WINDOW_US 5000u
#define PEAK_DETACH_CONTROLLER_MAX_BATCH_REQUESTS \
    PEAK_DETACH_HELPER_MAX_BATCH_WRITES
#define PEAK_STRICT_GATE_WAIT_TIMEOUT_MS_ENV "PEAK_STRICT_GATE_WAIT_TIMEOUT_MS"
#define PEAK_STRICT_GATE_WAIT_DEFAULT_TIMEOUT_MS 10000u

#undef g_printerr
#define g_printerr(...) peak_log_warn(__VA_ARGS__)

typedef enum {
    PEAK_SAFE_DETACH_MODE_COMPATIBILITY = 0,
    PEAK_SAFE_DETACH_MODE_STRICT
} PeakSafeDetachMode;

static gsize mode_initialized = 0;
static PeakSafeDetachMode configured_mode = PEAK_SAFE_DETACH_MODE_STRICT;
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
static gboolean warned_missing_gum_api = FALSE;
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#ifdef __linux__
#include <linux/futex.h>
#endif
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ucontext.h>

typedef enum {
    PEAK_DETACH_REQUESTED_BACKEND_AUTO = 0,
    PEAK_DETACH_REQUESTED_BACKEND_HELPER,
    PEAK_DETACH_REQUESTED_BACKEND_SIGNAL
} PeakDetachRequestedBackend;

typedef enum {
    PEAK_DETACH_HOLD_BACKEND_NONE = 0,
    PEAK_DETACH_HOLD_BACKEND_HELPER,
    PEAK_DETACH_HOLD_BACKEND_SIGNAL
} PeakDetachHoldBackend;

typedef struct {
    gboolean active;
    gboolean finishing;
    gboolean batch;
    gboolean auto_helper_candidate;
    gboolean uses_physical_patch;
    gboolean mutates_entry_bytes;
    gboolean mutates_gum_metadata;
    gboolean frees_listener_state;
    gboolean requires_target_entry_idle;
    gboolean owner_thread_set;
    pthread_t owner_thread;
    PeakDetachOperation operation;
    size_t hook_id;
    gpointer function_address;
    GumInvocationListener* listener;
    PeakDetachHoldBackend backend;
    unsigned int timeout_ms;
    double deadline;
    PeakDetachHelperInstruction instructions[PEAK_DETACH_HELPER_MAX_INSTRUCTIONS];
    uint32_t instruction_count;
    void* deferred_cleanup_1;
    void* deferred_cleanup_2;
} PeakDetachHeldMutation;

typedef struct {
    gboolean used;
    size_t hook_id;
    gpointer function_address;
    uint8_t active_patch[PEAK_DETACH_HELPER_MAX_PATCH_BYTES];
    uint32_t patch_size;
} PeakDetachPhysicalPatchRecord;

typedef struct {
    gboolean has_context;
    gboolean has_patch;
    GumPeakPcDiagnostics diagnostics;
    uint8_t active_patch[PEAK_DETACH_HELPER_MAX_PATCH_BYTES];
    uint8_t original_prologue[PEAK_DETACH_HELPER_MAX_PATCH_BYTES];
    guint prologue_len;
} PeakDetachGumSnapshot;

typedef struct {
    gboolean valid;
    gboolean safe;
    PeakDetachStatus status;
    PeakDetachGumSnapshot snapshot;
    PeakDetachHeldMutation mutation;
} PeakDetachBatchCandidate;

typedef struct {
    _Atomic int active_epoch;
    _Atomic int arrived_epoch;
    _Atomic int done_epoch;
    _Atomic int rewrite_epoch;
    _Atomic int rewrite_status;
    _Atomic int evacuate_epoch;
    _Atomic int evacuate_started_epoch;
    _Atomic int evacuated_epoch;
    _Atomic int evacuate_status;
    _Atomic int tid;
    _Atomic(uintptr_t) pc;
    _Atomic(uintptr_t) new_pc;
    _Atomic(uintptr_t) evacuate_breakpoint_pc;
} PeakDetachSignalSlot;

typedef struct {
    _Atomic int tid;
    _Atomic int gate_epoch;
} PeakDetachGateWaiterSlot;

typedef struct {
    unsigned long suppressed_rows;
    int last_epoch;
    PeakDetachStatus last_status;
    uint32_t active;
    uint32_t arrived;
    uint32_t done;
    uint32_t evacuate_started;
    uint32_t evacuated;
    pid_t first_pending_tid;
    pid_t first_not_done_tid;
} PeakDetachSignalDeferredTrace;

static void peak_detach_controller_signal_release_or_fatal_with_timeout(
    const char* context,
    unsigned int timeout_ms);
static void peak_detach_controller_signal_handler(int signo,
                                                  siginfo_t* info,
                                                  void* context);
static void peak_detach_controller_signal_trap_handler(int signo,
                                                       siginfo_t* info,
                                                       void* context);
static void peak_detach_controller_init_signal_backend_once(void);
static gboolean peak_detach_controller_ensure_signal_backend(
    PeakDetachStatus* status_out);
static gboolean peak_detach_controller_close_helper(void);
static int peak_detach_controller_signal_tid_blocks_reserved_once(pid_t tid);
static gboolean peak_detach_controller_signal_slot_active(
    const PeakDetachSignalSlot* slot,
    int epoch);
static PeakDetachStatus peak_detach_controller_errno_status(void);
static double peak_detach_controller_monotonic_second(void);
extern char** environ;

static pthread_mutex_t mutation_guard_mutex;
static pthread_once_t mutation_guard_mutex_initialized = PTHREAD_ONCE_INIT;
static pthread_once_t atfork_initialized = PTHREAD_ONCE_INIT;
static int helper_fd = -1;
static pid_t helper_pid = -1;
static pid_t helper_owner_pid = -1;
static gboolean helper_warmup_active = FALSE;
static gboolean helper_warmup_failed = FALSE;
static PeakDetachStatus helper_warmup_status = PEAK_DETACH_STATUS_SAFE;
static gboolean warned_helper_unavailable = FALSE;
static gboolean warned_helper_resume_failed = FALSE;
static gboolean warned_helper_fatal = FALSE;
static gboolean warned_auto_helper_ptrace_scope = FALSE;
static gboolean warned_auto_helper_fallback_cached = FALSE;
static gboolean warned_auto_helper_performance_fallback_cached = FALSE;
static gboolean helper_state_fatal = FALSE;
static gboolean warned_signal_gate_unavailable = FALSE;
static gboolean warned_helper_gate_unavailable = FALSE;
static gsize auto_helper_perf_fallback_initialized = 0;
static unsigned int auto_helper_perf_fallback_stop_window_us =
    PEAK_AUTO_HELPER_PERF_FALLBACK_DEFAULT_STOP_WINDOW_US;
static gboolean warned_strict_gate_wait_timeout = FALSE;
static gboolean signal_breakpoint_supported = FALSE;
static struct sigaction previous_trap_action;
static char resolved_helper_path[PATH_MAX];
static PeakDetachHeldMutation held_mutation = { 0 };
static PeakDetachPhysicalPatchRecord physical_patch_records[PEAK_DETACH_MAX_PHYSICAL_PATCH_RECORDS];
static double held_mutation_started_at = 0.0;
static _Atomic int held_mutation_window_active = 0;
static double last_stop_window_us = 0.0;
static PeakDetachSignalDeferredTrace signal_deferred_trace = { 0 };
static _Atomic int trace_diagnostics_enabled = 0;
static _Atomic int trace_diagnostics_fd = -1;
static _Atomic int auto_signal_fallback_cached = 0;
static char trace_diagnostics_path[PATH_MAX];
static pthread_once_t signal_backend_initialized = PTHREAD_ONCE_INIT;
static _Atomic int signal_backend_signum = 0;
static int signal_trap_handler_installed = 0;
static _Atomic int signal_epoch_counter = 1;
static _Atomic int signal_hold_epoch = 0;
static _Atomic int signal_release_epoch = 0;
static _Atomic int strict_mutation_thread_gate = 0;
static _Atomic int strict_mutation_thread_gate_epoch = 1;
static _Atomic int strict_mutation_thread_gate_installed = 0;
static PeakDetachSignalSlot signal_slots[PEAK_DETACH_HELPER_MAX_THREADS];
static PeakDetachGateWaiterSlot gate_waiter_slots[PEAK_DETACH_HELPER_MAX_THREADS];
static _Atomic uint32_t signal_slot_count = 0;
static _Atomic int signal_stale_delivery_count = 0;
static gsize strict_gate_wait_timeout_initialized = 0;
static double strict_gate_wait_timeout_s =
    (double)PEAK_STRICT_GATE_WAIT_DEFAULT_TIMEOUT_MS / 1000.0;
static gsize controller_io_timeout_initialized = 0;
static unsigned int controller_io_timeout_ms =
    PEAK_DETACH_CONTROLLER_DEFAULT_IO_TIMEOUT_MS;

static gboolean peak_detach_controller_stop_window_trace_deferred(void);

static void
peak_detach_controller_raw_close(int fd)
{
    if (fd >= 0) {
#if defined(__linux__) && defined(__x86_64__) && defined(SYS_close)
        long result;
        __asm__ volatile("syscall"
                         : "=a"(result)
                         : "a"((long)SYS_close),
                           "D"((long)fd)
                         : "rcx", "r11", "memory");
        (void)result;
#elif defined(__linux__) && defined(__aarch64__) && defined(SYS_close)
        register long x0 __asm__("x0") = (long)fd;
        register long x8 __asm__("x8") = SYS_close;
        __asm__ volatile("svc #0"
                         : "+r"(x0)
                         : "r"(x8)
                         : "memory");
#else
        (void)syscall(SYS_close, fd);
#endif
    }
}

static void
peak_detach_controller_raw_close_range(unsigned int first,
                                       unsigned int last,
                                       unsigned int flags)
{
#ifdef SYS_close_range
#if defined(__linux__) && defined(__x86_64__)
    long result;
    register long r10 __asm__("r10") = 0;
    register long r8 __asm__("r8") = 0;
    register long r9 __asm__("r9") = 0;

    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"((long)SYS_close_range),
                       "D"((long)first),
                       "S"((long)last),
                       "d"((long)flags),
                       "r"(r10),
                       "r"(r8),
                       "r"(r9)
                     : "rcx", "r11", "memory");
    (void)result;
#elif defined(__linux__) && defined(__aarch64__)
    register long x0 __asm__("x0") = (long)first;
    register long x1 __asm__("x1") = (long)last;
    register long x2 __asm__("x2") = (long)flags;
    register long x8 __asm__("x8") = SYS_close_range;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x8)
                     : "memory");
#else
    (void)syscall(SYS_close_range, first, last, flags);
#endif
#else
    (void)first;
    (void)last;
    (void)flags;
#endif
}

static int*
peak_detach_controller_signal_futex_word(_Atomic int* word)
{
    return (int*)(void*)word;
}

static void
peak_detach_controller_signal_futex_wait(_Atomic int* word, int expected)
{
#if defined(__linux__) && defined(SYS_futex) && defined(FUTEX_WAIT)
    /*
     * Signal-backend stop windows park application threads from inside the
     * PEAK reserved signal handler.  A raw futex wait avoids burning an entire
     * core per parked thread while preserving a dependency-free wait path.
     * The short timeout is intentional: evacuation is announced through a
     * per-slot word, while the futex wait is on the shared release word.
     */
    struct timespec timeout = { 0, 1000000L };
    (void)syscall(SYS_futex,
                  peak_detach_controller_signal_futex_word(word),
                  FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                  expected,
                  &timeout,
                  NULL,
                  0);
#else
    (void)word;
    (void)expected;
#ifdef SYS_sched_yield
    (void)syscall(SYS_sched_yield);
#endif
#endif
}

static void
peak_detach_controller_signal_futex_wake_all(_Atomic int* word)
{
#if defined(__linux__) && defined(SYS_futex) && defined(FUTEX_WAKE)
    (void)syscall(SYS_futex,
                  peak_detach_controller_signal_futex_word(word),
                  FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
                  INT_MAX,
                  NULL,
                  NULL,
                  0);
#else
    (void)word;
#endif
}

static void
peak_detach_controller_signal_wake_waiters(void)
{
    peak_detach_controller_signal_futex_wake_all(&signal_release_epoch);
}

static unsigned int
peak_detach_controller_parse_uint_env(const char* name,
                                      unsigned int default_value)
{
    const char* value = g_getenv(name);
    char* end = NULL;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') {
        return default_value;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT_MAX) {
        peak_log_info("[peak] ignoring invalid %s=%s\n", name, value);
        return default_value;
    }

    return (unsigned int)parsed;
}

static void
peak_detach_controller_init_io_timeout_once(void)
{
    controller_io_timeout_ms =
        peak_detach_controller_parse_uint_env(
            PEAK_DETACH_CONTROLLER_IO_TIMEOUT_MS_ENV,
            PEAK_DETACH_CONTROLLER_DEFAULT_IO_TIMEOUT_MS);
    if (controller_io_timeout_ms == 0) {
        controller_io_timeout_ms =
            PEAK_DETACH_CONTROLLER_DEFAULT_IO_TIMEOUT_MS;
    }
}

static unsigned int
peak_detach_controller_default_io_timeout_ms(void)
{
    if (g_once_init_enter(&controller_io_timeout_initialized)) {
        peak_detach_controller_init_io_timeout_once();
        g_once_init_leave(&controller_io_timeout_initialized, 1);
    }

    return controller_io_timeout_ms;
}

static unsigned int
peak_detach_controller_effective_timeout_ms(unsigned int request_timeout_ms)
{
    return request_timeout_ms > 0
               ? request_timeout_ms
               : peak_detach_controller_default_io_timeout_ms();
}

static void
peak_detach_controller_init_auto_helper_perf_fallback_once(void)
{
    auto_helper_perf_fallback_stop_window_us =
        peak_detach_controller_parse_uint_env(
            PEAK_AUTO_HELPER_PERF_FALLBACK_STOP_WINDOW_US_ENV,
            PEAK_AUTO_HELPER_PERF_FALLBACK_DEFAULT_STOP_WINDOW_US);
}

static unsigned int
peak_detach_controller_auto_helper_perf_fallback_stop_window_us(void)
{
    if (g_once_init_enter(&auto_helper_perf_fallback_initialized)) {
        peak_detach_controller_init_auto_helper_perf_fallback_once();
        g_once_init_leave(&auto_helper_perf_fallback_initialized, 1);
    }

    return auto_helper_perf_fallback_stop_window_us;
}

static double
peak_detach_controller_deadline_for_timeout(unsigned int timeout_ms)
{
    return peak_detach_controller_monotonic_second() +
           ((double)peak_detach_controller_effective_timeout_ms(timeout_ms) /
            1000.0);
}

static int
peak_detach_controller_raw_open_devnull(void)
{
#if defined(__linux__) && defined(__x86_64__) && defined(SYS_openat)
    long result;
    register long r10 __asm__("r10") = 0;
    register long r8 __asm__("r8") = 0;
    register long r9 __asm__("r9") = 0;

    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"((long)SYS_openat),
                       "D"((long)AT_FDCWD),
                       "S"((long)"/dev/null"),
                       "d"((long)O_RDWR),
                       "r"(r10),
                       "r"(r8),
                       "r"(r9)
                     : "rcx", "r11", "memory");
    return result >= 0 ? (int)result : -1;
#elif defined(__linux__) && defined(__aarch64__) && defined(SYS_openat)
    register long x0 __asm__("x0") = (long)AT_FDCWD;
    register long x1 __asm__("x1") = (long)"/dev/null";
    register long x2 __asm__("x2") = (long)O_RDWR;
    register long x3 __asm__("x3") = 0;
    register long x8 __asm__("x8") = SYS_openat;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                     : "memory");
    return x0 >= 0 ? (int)x0 : -1;
#else
    return open("/dev/null", O_RDWR);
#endif
}

static void
peak_detach_controller_raw_dup2_or_exit(int oldfd, int newfd)
{
    if (oldfd == newfd) {
        return;
    }

#if defined(__linux__) && defined(__x86_64__) && defined(SYS_dup2)
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"((long)SYS_dup2),
                       "D"((long)oldfd),
                       "S"((long)newfd)
                     : "rcx", "r11", "memory");
    if (result < 0) {
        _exit(127);
    }
#elif defined(__linux__) && defined(__aarch64__) && defined(SYS_dup3)
    register long x0 __asm__("x0") = (long)oldfd;
    register long x1 __asm__("x1") = (long)newfd;
    register long x2 __asm__("x2") = 0;
    register long x8 __asm__("x8") = SYS_dup3;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x8)
                     : "memory");
    if (x0 < 0) {
        _exit(127);
    }
#else
    if (dup2(oldfd, newfd) < 0) {
        _exit(127);
    }
#endif
}

static int
peak_detach_controller_signal_backend_signum_load(void)
{
    return atomic_load_explicit(&signal_backend_signum, memory_order_acquire);
}

static void
peak_detach_controller_signal_backend_signum_store(int signum)
{
    atomic_store_explicit(&signal_backend_signum, signum, memory_order_release);
}

static void
peak_detach_controller_init_strict_gate_wait_timeout_once(void)
{
    unsigned int timeout_ms =
        peak_detach_controller_parse_uint_env(
            PEAK_STRICT_GATE_WAIT_TIMEOUT_MS_ENV,
            PEAK_STRICT_GATE_WAIT_DEFAULT_TIMEOUT_MS);

    strict_gate_wait_timeout_s =
        timeout_ms == 0 ? 0.0 : (double)timeout_ms / 1000.0;
}

static void
peak_detach_controller_init_strict_gate_wait_timeout(void)
{
    if (g_once_init_enter(&strict_gate_wait_timeout_initialized)) {
        peak_detach_controller_init_strict_gate_wait_timeout_once();
        g_once_init_leave(&strict_gate_wait_timeout_initialized, 1);
    }
}

#endif

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
static gboolean
peak_detach_controller_write_ranges_overlap(
    const PeakDetachHelperInstruction* left,
    const PeakDetachHelperInstruction* right)
{
    uint64_t left_start = left->address;
    uint64_t right_start = right->address;
    uint64_t left_end = left_start + left->size;
    uint64_t right_end = right_start + right->size;

    if (left_end < left_start || right_end < right_start) {
        return TRUE;
    }

    return left_start < right_end && right_start < left_end;
}

static void
peak_detach_controller_init_mutation_guard(void)
{
    pthread_mutexattr_t attr;

    if (pthread_mutexattr_init(&attr) != 0) {
        _exit(128);
    }
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0 ||
        pthread_mutex_init(&mutation_guard_mutex, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        _exit(128);
    }
    pthread_mutexattr_destroy(&attr);
}

static void
peak_detach_controller_lock_mutation_guard(void)
{
    (void)pthread_once(&mutation_guard_mutex_initialized,
                       peak_detach_controller_init_mutation_guard);
    (void)pthread_mutex_lock(&mutation_guard_mutex);
    peak_signal_policy_push_migration_disabled();
}

static void
peak_detach_controller_unlock_mutation_guard(void)
{
    peak_signal_policy_pop_migration_disabled();
    (void)pthread_mutex_unlock(&mutation_guard_mutex);
}

static void
peak_detach_controller_begin_thread_creation_gate(void)
{
    int epoch = atomic_fetch_add_explicit(&strict_mutation_thread_gate_epoch,
                                          1,
                                          memory_order_acq_rel) + 1;
    if (epoch <= 0) {
        epoch = 1;
        atomic_store_explicit(&strict_mutation_thread_gate_epoch,
                              1,
                              memory_order_release);
    }
    atomic_store_explicit(&strict_mutation_thread_gate, epoch, memory_order_release);
}

#ifdef PEAK_ENABLE_TEST_HOOKS
static void
peak_detach_controller_test_delay_after_gate_begin(void)
{
    const char* enabled_env = getenv("PEAK_TEST_ENABLE_STRICT_GATE_DELAY");
    if (enabled_env == NULL ||
        !(strcmp(enabled_env, "1") == 0 ||
          g_ascii_strcasecmp(enabled_env, "true") == 0 ||
          g_ascii_strcasecmp(enabled_env, "yes") == 0 ||
          g_ascii_strcasecmp(enabled_env, "on") == 0)) {
        return;
    }

    const char* delay_env = getenv("PEAK_TEST_STRICT_GATE_DELAY_US");
    if (delay_env == NULL || delay_env[0] == '\0') {
        return;
    }

    char* end = NULL;
    errno = 0;
    unsigned long delay_us = strtoul(delay_env, &end, 10);
    if (errno != 0 || end == delay_env || *end != '\0' || delay_us == 0) {
        return;
    }
    if (delay_us > 1000000UL) {
        delay_us = 1000000UL;
    }
    usleep((useconds_t)delay_us);
}
#else
static void
peak_detach_controller_test_delay_after_gate_begin(void)
{
}
#endif

static void
peak_detach_controller_end_thread_creation_gate(void)
{
    atomic_store_explicit(&strict_mutation_thread_gate, 0, memory_order_release);
}

static void
peak_detach_controller_publish_gate_waiter(pid_t tid, int gate_epoch)
{
    if (tid <= 0 || gate_epoch <= 0) {
        return;
    }

    for (uint32_t i = 0; i < PEAK_DETACH_HELPER_MAX_THREADS; i++) {
        PeakDetachGateWaiterSlot* slot = &gate_waiter_slots[i];
        int slot_tid = atomic_load_explicit(&slot->tid, memory_order_acquire);
        if (slot_tid == (int)tid) {
            atomic_store_explicit(&slot->gate_epoch,
                                  gate_epoch,
                                  memory_order_release);
            return;
        }
        if (slot_tid == 0) {
            int expected = 0;
            if (atomic_compare_exchange_strong_explicit(&slot->tid,
                                                        &expected,
                                                        (int)tid,
                                                        memory_order_acq_rel,
                                                        memory_order_acquire)) {
                atomic_store_explicit(&slot->gate_epoch,
                                      gate_epoch,
                                      memory_order_release);
                return;
            }
        }
    }
}

static void
peak_detach_controller_clear_gate_waiter(pid_t tid, int gate_epoch)
{
    if (tid <= 0 || gate_epoch <= 0) {
        return;
    }

    for (uint32_t i = 0; i < PEAK_DETACH_HELPER_MAX_THREADS; i++) {
        PeakDetachGateWaiterSlot* slot = &gate_waiter_slots[i];
        if (atomic_load_explicit(&slot->gate_epoch, memory_order_acquire) == gate_epoch &&
            atomic_load_explicit(&slot->tid, memory_order_acquire) == (int)tid) {
            atomic_store_explicit(&slot->gate_epoch, 0, memory_order_release);
            atomic_store_explicit(&slot->tid, 0, memory_order_release);
            return;
        }
    }
}

static gboolean
peak_detach_controller_thread_is_gate_waiter(pid_t tid, int gate_epoch)
{
    if (tid <= 0 || gate_epoch <= 0) {
        return FALSE;
    }

    for (uint32_t i = 0; i < PEAK_DETACH_HELPER_MAX_THREADS; i++) {
        PeakDetachGateWaiterSlot* slot = &gate_waiter_slots[i];
        if (atomic_load_explicit(&slot->gate_epoch, memory_order_acquire) == gate_epoch &&
            atomic_load_explicit(&slot->tid, memory_order_acquire) == (int)tid) {
            return TRUE;
        }
    }
    return FALSE;
}

void
peak_detach_controller_note_thread_creation_gate_installed(gboolean installed)
{
    atomic_store_explicit(&strict_mutation_thread_gate_installed,
                          installed ? 1 : 0,
                          memory_order_release);
}

PEAK_DETACH_CONTROLLER_TEST_API int
peak_detach_controller_signal_stale_delivery_count(void)
{
    return atomic_load_explicit(&signal_stale_delivery_count,
                                memory_order_acquire);
}

static gboolean
peak_detach_controller_signal_cookie_epoch_is_stale(int cookie_epoch,
                                                    int active_epoch)
{
    if (cookie_epoch <= 0) {
        return FALSE;
    }
    if (active_epoch > 0) {
        return cookie_epoch < active_epoch;
    }

    int next_epoch =
        atomic_load_explicit(&signal_epoch_counter, memory_order_acquire);
    return next_epoch > 0 && cookie_epoch < next_epoch;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
static gboolean
peak_detach_controller_test_env_truthy(const char* name)
{
    const char* value = getenv(name);

    return value != NULL && value[0] != '\0' &&
           strcmp(value, "0") != 0 &&
           strcasecmp(value, "false") != 0 &&
           strcasecmp(value, "no") != 0 &&
           strcasecmp(value, "off") != 0;
}
#endif

#ifdef PEAK_ENABLE_TEST_HOOKS
int
peak_detach_controller_test_thread_creation_gate_epoch(void)
{
    return atomic_load_explicit(&strict_mutation_thread_gate,
                                memory_order_acquire);
}

size_t
peak_detach_controller_test_gate_waiter_count(void)
{
    size_t count = 0;

    for (uint32_t i = 0; i < PEAK_DETACH_HELPER_MAX_THREADS; i++) {
        PeakDetachGateWaiterSlot* slot = &gate_waiter_slots[i];
        if (atomic_load_explicit(&slot->tid, memory_order_acquire) != 0 &&
            atomic_load_explicit(&slot->gate_epoch, memory_order_acquire) != 0) {
            count++;
        }
    }
    return count;
}

int
peak_detach_controller_test_signal_backend_signum(void)
{
    (void)pthread_once(&signal_backend_initialized,
                       peak_detach_controller_init_signal_backend_once);
    int reserved = peak_signal_policy_reserved_signal();
    return reserved > 0 ? reserved :
           peak_detach_controller_signal_backend_signum_load();
}

int
peak_detach_controller_test_send_stale_signal_to_tid(long target_tid)
{
    (void)pthread_once(&signal_backend_initialized,
                       peak_detach_controller_init_signal_backend_once);
    int signum = peak_detach_controller_signal_backend_signum_load();
    if (signum <= 0) {
        signum = peak_signal_policy_reserved_signal();
    }
    if (signum <= 0 || signum > SIGRTMAX) {
        errno = ENOSYS;
        return -1;
    }
    if (target_tid <= 0) {
        errno = EINVAL;
        return -1;
    }

    int stale_epoch =
        atomic_fetch_add_explicit(&signal_epoch_counter,
                                  1,
                                  memory_order_acq_rel);
    if (stale_epoch <= 0) {
        stale_epoch = 1;
        atomic_store_explicit(&signal_epoch_counter,
                              2,
                              memory_order_release);
    }
    pid_t tid = (pid_t)target_tid;
    return peak_signal_policy_send_thread_signal(
        tid,
        signum,
        peak_signal_policy_cookie_for(stale_epoch, tid));
}

int
peak_detach_controller_test_send_stale_signal_to_current_thread(void)
{
    return peak_detach_controller_test_send_stale_signal_to_tid(
        (long)syscall(SYS_gettid));
}
#endif

static gboolean
peak_detach_controller_thread_creation_gate_installed(void)
{
    return atomic_load_explicit(&strict_mutation_thread_gate_installed,
                                memory_order_acquire) != 0;
}

static void
peak_detach_controller_atfork_prepare(void)
{
    peak_detach_controller_lock_mutation_guard();
}

static void
peak_detach_controller_atfork_parent(void)
{
    peak_detach_controller_unlock_mutation_guard();
}

static void
peak_detach_controller_atfork_child(void)
{
    if (helper_fd >= 0) {
        peak_detach_controller_raw_close(helper_fd);
    }
    helper_fd = -1;
    helper_pid = -1;
    helper_owner_pid = -1;
    helper_state_fatal = FALSE;
    atomic_store_explicit(&auto_signal_fallback_cached,
                          0,
                          memory_order_release);
    held_mutation = (PeakDetachHeldMutation){ 0 };
    held_mutation_started_at = 0.0;
    atomic_store_explicit(&held_mutation_window_active,
                          0,
                          memory_order_release);
    last_stop_window_us = 0.0;
    memset(physical_patch_records, 0, sizeof(physical_patch_records));
    peak_detach_controller_init_mutation_guard();
}

static void
peak_detach_controller_register_atfork(void)
{
    (void)pthread_atfork(peak_detach_controller_atfork_prepare,
                         peak_detach_controller_atfork_parent,
                         peak_detach_controller_atfork_child);
}

static void
peak_detach_controller_init_atfork_once(void)
{
    (void)pthread_once(&atfork_initialized,
                       peak_detach_controller_register_atfork);
}

static void
peak_detach_controller_reset_inherited_helper_if_needed(void)
{
    pid_t current_pid = getpid();

    if (helper_owner_pid > 0 && helper_owner_pid != current_pid) {
        if (helper_fd >= 0) {
            peak_detach_controller_raw_close(helper_fd);
        }
        helper_fd = -1;
        helper_pid = -1;
        helper_owner_pid = -1;
        helper_state_fatal = FALSE;
        atomic_store_explicit(&auto_signal_fallback_cached,
                              0,
                              memory_order_release);
        held_mutation = (PeakDetachHeldMutation){ 0 };
        held_mutation_started_at = 0.0;
        atomic_store_explicit(&held_mutation_window_active,
                              0,
                              memory_order_release);
        last_stop_window_us = 0.0;
        memset(physical_patch_records, 0, sizeof(physical_patch_records));
    }
}
#endif

static PeakSafeDetachMode
peak_detach_controller_mode(void)
{
    if (g_once_init_enter(&mode_initialized)) {
        const char* mode_env = g_getenv("PEAK_SAFE_DETACH_MODE");
        PeakSafeDetachMode mode = PEAK_SAFE_DETACH_MODE_STRICT;

        if (mode_env == NULL || mode_env[0] == '\0' ||
            g_ascii_strcasecmp(mode_env, "strict") == 0 ||
            g_ascii_strcasecmp(mode_env, "auto") == 0 ||
            g_ascii_strcasecmp(mode_env, "helper") == 0 ||
            g_ascii_strcasecmp(mode_env, "debugger") == 0 ||
            g_ascii_strcasecmp(mode_env, "ptrace") == 0 ||
            g_ascii_strcasecmp(mode_env, "signal") == 0 ||
            g_ascii_strcasecmp(mode_env, "signals") == 0 ||
            g_ascii_strcasecmp(mode_env, "in-process") == 0) {
            mode = PEAK_SAFE_DETACH_MODE_STRICT;
        }

        configured_mode = mode;
        g_once_init_leave(&mode_initialized, 1);
    }

    return configured_mode;
}

static PeakDetachRequestedBackend
peak_detach_controller_requested_backend(void)
{
    const char* backend_env = g_getenv("PEAK_DETACH_BACKEND");
    const char* mode_env = g_getenv("PEAK_SAFE_DETACH_MODE");
    const char* value = backend_env != NULL && backend_env[0] != '\0' ?
        backend_env : mode_env;

    if (value != NULL) {
        if (g_ascii_strcasecmp(value, "signal") == 0 ||
            g_ascii_strcasecmp(value, "signals") == 0 ||
            g_ascii_strcasecmp(value, "in-process") == 0) {
            return PEAK_DETACH_REQUESTED_BACKEND_SIGNAL;
        }
        if (g_ascii_strcasecmp(value, "helper") == 0 ||
            g_ascii_strcasecmp(value, "debugger") == 0 ||
            g_ascii_strcasecmp(value, "ptrace") == 0) {
            return PEAK_DETACH_REQUESTED_BACKEND_HELPER;
        }
    }

    return PEAK_DETACH_REQUESTED_BACKEND_AUTO;
}

static gboolean
peak_detach_controller_auto_should_use_signal_backend(void)
{
    if (atomic_load_explicit(&auto_signal_fallback_cached,
                             memory_order_acquire) != 0) {
        return TRUE;
    }

#ifdef __linux__
    const char* override = g_getenv("PEAK_TEST_PTRACE_SCOPE");
    char buffer[32];
    const char* value = override;

    if (value == NULL || value[0] == '\0') {
        FILE* fp = fopen("/proc/sys/kernel/yama/ptrace_scope", "r");

        if (fp == NULL) {
            return FALSE;
        }
        if (fgets(buffer, sizeof(buffer), fp) == NULL) {
            fclose(fp);
            return FALSE;
        }
        fclose(fp);
        value = buffer;
    }

    char* end = NULL;
    errno = 0;
    long scope = strtol(value, &end, 10);

    if (errno == 0 && end != value && scope >= 2) {
        if (!warned_auto_helper_ptrace_scope) {
            warned_auto_helper_ptrace_scope = TRUE;
            peak_log_info("[peak] auto safe detach using signal backend because ptrace_scope=%ld blocks helper attachment\n",
                       scope);
        }
        return TRUE;
    }
#endif
    return FALSE;
}

static void
peak_detach_controller_cache_auto_signal_fallback(PeakDetachStatus status,
                                                  const char* context)
{
    if (status != PEAK_DETACH_STATUS_PERMISSION_DENIED &&
        status != PEAK_DETACH_STATUS_UNSUPPORTED &&
        status != PEAK_DETACH_STATUS_TIMEOUT) {
        return;
    }

    atomic_store_explicit(&auto_signal_fallback_cached,
                          1,
                          memory_order_release);
    if (!warned_auto_helper_fallback_cached) {
        warned_auto_helper_fallback_cached = TRUE;
        peak_log_info("[peak] auto safe detach using signal backend after helper %s: %s\n",
                      context != NULL ? context : "fallback",
                      peak_detach_controller_status_string(status));
    }
}

static void
peak_detach_controller_cache_auto_signal_performance_fallback(
    double stop_window_us,
    const char* context)
{
    PeakDetachStatus signal_status = PEAK_DETACH_STATUS_ERROR;
    unsigned int threshold_us =
        peak_detach_controller_auto_helper_perf_fallback_stop_window_us();

    if (threshold_us == 0 ||
        stop_window_us < (double)threshold_us ||
        atomic_load_explicit(&auto_signal_fallback_cached,
                             memory_order_acquire) != 0) {
        return;
    }

    if (!peak_detach_controller_ensure_signal_backend(&signal_status)) {
        return;
    }

    (void)peak_detach_controller_close_helper();
    atomic_store_explicit(&auto_signal_fallback_cached,
                          1,
                          memory_order_release);
    if (!warned_auto_helper_performance_fallback_cached) {
        warned_auto_helper_performance_fallback_cached = TRUE;
        peak_log_info("[peak] auto safe detach switching to signal backend after slow helper %s stop window %.3f us (threshold %u us)\n",
                      context != NULL ? context : "mutation",
                      stop_window_us,
                      threshold_us);
    }
}

static gboolean
peak_detach_controller_status_allows_auto_signal_fallback(PeakDetachStatus status)
{
    return status == PEAK_DETACH_STATUS_PERMISSION_DENIED ||
           status == PEAK_DETACH_STATUS_UNSUPPORTED ||
           status == PEAK_DETACH_STATUS_TIMEOUT;
}

static size_t
peak_detach_controller_count_proc_threads(gboolean* ok_out)
{
#ifdef __linux__
    DIR* dir = opendir("/proc/self/task");
    size_t count = 0;

    if (ok_out != NULL) {
        *ok_out = FALSE;
    }

    if (dir == NULL) {
        return 0;
    }

    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (entry == NULL) {
            break;
        }

        if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') {
            count++;
        }
    }

    if (ok_out != NULL && errno == 0) {
        *ok_out = TRUE;
    }
    closedir(dir);
    return count;
#else
    (void)ok_out;
    return 0;
#endif
}

const char*
peak_detach_controller_operation_string(PeakDetachOperation operation)
{
    switch (operation) {
        case PEAK_DETACH_OPERATION_ATTACH:
            return "attach";
        case PEAK_DETACH_OPERATION_DETACH:
            return "detach";
        case PEAK_DETACH_OPERATION_REATTACH:
            return "reattach";
        case PEAK_DETACH_OPERATION_SHUTDOWN:
            return "shutdown";
        case PEAK_DETACH_OPERATION_REPLACE:
            return "replace";
        case PEAK_DETACH_OPERATION_REVERT:
            return "revert";
        default:
            return "unknown";
    }
}

static gboolean
peak_detach_controller_operation_is_valid(PeakDetachOperation operation)
{
    switch (operation) {
        case PEAK_DETACH_OPERATION_ATTACH:
        case PEAK_DETACH_OPERATION_DETACH:
        case PEAK_DETACH_OPERATION_REATTACH:
        case PEAK_DETACH_OPERATION_SHUTDOWN:
        case PEAK_DETACH_OPERATION_REPLACE:
        case PEAK_DETACH_OPERATION_REVERT:
            return TRUE;
        default:
            return FALSE;
    }
}

static gboolean
peak_detach_controller_validate_request(const PeakDetachRequest* request,
                                        PeakDetachStatus* status_out)
{
    if (request == NULL ||
        request->interceptor == NULL ||
        request->function_address == NULL ||
        !peak_detach_controller_operation_is_valid(request->operation)) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }
    if (request->operation != PEAK_DETACH_OPERATION_ATTACH &&
        request->operation != PEAK_DETACH_OPERATION_REPLACE &&
        request->operation != PEAK_DETACH_OPERATION_REVERT &&
        request->listener == NULL) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

const char*
peak_detach_controller_status_string(PeakDetachStatus status)
{
    switch (status) {
        case PEAK_DETACH_STATUS_SAFE:
            return "safe";
        case PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED:
            return "compatibility-allowed";
        case PEAK_DETACH_STATUS_DISABLED:
            return "disabled";
        case PEAK_DETACH_STATUS_UNSUPPORTED:
            return "unsupported";
        case PEAK_DETACH_STATUS_MISSING_GUM_API:
            return "missing-gum-api";
        case PEAK_DETACH_STATUS_PERMISSION_DENIED:
            return "permission-denied";
        case PEAK_DETACH_STATUS_TIMEOUT:
            return "timeout";
        case PEAK_DETACH_STATUS_CLASSIFY_FAILED:
            return "classify-failed";
        case PEAK_DETACH_STATUS_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

static __thread PeakDetachFailureDetail last_failure_detail = { "none", 0, 0, 0 };

static void
peak_detach_controller_clear_failure_detail(void)
{
    last_failure_detail.reason = "none";
    last_failure_detail.tid = 0;
    last_failure_detail.pc = 0;
    last_failure_detail.aux = 0;
}

static void
peak_detach_controller_note_failure_detail(const char* reason,
                                           long tid,
                                           uintptr_t pc,
                                           uintptr_t aux)
{
    last_failure_detail.reason = reason != NULL ? reason : "unknown";
    last_failure_detail.tid = tid;
    last_failure_detail.pc = pc;
    last_failure_detail.aux = aux;
}

const PeakDetachFailureDetail*
peak_detach_controller_last_failure_detail(void)
{
    return &last_failure_detail;
}

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
static int
peak_detach_controller_deadline_remaining_ms(double deadline)
{
    double remaining_s = deadline - peak_detach_controller_monotonic_second();

    if (remaining_s <= 0.0) {
        return 0;
    }
    if (remaining_s > ((double)INT_MAX / 1000.0)) {
        return INT_MAX;
    }

    int remaining_ms = (int)(remaining_s * 1000.0);
    return remaining_ms > 0 ? remaining_ms : 1;
}

static unsigned int
peak_detach_controller_timeout_until_deadline(double deadline,
                                              gboolean cleanup_grace)
{
    int remaining_ms = peak_detach_controller_deadline_remaining_ms(deadline);

    if (remaining_ms <= 0) {
        return cleanup_grace ? 1u : 0u;
    }
    return (unsigned int)remaining_ms;
}

static gboolean
peak_detach_controller_wait_fd_until(int fd, short events, double deadline)
{
    struct pollfd pfd = {
        .fd = fd,
        .events = events,
        .revents = 0
    };

    for (;;) {
        int timeout_ms = peak_detach_controller_deadline_remaining_ms(deadline);

        if (timeout_ms <= 0) {
            errno = ETIMEDOUT;
            return FALSE;
        }

        int ret = poll(&pfd, 1, timeout_ms);

        if (ret > 0) {
            if ((pfd.revents & events) != 0) {
                return TRUE;
            }
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                errno = EPIPE;
                return FALSE;
            }
            errno = EAGAIN;
            return FALSE;
        }
        if (ret == 0) {
            errno = ETIMEDOUT;
            return FALSE;
        }
        if (errno == EINTR) {
            continue;
        }
        return FALSE;
    }
}

static gboolean
peak_detach_controller_read_exact_until(int fd,
                                        void* buffer,
                                        size_t size,
                                        double deadline)
{
    char* cursor = (char*)buffer;
    size_t done = 0;

    while (done < size) {
        if (!peak_detach_controller_wait_fd_until(fd, POLLIN, deadline)) {
            return FALSE;
        }
        ssize_t n = read(fd, cursor + done, size - done);
        if (n == 0) {
            errno = EPIPE;
            return FALSE;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FALSE;
        }
        done += (size_t)n;
    }

    return TRUE;
}

static gboolean
peak_detach_controller_write_exact_until(int fd,
                                         const void* buffer,
                                         size_t size,
                                         double deadline)
{
    const char* cursor = (const char*)buffer;
    size_t done = 0;

    while (done < size) {
        if (!peak_detach_controller_wait_fd_until(fd, POLLOUT, deadline)) {
            return FALSE;
        }
        ssize_t n = send(fd, cursor + done, size - done, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EPERM || errno == ENOTSOCK) {
                n = write(fd, cursor + done, size - done);
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    return FALSE;
                }
                done += (size_t)n;
                continue;
            }
            return FALSE;
        }
        done += (size_t)n;
    }

    return TRUE;
}

static PeakDetachStatus
peak_detach_controller_errno_status(void)
{
    if (errno == ETIMEDOUT) {
        return PEAK_DETACH_STATUS_TIMEOUT;
    }
    if (errno == EACCES || errno == EPERM) {
        return PEAK_DETACH_STATUS_PERMISSION_DENIED;
    }
    return PEAK_DETACH_STATUS_ERROR;
}

static PeakDetachStatus
peak_detach_controller_helper_status_to_detach_status(uint32_t helper_status)
{
    switch ((PeakDetachHelperStatus)helper_status) {
        case PEAK_DETACH_HELPER_STATUS_OK:
            return PEAK_DETACH_STATUS_SAFE;
        case PEAK_DETACH_HELPER_STATUS_PERMISSION_DENIED:
            return PEAK_DETACH_STATUS_PERMISSION_DENIED;
        case PEAK_DETACH_HELPER_STATUS_UNSUPPORTED:
        case PEAK_DETACH_HELPER_STATUS_THREAD_LIMIT:
            return PEAK_DETACH_STATUS_UNSUPPORTED;
        case PEAK_DETACH_HELPER_STATUS_TIMEOUT:
            return PEAK_DETACH_STATUS_TIMEOUT;
        case PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED:
            return PEAK_DETACH_STATUS_ERROR;
        case PEAK_DETACH_HELPER_STATUS_PROTOCOL_ERROR:
        case PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR:
        case PEAK_DETACH_HELPER_STATUS_REGISTER_ERROR:
        default:
            return PEAK_DETACH_STATUS_ERROR;
    }
}

static void __attribute__((noreturn))
peak_detach_controller_mark_helper_fatal(const char* reason,
                                         PeakDetachStatus status)
{
    char message[256];
    int length;

    length = snprintf(message,
                      sizeof(message),
                      "[peak] fatal safe-detach helper failure (%s: %s); terminating to avoid running with unknown stopped-thread state\n",
                      reason != NULL ? reason : "unknown",
                      peak_detach_controller_status_string(status));

    helper_state_fatal = TRUE;
    warned_helper_fatal = TRUE;
    if (length > 0) {
        size_t size = (size_t)length;
        if (size >= sizeof(message)) {
            size = sizeof(message) - 1;
        }
        int flags = fcntl(STDERR_FILENO, F_GETFL);
        if (flags >= 0) {
            (void)fcntl(STDERR_FILENO, F_SETFL, flags | O_NONBLOCK);
        }
        (void)write(STDERR_FILENO, message, size);
    }
    _exit(128);
}

static const char*
peak_detach_controller_helper_path(void)
{
    const char* path = g_getenv("PEAK_DETACH_HELPER");

    if (path != NULL && path[0] != '\0') {
        return path;
    }

    Dl_info info;
    if (dladdr((void*)peak_detach_controller_helper_path, &info) != 0 &&
        info.dli_fname != NULL) {
        gchar* lib_dir = g_path_get_dirname(info.dli_fname);
        if (lib_dir != NULL) {
            gchar* install_root = g_path_get_dirname(lib_dir);
            gchar* candidates[3] = {
                g_build_filename(install_root, "bin", "peak_detach_helper", NULL),
                g_build_filename(lib_dir, "peak_detach_helper", NULL),
                NULL
            };

            for (size_t i = 0; candidates[i] != NULL; i++) {
                if (access(candidates[i], X_OK) == 0) {
                    g_strlcpy(resolved_helper_path,
                              candidates[i],
                              sizeof(resolved_helper_path));
                    for (size_t j = 0; candidates[j] != NULL; j++) {
                        g_free(candidates[j]);
                    }
                    g_free(install_root);
                    g_free(lib_dir);
                    return resolved_helper_path;
                }
            }

            for (size_t i = 0; candidates[i] != NULL; i++) {
                g_free(candidates[i]);
            }
            g_free(install_root);
            g_free(lib_dir);
        }
    }

#ifdef PEAK_INSTALL_DETACH_HELPER_PATH
    if (access(PEAK_INSTALL_DETACH_HELPER_PATH, X_OK) == 0) {
        return PEAK_INSTALL_DETACH_HELPER_PATH;
    }
#endif

#ifdef PEAK_DEFAULT_DETACH_HELPER_PATH
    if (access(PEAK_DEFAULT_DETACH_HELPER_PATH, X_OK) == 0) {
        return PEAK_DEFAULT_DETACH_HELPER_PATH;
    }
#endif

    return NULL;
}

static gboolean
peak_detach_controller_env_should_strip(const char* entry)
{
    return g_str_has_prefix(entry, "LD_PRELOAD=") ||
           g_str_has_prefix(entry, "LD_AUDIT=") ||
           g_str_has_prefix(entry, "PEAK_");
}

static char**
peak_detach_controller_build_helper_env(void)
{
    size_t count = 0;
    size_t kept = 0;

    if (environ == NULL) {
        return NULL;
    }

    while (environ[count] != NULL) {
        count++;
    }

    char** helper_env = calloc(count + 1, sizeof(*helper_env));
    if (helper_env == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        if (!peak_detach_controller_env_should_strip(environ[i])) {
            helper_env[kept] = strdup(environ[i]);
            if (helper_env[kept] == NULL) {
                for (size_t j = 0; j < kept; j++) {
                    free(helper_env[j]);
                }
                free(helper_env);
                return NULL;
            }
            kept++;
        }
    }
    helper_env[kept] = NULL;
    return helper_env;
}

static void
peak_detach_controller_free_helper_env(char** helper_env)
{
    if (helper_env == NULL) {
        return;
    }

    for (size_t i = 0; helper_env[i] != NULL; i++) {
        free(helper_env[i]);
    }
    free(helper_env);
}

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
static void
peak_detach_controller_child_close_inherited_fds(int protocol_fd)
{
    if (protocol_fd != PEAK_DETACH_HELPER_PROTOCOL_FD) {
        peak_detach_controller_raw_dup2_or_exit(
            protocol_fd,
            PEAK_DETACH_HELPER_PROTOCOL_FD);
        peak_detach_controller_raw_close(protocol_fd);
        protocol_fd = PEAK_DETACH_HELPER_PROTOCOL_FD;
    }

    int devnull_fd = peak_detach_controller_raw_open_devnull();
    if (devnull_fd >= 0) {
        peak_detach_controller_raw_dup2_or_exit(devnull_fd, STDIN_FILENO);
        peak_detach_controller_raw_dup2_or_exit(devnull_fd, STDOUT_FILENO);
        peak_detach_controller_raw_dup2_or_exit(devnull_fd, STDERR_FILENO);
        if (devnull_fd > STDERR_FILENO &&
            devnull_fd != PEAK_DETACH_HELPER_PROTOCOL_FD) {
            peak_detach_controller_raw_close(devnull_fd);
        }
    } else {
        peak_detach_controller_raw_close(STDIN_FILENO);
        peak_detach_controller_raw_close(STDOUT_FILENO);
        peak_detach_controller_raw_close(STDERR_FILENO);
    }

    peak_detach_controller_raw_close_range(
        (unsigned int)(PEAK_DETACH_HELPER_PROTOCOL_FD + 1),
        ~0U,
        0);
    (void)protocol_fd;
}
#endif

static void
peak_detach_controller_child_raw_execve(const char* path,
                                        char* const helper_argv[],
                                        char* const helper_env[])
{
#if defined(__linux__) && defined(__x86_64__) && defined(SYS_execve)
    long result;
    register long r10 __asm__("r10") = 0;
    register long r8 __asm__("r8") = 0;
    register long r9 __asm__("r9") = 0;

    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"((long)SYS_execve),
                       "D"((long)path),
                       "S"((long)helper_argv),
                       "d"((long)helper_env),
                       "r"(r10),
                       "r"(r8),
                       "r"(r9)
                     : "rcx", "r11", "memory");
    (void)result;
#elif defined(__linux__) && defined(__aarch64__) && defined(SYS_execve)
    register long x0 __asm__("x0") = (long)path;
    register long x1 __asm__("x1") = (long)helper_argv;
    register long x2 __asm__("x2") = (long)helper_env;
    register long x8 __asm__("x8") = SYS_execve;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x8)
                     : "memory");
#elif defined(SYS_execve)
    (void)syscall(SYS_execve, path, helper_argv, helper_env);
#else
    execve(path, helper_argv, helper_env);
#endif
}

static void
peak_detach_controller_child_exec_helper(const char* path,
                                         char* const helper_argv[],
                                         char* const helper_env[])
{
    peak_detach_controller_child_raw_execve(path, helper_argv, helper_env);
}

typedef struct {
    const char* path;
    int parent_fd;
    int child_fd;
    char fd_arg[32];
    char** helper_env;
} PeakDetachHelperSpawnArgs;

static int
peak_detach_controller_child_exec_helper_from_clone(void* data)
{
    PeakDetachHelperSpawnArgs* args = (PeakDetachHelperSpawnArgs*)data;
    char* const helper_argv[] = {
        (char*)args->path,
        args->fd_arg,
        NULL
    };

    peak_detach_controller_raw_close(args->parent_fd);
    peak_detach_controller_child_close_inherited_fds(args->child_fd);
    peak_detach_controller_child_exec_helper(args->path,
                                             helper_argv,
                                             args->helper_env);
    _exit(127);
}

static gboolean
peak_detach_controller_helper_spawn_should_use_fork(void)
{
    const char* value = g_getenv(PEAK_DETACH_HELPER_SPAWN_ENV);

    if (value == NULL || value[0] == '\0') {
        return TRUE;
    }

    if (g_ascii_strcasecmp(value, "clone") == 0 ||
        g_ascii_strcasecmp(value, "clone-vfork") == 0 ||
        g_ascii_strcasecmp(value, "no-atfork") == 0) {
        return FALSE;
    }

    return TRUE;
}

static gboolean
peak_detach_controller_trace_diagnostics_enabled(void)
{
    return atomic_load_explicit(&trace_diagnostics_enabled,
                                memory_order_acquire) != 0;
}

static void
peak_detach_controller_trace_helper_spawn(const char* event,
                                          const char* method,
                                          pid_t pid,
                                          PeakDetachStatus status)
{
    if (!peak_detach_controller_trace_diagnostics_enabled()) {
        return;
    }

    peak_log_info("[peak] detach helper %s method=%s pid=%ld status=%s\n",
                  event != NULL ? event : "unknown",
                  method != NULL ? method : "unknown",
                  (long)pid,
                  peak_detach_controller_status_string(status));
}

static const char*
peak_detach_controller_requested_backend_string(PeakDetachRequestedBackend backend)
{
    switch (backend) {
        case PEAK_DETACH_REQUESTED_BACKEND_AUTO:
            return "auto";
        case PEAK_DETACH_REQUESTED_BACKEND_HELPER:
            return "helper";
        case PEAK_DETACH_REQUESTED_BACKEND_SIGNAL:
            return "signal";
        default:
            return "unknown";
    }
}

static const char*
peak_detach_controller_hold_backend_string(PeakDetachHoldBackend backend)
{
    switch (backend) {
        case PEAK_DETACH_HOLD_BACKEND_NONE:
            return "none";
        case PEAK_DETACH_HOLD_BACKEND_HELPER:
            return "helper";
        case PEAK_DETACH_HOLD_BACKEND_SIGNAL:
            return "signal";
        default:
            return "unknown";
    }
}

static void
peak_detach_controller_trace_backend_phase(const char* phase,
                                           const PeakDetachRequest* request,
                                           PeakDetachRequestedBackend requested_backend,
                                           PeakDetachHoldBackend selected_backend,
                                           PeakDetachStatus status,
                                           const char* reason,
                                           size_t request_count)
{
    int saved_errno = errno;
    char detail[512];
    char row[1024];
    int fd;

    if (!peak_detach_controller_trace_diagnostics_enabled()) {
        errno = saved_errno;
        return;
    }
    if (peak_detach_controller_stop_window_trace_deferred()) {
        errno = saved_errno;
        return;
    }

    fd = atomic_load_explicit(&trace_diagnostics_fd, memory_order_acquire);
    if (fd < 0) {
        errno = saved_errno;
        return;
    }

    snprintf(detail,
             sizeof(detail),
             "phase=%s;requested=%s;selected=%s;reason=%s;operation=%s;avoid_external_helper=%d;requests=%lu",
             phase != NULL ? phase : "unknown",
             peak_detach_controller_requested_backend_string(requested_backend),
             peak_detach_controller_hold_backend_string(selected_backend),
             reason != NULL ? reason : "none",
             request != NULL
                 ? peak_detach_controller_operation_string(request->operation)
                 : "batch",
             request != NULL && request->avoid_external_helper ? 1 : 0,
             (unsigned long)request_count);

    int n = snprintf(row,
                     sizeof(row),
                     "%.9f,-1,__peak_controller_backend__,backend,%s,0,%s,0,0.000000000,%lu,0.000,0,%s,%s,0,0x0,0x0,%s,%lu,0.000000000,0.000000000,0.000000000,0.000000000\n",
                     peak_detach_controller_monotonic_second(),
                     phase != NULL ? phase : "unknown",
                     peak_detach_controller_status_string(status),
                     (unsigned long)request_count,
                     peak_detach_controller_status_string(status),
                     detail,
                     peak_detach_controller_hold_backend_string(selected_backend),
                     (unsigned long)request_count);
    if (n <= 0) {
        errno = saved_errno;
        return;
    }
    if ((size_t)n >= sizeof(row)) {
        row[sizeof(row) - 2] = '\n';
        row[sizeof(row) - 1] = '\0';
        n = (int)strlen(row);
    }

    ssize_t ignored = write(fd, row, (size_t)n);
    (void)ignored;
    errno = saved_errno;
}

void
peak_detach_controller_trace_diagnostic_phase(const char* phase,
                                              PeakDetachStatus status,
                                              size_t request_count,
                                              const char* reason)
{
    peak_detach_controller_trace_backend_phase(
        phase,
        NULL,
        PEAK_DETACH_REQUESTED_BACKEND_AUTO,
        PEAK_DETACH_HOLD_BACKEND_NONE,
        status,
        reason,
        request_count);
}

static void
peak_detach_controller_signal_count_epoch(int epoch,
                                          uint32_t* active_out,
                                          uint32_t* arrived_out,
                                          uint32_t* done_out,
                                          uint32_t* evacuate_started_out,
                                          uint32_t* evacuated_out,
                                          pid_t* first_pending_tid_out,
                                          pid_t* first_not_done_tid_out)
{
    uint32_t active = 0;
    uint32_t arrived = 0;
    uint32_t done = 0;
    uint32_t evacuate_started = 0;
    uint32_t evacuated = 0;
    pid_t first_pending_tid = 0;
    pid_t first_not_done_tid = 0;
    uint32_t count =
        atomic_load_explicit(&signal_slot_count, memory_order_acquire);

    for (uint32_t i = 0; i < count; i++) {
        PeakDetachSignalSlot* slot = &signal_slots[i];
        if (!peak_detach_controller_signal_slot_active(slot, epoch)) {
            continue;
        }
        active++;
        if (atomic_load_explicit(&slot->arrived_epoch,
                                 memory_order_acquire) == epoch) {
            arrived++;
        }
        if (atomic_load_explicit(&slot->done_epoch,
                                 memory_order_acquire) == epoch) {
            done++;
        }
        if (atomic_load_explicit(&slot->evacuate_started_epoch,
                                 memory_order_acquire) == epoch) {
            evacuate_started++;
        }
        if (atomic_load_explicit(&slot->evacuated_epoch,
                                 memory_order_acquire) == epoch) {
            evacuated++;
        }
        if (first_pending_tid == 0 &&
            atomic_load_explicit(&slot->arrived_epoch,
                                 memory_order_acquire) != epoch) {
            first_pending_tid =
                (pid_t)atomic_load_explicit(&slot->tid,
                                            memory_order_acquire);
        }
        if (first_not_done_tid == 0 &&
            atomic_load_explicit(&slot->arrived_epoch,
                                 memory_order_acquire) == epoch &&
            atomic_load_explicit(&slot->done_epoch,
                                 memory_order_acquire) != epoch) {
            first_not_done_tid =
                (pid_t)atomic_load_explicit(&slot->tid,
                                            memory_order_acquire);
        }
    }

    if (active_out != NULL) {
        *active_out = active;
    }
    if (arrived_out != NULL) {
        *arrived_out = arrived;
    }
    if (done_out != NULL) {
        *done_out = done;
    }
    if (evacuate_started_out != NULL) {
        *evacuate_started_out = evacuate_started;
    }
    if (evacuated_out != NULL) {
        *evacuated_out = evacuated;
    }
    if (first_pending_tid_out != NULL) {
        *first_pending_tid_out = first_pending_tid;
    }
    if (first_not_done_tid_out != NULL) {
        *first_not_done_tid_out = first_not_done_tid;
    }
}

static void
peak_detach_controller_signal_record_deferred_trace(int epoch,
                                                    PeakDetachStatus status)
{
    if (epoch <= 0) {
        return;
    }

    signal_deferred_trace.suppressed_rows++;
    signal_deferred_trace.last_epoch = epoch;
    signal_deferred_trace.last_status = status;
    peak_detach_controller_signal_count_epoch(
        epoch,
        &signal_deferred_trace.active,
        &signal_deferred_trace.arrived,
        &signal_deferred_trace.done,
        &signal_deferred_trace.evacuate_started,
        &signal_deferred_trace.evacuated,
        &signal_deferred_trace.first_pending_tid,
        &signal_deferred_trace.first_not_done_tid);
}

static void
peak_detach_controller_signal_reset_deferred_trace(void)
{
    signal_deferred_trace = (PeakDetachSignalDeferredTrace){ 0 };
    signal_deferred_trace.last_status = PEAK_DETACH_STATUS_SAFE;
}

static void
peak_detach_controller_signal_flush_deferred_trace(double stop_window_us)
{
    int saved_errno = errno;
    char detail[512];
    char row[1024];
    int fd;

    if (signal_deferred_trace.suppressed_rows == 0 ||
        !peak_detach_controller_trace_diagnostics_enabled()) {
        errno = saved_errno;
        return;
    }

    fd = atomic_load_explicit(&trace_diagnostics_fd, memory_order_acquire);
    if (fd < 0) {
        errno = saved_errno;
        return;
    }

    snprintf(detail,
             sizeof(detail),
             "phase=held-summary;suppressed=%lu;last_epoch=%d;active=%u;arrived=%u;done=%u;evac_started=%u;evacuated=%u;first_pending_tid=%ld;first_not_done_tid=%ld;stop_window_us=%.3f",
             signal_deferred_trace.suppressed_rows,
             signal_deferred_trace.last_epoch,
             signal_deferred_trace.active,
             signal_deferred_trace.arrived,
             signal_deferred_trace.done,
             signal_deferred_trace.evacuate_started,
             signal_deferred_trace.evacuated,
             (long)signal_deferred_trace.first_pending_tid,
             (long)signal_deferred_trace.first_not_done_tid,
             stop_window_us);

    int n = snprintf(row,
                     sizeof(row),
                     "%.9f,-1,__peak_signal,signal,held-summary,0,%s,0,0.000000000,%u,%.3f,%u,%s,%s,%ld,0x0,0x0,signal,%u,%.9f,%.9f,%.9f,0.000000000\n",
                     peak_detach_controller_monotonic_second(),
                     peak_detach_controller_status_string(
                         signal_deferred_trace.last_status),
                     signal_deferred_trace.active,
                     stop_window_us,
                     signal_deferred_trace.last_epoch > 0
                         ? (unsigned int)signal_deferred_trace.last_epoch
                         : 0u,
                     peak_detach_controller_status_string(
                         signal_deferred_trace.last_status),
                     detail,
                     signal_deferred_trace.first_pending_tid != 0
                         ? (long)signal_deferred_trace.first_pending_tid
                         : (long)signal_deferred_trace.first_not_done_tid,
                     signal_deferred_trace.arrived,
                     (double)signal_deferred_trace.done,
                     (double)signal_deferred_trace.evacuate_started,
                     (double)signal_deferred_trace.evacuated);
    if (n > 0) {
        if ((size_t)n >= sizeof(row)) {
            row[sizeof(row) - 2] = '\n';
            row[sizeof(row) - 1] = '\0';
            n = (int)strlen(row);
        }
        ssize_t ignored = write(fd, row, (size_t)n);
        (void)ignored;
    }
    errno = saved_errno;
}

static void
peak_detach_controller_trace_signal_phase(const char* phase,
                                          int epoch,
                                          PeakDetachStatus status,
                                          long tid,
                                          uintptr_t detail_pc,
                                          uintptr_t aux)
{
    int saved_errno = errno;
    uint32_t active = 0;
    uint32_t arrived = 0;
    uint32_t done = 0;
    uint32_t evacuate_started = 0;
    uint32_t evacuated = 0;
    pid_t first_pending_tid = 0;
    pid_t first_not_done_tid = 0;
    char detail[256];
    char row[1024];
    int fd;

    if (!peak_detach_controller_trace_diagnostics_enabled()) {
        errno = saved_errno;
        return;
    }
    if (peak_detach_controller_stop_window_trace_deferred()) {
        peak_detach_controller_signal_record_deferred_trace(epoch, status);
        errno = saved_errno;
        return;
    }

    fd = atomic_load_explicit(&trace_diagnostics_fd, memory_order_acquire);
    if (fd < 0) {
        errno = saved_errno;
        return;
    }

    peak_detach_controller_signal_count_epoch(epoch,
                                              &active,
                                              &arrived,
                                              &done,
                                              &evacuate_started,
                                              &evacuated,
                                              &first_pending_tid,
                                              &first_not_done_tid);
    snprintf(detail,
             sizeof(detail),
             "phase=%s;active=%u;arrived=%u;done=%u;evac_started=%u;evacuated=%u;first_pending_tid=%ld;first_not_done_tid=%ld",
             phase != NULL ? phase : "unknown",
             active,
             arrived,
             done,
             evacuate_started,
             evacuated,
             (long)first_pending_tid,
             (long)first_not_done_tid);
    int n = snprintf(row,
                     sizeof(row),
                     "%.9f,-1,__peak_signal,signal,%s,0,%s,0,0.000000000,%u,0.000,%u,%s,%s,%ld,0x%llx,0x%llx,signal,%u,%.9f,%.9f,%.9f,0.000000000\n",
                     peak_detach_controller_monotonic_second(),
                     phase != NULL ? phase : "unknown",
                     peak_detach_controller_status_string(status),
                     active,
                     epoch > 0 ? (unsigned int)epoch : 0u,
                     peak_detach_controller_status_string(status),
                     detail,
                     tid != 0 ? tid :
                         first_pending_tid != 0 ? (long)first_pending_tid :
                                                  (long)first_not_done_tid,
                     (unsigned long long)detail_pc,
                     (unsigned long long)aux,
                     arrived,
                     (double)done,
                     (double)evacuate_started,
                     (double)evacuated);
    if (n <= 0) {
        errno = saved_errno;
        return;
    }
    if ((size_t)n >= sizeof(row)) {
        row[sizeof(row) - 2] = '\n';
        row[sizeof(row) - 1] = '\0';
        n = (int)strlen(row);
    }

    ssize_t ignored = write(fd, row, (size_t)n);
    (void)ignored;
    errno = saved_errno;
}

static pid_t
peak_detach_controller_spawn_helper_no_atfork(const char* path,
                                              const char* fd_arg,
                                              char** helper_env,
                                              int parent_fd,
                                              int child_fd)
{
#if defined(__linux__) && defined(CLONE_VM) && defined(CLONE_VFORK)
    const size_t stack_size = 64 * 1024;
#if defined(PEAK_ENABLE_TEST_HOOKS)
    if (g_getenv(PEAK_TEST_DETACH_HELPER_FORCE_CLONE_SPAWN_FAIL_ENV) != NULL) {
        errno = ENOSYS;
        return -1;
    }
#endif

    void* stack = mmap(NULL,
                       stack_size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                       -1,
                       0);
    if (stack == MAP_FAILED) {
        return -1;
    }

    PeakDetachHelperSpawnArgs args = {
        .path = path,
        .parent_fd = parent_fd,
        .child_fd = child_fd,
        .helper_env = helper_env
    };
    g_strlcpy(args.fd_arg, fd_arg, sizeof(args.fd_arg));

    /*
     * The helper may be started from a large MPI rank after MPI_Init.  Plain
     * fork() runs every pthread_atfork handler in the process and duplicates a
     * huge address space, both of which are fragile inside Intel MPI/libfabric
     * at scale.  clone(CLONE_VM | CLONE_VFORK | SIGCHLD) gives the child its
     * own stack, blocks the parent until exec/_exit, and avoids atfork handlers.
     * The child path below must remain raw-syscall-only until exec.
     */
    void* stack_top = (char*)stack + stack_size;
    pid_t pid = (pid_t)clone(peak_detach_controller_child_exec_helper_from_clone,
                             stack_top,
                             CLONE_VM | CLONE_VFORK | SIGCHLD,
                             &args);
    int saved_errno = errno;
    (void)munmap(stack, stack_size);
    errno = saved_errno;
    return pid;
#else
    (void)path;
    (void)fd_arg;
    (void)helper_env;
    (void)parent_fd;
    (void)child_fd;
    errno = ENOSYS;
    return -1;
#endif
}

static pid_t
peak_detach_controller_spawn_helper_with_fork(const char* path,
                                              const char* fd_arg,
                                              char** helper_env,
                                              int parent_fd,
                                              int child_fd)
{
    pid_t pid = fork();

    if (pid == 0) {
        char* const helper_argv[] = { (char*)path, (char*)fd_arg, NULL };
        peak_detach_controller_raw_close(parent_fd);
        peak_detach_controller_child_close_inherited_fds(child_fd);
        peak_detach_controller_child_exec_helper(path, helper_argv, helper_env);
        _exit(127);
    }

    return pid;
}

static gboolean
peak_detach_controller_wait_helper_exit_until(pid_t pid, double deadline)
{
    int status = 0;

    for (;;) {
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD)) {
            return TRUE;
        }
        if (waited < 0 && errno != EINTR) {
            return FALSE;
        }

        int remaining_ms = peak_detach_controller_deadline_remaining_ms(deadline);
        if (remaining_ms <= 0) {
            errno = ETIMEDOUT;
            return FALSE;
        }

        useconds_t sleep_us = PEAK_DETACH_HELPER_CLOSE_POLL_US;
        if (remaining_ms == 1 && sleep_us > 500u) {
            sleep_us = 500u;
        }
        usleep(sleep_us);
    }
}

static gboolean
peak_detach_controller_close_helper(void)
{
    pid_t pid = helper_pid;
    double start = peak_detach_controller_monotonic_second();
    double graceful_deadline = start + 0.025;
    double term_deadline = start + 0.125;
    double kill_deadline =
        start + ((double)PEAK_DETACH_HELPER_CLOSE_GRACE_MS / 1000.0);

    if (helper_fd >= 0) {
        peak_detach_controller_raw_close(helper_fd);
        helper_fd = -1;
    }
    helper_owner_pid = -1;

    if (pid > 0) {
        if (peak_detach_controller_wait_helper_exit_until(pid,
                                                          graceful_deadline)) {
            helper_pid = -1;
            return TRUE;
        }

        kill(pid, SIGTERM);
        if (peak_detach_controller_wait_helper_exit_until(pid, term_deadline)) {
            helper_pid = -1;
            return TRUE;
        }

        kill(pid, SIGKILL);
        if (peak_detach_controller_wait_helper_exit_until(pid, kill_deadline)) {
            helper_pid = -1;
            return TRUE;
        }

        peak_detach_controller_note_failure_detail(
            "helper-close-timeout",
            (long)pid,
            (uintptr_t)errno,
            0);
        return FALSE;
    }

    helper_pid = -1;
    return TRUE;
}

static gboolean
peak_detach_controller_helper_is_closed(void)
{
    return helper_fd < 0 && helper_pid <= 0;
}

static gboolean
peak_detach_controller_ensure_helper_until(double deadline,
                                           PeakDetachStatus* status_out)
{
    const char* path;
    int sockets[2];
    char fd_arg[32];
    char** helper_env;
    pid_t pid;
    PeakDetachHelperResponse handshake;
    gboolean spawn_with_fork;
    const char* spawn_method;

    peak_detach_controller_reset_inherited_helper_if_needed();
    memset(&handshake, 0, sizeof(handshake));
    if (helper_fd >= 0 && helper_pid > 0) {
        return TRUE;
    }
    if (helper_fd < 0 && helper_pid > 0 &&
        !peak_detach_controller_close_helper()) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_TIMEOUT;
        }
        return FALSE;
    }
    if (helper_warmup_failed && !helper_warmup_active) {
        if (status_out != NULL) {
            *status_out = helper_warmup_status;
        }
        return FALSE;
    }

    if (!peak_detach_controller_thread_creation_gate_installed()) {
        if (!warned_helper_gate_unavailable) {
            warned_helper_gate_unavailable = TRUE;
            g_printerr("[peak] strict helper detach requested, but pthread_create interception is not installed; refusing helper-backed physical mutation\n");
        }
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_UNSUPPORTED;
        }
        return FALSE;
    }

    path = peak_detach_controller_helper_path();
    if (path == NULL || access(path, X_OK) != 0) {
        if (!warned_helper_unavailable) {
            warned_helper_unavailable = TRUE;
            g_printerr("[peak] strict safe detach requested, but detach helper is unavailable; set PEAK_DETACH_HELPER to an executable helper\n");
        }
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_UNSUPPORTED;
        }
        return FALSE;
    }

    helper_env = peak_detach_controller_build_helper_env();
    if (helper_env == NULL) {
        peak_detach_controller_note_failure_detail(
            "helper-env-build",
            0,
            (uintptr_t)errno,
            0);
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        peak_detach_controller_note_failure_detail(
            "helper-socketpair",
            0,
            (uintptr_t)errno,
            0);
        peak_detach_controller_free_helper_env(helper_env);
        if (status_out != NULL) {
            *status_out = errno == EACCES || errno == EPERM ?
                PEAK_DETACH_STATUS_PERMISSION_DENIED :
                PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    snprintf(fd_arg, sizeof(fd_arg), "%d", PEAK_DETACH_HELPER_PROTOCOL_FD);
    spawn_with_fork = peak_detach_controller_helper_spawn_should_use_fork();
    spawn_method = spawn_with_fork ? "fork" : "clone-vfork";
    peak_detach_controller_trace_helper_spawn("spawn-begin",
                                              spawn_method,
                                              -1,
                                              PEAK_DETACH_STATUS_SAFE);
    if (spawn_with_fork) {
        pid = peak_detach_controller_spawn_helper_with_fork(path,
                                                            fd_arg,
                                                            helper_env,
                                                            sockets[0],
                                                            sockets[1]);
    } else {
        pid = peak_detach_controller_spawn_helper_no_atfork(path,
                                                            fd_arg,
                                                            helper_env,
                                                            sockets[0],
                                                            sockets[1]);
    }
    if (pid < 0) {
        int spawn_errno = errno;
        PeakDetachStatus spawn_status = spawn_with_fork
                                            ? peak_detach_controller_errno_status()
                                            : PEAK_DETACH_STATUS_UNSUPPORTED;

        peak_detach_controller_trace_helper_spawn("spawn-failed",
                                                  spawn_method,
                                                  -1,
                                                  spawn_status);
        peak_detach_controller_note_failure_detail(
            "helper-spawn",
            0,
            (uintptr_t)spawn_errno,
            0);
        peak_detach_controller_free_helper_env(helper_env);
        peak_detach_controller_raw_close(sockets[0]);
        peak_detach_controller_raw_close(sockets[1]);
        if (status_out != NULL) {
            *status_out = spawn_status;
        }
        return FALSE;
    }

    peak_detach_controller_free_helper_env(helper_env);
    peak_detach_controller_raw_close(sockets[1]);
    helper_fd = sockets[0];
    helper_pid = pid;
    helper_owner_pid = getpid();
    peak_detach_controller_trace_helper_spawn("spawned",
                                              spawn_method,
                                              helper_pid,
                                              PEAK_DETACH_STATUS_SAFE);

    int flags = fcntl(helper_fd, F_GETFD);
    if (flags >= 0) {
        (void)fcntl(helper_fd, F_SETFD, flags | FD_CLOEXEC);
    }

#ifdef PR_SET_PTRACER
    if (prctl(PR_SET_PTRACER, helper_pid, 0, 0, 0) != 0 &&
        errno != EINVAL && errno != ENOSYS &&
        errno != EACCES && errno != EPERM) {
        peak_detach_controller_note_failure_detail(
            "helper-prctl",
            0,
            (uintptr_t)errno,
            0);
        peak_detach_controller_close_helper();
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }
#endif

    if (!peak_detach_controller_read_exact_until(helper_fd,
                                                 &handshake,
                                                 sizeof(handshake),
                                                 deadline) ||
        handshake.magic != PEAK_DETACH_HELPER_MAGIC ||
        handshake.version != PEAK_DETACH_HELPER_VERSION ||
        handshake.status != PEAK_DETACH_HELPER_STATUS_OK) {
        PeakDetachStatus status = peak_detach_controller_errno_status();
        int child_status = 0;
        uintptr_t child_detail = (uintptr_t)errno;
        pid_t waited = helper_pid > 0 ?
            waitpid(helper_pid, &child_status, WNOHANG) : -1;
        if (waited == helper_pid) {
            child_detail = (uintptr_t)child_status;
        }
        peak_detach_controller_trace_helper_spawn("handshake-failed",
                                                  spawn_method,
                                                  helper_pid,
                                                  status);
        peak_detach_controller_note_failure_detail(
            "helper-handshake",
            0,
            child_detail,
            (uintptr_t)handshake.status);
        peak_detach_controller_close_helper();
        if (status_out != NULL) {
            *status_out = status;
        }
        return FALSE;
    }

    peak_detach_controller_trace_helper_spawn("handshake-ok",
                                              spawn_method,
                                              helper_pid,
                                              PEAK_DETACH_STATUS_SAFE);
    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

static gboolean
peak_detach_controller_ensure_helper(PeakDetachStatus* status_out)
{
    return peak_detach_controller_ensure_helper_until(
        peak_detach_controller_deadline_for_timeout(0),
        status_out);
}

static gboolean
peak_detach_controller_send_helper_command(PeakDetachHelperCommand command,
                                           uint32_t instruction_count,
                                           PeakDetachHelperInstruction* instructions,
                                           unsigned int timeout_ms,
                                           double deadline,
                                           gboolean close_on_io_failure,
                                           PeakDetachStatus* status_out)
{
    unsigned int effective_timeout_ms = timeout_ms > 0
        ? timeout_ms
        : peak_detach_controller_timeout_until_deadline(deadline, TRUE);
    PeakDetachHelperRequest request = {
        .magic = PEAK_DETACH_HELPER_MAGIC,
        .version = PEAK_DETACH_HELPER_VERSION,
        .command = (uint32_t)command,
        .pid = (int32_t)getpid(),
        .controller_tid = (int32_t)syscall(SYS_gettid),
        .instruction_count = instruction_count,
        .timeout_ms = effective_timeout_ms
    };
    PeakDetachHelperResponse response;
    gboolean trace_resume =
        command == PEAK_DETACH_HELPER_CMD_RESUME;

    if (trace_resume) {
        peak_detach_controller_trace_backend_phase(
            "helper-resume-command-start",
            NULL,
            PEAK_DETACH_REQUESTED_BACKEND_AUTO,
            PEAK_DETACH_HOLD_BACKEND_HELPER,
            PEAK_DETACH_STATUS_SAFE,
            "helper-command",
            effective_timeout_ms);
    }

    if (helper_fd < 0 ||
        !peak_detach_controller_write_exact_until(helper_fd,
                                                  &request,
                                                  sizeof(request),
                                                  deadline) ||
        (instruction_count > 0 &&
         !peak_detach_controller_write_exact_until(
             helper_fd,
             instructions,
             sizeof(instructions[0]) * instruction_count,
             deadline)) ||
        !peak_detach_controller_read_exact_until(helper_fd,
                                                 &response,
                                                 sizeof(response),
                                                 deadline) ||
        response.magic != PEAK_DETACH_HELPER_MAGIC ||
        response.version != PEAK_DETACH_HELPER_VERSION) {
        PeakDetachStatus status = peak_detach_controller_errno_status();
        if (close_on_io_failure) {
            peak_detach_controller_close_helper();
        } else {
            peak_detach_controller_mark_helper_fatal("held helper protocol failure",
                                                     status);
        }
        if (status_out != NULL) {
            *status_out = status;
        }
        if (trace_resume) {
            peak_detach_controller_trace_backend_phase(
                "helper-resume-command-failed",
                NULL,
                PEAK_DETACH_REQUESTED_BACKEND_AUTO,
                PEAK_DETACH_HOLD_BACKEND_HELPER,
                status,
                "helper-command",
                effective_timeout_ms);
        }
        return FALSE;
    }

    if (response.status != PEAK_DETACH_HELPER_STATUS_OK) {
        PeakDetachStatus detach_status =
            peak_detach_controller_helper_status_to_detach_status(response.status);
        if (response.status == PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED) {
            peak_detach_controller_mark_helper_fatal("stop cleanup failure",
                                                     PEAK_DETACH_STATUS_ERROR);
        }
        if (close_on_io_failure) {
            peak_detach_controller_close_helper();
        }
        if (status_out != NULL) {
            *status_out = detach_status;
        }
        if (trace_resume) {
            peak_detach_controller_trace_backend_phase(
                "helper-resume-command-failed",
                NULL,
                PEAK_DETACH_REQUESTED_BACKEND_AUTO,
                PEAK_DETACH_HOLD_BACKEND_HELPER,
                detach_status,
                "helper-command-status",
                effective_timeout_ms);
        }
        return FALSE;
    }

    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    if (trace_resume) {
        peak_detach_controller_trace_backend_phase(
            "helper-resume-command-complete",
            NULL,
            PEAK_DETACH_REQUESTED_BACKEND_AUTO,
            PEAK_DETACH_HOLD_BACKEND_HELPER,
            PEAK_DETACH_STATUS_SAFE,
            "helper-command",
            effective_timeout_ms);
    }
    return TRUE;
}

static gboolean
peak_detach_controller_send_evacuate(uint32_t instruction_count,
                                     PeakDetachHelperInstruction* instructions,
                                     double deadline,
                                     PeakDetachStatus* status_out)
{
    return peak_detach_controller_send_helper_command(
        PEAK_DETACH_HELPER_CMD_EVACUATE,
        instruction_count,
        instructions,
        peak_detach_controller_timeout_until_deadline(deadline, TRUE),
        deadline,
        FALSE,
        status_out);
}

static gboolean
peak_detach_controller_send_resume(double deadline,
                                   PeakDetachStatus* status_out)
{
    return peak_detach_controller_send_helper_command(
        PEAK_DETACH_HELPER_CMD_RESUME,
        0,
        NULL,
        peak_detach_controller_timeout_until_deadline(deadline, TRUE),
        deadline,
        FALSE,
        status_out);
}

static gboolean
peak_detach_controller_send_shutdown(gboolean close_on_io_failure,
                                     PeakDetachStatus* status_out)
{
    gboolean ok = peak_detach_controller_send_helper_command(
        PEAK_DETACH_HELPER_CMD_SHUTDOWN,
        0,
        NULL,
        0,
        peak_detach_controller_deadline_for_timeout(0),
        close_on_io_failure,
        status_out);

    if (ok) {
        peak_detach_controller_close_helper();
    }
    return ok;
}

static gboolean
peak_detach_controller_stop_threads(PeakDetachHelperThreadSnapshot* snapshots,
                                    uint32_t* snapshot_count_out,
                                    double deadline,
                                    PeakDetachStatus* status_out)
{
    unsigned int timeout_ms =
        peak_detach_controller_timeout_until_deadline(deadline, FALSE);
    PeakDetachHelperRequest request = {
        .magic = PEAK_DETACH_HELPER_MAGIC,
        .version = PEAK_DETACH_HELPER_VERSION,
        .command = PEAK_DETACH_HELPER_CMD_STOP,
        .pid = (int32_t)getpid(),
        .controller_tid = (int32_t)syscall(SYS_gettid),
        .instruction_count = 0,
        .timeout_ms = timeout_ms
    };
    PeakDetachHelperResponse response;

    *snapshot_count_out = 0;

    if (timeout_ms == 0) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_TIMEOUT;
        }
        return FALSE;
    }

    if (!peak_detach_controller_write_exact_until(helper_fd,
                                                  &request,
                                                  sizeof(request),
                                                  deadline)) {
        PeakDetachStatus status = peak_detach_controller_errno_status();
        peak_detach_controller_note_failure_detail(
            "helper-stop-write",
            0,
            (uintptr_t)errno,
            0);
        peak_detach_controller_close_helper();
        if (status_out != NULL) {
            *status_out = status;
        }
        return FALSE;
    }

    if (!peak_detach_controller_read_exact_until(helper_fd,
                                                 &response,
                                                 sizeof(response),
                                                 deadline) ||
        response.magic != PEAK_DETACH_HELPER_MAGIC ||
        response.version != PEAK_DETACH_HELPER_VERSION) {
        PeakDetachStatus status = peak_detach_controller_errno_status();
        peak_detach_controller_note_failure_detail(
            "helper-stop-response",
            0,
            (uintptr_t)errno,
            0);
        peak_detach_controller_close_helper();
        if (status_out != NULL) {
            *status_out = status;
        }
        return FALSE;
    }

    if (response.status != PEAK_DETACH_HELPER_STATUS_OK) {
        PeakDetachStatus detach_status =
            peak_detach_controller_helper_status_to_detach_status(response.status);
        if (response.status == PEAK_DETACH_HELPER_STATUS_PTRACE_ERROR &&
            response.errno_value == EAGAIN) {
            detach_status = PEAK_DETACH_STATUS_TIMEOUT;
        }
        peak_detach_controller_note_failure_detail(
            "helper-stop-status",
            0,
            (uintptr_t)response.errno_value,
            (uintptr_t)response.status);
        if (response.status == PEAK_DETACH_HELPER_STATUS_RELEASE_FAILED) {
            peak_detach_controller_mark_helper_fatal("stop cleanup failure",
                                                     PEAK_DETACH_STATUS_ERROR);
        }
        if (status_out != NULL) {
            *status_out = detach_status;
        }
        return FALSE;
    }

    if (response.thread_count > PEAK_DETACH_HELPER_MAX_THREADS ||
        !peak_detach_controller_read_exact_until(
            helper_fd,
            snapshots,
            sizeof(snapshots[0]) * response.thread_count,
            deadline)) {
        PeakDetachStatus status = peak_detach_controller_errno_status();
        peak_detach_controller_note_failure_detail(
            "helper-stop-snapshot",
            0,
            (uintptr_t)errno,
            (uintptr_t)response.thread_count);
        peak_detach_controller_close_helper();
        if (status_out != NULL) {
            *status_out = status;
        }
        return FALSE;
    }

    *snapshot_count_out = response.thread_count;
    return TRUE;
}

static gboolean
peak_detach_controller_signal_atomics_supported(void)
{
    return atomic_is_lock_free(&signal_epoch_counter) &&
           atomic_is_lock_free(&signal_backend_signum) &&
           atomic_is_lock_free(&signal_hold_epoch) &&
           atomic_is_lock_free(&signal_release_epoch) &&
           atomic_is_lock_free(&signal_slot_count) &&
           atomic_is_lock_free(&signal_slots[0].active_epoch) &&
           atomic_is_lock_free(&signal_slots[0].arrived_epoch) &&
           atomic_is_lock_free(&signal_slots[0].done_epoch) &&
           atomic_is_lock_free(&signal_slots[0].rewrite_epoch) &&
           atomic_is_lock_free(&signal_slots[0].rewrite_status) &&
           atomic_is_lock_free(&signal_slots[0].evacuate_epoch) &&
           atomic_is_lock_free(&signal_slots[0].evacuate_started_epoch) &&
           atomic_is_lock_free(&signal_slots[0].evacuated_epoch) &&
           atomic_is_lock_free(&signal_slots[0].evacuate_status) &&
           atomic_is_lock_free(&signal_slots[0].tid) &&
           atomic_is_lock_free(&signal_slots[0].pc) &&
           atomic_is_lock_free(&signal_slots[0].new_pc) &&
           atomic_is_lock_free(&signal_slots[0].evacuate_breakpoint_pc) &&
           atomic_is_lock_free(&signal_stale_delivery_count) &&
           peak_signal_policy_atomics_lock_free();
}

static gboolean
peak_detach_controller_signal_backend_supported(void)
{
#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__) || defined(__aarch64__))
    return peak_detach_controller_signal_atomics_supported();
#else
    return FALSE;
#endif
}

static gboolean
peak_detach_controller_signal_get_pc(void* context, uintptr_t* pc_out)
{
#if defined(__x86_64__) || defined(__amd64__)
    ucontext_t* uc = (ucontext_t*)context;
    if (uc == NULL || pc_out == NULL) {
        return FALSE;
    }
    *pc_out = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    return TRUE;
#elif defined(__aarch64__)
    ucontext_t* uc = (ucontext_t*)context;
    if (uc == NULL || pc_out == NULL) {
        return FALSE;
    }
    *pc_out = (uintptr_t)uc->uc_mcontext.pc;
    return TRUE;
#else
    (void)context;
    (void)pc_out;
    return FALSE;
#endif
}

static gboolean
peak_detach_controller_signal_set_pc(void* context, uintptr_t pc)
{
#if defined(__x86_64__) || defined(__amd64__)
    ucontext_t* uc = (ucontext_t*)context;
    if (uc == NULL) {
        return FALSE;
    }
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)pc;
    return TRUE;
#elif defined(__aarch64__)
    ucontext_t* uc = (ucontext_t*)context;
    if (uc == NULL) {
        return FALSE;
    }
    uc->uc_mcontext.pc = (unsigned long long)pc;
    return TRUE;
#else
    (void)context;
    (void)pc;
    return FALSE;
#endif
}

static gboolean
peak_detach_controller_signal_breakpoint_bytes(uint8_t* bytes, uint32_t* size_out)
{
#if defined(__x86_64__) || defined(__amd64__)
    if (bytes == NULL || size_out == NULL) {
        return FALSE;
    }
    bytes[0] = 0xcc;
    *size_out = 1;
    return TRUE;
#elif defined(__aarch64__)
    uint32_t brk = 0xd4200000u;
    if (bytes == NULL || size_out == NULL) {
        return FALSE;
    }
    memcpy(bytes, &brk, sizeof(brk));
    *size_out = (uint32_t)sizeof(brk);
    return TRUE;
#else
    (void)bytes;
    (void)size_out;
    return FALSE;
#endif
}

static gboolean
peak_detach_controller_signal_breakpoint_pc_matches(uintptr_t pc,
                                                    uintptr_t breakpoint_pc)
{
#if defined(__x86_64__) || defined(__amd64__)
    return pc == breakpoint_pc || pc == breakpoint_pc + 1u;
#elif defined(__aarch64__)
    return pc == breakpoint_pc || pc == breakpoint_pc + 4u;
#else
    (void)pc;
    (void)breakpoint_pc;
    return FALSE;
#endif
}

static gboolean
peak_detach_controller_signal_breakpoint_arch_supported(void)
{
    uint8_t bytes[PEAK_DETACH_HELPER_MAX_PATCH_BYTES];
    uint32_t size = 0;

    return peak_detach_controller_signal_breakpoint_bytes(bytes, &size) &&
           size > 0 && size <= PEAK_DETACH_HELPER_MAX_PATCH_BYTES;
}

static gboolean
peak_detach_controller_signal_breakpoint_can_evac(void)
{
    return signal_breakpoint_supported &&
           peak_detach_controller_signal_breakpoint_arch_supported();
}

static gboolean
peak_detach_controller_signal_instruction_range_contains(uint64_t address,
                                                         uint32_t size,
                                                         uintptr_t pc)
{
    uint64_t end = address + (uint64_t)size;

    return address != 0 && size > 0 && end >= address &&
           (uint64_t)pc >= address && (uint64_t)pc < end;
}

static gboolean
peak_detach_controller_signal_handler_is_installed(void)
{
    int signum = peak_detach_controller_signal_backend_signum_load();
    if (signum <= 0) {
        return FALSE;
    }

    struct sigaction current;
    memset(&current, 0, sizeof(current));
    peak_signal_policy_enter_internal();
    int rc = sigaction(signum, NULL, &current);
    peak_signal_policy_leave_internal();
    return rc == 0 &&
           (current.sa_flags & SA_SIGINFO) != 0 &&
           current.sa_sigaction == peak_detach_controller_signal_handler;
}

static gboolean
peak_detach_controller_signal_trap_handler_is_installed(void)
{
    if (!signal_trap_handler_installed) {
        return FALSE;
    }

    struct sigaction current;
    memset(&current, 0, sizeof(current));
    peak_signal_policy_enter_internal();
    int rc = sigaction(SIGTRAP, NULL, &current);
    peak_signal_policy_leave_internal();
    return rc == 0 &&
           (current.sa_flags & SA_SIGINFO) != 0 &&
           current.sa_sigaction == peak_detach_controller_signal_trap_handler;
}

static int
peak_detach_controller_signal_tid_blocks_reserved_until(pid_t tid,
                                                        double deadline)
{
    const unsigned int max_attempts = 100;

    for (unsigned int attempt = 0; attempt < max_attempts; attempt++) {
        if (peak_detach_controller_deadline_remaining_ms(deadline) <= 0) {
            return -2;
        }
        int blocked = peak_detach_controller_signal_tid_blocks_reserved_once(tid);
        if (blocked <= 0) {
            return blocked;
        }
        int remaining_ms =
            peak_detach_controller_deadline_remaining_ms(deadline);
        if (remaining_ms <= 0) {
            return -2;
        }
        useconds_t sleep_us =
            remaining_ms > 1 ? 1000u : (useconds_t)(remaining_ms * 1000);
        usleep(sleep_us);
    }
    return 1;
}

static int
peak_detach_controller_signal_tid_is_gone(pid_t tid)
{
    return syscall(SYS_tgkill, getpid(), tid, 0) != 0 && errno == ESRCH;
}

static int
peak_detach_controller_signal_tid_blocks_reserved_once(pid_t tid)
{
    int signum = peak_detach_controller_signal_backend_signum_load();
    if (signum <= 0) {
        return -1;
    }

    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%ld/status", (long)tid);
    FILE* fp = fopen(path, "r");
    if (fp == NULL) {
        return errno == ENOENT || errno == ESRCH ||
               peak_detach_controller_signal_tid_is_gone(tid) ? 0 : -1;
    }

    char line[256];
    int blocked = -1;
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strncmp(line, "SigBlk:", 7) != 0) {
            continue;
        }
        char* value = line + 7;
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        errno = 0;
        unsigned long long mask = strtoull(value, NULL, 16);
        if (errno != 0 || signum <= 0 || signum > 64) {
            blocked = -1;
        } else {
            unsigned long long bit =
                1ull << (unsigned int)(signum - 1);
            blocked = (mask & bit) != 0 ? 1 : 0;
        }
        break;
    }
    fclose(fp);
    if (blocked < 0 && peak_detach_controller_signal_tid_is_gone(tid)) {
        return 0;
    }
    return blocked;
}

static PeakDetachSignalSlot*
peak_detach_controller_signal_find_slot(pid_t tid, int epoch)
{
    uint32_t count = atomic_load_explicit(&signal_slot_count, memory_order_acquire);
    for (uint32_t i = 0; i < count; i++) {
        PeakDetachSignalSlot* slot = &signal_slots[i];
        if (atomic_load_explicit(&slot->active_epoch, memory_order_acquire) == epoch &&
            atomic_load_explicit(&slot->tid, memory_order_acquire) == (int)tid) {
            return slot;
        }
    }
    return NULL;
}

static PeakDetachSignalSlot*
peak_detach_controller_signal_alloc_slot(pid_t tid, int epoch)
{
    PeakDetachSignalSlot* slot = peak_detach_controller_signal_find_slot(tid, epoch);
    if (slot != NULL) {
        return slot;
    }

    uint32_t count = atomic_load_explicit(&signal_slot_count, memory_order_acquire);
    for (uint32_t i = 0; i < count; i++) {
        slot = &signal_slots[i];
        if (atomic_load_explicit(&slot->active_epoch, memory_order_acquire) != epoch) {
            memset(slot, 0, sizeof(*slot));
            atomic_store_explicit(&slot->tid, (int)tid, memory_order_release);
            atomic_store_explicit(&slot->active_epoch, epoch, memory_order_release);
            return slot;
        }
    }

    if (count >= PEAK_DETACH_HELPER_MAX_THREADS) {
        return NULL;
    }

    slot = &signal_slots[count];
    memset(slot, 0, sizeof(*slot));
    atomic_store_explicit(&slot->tid, (int)tid, memory_order_release);
    atomic_store_explicit(&slot->active_epoch, epoch, memory_order_release);
    atomic_store_explicit(&signal_slot_count, count + 1, memory_order_release);
    return slot;
}

static gboolean
peak_detach_controller_signal_wait_for_release(PeakDetachSignalSlot* slot,
                                               int epoch,
                                               void* context,
                                               gboolean allow_evacuation)
{
    while (atomic_load_explicit(&signal_release_epoch, memory_order_acquire) != epoch &&
           atomic_load_explicit(&signal_hold_epoch, memory_order_acquire) == epoch) {
        int release_epoch;

        if (allow_evacuation &&
            atomic_load_explicit(&slot->evacuate_epoch, memory_order_acquire) == epoch) {
            atomic_store_explicit(&slot->evacuate_started_epoch,
                                  epoch,
                                  memory_order_release);
            return FALSE;
        }

        release_epoch =
            atomic_load_explicit(&signal_release_epoch, memory_order_acquire);
        if (release_epoch == epoch) {
            break;
        }
        peak_detach_controller_signal_futex_wait(&signal_release_epoch,
                                                 release_epoch);
    }

    if (atomic_load_explicit(&signal_hold_epoch, memory_order_acquire) == epoch &&
        atomic_load_explicit(&slot->rewrite_epoch, memory_order_acquire) == epoch) {
        uintptr_t new_pc =
            atomic_load_explicit(&slot->new_pc, memory_order_acquire);
        gboolean rewrite_ok =
            peak_detach_controller_signal_set_pc(context, new_pc);
        atomic_store_explicit(&slot->rewrite_status,
                              rewrite_ok ? 1 : -1,
                              memory_order_release);
    }
    atomic_store_explicit(&slot->done_epoch, epoch, memory_order_release);
    return TRUE;
}

static void
peak_detach_controller_signal_handler(int signo, siginfo_t* info, void* context)
{
    int backend_signum =
        peak_detach_controller_signal_backend_signum_load();
    if (signo != backend_signum) {
        return;
    }

    int epoch = atomic_load_explicit(&signal_hold_epoch, memory_order_acquire);
    pid_t tid = (pid_t)syscall(SYS_gettid);
    if (epoch == 0 ||
        !peak_signal_policy_cookie_matches_async(info, epoch, tid)) {
        int cookie_epoch = 0;
        if (peak_signal_policy_cookie_decode_epoch_async(info,
                                                         tid,
                                                         &cookie_epoch) &&
            peak_detach_controller_signal_cookie_epoch_is_stale(cookie_epoch,
                                                                epoch)) {
            atomic_fetch_add_explicit(&signal_stale_delivery_count,
                                      1,
                                      memory_order_relaxed);
            return;
        }
        peak_signal_policy_note_unexpected_delivery();
        return;
    }

    PeakDetachSignalSlot* slot =
        peak_detach_controller_signal_find_slot(tid, epoch);
    if (slot == NULL) {
        return;
    }

    uintptr_t pc = 0;
    if (peak_detach_controller_signal_get_pc(context, &pc)) {
        atomic_store_explicit(&slot->pc, pc, memory_order_relaxed);
    } else {
        atomic_store_explicit(&slot->pc, 0, memory_order_relaxed);
    }
    atomic_store_explicit(&slot->arrived_epoch, epoch, memory_order_release);

    (void)peak_detach_controller_signal_wait_for_release(slot,
                                                         epoch,
                                                         context,
                                                         TRUE);
}

static void
peak_detach_controller_forward_unhandled_trap(int signo,
                                             siginfo_t* info,
                                             void* context)
{
    if ((previous_trap_action.sa_flags & SA_SIGINFO) != 0 &&
        previous_trap_action.sa_sigaction != NULL) {
        previous_trap_action.sa_sigaction(signo, info, context);
        return;
    }

    if (previous_trap_action.sa_handler == SIG_IGN) {
        return;
    }

    if (previous_trap_action.sa_handler == SIG_DFL ||
        previous_trap_action.sa_handler == NULL) {
        (void)sigaction(SIGTRAP, &previous_trap_action, NULL);
        (void)syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGTRAP);
        return;
    }

    previous_trap_action.sa_handler(signo);
}

static void
peak_detach_controller_signal_trap_handler(int signo, siginfo_t* info, void* context)
{
    (void)info;
    if (signo != SIGTRAP) {
        return;
    }

    int epoch = atomic_load_explicit(&signal_hold_epoch, memory_order_acquire);
    if (epoch == 0) {
        peak_detach_controller_forward_unhandled_trap(signo, info, context);
        return;
    }

    pid_t tid = (pid_t)syscall(SYS_gettid);
    PeakDetachSignalSlot* slot =
        peak_detach_controller_signal_find_slot(tid, epoch);
    if (slot == NULL ||
        atomic_load_explicit(&slot->evacuate_epoch, memory_order_acquire) != epoch) {
        peak_detach_controller_forward_unhandled_trap(signo, info, context);
        return;
    }

    uintptr_t pc = 0;
    uintptr_t breakpoint_pc =
        atomic_load_explicit(&slot->evacuate_breakpoint_pc, memory_order_acquire);
    if (!peak_detach_controller_signal_get_pc(context, &pc) ||
        !peak_detach_controller_signal_breakpoint_pc_matches(pc, breakpoint_pc) ||
        !peak_detach_controller_signal_set_pc(context, breakpoint_pc)) {
        atomic_store_explicit(&slot->evacuate_status, -1, memory_order_release);
        return;
    }

    atomic_store_explicit(&slot->pc, breakpoint_pc, memory_order_release);
    atomic_store_explicit(&slot->evacuate_status, 1, memory_order_release);
    atomic_store_explicit(&slot->evacuated_epoch, epoch, memory_order_release);
    (void)peak_detach_controller_signal_wait_for_release(slot,
                                                         epoch,
                                                         context,
                                                         FALSE);
}

static gboolean
peak_detach_controller_install_signal_backend_handler(int candidate);

static gboolean
peak_detach_controller_install_signal_trap_handler(int candidate);

static void
peak_detach_controller_init_signal_backend_once(void)
{
    if (!peak_detach_controller_signal_backend_supported()) {
        peak_detach_controller_signal_backend_signum_store(0);
        return;
    }

    int candidate = peak_signal_policy_choose_reserved_signal();
    if (candidate <= 0 || candidate > SIGRTMAX) {
        peak_detach_controller_signal_backend_signum_store(0);
        return;
    }

    if (!peak_detach_controller_install_signal_backend_handler(candidate)) {
        peak_signal_policy_clear_reserved_signal();
        peak_detach_controller_signal_backend_signum_store(0);
        return;
    }
    (void)peak_detach_controller_install_signal_trap_handler(candidate);

    peak_detach_controller_signal_backend_signum_store(candidate);
}

static gboolean
peak_detach_controller_install_signal_backend_handler(int candidate)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, candidate);
    action.sa_sigaction = peak_detach_controller_signal_handler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    peak_signal_policy_enter_internal();
    int install_rc = sigaction(candidate, &action, NULL);
    peak_signal_policy_leave_internal();
    if (install_rc != 0) {
        return FALSE;
    }
    (void)peak_signal_policy_unblock_reserved_for_current_thread();
    return TRUE;
}

static gboolean
peak_detach_controller_install_signal_trap_handler(int candidate)
{
    signal_breakpoint_supported = FALSE;
    if (!signal_trap_handler_installed) {
        memset(&previous_trap_action, 0, sizeof(previous_trap_action));
        peak_signal_policy_enter_internal();
        int trap_query_rc = sigaction(SIGTRAP, NULL, &previous_trap_action);
        peak_signal_policy_leave_internal();
        if (trap_query_rc != 0) {
            return FALSE;
        }
    }

    struct sigaction trap_action;
    memset(&trap_action, 0, sizeof(trap_action));
    sigemptyset(&trap_action.sa_mask);
    sigaddset(&trap_action.sa_mask, SIGTRAP);
    sigaddset(&trap_action.sa_mask, candidate);
    trap_action.sa_sigaction = peak_detach_controller_signal_trap_handler;
    trap_action.sa_flags = SA_SIGINFO | SA_RESTART;
    peak_signal_policy_enter_internal();
    int trap_install_rc = sigaction(SIGTRAP, &trap_action, NULL);
    peak_signal_policy_leave_internal();
    if (trap_install_rc == 0) {
        signal_trap_handler_installed = 1;
        signal_breakpoint_supported =
            peak_detach_controller_signal_breakpoint_arch_supported();
        return TRUE;
    }

    return FALSE;
}

static gboolean
peak_detach_controller_ensure_signal_backend(PeakDetachStatus* status_out)
{
    (void)pthread_once(&signal_backend_initialized,
                       peak_detach_controller_init_signal_backend_once);
    int backend_signum =
        peak_detach_controller_signal_backend_signum_load();
    if (backend_signum == 0) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_UNSUPPORTED;
        }
        return FALSE;
    }

    int reserved_signum = peak_signal_policy_reserved_signal();
    if (reserved_signum <= 0 || reserved_signum > SIGRTMAX) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_UNSUPPORTED;
        }
        return FALSE;
    }
    if (reserved_signum != backend_signum) {
        if (!peak_detach_controller_install_signal_backend_handler(reserved_signum)) {
            peak_detach_controller_note_failure_detail(
                "signal-handler-not-installed",
                0,
                (uintptr_t)reserved_signum,
                0);
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_UNSUPPORTED;
            }
            return FALSE;
        }
        (void)peak_detach_controller_install_signal_trap_handler(reserved_signum);
        peak_detach_controller_signal_backend_signum_store(reserved_signum);
        backend_signum = reserved_signum;
    }

    gboolean handler_installed =
        peak_detach_controller_signal_handler_is_installed();
    int unexpected_deliveries =
        peak_signal_policy_unexpected_delivery_count();
    if (!handler_installed || unexpected_deliveries != 0) {
        peak_detach_controller_note_failure_detail(
            unexpected_deliveries != 0 ? "signal-unexpected-delivery" :
                                         "signal-handler-not-installed",
            0,
            (uintptr_t)backend_signum,
            unexpected_deliveries > 0 ? (uintptr_t)unexpected_deliveries : 0);
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_UNSUPPORTED;
        }
        return FALSE;
    }
    if (!peak_detach_controller_thread_creation_gate_installed()) {
        if (!warned_signal_gate_unavailable) {
            warned_signal_gate_unavailable = TRUE;
            g_printerr("[peak] strict signal detach requested, but pthread_create interception is not installed; refusing signal-backed physical mutation\n");
        }
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_UNSUPPORTED;
        }
        return FALSE;
    }
    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

static gboolean
peak_detach_controller_signal_slot_active(const PeakDetachSignalSlot* slot,
                                          int epoch)
{
    return atomic_load_explicit(&slot->active_epoch, memory_order_acquire) == epoch;
}

static void
peak_detach_controller_signal_clear_slots(void)
{
    memset(signal_slots, 0, sizeof(signal_slots));
    atomic_store_explicit(&signal_slot_count, 0, memory_order_release);
    atomic_store_explicit(&signal_hold_epoch, 0, memory_order_release);
    atomic_store_explicit(&signal_release_epoch, 0, memory_order_release);
    peak_detach_controller_signal_wake_waiters();
}

static void
peak_detach_controller_signal_deactivate_exited_slots(int epoch)
{
    pid_t pid = getpid();

    for (uint32_t i = 0, count = atomic_load_explicit(&signal_slot_count, memory_order_acquire); i < count; i++) {
        PeakDetachSignalSlot* slot = &signal_slots[i];
        if (!peak_detach_controller_signal_slot_active(slot, epoch) ||
            atomic_load_explicit(&slot->arrived_epoch, memory_order_acquire) == epoch) {
            continue;
        }
        if (syscall(SYS_tgkill, pid, atomic_load_explicit(&slot->tid, memory_order_acquire), 0) != 0 && errno == ESRCH) {
            atomic_store_explicit(&slot->active_epoch, 0, memory_order_release);
        }
    }
}

static gboolean
peak_detach_controller_signal_tid_is_active(pid_t tid, int epoch)
{
    for (uint32_t i = 0, count = atomic_load_explicit(&signal_slot_count, memory_order_acquire); i < count; i++) {
        const PeakDetachSignalSlot* slot = &signal_slots[i];
        if (atomic_load_explicit(&slot->tid, memory_order_acquire) == (int)tid &&
            atomic_load_explicit(&slot->active_epoch, memory_order_acquire) == epoch &&
            atomic_load_explicit(&slot->arrived_epoch, memory_order_acquire) == epoch) {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
peak_detach_controller_signal_admit_thread(pid_t tid,
                                           int epoch,
                                           double deadline,
                                           PeakDetachStatus* status_out)
{
    if (peak_detach_controller_deadline_remaining_ms(deadline) <= 0) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_TIMEOUT;
        }
        return FALSE;
    }

    PeakDetachSignalSlot* slot =
        peak_detach_controller_signal_alloc_slot(tid, epoch);

    if (slot == NULL) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_UNSUPPORTED;
        }
        return FALSE;
    }

    if (atomic_load_explicit(&slot->arrived_epoch, memory_order_acquire) == epoch) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_SAFE;
        }
        return TRUE;
    }

    int backend_signum =
        peak_detach_controller_signal_backend_signum_load();
    int blocked = peak_detach_controller_signal_tid_blocks_reserved_until(tid,
                                                                          deadline);
    if (blocked != 0) {
        peak_detach_controller_note_failure_detail(
            blocked == -2 ? "signal-reserved-mask-timeout" :
            blocked > 0 ? "signal-reserved-blocked" :
                          "signal-reserved-mask-unknown",
            (long)tid,
            (uintptr_t)backend_signum,
            (uintptr_t)epoch);
        if (status_out != NULL) {
            *status_out = blocked == -2 ? PEAK_DETACH_STATUS_TIMEOUT :
                          PEAK_DETACH_STATUS_UNSUPPORTED;
        }
        return FALSE;
    }

    if (peak_signal_policy_send_thread_signal(
            tid,
            backend_signum,
            peak_signal_policy_cookie_for(epoch, tid)) != 0) {
        if (errno == ESRCH) {
            atomic_store_explicit(&slot->active_epoch, 0, memory_order_release);
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_SAFE;
            }
            return TRUE;
        }
        peak_detach_controller_signal_release_or_fatal_with_timeout(
            "signal admit send failure",
            peak_detach_controller_timeout_until_deadline(deadline, TRUE));
        if (status_out != NULL) {
            *status_out = errno == EAGAIN ? PEAK_DETACH_STATUS_TIMEOUT :
                          peak_detach_controller_errno_status();
        }
        return FALSE;
    }

    for (;;) {
        if (atomic_load_explicit(&slot->arrived_epoch, memory_order_acquire) == epoch) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_SAFE;
            }
            return TRUE;
        }
        if (syscall(SYS_tgkill, getpid(), tid, 0) != 0 && errno == ESRCH) {
            atomic_store_explicit(&slot->active_epoch, 0, memory_order_release);
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_SAFE;
            }
            return TRUE;
        }
        if (peak_detach_controller_monotonic_second() >= deadline) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_TIMEOUT;
            }
            return FALSE;
        }
        usleep(1000);
    }
}

static gboolean
peak_detach_controller_signal_verify_no_unheld_threads(pid_t controller_tid,
                                                       int epoch,
                                                       gboolean allow_admit,
                                                       gboolean require_gate_waiter,
                                                       const char* failure_reason,
                                                       double deadline,
                                                       PeakDetachStatus* status_out)
{
    for (;;) {
        gboolean admitted = FALSE;
        gboolean waiting_for_gate = FALSE;
        pid_t waiting_tid = 0;
        int gate_epoch = atomic_load_explicit(&strict_mutation_thread_gate,
                                              memory_order_acquire);
        DIR* dir = opendir("/proc/self/task");
        if (dir == NULL) {
            if (status_out != NULL) {
                *status_out = peak_detach_controller_errno_status();
            }
            return FALSE;
        }

        for (;;) {
            errno = 0;
            struct dirent* entry = readdir(dir);
            if (entry == NULL) {
                break;
            }
            if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
                continue;
            }
            char* end = NULL;
            long value = strtol(entry->d_name, &end, 10);
            if (end == entry->d_name || *end != '\0' || value <= 0) {
                continue;
            }
            pid_t tid = (pid_t)value;
            if (tid == controller_tid) {
                continue;
            }
            if (peak_detach_controller_signal_tid_is_active(tid, epoch)) {
                continue;
            }
            if (!allow_admit) {
                peak_detach_controller_note_failure_detail(
                    failure_reason != NULL ? failure_reason :
                                             "signal-unheld-thread",
                    (long)tid,
                    0,
                    (uintptr_t)epoch);
                closedir(dir);
                if (status_out != NULL) {
                    *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
                }
                return FALSE;
            }
            if (require_gate_waiter &&
                !peak_detach_controller_thread_is_gate_waiter(tid, gate_epoch)) {
                waiting_for_gate = TRUE;
                waiting_tid = tid;
                continue;
            }
            if (!peak_detach_controller_signal_admit_thread(tid,
                                                            epoch,
                                                            deadline,
                                                            status_out)) {
                closedir(dir);
                return FALSE;
            }
            admitted = TRUE;
        }

        gboolean ok = errno == 0;
        closedir(dir);
        if (!ok) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_ERROR;
            }
            return FALSE;
        }
        if (!admitted && !waiting_for_gate) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_SAFE;
            }
            return TRUE;
        }
        if (peak_detach_controller_monotonic_second() >= deadline) {
            if (waiting_for_gate) {
                peak_detach_controller_note_failure_detail(
                    failure_reason != NULL ? failure_reason :
                                             "signal-unheld-not-gated",
                    (long)waiting_tid,
                    0,
                    (uintptr_t)gate_epoch);
            }
            if (status_out != NULL) {
                *status_out = waiting_for_gate ?
                    PEAK_DETACH_STATUS_CLASSIFY_FAILED :
                    PEAK_DETACH_STATUS_TIMEOUT;
            }
            return FALSE;
        }
        usleep(1000);
    }
}

static gboolean
peak_detach_controller_signal_release(PeakDetachStatus* status_out,
                                      unsigned int timeout_ms)
{
    int epoch = atomic_load_explicit(&signal_hold_epoch, memory_order_acquire);
    if (epoch == 0) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_SAFE;
        }
        return TRUE;
    }

    atomic_store_explicit(&signal_release_epoch, epoch, memory_order_release);
    peak_detach_controller_signal_wake_waiters();
    peak_detach_controller_trace_signal_phase("release-start",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              0,
                                              0,
                                              0);

    double deadline = peak_detach_controller_monotonic_second() +
        ((double)peak_detach_controller_effective_timeout_ms(timeout_ms) /
         1000.0);
    for (;;) {
        gboolean all_done = TRUE;
        for (uint32_t i = 0, count = atomic_load_explicit(&signal_slot_count, memory_order_acquire); i < count; i++) {
            PeakDetachSignalSlot* slot = &signal_slots[i];
            if (peak_detach_controller_signal_slot_active(slot, epoch) &&
                atomic_load_explicit(&slot->arrived_epoch, memory_order_acquire) == epoch &&
                atomic_load_explicit(&slot->done_epoch, memory_order_acquire) != epoch) {
                all_done = FALSE;
                break;
            }
        }
        if (all_done) {
            gboolean all_arrived = TRUE;
            gboolean rewrite_ok = TRUE;
            for (uint32_t i = 0, count = atomic_load_explicit(&signal_slot_count, memory_order_acquire); i < count; i++) {
                PeakDetachSignalSlot* slot = &signal_slots[i];
                if (!peak_detach_controller_signal_slot_active(slot, epoch)) {
                    continue;
                }
                if (atomic_load_explicit(&slot->arrived_epoch, memory_order_acquire) != epoch) {
                    all_arrived = FALSE;
                    break;
                }
                if (atomic_load_explicit(&slot->rewrite_epoch, memory_order_acquire) == epoch &&
                    atomic_load_explicit(&slot->rewrite_status, memory_order_acquire) != 1) {
                    rewrite_ok = FALSE;
                    break;
                }
            }
            if (!rewrite_ok) {
                peak_detach_controller_trace_signal_phase(
                    "release-rewrite-failed",
                    epoch,
                    PEAK_DETACH_STATUS_ERROR,
                    0,
                    0,
                    0);
                if (status_out != NULL) {
                    *status_out = PEAK_DETACH_STATUS_ERROR;
                }
                return FALSE;
            }
            peak_detach_controller_trace_signal_phase("release-complete",
                                                      epoch,
                                                      PEAK_DETACH_STATUS_SAFE,
                                                      0,
                                                      0,
                                                      0);
            if (all_arrived) {
                peak_detach_controller_signal_clear_slots();
            } else {
                atomic_store_explicit(&signal_hold_epoch, 0, memory_order_release);
                atomic_store_explicit(&signal_slot_count, 0, memory_order_release);
            }
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_SAFE;
            }
            return TRUE;
        }
        if (peak_detach_controller_monotonic_second() >= deadline) {
            peak_detach_controller_trace_signal_phase("release-timeout",
                                                      epoch,
                                                      PEAK_DETACH_STATUS_TIMEOUT,
                                                      0,
                                                      0,
                                                      0);
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_TIMEOUT;
            }
            return FALSE;
        }
        usleep(1000);
    }
}

static void
peak_detach_controller_signal_release_or_fatal_with_timeout(
    const char* context,
    unsigned int timeout_ms)
{
    PeakDetachStatus release_status = PEAK_DETACH_STATUS_ERROR;

    if (!peak_detach_controller_signal_release(&release_status, timeout_ms)) {
        peak_detach_controller_mark_helper_fatal(context, release_status);
    }
}

static gboolean
peak_detach_controller_signal_stop_threads(PeakDetachHelperThreadSnapshot* snapshots,
                                           uint32_t* snapshot_count_out,
                                           double deadline,
                                           PeakDetachStatus* status_out)
{
    if (peak_detach_controller_deadline_remaining_ms(deadline) <= 0) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_TIMEOUT;
        }
        return FALSE;
    }
    PeakDetachStatus ensure_status = PEAK_DETACH_STATUS_ERROR;
    if (!peak_detach_controller_ensure_signal_backend(&ensure_status)) {
        peak_detach_controller_trace_signal_phase("ensure-failed",
                                                  0,
                                                  ensure_status,
                                                  0,
                                                  0,
                                                  0);
        if (status_out != NULL) {
            *status_out = ensure_status;
        }
        return FALSE;
    }
    int backend_signum =
        peak_detach_controller_signal_backend_signum_load();

    pid_t controller_tid = (pid_t)syscall(SYS_gettid);
    int epoch = atomic_fetch_add_explicit(&signal_epoch_counter,
                                          1,
                                          memory_order_acq_rel);
    if (epoch <= 0) {
        epoch = 1;
        atomic_store_explicit(&signal_epoch_counter, 2, memory_order_release);
    }

    peak_detach_controller_signal_clear_slots();
    atomic_store_explicit(&signal_hold_epoch, epoch, memory_order_release);
    *snapshot_count_out = 0;
    peak_detach_controller_trace_signal_phase("stop-start",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              0,
                                              0,
                                              (uintptr_t)backend_signum);

    DIR* dir = opendir("/proc/self/task");
    if (dir == NULL) {
        peak_detach_controller_trace_signal_phase(
            "enum-failed",
            epoch,
            peak_detach_controller_errno_status(),
            0,
            (uintptr_t)errno,
            (uintptr_t)backend_signum);
        peak_detach_controller_signal_clear_slots();
        if (status_out != NULL) {
            *status_out = peak_detach_controller_errno_status();
        }
        return FALSE;
    }

    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (entry == NULL) {
            break;
        }
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        char* end = NULL;
        long value = strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || value <= 0) {
            continue;
        }
        pid_t tid = (pid_t)value;
        if (tid == controller_tid) {
            continue;
        }
        PeakDetachSignalSlot* slot =
            peak_detach_controller_signal_alloc_slot(tid, epoch);
        if (slot == NULL) {
            peak_detach_controller_trace_signal_phase(
                "slot-overflow",
                epoch,
                PEAK_DETACH_STATUS_UNSUPPORTED,
                (long)tid,
                (uintptr_t)atomic_load_explicit(&signal_slot_count,
                                                memory_order_acquire),
                (uintptr_t)backend_signum);
            closedir(dir);
            peak_detach_controller_signal_clear_slots();
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_UNSUPPORTED;
            }
            return FALSE;
        }
    }

    gboolean enum_ok = errno == 0;
    closedir(dir);
    if (!enum_ok) {
        peak_detach_controller_trace_signal_phase("enum-failed",
                                                  epoch,
                                                  PEAK_DETACH_STATUS_ERROR,
                                                  0,
                                                  (uintptr_t)errno,
                                                  (uintptr_t)backend_signum);
        peak_detach_controller_signal_clear_slots();
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }
    peak_detach_controller_trace_signal_phase("enum-done",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              0,
                                              0,
                                              (uintptr_t)backend_signum);

    for (uint32_t i = 0, count = atomic_load_explicit(&signal_slot_count, memory_order_acquire); i < count; i++) {
        PeakDetachSignalSlot* slot = &signal_slots[i];
        if (!peak_detach_controller_signal_slot_active(slot, epoch)) {
            continue;
        }
        pid_t tid =
            (pid_t)atomic_load_explicit(&slot->tid, memory_order_acquire);
        int blocked =
            peak_detach_controller_signal_tid_blocks_reserved_until(tid,
                                                                    deadline);
        if (blocked != 0) {
            peak_detach_controller_note_failure_detail(
                blocked == -2 ? "signal-reserved-mask-timeout" :
                blocked > 0 ? "signal-reserved-blocked" :
                              "signal-reserved-mask-unknown",
                (long)tid,
                (uintptr_t)backend_signum,
                (uintptr_t)epoch);
            peak_detach_controller_trace_signal_phase(
                "mask-failed",
                epoch,
                blocked == -2 ? PEAK_DETACH_STATUS_TIMEOUT :
                                PEAK_DETACH_STATUS_UNSUPPORTED,
                (long)tid,
                (uintptr_t)blocked,
                (uintptr_t)backend_signum);
            peak_detach_controller_signal_clear_slots();
            if (status_out != NULL) {
                *status_out = blocked == -2 ? PEAK_DETACH_STATUS_TIMEOUT :
                              PEAK_DETACH_STATUS_UNSUPPORTED;
            }
            return FALSE;
        }
    }
    peak_detach_controller_trace_signal_phase("mask-done",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              0,
                                              0,
                                              (uintptr_t)backend_signum);

    for (uint32_t i = 0, count = atomic_load_explicit(&signal_slot_count, memory_order_acquire); i < count; i++) {
        PeakDetachSignalSlot* slot = &signal_slots[i];
        pid_t tid =
            (pid_t)atomic_load_explicit(&slot->tid, memory_order_acquire);
#ifdef PEAK_ENABLE_TEST_HOOKS
        if (epoch > 1 &&
            peak_detach_controller_test_env_truthy(
                PEAK_TEST_DETACH_SIGNAL_INJECT_STALE_BEFORE_STOP_ENV)) {
            (void)peak_signal_policy_send_thread_signal(
                tid,
                backend_signum,
                peak_signal_policy_cookie_for(epoch - 1, tid));
        }
        if (epoch < INT_MAX &&
            peak_detach_controller_test_env_truthy(
                PEAK_TEST_DETACH_SIGNAL_INJECT_FUTURE_BEFORE_STOP_ENV)) {
            (void)peak_signal_policy_send_thread_signal(
                tid,
                backend_signum,
                peak_signal_policy_cookie_for(epoch + 1, tid));
        }
#endif
        if (peak_signal_policy_send_thread_signal(
                tid,
                backend_signum,
                peak_signal_policy_cookie_for(epoch, tid)) != 0) {
            int send_errno = errno;
            PeakDetachStatus send_status =
                send_errno == EAGAIN ? PEAK_DETACH_STATUS_TIMEOUT :
                                       peak_detach_controller_errno_status();
            if (errno == ESRCH) {
                atomic_store_explicit(&slot->active_epoch, 0, memory_order_release);
                continue;
            }
            peak_detach_controller_trace_signal_phase(
                "send-failed",
                epoch,
                send_status,
                (long)tid,
                (uintptr_t)send_errno,
                (uintptr_t)backend_signum);
            peak_detach_controller_signal_release_or_fatal_with_timeout(
                "signal stop send failure",
                peak_detach_controller_timeout_until_deadline(deadline, TRUE));
            if (status_out != NULL) {
                *status_out = send_status;
            }
            return FALSE;
        }
    }
    peak_detach_controller_trace_signal_phase("signals-sent",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              0,
                                              0,
                                              (uintptr_t)backend_signum);

    for (;;) {
        gboolean all_arrived = TRUE;
        peak_detach_controller_signal_deactivate_exited_slots(epoch);
        for (uint32_t i = 0, count = atomic_load_explicit(&signal_slot_count, memory_order_acquire); i < count; i++) {
            PeakDetachSignalSlot* slot = &signal_slots[i];
            if (peak_detach_controller_signal_slot_active(slot, epoch) &&
                atomic_load_explicit(&slot->arrived_epoch, memory_order_acquire) != epoch) {
                all_arrived = FALSE;
                break;
            }
        }
        if (all_arrived) {
            break;
        }
        if (peak_detach_controller_monotonic_second() >= deadline) {
            peak_detach_controller_trace_signal_phase("arrivals-timeout",
                                                      epoch,
                                                      PEAK_DETACH_STATUS_TIMEOUT,
                                                      0,
                                                      0,
                                                      (uintptr_t)backend_signum);
            peak_detach_controller_signal_release_or_fatal_with_timeout(
                "signal stop timeout",
                peak_detach_controller_timeout_until_deadline(deadline, TRUE));
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_TIMEOUT;
            }
            return FALSE;
        }
        usleep(1000);
    }
    peak_detach_controller_trace_signal_phase("arrivals-complete",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              0,
                                              0,
                                              (uintptr_t)backend_signum);

    if (!peak_detach_controller_signal_verify_no_unheld_threads(controller_tid,
                                                               epoch,
                                                               TRUE,
                                                               FALSE,
                                                               "signal-unheld-stop",
                                                               deadline,
                                                               status_out)) {
        peak_detach_controller_trace_signal_phase(
            "verify-failed",
            epoch,
            status_out != NULL ? *status_out : PEAK_DETACH_STATUS_ERROR,
            0,
            0,
            (uintptr_t)backend_signum);
        peak_detach_controller_signal_release_or_fatal_with_timeout(
            "signal stop verification failure",
            peak_detach_controller_timeout_until_deadline(deadline, TRUE));
        return FALSE;
    }
    peak_detach_controller_trace_signal_phase("verify-complete",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              0,
                                              0,
                                              (uintptr_t)backend_signum);

    for (uint32_t i = 0, count = atomic_load_explicit(&signal_slot_count, memory_order_acquire); i < count; i++) {
        PeakDetachSignalSlot* slot = &signal_slots[i];
        if (!peak_detach_controller_signal_slot_active(slot, epoch) ||
            atomic_load_explicit(&slot->arrived_epoch, memory_order_acquire) != epoch) {
            continue;
        }
        if (*snapshot_count_out >= PEAK_DETACH_HELPER_MAX_THREADS) {
            peak_detach_controller_trace_signal_phase(
                "snapshot-overflow",
                epoch,
                PEAK_DETACH_STATUS_UNSUPPORTED,
                0,
                (uintptr_t)*snapshot_count_out,
                (uintptr_t)backend_signum);
            peak_detach_controller_signal_release_or_fatal_with_timeout(
                "signal stop snapshot overflow",
                peak_detach_controller_timeout_until_deadline(deadline, TRUE));
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_UNSUPPORTED;
            }
            return FALSE;
        }
        PeakDetachHelperThreadSnapshot* snapshot =
            &snapshots[(*snapshot_count_out)++];
        snapshot->tid = atomic_load_explicit(&slot->tid, memory_order_acquire);
        snapshot->status = PEAK_DETACH_HELPER_THREAD_OK;
        snapshot->pc = (uint64_t)atomic_load_explicit(&slot->pc,
                                                       memory_order_acquire);
    }
    peak_detach_controller_trace_signal_phase("snapshots-complete",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              0,
                                              (uintptr_t)*snapshot_count_out,
                                              (uintptr_t)backend_signum);

    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

static int
peak_detach_controller_signal_write_memory(uint64_t address,
                                           const uint8_t* bytes,
                                           size_t size)
{
    if (address == 0 || bytes == NULL || size == 0 ||
        size > PEAK_DETACH_HELPER_MAX_PATCH_BYTES) {
        errno = EINVAL;
        return -1;
    }

    long page_size_long = sysconf(_SC_PAGESIZE);
    if (page_size_long <= 0) {
        errno = EINVAL;
        return -1;
    }
    uintptr_t page_size = (uintptr_t)page_size_long;
    uintptr_t start = (uintptr_t)address & ~(page_size - 1u);
    uintptr_t finish = ((uintptr_t)address + size + page_size - 1u) &
        ~(page_size - 1u);
    if (finish <= start) {
        errno = EINVAL;
        return -1;
    }

    size_t protect_size = (size_t)(finish - start);
    if (mprotect((void*)start, protect_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return -1;
    }
    memcpy((void*)(uintptr_t)address, bytes, size);
    __builtin___clear_cache((char*)(uintptr_t)address,
                            (char*)((uintptr_t)address + size));
    int saved_errno = 0;
    if (mprotect((void*)start, protect_size, PROT_READ | PROT_EXEC) != 0) {
        saved_errno = errno;
    }
    if (memcmp((void*)(uintptr_t)address, bytes, size) != 0) {
        errno = EIO;
        return -1;
    }
    if (saved_errno != 0) {
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static gboolean
peak_detach_controller_signal_temp_breakpoint_out_of_range(
    const PeakDetachHelperInstruction* instruction,
    int epoch,
    double deadline,
    PeakDetachStatus* status_out)
{
    PeakDetachSignalSlot* slot =
        peak_detach_controller_signal_find_slot((pid_t)instruction->tid, epoch);
    uint64_t breakpoint_u64 = instruction->address + instruction->size;
    uintptr_t breakpoint_pc = (uintptr_t)breakpoint_u64;
    uint8_t original[PEAK_DETACH_HELPER_MAX_PATCH_BYTES];
    uint8_t trap[PEAK_DETACH_HELPER_MAX_PATCH_BYTES];
    uint32_t trap_size = 0;

    if (!signal_breakpoint_supported ||
        !peak_detach_controller_signal_trap_handler_is_installed() ||
        slot == NULL ||
        atomic_load_explicit(&slot->arrived_epoch, memory_order_acquire) != epoch ||
        instruction->address == 0 || instruction->size == 0 ||
        instruction->size > PEAK_DETACH_HELPER_MAX_PATCH_BYTES ||
        breakpoint_u64 < instruction->address ||
        !peak_detach_controller_signal_breakpoint_bytes(trap, &trap_size) ||
        trap_size == 0 || trap_size > PEAK_DETACH_HELPER_MAX_PATCH_BYTES) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

#if defined(__aarch64__)
    if ((breakpoint_pc & 3u) != 0) {
        peak_detach_controller_note_failure_detail(
            "signal-breakpoint-unaligned",
            instruction != NULL ? (long)instruction->tid : 0,
            breakpoint_pc,
            instruction != NULL ? (uintptr_t)instruction->size : 0);
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
        }
        return FALSE;
    }
#endif

    uintptr_t pc = atomic_load_explicit(&slot->pc, memory_order_acquire);
    if (!peak_detach_controller_signal_instruction_range_contains(
            instruction->address, instruction->size, pc)) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_SAFE;
        }
        return TRUE;
    }
    peak_detach_controller_trace_signal_phase("evac-breakpoint-start",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              instruction != NULL ? (long)instruction->tid : 0,
                                              (uintptr_t)instruction->address,
                                              (uintptr_t)instruction->size);

    memcpy(original, (void*)breakpoint_pc, trap_size);
    if (peak_detach_controller_signal_write_memory(breakpoint_u64,
                                                   trap,
                                                   trap_size) != 0) {
        PeakDetachStatus status = peak_detach_controller_errno_status();
        peak_detach_controller_trace_signal_phase(
            "evac-breakpoint-write-failed",
            epoch,
            status,
            instruction != NULL ? (long)instruction->tid : 0,
            breakpoint_pc,
            (uintptr_t)trap_size);
        if (status_out != NULL) {
            *status_out = status;
        }
        return FALSE;
    }

    atomic_store_explicit(&slot->evacuate_breakpoint_pc,
                          breakpoint_pc,
                          memory_order_release);
    atomic_store_explicit(&slot->evacuate_status, 0, memory_order_release);
    atomic_store_explicit(&slot->evacuated_epoch, 0, memory_order_release);
    atomic_store_explicit(&slot->evacuate_started_epoch, 0, memory_order_release);
    atomic_store_explicit(&slot->evacuate_epoch, epoch, memory_order_release);
    peak_detach_controller_signal_wake_waiters();
    peak_detach_controller_trace_signal_phase("evac-breakpoint-armed",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              instruction != NULL ? (long)instruction->tid : 0,
                                              breakpoint_pc,
                                              (uintptr_t)trap_size);

    for (;;) {
        if (atomic_load_explicit(&slot->evacuated_epoch, memory_order_acquire) == epoch) {
            peak_detach_controller_trace_signal_phase(
                "evac-breakpoint-hit",
                epoch,
                PEAK_DETACH_STATUS_SAFE,
                instruction != NULL ? (long)instruction->tid : 0,
                breakpoint_pc,
                (uintptr_t)trap_size);
            break;
        }
        if (atomic_load_explicit(&slot->evacuate_status, memory_order_acquire) < 0) {
            if (peak_detach_controller_signal_write_memory(breakpoint_u64,
                                                           original,
                                                           trap_size) != 0) {
                PeakDetachStatus restore_status = peak_detach_controller_errno_status();
                peak_detach_controller_mark_helper_fatal(
                    "signal breakpoint abort restore failure",
                    restore_status);
                if (status_out != NULL) {
                    *status_out = restore_status;
                }
                return FALSE;
            }
            peak_detach_controller_trace_signal_phase(
                "evac-breakpoint-aborted",
                epoch,
                PEAK_DETACH_STATUS_ERROR,
                instruction != NULL ? (long)instruction->tid : 0,
                breakpoint_pc,
                (uintptr_t)trap_size);
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_ERROR;
            }
            return FALSE;
        }
        if (peak_detach_controller_monotonic_second() >= deadline) {
            (void)peak_detach_controller_signal_write_memory(breakpoint_u64,
                                                             original,
                                                             trap_size);
            peak_detach_controller_trace_signal_phase(
                "evac-breakpoint-timeout",
                epoch,
                PEAK_DETACH_STATUS_TIMEOUT,
                instruction != NULL ? (long)instruction->tid : 0,
                breakpoint_pc,
                (uintptr_t)trap_size);
            peak_detach_controller_mark_helper_fatal(
                "signal breakpoint evacuation timeout",
                PEAK_DETACH_STATUS_TIMEOUT);
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_TIMEOUT;
            }
            return FALSE;
        }
        usleep(1000);
    }

    if (peak_detach_controller_signal_write_memory(breakpoint_u64,
                                                   original,
                                                   trap_size) != 0) {
        PeakDetachStatus restore_status = peak_detach_controller_errno_status();
        peak_detach_controller_mark_helper_fatal(
            "signal breakpoint restore failure",
            restore_status);
        if (status_out != NULL) {
            *status_out = restore_status;
        }
        return FALSE;
    }

    atomic_store_explicit(&slot->evacuate_epoch, 0, memory_order_release);
    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

static gboolean
peak_detach_controller_signal_evacuate(uint32_t instruction_count,
                                       PeakDetachHelperInstruction* instructions,
                                       double deadline,
                                       PeakDetachStatus* status_out)
{
    int epoch = atomic_load_explicit(&signal_hold_epoch, memory_order_acquire);
    if (epoch == 0) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }
    peak_detach_controller_trace_signal_phase("evac-start",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              0,
                                              (uintptr_t)instruction_count,
                                              0);

    for (uint32_t i = 0; i < instruction_count; i++) {
        PeakDetachHelperInstruction* instruction = &instructions[i];
        if (instruction->action == PEAK_DETACH_HELPER_INSTRUCTION_SET_PC) {
            PeakDetachSignalSlot* slot =
                peak_detach_controller_signal_find_slot((pid_t)instruction->tid,
                                                        epoch);
            if (slot == NULL ||
                atomic_load_explicit(&slot->arrived_epoch, memory_order_acquire) != epoch) {
                peak_detach_controller_trace_signal_phase(
                    "evac-validation-failed",
                    epoch,
                    PEAK_DETACH_STATUS_ERROR,
                    instruction != NULL ? (long)instruction->tid : 0,
                    instruction != NULL ? (uintptr_t)instruction->address : 0,
                    instruction != NULL ? (uintptr_t)instruction->action : 0);
                if (status_out != NULL) {
                    *status_out = PEAK_DETACH_STATUS_ERROR;
                }
                return FALSE;
            }
        } else if (instruction->action == PEAK_DETACH_HELPER_INSTRUCTION_WRITE_MEMORY) {
            if (instruction->address == 0 || instruction->size == 0 ||
                instruction->size > PEAK_DETACH_HELPER_MAX_PATCH_BYTES) {
                peak_detach_controller_trace_signal_phase(
                    "evac-validation-failed",
                    epoch,
                    PEAK_DETACH_STATUS_ERROR,
                    instruction != NULL ? (long)instruction->tid : 0,
                    instruction != NULL ? (uintptr_t)instruction->address : 0,
                    instruction != NULL ? (uintptr_t)instruction->action : 0);
                if (status_out != NULL) {
                    *status_out = PEAK_DETACH_STATUS_ERROR;
                }
                return FALSE;
            }
        } else if (instruction->action ==
                   PEAK_DETACH_HELPER_INSTRUCTION_SINGLE_STEP_OUT_OF_RANGE) {
            if (!peak_detach_controller_signal_breakpoint_can_evac() ||
                instruction->tid <= 0 || instruction->address == 0 ||
                instruction->size == 0 ||
                instruction->size > PEAK_DETACH_HELPER_MAX_PATCH_BYTES ||
                instruction->address + instruction->size < instruction->address) {
                peak_detach_controller_note_failure_detail(
                    "signal-evac-validation",
                    instruction != NULL ? (long)instruction->tid : 0,
                    instruction != NULL ? (uintptr_t)instruction->address : 0,
                    instruction != NULL ? (uintptr_t)instruction->size : 0);
                peak_detach_controller_trace_signal_phase(
                    "evac-validation-failed",
                    epoch,
                    PEAK_DETACH_STATUS_CLASSIFY_FAILED,
                    instruction != NULL ? (long)instruction->tid : 0,
                    instruction != NULL ? (uintptr_t)instruction->address : 0,
                    instruction != NULL ? (uintptr_t)instruction->action : 0);
                if (status_out != NULL) {
                    *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
                }
                return FALSE;
            }
        } else {
            peak_detach_controller_trace_signal_phase(
                "evac-validation-failed",
                epoch,
                PEAK_DETACH_STATUS_ERROR,
                instruction != NULL ? (long)instruction->tid : 0,
                instruction != NULL ? (uintptr_t)instruction->address : 0,
                instruction != NULL ? (uintptr_t)instruction->action : 0);
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_ERROR;
            }
            return FALSE;
        }
    }

    if (!peak_detach_controller_signal_verify_no_unheld_threads(
            (pid_t)syscall(SYS_gettid),
            epoch,
            TRUE,
            TRUE,
            "signal-unheld-before-evac",
            deadline,
            status_out)) {
        peak_detach_controller_trace_signal_phase(
            "evac-preverify-failed",
            epoch,
            status_out != NULL ? *status_out : PEAK_DETACH_STATUS_ERROR,
            0,
            0,
            0);
        return FALSE;
    }
    peak_detach_controller_trace_signal_phase("evac-preverify-complete",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              0,
                                              (uintptr_t)instruction_count,
                                              0);

    for (uint32_t i = 0; i < instruction_count; i++) {
        PeakDetachHelperInstruction* instruction = &instructions[i];
        if (instruction->action ==
            PEAK_DETACH_HELPER_INSTRUCTION_SINGLE_STEP_OUT_OF_RANGE &&
            !peak_detach_controller_signal_temp_breakpoint_out_of_range(
                instruction,
                epoch,
                deadline,
                status_out)) {
            peak_detach_controller_trace_signal_phase(
                "evac-breakpoint-failed",
                epoch,
                status_out != NULL ? *status_out : PEAK_DETACH_STATUS_ERROR,
                instruction != NULL ? (long)instruction->tid : 0,
                instruction != NULL ? (uintptr_t)instruction->address : 0,
                instruction != NULL ? (uintptr_t)instruction->size : 0);
            return FALSE;
        }
    }
    peak_detach_controller_trace_signal_phase("evac-breakpoint-complete",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              0,
                                              (uintptr_t)instruction_count,
                                              0);

    if (!peak_detach_controller_signal_verify_no_unheld_threads(
            (pid_t)syscall(SYS_gettid),
            epoch,
            TRUE,
            TRUE,
            "signal-unheld-after-evac",
            deadline,
            status_out)) {
        peak_detach_controller_trace_signal_phase(
            "evac-postverify-failed",
            epoch,
            status_out != NULL ? *status_out : PEAK_DETACH_STATUS_ERROR,
            0,
            0,
            0);
        return FALSE;
    }
    peak_detach_controller_trace_signal_phase("evac-postverify-complete",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              0,
                                              (uintptr_t)instruction_count,
                                              0);

    gboolean mutation_started = FALSE;
    for (uint32_t i = 0; i < instruction_count; i++) {
        PeakDetachHelperInstruction* instruction = &instructions[i];
        if (instruction->action == PEAK_DETACH_HELPER_INSTRUCTION_WRITE_MEMORY) {
            mutation_started = TRUE;
            if (peak_detach_controller_signal_write_memory(instruction->address,
                                                           instruction->bytes,
                                                           instruction->size) != 0) {
                PeakDetachStatus status = peak_detach_controller_errno_status();
                peak_detach_controller_trace_signal_phase(
                    "evac-write-failed",
                    epoch,
                    status,
                    instruction != NULL ? (long)instruction->tid : 0,
                    instruction != NULL ? (uintptr_t)instruction->address : 0,
                    instruction != NULL ? (uintptr_t)instruction->size : 0);
                if (status_out != NULL) {
                    *status_out = status;
                }
                if (mutation_started) {
                    peak_detach_controller_mark_helper_fatal("signal evacuate",
                                                             status);
                }
                return FALSE;
            }
        }
    }
    peak_detach_controller_trace_signal_phase("evac-write-complete",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              0,
                                              (uintptr_t)instruction_count,
                                              0);

    for (uint32_t i = 0; i < instruction_count; i++) {
        PeakDetachHelperInstruction* instruction = &instructions[i];
        if (instruction->action == PEAK_DETACH_HELPER_INSTRUCTION_SET_PC) {
            PeakDetachSignalSlot* slot =
                peak_detach_controller_signal_find_slot((pid_t)instruction->tid,
                                                        epoch);
            atomic_store_explicit(&slot->new_pc,
                                  (uintptr_t)instruction->pc,
                                  memory_order_release);
            atomic_store_explicit(&slot->rewrite_status,
                                  0,
                                  memory_order_release);
            atomic_store_explicit(&slot->rewrite_epoch,
                                  epoch,
                                  memory_order_release);
        }
    }
    peak_detach_controller_trace_signal_phase("evac-complete",
                                              epoch,
                                              PEAK_DETACH_STATUS_SAFE,
                                              0,
                                              (uintptr_t)instruction_count,
                                              0);

    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

static gboolean
peak_detach_controller_resume_backend(PeakDetachHoldBackend backend,
                                      double deadline,
                                      PeakDetachStatus* status_out)
{
    if (backend == PEAK_DETACH_HOLD_BACKEND_SIGNAL) {
        return peak_detach_controller_signal_release(
            status_out,
            peak_detach_controller_timeout_until_deadline(deadline, TRUE));
    }
    return peak_detach_controller_send_resume(deadline, status_out);
}

static gboolean
peak_detach_controller_evacuate_backend(PeakDetachHoldBackend backend,
                                        uint32_t instruction_count,
                                        PeakDetachHelperInstruction* instructions,
                                        double deadline,
                                        PeakDetachStatus* status_out)
{
    if (peak_detach_controller_deadline_remaining_ms(deadline) <= 0) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_TIMEOUT;
        }
        return FALSE;
    }

    if (backend == PEAK_DETACH_HOLD_BACKEND_SIGNAL) {
        return peak_detach_controller_signal_evacuate(instruction_count,
                                                      instructions,
                                                     deadline,
                                                     status_out);
    }
    return peak_detach_controller_send_evacuate(instruction_count,
                                               instructions,
                                               deadline,
                                               status_out);
}


static PeakDetachPhysicalPatchRecord*
peak_detach_controller_find_physical_patch_record(size_t hook_id,
                                                  gboolean create)
{
    PeakDetachPhysicalPatchRecord* free_record = NULL;

    for (size_t i = 0; i < PEAK_DETACH_MAX_PHYSICAL_PATCH_RECORDS; i++) {
        PeakDetachPhysicalPatchRecord* record = &physical_patch_records[i];
        if (record->used && record->hook_id == hook_id) {
            return record;
        }
        if (!record->used && free_record == NULL) {
            free_record = record;
        }
    }

    if (!create || free_record == NULL) {
        return NULL;
    }

    memset(free_record, 0, sizeof(*free_record));
    free_record->used = TRUE;
    free_record->hook_id = hook_id;
    return free_record;
}

static double
peak_detach_controller_monotonic_second(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }

    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

void
peak_detach_controller_configure_trace_diagnostics(gboolean enabled,
                                                   const char* path)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    (void)enabled;
    (void)path;
#else
    int old_fd = atomic_exchange_explicit(&trace_diagnostics_fd,
                                          -1,
                                          memory_order_acq_rel);
    if (old_fd >= 0) {
        peak_detach_controller_raw_close(old_fd);
    }

    if (enabled && path != NULL && path[0] != '\0') {
        int fd;

        snprintf(trace_diagnostics_path,
                 sizeof(trace_diagnostics_path),
                 "%s",
                 path);
        fd = open(trace_diagnostics_path,
                  O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                  0600);
        if (fd < 0) {
            trace_diagnostics_path[0] = '\0';
            enabled = FALSE;
        } else {
            atomic_store_explicit(&trace_diagnostics_fd,
                                  fd,
                                  memory_order_release);
        }
    } else {
        trace_diagnostics_path[0] = '\0';
        enabled = FALSE;
    }
    atomic_store_explicit(&trace_diagnostics_enabled,
                          enabled ? 1 : 0,
                          memory_order_release);
#endif
}

static void
peak_detach_controller_note_stop_window_started(void)
{
    held_mutation_started_at = peak_detach_controller_monotonic_second();
    atomic_store_explicit(&held_mutation_window_active,
                          1,
                          memory_order_release);
    last_stop_window_us = 0.0;
    peak_detach_controller_signal_reset_deferred_trace();
}

static void
peak_detach_controller_clear_stop_window_started(void)
{
    held_mutation_started_at = 0.0;
    atomic_store_explicit(&held_mutation_window_active,
                          0,
                          memory_order_release);
}

static gboolean
peak_detach_controller_stop_window_trace_deferred(void)
{
    return atomic_load_explicit(&held_mutation_window_active,
                                memory_order_acquire) != 0;
}

static void
peak_detach_controller_note_stop_window_finished(void)
{
    if (held_mutation_started_at > 0.0) {
        last_stop_window_us =
            (peak_detach_controller_monotonic_second() -
             held_mutation_started_at) *
            1000000.0;
    }
    peak_detach_controller_clear_stop_window_started();
    peak_detach_controller_signal_flush_deferred_trace(last_stop_window_us);
}

static gboolean
peak_detach_controller_append_write_instruction(PeakDetachHeldMutation* mutation,
                                                gpointer address,
                                                const uint8_t* bytes,
                                                uint32_t size)
{
    if (address == NULL || bytes == NULL || size == 0 ||
        size > PEAK_DETACH_HELPER_MAX_PATCH_BYTES ||
        mutation->instruction_count >= PEAK_DETACH_HELPER_MAX_INSTRUCTIONS) {
        return FALSE;
    }

    PeakDetachHelperInstruction* instruction =
        &mutation->instructions[mutation->instruction_count++];
    memset(instruction, 0, sizeof(*instruction));
    instruction->tid = 0;
    instruction->action = PEAK_DETACH_HELPER_INSTRUCTION_WRITE_MEMORY;
    instruction->address = (uint64_t)(uintptr_t)address;
    instruction->size = size;
    memcpy(instruction->bytes, bytes, size);
    return TRUE;
}

static gboolean
peak_detach_controller_pointer_in_range(gpointer pointer,
                                        gpointer start,
                                        size_t size)
{
    uintptr_t value = (uintptr_t)pointer;
    uintptr_t begin = (uintptr_t)start;
    uintptr_t end;

    if (pointer == NULL || start == NULL || size == 0) {
        return FALSE;
    }

    end = begin + (uintptr_t)size;
    return end >= begin && value >= begin && value < end;
}

static gboolean
peak_detach_controller_pointer_between(gpointer pointer,
                                       gpointer start,
                                       gpointer end)
{
    uintptr_t value = (uintptr_t)pointer;
    uintptr_t begin = (uintptr_t)start;
    uintptr_t finish = (uintptr_t)end;

    if (pointer == NULL || start == NULL || end == NULL || finish <= begin) {
        return FALSE;
    }

    return value >= begin && value < finish;
}

static gboolean
peak_detach_controller_append_single_step_out_of_range_instruction(
    PeakDetachHeldMutation* mutation,
    pid_t tid,
    gpointer address,
    uint32_t size)
{
    if (mutation == NULL || tid <= 0 || address == NULL || size == 0 ||
        size > PEAK_DETACH_HELPER_MAX_PATCH_BYTES ||
        mutation->instruction_count >= PEAK_DETACH_HELPER_MAX_INSTRUCTIONS) {
        return FALSE;
    }

    PeakDetachHelperInstruction* instruction =
        &mutation->instructions[mutation->instruction_count++];
    memset(instruction, 0, sizeof(*instruction));
    instruction->tid = tid;
    instruction->action =
        PEAK_DETACH_HELPER_INSTRUCTION_SINGLE_STEP_OUT_OF_RANGE;
    instruction->address = (uint64_t)(uintptr_t)address;
    instruction->size = size;
    return TRUE;
}

static gboolean
peak_detach_controller_capture_gum_snapshot(const PeakDetachRequest* request,
                                            PeakDetachGumSnapshot* snapshot,
                                            PeakDetachStatus* status_out)
{
    memset(snapshot, 0, sizeof(*snapshot));

    gboolean needs_existing_hook_context =
        request->operation == PEAK_DETACH_OPERATION_DETACH ||
        request->operation == PEAK_DETACH_OPERATION_REATTACH ||
        request->operation == PEAK_DETACH_OPERATION_SHUTDOWN ||
        request->operation == PEAK_DETACH_OPERATION_REVERT;

    if (gum_interceptor_peak_get_pc_diagnostics(request->interceptor,
                                                request->function_address,
                                                request->listener,
                                                &snapshot->diagnostics)) {
        snapshot->has_context = TRUE;
    } else if (needs_existing_hook_context) {
        peak_detach_controller_note_failure_detail(
            "gum-diagnostics-missing",
            0,
            (uintptr_t)request->function_address,
            (uintptr_t)request->operation);
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
        }
        return FALSE;
    }

    if (request->operation == PEAK_DETACH_OPERATION_DETACH ||
        request->operation == PEAK_DETACH_OPERATION_SHUTDOWN ||
        request->operation == PEAK_DETACH_OPERATION_REVERT) {
        if (!gum_interceptor_peak_get_function_patch(request->interceptor,
                                                     request->function_address,
                                                     request->listener,
                                                     snapshot->active_patch,
                                                     snapshot->original_prologue,
                                                     &snapshot->prologue_len)) {
            peak_detach_controller_note_failure_detail(
                "gum-patch-missing",
                0,
                (uintptr_t)request->function_address,
                (uintptr_t)request->operation);
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
            }
            return FALSE;
        }
        snapshot->has_patch = TRUE;
    }

    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

static GumPeakPcState
peak_detach_controller_classify_pc_from_snapshot(
    const PeakDetachGumSnapshot* gum_snapshot,
    gpointer pc)
{
    const GumPeakPcDiagnostics* diagnostics = &gum_snapshot->diagnostics;
    gsize overwritten_prologue_len = diagnostics->overwritten_prologue_len;
    uintptr_t slice_start;
    uintptr_t slice_end;

    if (pc == NULL) {
        return GUM_PEAK_PC_UNKNOWN;
    }

    if (!gum_snapshot->has_context) {
        return GUM_PEAK_PC_SAFE;
    }

    if (overwritten_prologue_len == 0 && gum_snapshot->has_patch) {
        overwritten_prologue_len = gum_snapshot->prologue_len;
    }

    if (peak_detach_controller_pointer_in_range(
            pc,
            diagnostics->function_address,
            overwritten_prologue_len)) {
        return GUM_PEAK_PC_AT_PATCH_ENTRY;
    }

    if (peak_detach_controller_pointer_in_range(pc,
                                                diagnostics->enter_thunk_start,
                                                diagnostics->enter_thunk_size) ||
        peak_detach_controller_pointer_in_range(pc,
                                                diagnostics->leave_thunk_start,
                                                diagnostics->leave_thunk_size)) {
        return GUM_PEAK_PC_IN_DISPATCH;
    }

    if (diagnostics->trampoline_slice_start == NULL ||
        diagnostics->trampoline_slice_size == 0) {
        return GUM_PEAK_PC_SAFE;
    }

    if (!peak_detach_controller_pointer_in_range(
            pc,
            diagnostics->trampoline_slice_start,
            diagnostics->trampoline_slice_size)) {
        return GUM_PEAK_PC_SAFE;
    }

    if (peak_detach_controller_pointer_between(
            pc,
            diagnostics->on_enter_trampoline,
            diagnostics->on_leave_trampoline)) {
        return GUM_PEAK_PC_IN_ENTER_TRAMPOLINE;
    }

    if (peak_detach_controller_pointer_between(
            pc,
            diagnostics->on_leave_trampoline,
            diagnostics->on_invoke_trampoline)) {
        return GUM_PEAK_PC_IN_LEAVE_TRAMPOLINE;
    }

    if (diagnostics->on_invoke_trampoline == NULL) {
        return GUM_PEAK_PC_UNKNOWN;
    }

    slice_start = (uintptr_t)diagnostics->trampoline_slice_start;
    slice_end = slice_start + (uintptr_t)diagnostics->trampoline_slice_size;
    if (slice_end < slice_start ||
        (uintptr_t)diagnostics->on_invoke_trampoline >= slice_end) {
        return GUM_PEAK_PC_UNKNOWN;
    }

    if (peak_detach_controller_pointer_in_range(
            pc,
            diagnostics->on_invoke_trampoline,
            (size_t)(slice_end -
                     (uintptr_t)diagnostics->on_invoke_trampoline))) {
        return GUM_PEAK_PC_IN_INVOKE_TRAMPOLINE;
    }

    return GUM_PEAK_PC_UNKNOWN;
}

static gpointer
peak_detach_controller_safe_pc_from_snapshot(
    const PeakDetachGumSnapshot* snapshot,
    gpointer pc,
    GumPeakPcState state)
{
    if (state == GUM_PEAK_PC_IN_ENTER_TRAMPOLINE &&
        pc == snapshot->diagnostics.on_enter_trampoline) {
        return snapshot->diagnostics.function_address;
    }

    return NULL;
}

static gpointer
peak_detach_controller_canonical_function_address(
    const PeakDetachRequest* request,
    const PeakDetachGumSnapshot* snapshot)
{
    if (snapshot != NULL &&
        snapshot->has_context &&
        snapshot->diagnostics.function_address != NULL) {
        return snapshot->diagnostics.function_address;
    }

    return request != NULL ? request->function_address : NULL;
}

static gboolean
peak_detach_controller_append_physical_patch(const PeakDetachRequest* request,
                                             const PeakDetachGumSnapshot* snapshot,
                                             PeakDetachHeldMutation* mutation,
                                             PeakDetachStatus* status_out)
{
    if (request->operation != PEAK_DETACH_OPERATION_DETACH &&
        request->operation != PEAK_DETACH_OPERATION_REATTACH &&
        request->operation != PEAK_DETACH_OPERATION_SHUTDOWN &&
        request->operation != PEAK_DETACH_OPERATION_REVERT) {
        return TRUE;
    }

    gpointer patch_address =
        peak_detach_controller_canonical_function_address(request, snapshot);

    if (request->operation == PEAK_DETACH_OPERATION_REATTACH) {
        PeakDetachPhysicalPatchRecord* record =
            peak_detach_controller_find_physical_patch_record(request->hook_id,
                                                              FALSE);
        if (record == NULL || record->patch_size == 0 ||
            record->function_address != patch_address) {
            peak_detach_controller_note_failure_detail(
                "reattach-patch-record-missing",
                0,
                (uintptr_t)patch_address,
                record != NULL ? (uintptr_t)record->patch_size : 0);
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
            }
            return FALSE;
        }

        if (!peak_detach_controller_append_write_instruction(mutation,
                                                             patch_address,
                                                             record->active_patch,
                                                             record->patch_size)) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_ERROR;
            }
            return FALSE;
        }
        mutation->uses_physical_patch = TRUE;
        mutation->mutates_entry_bytes = TRUE;
        return TRUE;
    }

    if (!snapshot->has_patch || snapshot->prologue_len == 0 ||
        snapshot->prologue_len > PEAK_DETACH_HELPER_MAX_PATCH_BYTES) {
        peak_detach_controller_note_failure_detail(
            "physical-patch-size-invalid",
            0,
            (uintptr_t)request->function_address,
            (uintptr_t)snapshot->prologue_len);
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
        }
        return FALSE;
    }

    PeakDetachPhysicalPatchRecord* detach_record = NULL;
    gboolean detach_record_created = FALSE;

    if (request->operation == PEAK_DETACH_OPERATION_DETACH) {
        detach_record =
            peak_detach_controller_find_physical_patch_record(request->hook_id,
                                                              FALSE);
        if (detach_record == NULL) {
            detach_record = peak_detach_controller_find_physical_patch_record(
                request->hook_id,
                TRUE);
            detach_record_created = TRUE;
        }
        if (detach_record == NULL) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_ERROR;
            }
            return FALSE;
        }
    }

    if (!peak_detach_controller_append_write_instruction(mutation,
                                                         patch_address,
                                                         snapshot->original_prologue,
                                                         (uint32_t)snapshot->prologue_len)) {
        if (detach_record_created) {
            memset(detach_record, 0, sizeof(*detach_record));
        }
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    if (detach_record != NULL) {
        detach_record->function_address = patch_address;
        detach_record->patch_size = (uint32_t)snapshot->prologue_len;
        memcpy(detach_record->active_patch,
               snapshot->active_patch,
               snapshot->prologue_len);
    }

    mutation->uses_physical_patch = TRUE;
    mutation->mutates_entry_bytes = TRUE;
    return TRUE;
}

static void
peak_detach_controller_init_mutation_semantics(
    PeakDetachHeldMutation* mutation,
    PeakDetachOperation operation)
{
    /* Gum-PC safe-no-action is guarded by these semantics, not operation names. */
    mutation->mutates_entry_bytes = FALSE;
    mutation->mutates_gum_metadata = TRUE;
    mutation->frees_listener_state = TRUE;
    mutation->requires_target_entry_idle = FALSE;

    switch (operation) {
        case PEAK_DETACH_OPERATION_DETACH:
        case PEAK_DETACH_OPERATION_REATTACH:
            mutation->mutates_entry_bytes = TRUE;
            mutation->mutates_gum_metadata = FALSE;
            mutation->frees_listener_state = FALSE;
            break;

        case PEAK_DETACH_OPERATION_ATTACH:
        case PEAK_DETACH_OPERATION_SHUTDOWN:
        case PEAK_DETACH_OPERATION_REPLACE:
        case PEAK_DETACH_OPERATION_REVERT:
            mutation->mutates_entry_bytes = TRUE;
            mutation->mutates_gum_metadata = TRUE;
            mutation->frees_listener_state =
                operation == PEAK_DETACH_OPERATION_SHUTDOWN;
            mutation->requires_target_entry_idle =
                operation != PEAK_DETACH_OPERATION_SHUTDOWN;
            break;

        default:
            break;
    }
}

static gboolean
peak_detach_controller_mutation_is_physical_entry_bytes_only(
    const PeakDetachHeldMutation* mutation)
{
    return mutation->mutates_entry_bytes &&
           !mutation->mutates_gum_metadata &&
           !mutation->frees_listener_state;
}

static void
peak_detach_controller_set_single_owner(PeakDetachHeldMutation* mutation,
                                        const PeakDetachRequest* request,
                                        const PeakDetachGumSnapshot* snapshot)
{
    mutation->batch = FALSE;
    mutation->owner_thread_set = TRUE;
    mutation->owner_thread = pthread_self();
    mutation->hook_id = request->hook_id;
    mutation->function_address =
        peak_detach_controller_canonical_function_address(request, snapshot);
    mutation->listener = request->listener;
}

static gboolean
peak_detach_controller_request_matches_held_mutation(
    const PeakDetachRequest* request,
    const PeakDetachHeldMutation* mutation)
{
    if (mutation->batch) {
        return request == NULL;
    }
    if (request == NULL) {
        return FALSE;
    }
    if (request->operation != mutation->operation ||
        request->hook_id != mutation->hook_id ||
        request->listener != mutation->listener) {
        return FALSE;
    }
    return TRUE;
}

static gboolean
peak_detach_controller_current_thread_owns_held_mutation(
    const PeakDetachHeldMutation* mutation)
{
    return mutation->owner_thread_set &&
           pthread_equal(pthread_self(), mutation->owner_thread);
}

static int
peak_detach_controller_compare_snapshot_tid(const void* lhs, const void* rhs)
{
    const PeakDetachHelperThreadSnapshot* a =
        (const PeakDetachHelperThreadSnapshot*)lhs;
    const PeakDetachHelperThreadSnapshot* b =
        (const PeakDetachHelperThreadSnapshot*)rhs;

    if (a->tid < b->tid) {
        return -1;
    }
    if (a->tid > b->tid) {
        return 1;
    }
    return 0;
}

static gboolean
peak_detach_controller_validate_stopped_snapshots(
    const PeakDetachHelperThreadSnapshot* snapshots,
    uint32_t snapshot_count,
    double deadline,
    PeakDetachStatus* status_out)
{
    PeakDetachHelperThreadSnapshot sorted[PEAK_DETACH_HELPER_MAX_THREADS];

    if (snapshot_count > PEAK_DETACH_HELPER_MAX_THREADS) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_UNSUPPORTED;
        }
        return FALSE;
    }

    for (uint32_t i = 0; i < snapshot_count; i++) {
        if ((i & 63u) == 0 &&
            peak_detach_controller_deadline_remaining_ms(deadline) <= 0) {
            peak_detach_controller_note_failure_detail("classify-timeout",
                                                       0,
                                                       (uintptr_t)i,
                                                       (uintptr_t)snapshot_count);
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_TIMEOUT;
            }
            return FALSE;
        }
        if (snapshots[i].status != PEAK_DETACH_HELPER_THREAD_OK) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_ERROR;
            }
            return FALSE;
        }
        sorted[i] = snapshots[i];
    }

    qsort(sorted,
          snapshot_count,
          sizeof(sorted[0]),
          peak_detach_controller_compare_snapshot_tid);

    for (uint32_t i = 1; i < snapshot_count; i++) {
        if ((i & 63u) == 0 &&
            peak_detach_controller_deadline_remaining_ms(deadline) <= 0) {
            peak_detach_controller_note_failure_detail("classify-timeout",
                                                       0,
                                                       (uintptr_t)i,
                                                       (uintptr_t)snapshot_count);
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_TIMEOUT;
            }
            return FALSE;
        }
        if (sorted[i - 1].tid == sorted[i].tid) {
            peak_detach_controller_note_failure_detail(
                "duplicate-stopped-tid",
                (long)sorted[i].tid,
                (uintptr_t)(gpointer)(uintptr_t)sorted[i].pc,
                0);
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_ERROR;
            }
            return FALSE;
        }
    }

    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

static gboolean
peak_detach_controller_classify_stopped_threads(
    const PeakDetachRequest* request,
    const PeakDetachGumSnapshot* snapshot,
    PeakDetachHoldBackend backend,
    PeakDetachHelperThreadSnapshot* snapshots,
    uint32_t snapshot_count,
    double deadline,
    PeakDetachHeldMutation* mutation,
    PeakDetachStatus* status_out)
{
    mutation->instruction_count = 0;
    mutation->uses_physical_patch = FALSE;
    mutation->operation = request->operation;
    peak_detach_controller_set_single_owner(mutation, request, snapshot);
    peak_detach_controller_init_mutation_semantics(mutation,
                                                   request->operation);

    for (uint32_t i = 0; i < snapshot_count; i++) {
        PeakDetachHelperThreadSnapshot* thread_snapshot = &snapshots[i];
        GumPeakPcState state = GUM_PEAK_PC_UNKNOWN;
        gpointer pc = (gpointer)(uintptr_t)thread_snapshot->pc;
        gpointer function_address =
            peak_detach_controller_canonical_function_address(request,
                                                              snapshot);

        if ((i & 63u) == 0 &&
            peak_detach_controller_deadline_remaining_ms(deadline) <= 0) {
            peak_detach_controller_note_failure_detail("classify-timeout",
                                                       (long)thread_snapshot->tid,
                                                       (uintptr_t)pc,
                                                       (uintptr_t)i);
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_TIMEOUT;
            }
            return FALSE;
        }

        if (peak_detach_controller_pointer_in_range(pc,
                                                    request->blocked_pc_start,
                                                    request->blocked_pc_size)) {
            peak_detach_controller_note_failure_detail(
                "blocked-pc",
                (long)thread_snapshot->tid,
                (uintptr_t)pc,
                (uintptr_t)request->blocked_pc_size);
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
            }
            return FALSE;
        }

        if (mutation->requires_target_entry_idle &&
            peak_detach_controller_pointer_in_range(
                pc, function_address, GUM_PEAK_MAX_PROLOGUE_SIZE)) {
            peak_detach_controller_note_failure_detail(
                "entry-patch-live-pc",
                (long)thread_snapshot->tid,
                (uintptr_t)pc,
                (uintptr_t)GUM_PEAK_MAX_PROLOGUE_SIZE);
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
            }
            return FALSE;
        }

        state = peak_detach_controller_classify_pc_from_snapshot(snapshot,
                                                                pc);

        switch (state) {
            case GUM_PEAK_PC_SAFE:
                break;

            case GUM_PEAK_PC_AT_PATCH_ENTRY:
                if (request->operation == PEAK_DETACH_OPERATION_DETACH ||
                    request->operation == PEAK_DETACH_OPERATION_SHUTDOWN) {
                    if (pc == function_address) {
                        break;
                    }
                    peak_detach_controller_note_failure_detail(
                        request->operation == PEAK_DETACH_OPERATION_DETACH
                            ? "detach-patch-interior-pc"
                            : "shutdown-patch-interior-pc",
                        (long)thread_snapshot->tid,
                        (uintptr_t)pc,
                        (uintptr_t)snapshot->diagnostics.overwritten_prologue_len);
                    if (status_out != NULL) {
                        *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
                    }
                    return FALSE;
                }
                if (request->operation == PEAK_DETACH_OPERATION_REATTACH) {
                    gsize prologue_len =
                        snapshot->diagnostics.overwritten_prologue_len;

                    if (prologue_len == 0 && snapshot->has_patch) {
                        prologue_len = snapshot->prologue_len;
                    }
                    if (pc == function_address) {
                        break;
                    }
                    if ((backend == PEAK_DETACH_HOLD_BACKEND_HELPER ||
                         (backend == PEAK_DETACH_HOLD_BACKEND_SIGNAL &&
                          peak_detach_controller_signal_breakpoint_can_evac())) &&
                        prologue_len > 0 &&
                        prologue_len <= PEAK_DETACH_HELPER_MAX_PATCH_BYTES &&
                        peak_detach_controller_append_single_step_out_of_range_instruction(
                            mutation,
                            (pid_t)thread_snapshot->tid,
                            function_address,
                            (uint32_t)prologue_len)) {
                        break;
                    }
                    peak_detach_controller_note_failure_detail(
                        "reattach-prologue-no-evac",
                        (long)thread_snapshot->tid,
                        (uintptr_t)pc,
                        (uintptr_t)prologue_len);
                    if (status_out != NULL) {
                        *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
                    }
                    return FALSE;
                }
                break;

            case GUM_PEAK_PC_IN_ENTER_TRAMPOLINE:
            case GUM_PEAK_PC_IN_INVOKE_TRAMPOLINE:
            case GUM_PEAK_PC_IN_LEAVE_TRAMPOLINE:
            case GUM_PEAK_PC_IN_DISPATCH: {
                if (peak_detach_controller_mutation_is_physical_entry_bytes_only(
                        mutation)) {
                    break;
                }

                gpointer safe_pc =
                    peak_detach_controller_safe_pc_from_snapshot(snapshot,
                                                                 pc,
                                                                 state);

                if (safe_pc == NULL ||
                    mutation->instruction_count >= PEAK_DETACH_HELPER_MAX_INSTRUCTIONS) {
                    peak_detach_controller_note_failure_detail(
                        "gum-metadata-no-safe-pc",
                        (long)thread_snapshot->tid,
                        (uintptr_t)pc,
                        (uintptr_t)state);
                    if (status_out != NULL) {
                        *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
                    }
                    return FALSE;
                }

                PeakDetachHelperInstruction* instruction =
                    &mutation->instructions[mutation->instruction_count++];
                instruction->tid = thread_snapshot->tid;
                instruction->action = PEAK_DETACH_HELPER_INSTRUCTION_SET_PC;
                instruction->pc = (uint64_t)(uintptr_t)safe_pc;
                break;
            }

            case GUM_PEAK_PC_UNKNOWN:
            default:
                peak_detach_controller_note_failure_detail(
                    "pc-unknown",
                    (long)thread_snapshot->tid,
                    (uintptr_t)pc,
                    (uintptr_t)state);
                if (status_out != NULL) {
                    *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
                }
                return FALSE;
        }
    }

    if (!peak_detach_controller_append_physical_patch(request,
                                                       snapshot,
                                                       mutation,
                                                       status_out)) {
        return FALSE;
    }

    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

static gboolean
peak_detach_controller_find_instruction_conflict(
    const PeakDetachHeldMutation* mutation,
    const PeakDetachHelperInstruction* candidate)
{
    for (uint32_t i = 0; i < mutation->instruction_count; i++) {
        const PeakDetachHelperInstruction* existing = &mutation->instructions[i];

        if (existing->action != candidate->action) {
            continue;
        }

        if (candidate->action == PEAK_DETACH_HELPER_INSTRUCTION_SET_PC &&
            existing->tid == candidate->tid) {
            return existing->pc != candidate->pc;
        }

        if (candidate->action ==
            PEAK_DETACH_HELPER_INSTRUCTION_SINGLE_STEP_OUT_OF_RANGE &&
            existing->tid == candidate->tid) {
            return existing->address != candidate->address ||
                   existing->size != candidate->size;
        }

        if (candidate->action == PEAK_DETACH_HELPER_INSTRUCTION_WRITE_MEMORY &&
            peak_detach_controller_write_ranges_overlap(existing, candidate)) {
            if (existing->address == candidate->address &&
                existing->size == candidate->size &&
                memcmp(existing->bytes, candidate->bytes, candidate->size) == 0) {
                return FALSE;
            }
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
peak_detach_controller_instruction_is_duplicate(
    const PeakDetachHeldMutation* mutation,
    const PeakDetachHelperInstruction* candidate)
{
    for (uint32_t i = 0; i < mutation->instruction_count; i++) {
        const PeakDetachHelperInstruction* existing = &mutation->instructions[i];

        if (existing->action != candidate->action) {
            continue;
        }
        if (candidate->action == PEAK_DETACH_HELPER_INSTRUCTION_SET_PC &&
            existing->tid == candidate->tid &&
            existing->pc == candidate->pc) {
            return TRUE;
        }
        if (candidate->action ==
            PEAK_DETACH_HELPER_INSTRUCTION_SINGLE_STEP_OUT_OF_RANGE &&
            existing->tid == candidate->tid &&
            existing->address == candidate->address &&
            existing->size == candidate->size) {
            return TRUE;
        }
        if (candidate->action == PEAK_DETACH_HELPER_INSTRUCTION_WRITE_MEMORY &&
            existing->address == candidate->address &&
            existing->size == candidate->size &&
            memcmp(existing->bytes, candidate->bytes, candidate->size) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
peak_detach_controller_append_candidate_mutation(
    PeakDetachHeldMutation* aggregate,
    const PeakDetachHeldMutation* candidate,
    PeakDetachStatus* status_out)
{
    for (uint32_t i = 0; i < candidate->instruction_count; i++) {
        const PeakDetachHelperInstruction* instruction =
            &candidate->instructions[i];

        if (peak_detach_controller_find_instruction_conflict(aggregate,
                                                             instruction)) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
            }
            return FALSE;
        }
        if (peak_detach_controller_instruction_is_duplicate(aggregate,
                                                           instruction)) {
            continue;
        }
        if (aggregate->instruction_count >= PEAK_DETACH_HELPER_MAX_INSTRUCTIONS) {
            if (status_out != NULL) {
                *status_out = PEAK_DETACH_STATUS_ERROR;
            }
            return FALSE;
        }

        aggregate->instructions[aggregate->instruction_count++] = *instruction;
    }

    aggregate->uses_physical_patch =
        aggregate->uses_physical_patch || candidate->uses_physical_patch;
    aggregate->mutates_entry_bytes =
        aggregate->mutates_entry_bytes || candidate->mutates_entry_bytes;
    aggregate->mutates_gum_metadata =
        aggregate->mutates_gum_metadata || candidate->mutates_gum_metadata;
    aggregate->frees_listener_state =
        aggregate->frees_listener_state || candidate->frees_listener_state;
    aggregate->requires_target_entry_idle =
        aggregate->requires_target_entry_idle ||
        candidate->requires_target_entry_idle;
    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

static PeakDetachStatus
peak_detach_controller_empty_batch_status(const PeakDetachBatchResult* results,
                                          size_t result_count)
{
    gboolean all_unsupported = TRUE;

    for (size_t i = 0; i < result_count; i++) {
        if (results[i].status != PEAK_DETACH_STATUS_UNSUPPORTED) {
            all_unsupported = FALSE;
            break;
        }
    }

    return all_unsupported ? PEAK_DETACH_STATUS_UNSUPPORTED :
                             PEAK_DETACH_STATUS_CLASSIFY_FAILED;
}
#endif

gboolean
peak_detach_controller_prepare_hook_mutation(const PeakDetachRequest* request,
                                             PeakDetachStatus* status_out)
{
    peak_detach_controller_clear_failure_detail();
#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    peak_detach_controller_init_atfork_once();
#endif
    gboolean proc_ok = FALSE;
    size_t proc_threads = peak_detach_controller_count_proc_threads(&proc_ok);

    (void)proc_threads;
    (void)proc_ok;

    if (!peak_detach_controller_validate_request(request, status_out)) {
        return FALSE;
    }

    if (peak_detach_controller_mode() == PEAK_SAFE_DETACH_MODE_COMPATIBILITY) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED;
        }
        return TRUE;
    }

#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    if (!warned_missing_gum_api) {
        warned_missing_gum_api = TRUE;
        g_printerr("[peak] strict safe detach requested, but selected Frida Gum does not expose PEAK PC classification APIs; refusing Gum %s for hook %lu (%s)\n",
                   peak_detach_controller_operation_string(request->operation),
                   (unsigned long)request->hook_id,
                   request->symbol_name != NULL ? request->symbol_name : "<unknown>");
    }
    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_MISSING_GUM_API;
    }
    return FALSE;
#else
    PeakDetachGumSnapshot gum_snapshot;
    PeakDetachHelperThreadSnapshot snapshots[PEAK_DETACH_HELPER_MAX_THREADS];
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    uint32_t snapshot_count = 0;

    peak_detach_controller_lock_mutation_guard();

    if (held_mutation.active) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        peak_detach_controller_unlock_mutation_guard();
        return FALSE;
    }
    peak_detach_controller_clear_stop_window_started();
    last_stop_window_us = 0.0;

    if (helper_state_fatal) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        peak_detach_controller_unlock_mutation_guard();
        return FALSE;
    }

    PeakDetachRequestedBackend requested_backend =
        peak_detach_controller_requested_backend();
    PeakDetachHoldBackend stop_backend = PEAK_DETACH_HOLD_BACKEND_NONE;
    gboolean creation_gate_active = FALSE;
    double mutation_deadline =
        peak_detach_controller_deadline_for_timeout(request->timeout_ms);
    gboolean auto_use_signal_backend =
        requested_backend == PEAK_DETACH_REQUESTED_BACKEND_AUTO &&
        (request->avoid_external_helper ||
         peak_detach_controller_auto_should_use_signal_backend());
    gboolean auto_signal_fallback_pending = FALSE;
    PeakDetachStatus auto_signal_fallback_status = PEAK_DETACH_STATUS_ERROR;
    const char* auto_signal_fallback_context = NULL;
    const char* backend_reason =
        requested_backend == PEAK_DETACH_REQUESTED_BACKEND_SIGNAL
            ? "explicit-signal"
            : requested_backend == PEAK_DETACH_REQUESTED_BACKEND_HELPER
                  ? "explicit-helper"
                  : auto_use_signal_backend
                        ? (request->avoid_external_helper
                               ? "auto-avoid-external-helper"
                               : "auto-cached-signal-fallback")
                        : "auto-helper-first";

    peak_detach_controller_trace_backend_phase("prepare-start",
                                               request,
                                               requested_backend,
                                               stop_backend,
                                               status,
                                               backend_reason,
                                               1);

    if (requested_backend == PEAK_DETACH_REQUESTED_BACKEND_SIGNAL ||
        auto_use_signal_backend) {
        if (!peak_detach_controller_ensure_signal_backend(&status)) {
            peak_detach_controller_trace_backend_phase("prepare-backend-failed",
                                                       request,
                                                       requested_backend,
                                                       PEAK_DETACH_HOLD_BACKEND_SIGNAL,
                                                       status,
                                                       backend_reason,
                                                       1);
            if (status_out != NULL) {
                *status_out = status;
            }
            peak_detach_controller_unlock_mutation_guard();
            return FALSE;
        }
        stop_backend = PEAK_DETACH_HOLD_BACKEND_SIGNAL;
        peak_detach_controller_trace_backend_phase("prepare-backend-selected",
                                                   request,
                                                   requested_backend,
                                                   stop_backend,
                                                   PEAK_DETACH_STATUS_SAFE,
                                                   backend_reason,
                                                   1);
    } else if (!peak_detach_controller_ensure_helper_until(mutation_deadline,
                                                           &status)) {
        PeakDetachStatus helper_status = status;
        if (requested_backend == PEAK_DETACH_REQUESTED_BACKEND_AUTO &&
            peak_detach_controller_status_allows_auto_signal_fallback(
                helper_status) &&
            peak_detach_controller_helper_is_closed() &&
            peak_detach_controller_ensure_signal_backend(&status)) {
            stop_backend = PEAK_DETACH_HOLD_BACKEND_SIGNAL;
            auto_signal_fallback_pending = TRUE;
            auto_signal_fallback_status = helper_status;
            auto_signal_fallback_context = "startup failure";
            peak_detach_controller_trace_backend_phase("prepare-backend-selected",
                                                       request,
                                                       requested_backend,
                                                       stop_backend,
                                                       PEAK_DETACH_STATUS_SAFE,
                                                       "auto-helper-fallback",
                                                       1);
        } else {
            peak_detach_controller_trace_backend_phase("prepare-backend-failed",
                                                       request,
                                                       requested_backend,
                                                       PEAK_DETACH_HOLD_BACKEND_HELPER,
                                                       status,
                                                       backend_reason,
                                                       1);
            if (status_out != NULL) {
                *status_out = status;
            }
            peak_detach_controller_unlock_mutation_guard();
            return FALSE;
        }
    } else {
        peak_detach_controller_trace_backend_phase("prepare-backend-selected",
                                                   request,
                                                   requested_backend,
                                                   PEAK_DETACH_HOLD_BACKEND_HELPER,
                                                   PEAK_DETACH_STATUS_SAFE,
                                                   backend_reason,
                                                   1);
    }

    if (!peak_detach_controller_capture_gum_snapshot(request,
                                                     &gum_snapshot,
                                                     &status)) {
        if (status_out != NULL) {
            *status_out = status;
        }
        peak_detach_controller_unlock_mutation_guard();
        return FALSE;
    }

    peak_detach_controller_begin_thread_creation_gate();
    creation_gate_active = TRUE;
    peak_detach_controller_test_delay_after_gate_begin();

    if (stop_backend == PEAK_DETACH_HOLD_BACKEND_SIGNAL) {
        peak_detach_controller_trace_backend_phase("stop-signal-start",
                                                   request,
                                                   requested_backend,
                                                   stop_backend,
                                                   PEAK_DETACH_STATUS_SAFE,
                                                   backend_reason,
                                                   1);
        peak_detach_controller_note_stop_window_started();
        if (!peak_detach_controller_signal_stop_threads(snapshots,
                                                        &snapshot_count,
                                                        mutation_deadline,
                                                        &status)) {
            peak_detach_controller_clear_stop_window_started();
            if (creation_gate_active) {
                peak_detach_controller_end_thread_creation_gate();
                creation_gate_active = FALSE;
            }
            if (status_out != NULL) {
                *status_out = status;
            }
            peak_detach_controller_unlock_mutation_guard();
            return FALSE;
        }
    } else {
        peak_detach_controller_trace_backend_phase("stop-helper-start",
                                                   request,
                                                   requested_backend,
                                                   PEAK_DETACH_HOLD_BACKEND_HELPER,
                                                   PEAK_DETACH_STATUS_SAFE,
                                                   backend_reason,
                                                   1);
        peak_detach_controller_note_stop_window_started();
        if (!peak_detach_controller_stop_threads(snapshots,
                                                 &snapshot_count,
                                                 mutation_deadline,
                                                 &status)) {
            if (requested_backend == PEAK_DETACH_REQUESTED_BACKEND_AUTO &&
                peak_detach_controller_status_allows_auto_signal_fallback(status)) {
                PeakDetachStatus helper_status = status;
                gboolean helper_closed;
                int remaining_ms = 0;
                peak_detach_controller_trace_backend_phase("stop-helper-fallback-start",
                                                           request,
                                                           requested_backend,
                                                           PEAK_DETACH_HOLD_BACKEND_HELPER,
                                                           status,
                                                           "auto-helper-stop-fallback",
                                                           1);
                helper_closed = peak_detach_controller_close_helper();
                if (helper_closed) {
                    remaining_ms =
                        peak_detach_controller_deadline_remaining_ms(
                            mutation_deadline);
                }
                if (helper_closed &&
                    remaining_ms > 0 &&
                    peak_detach_controller_ensure_signal_backend(&status) &&
                    peak_detach_controller_signal_stop_threads(snapshots,
                                                               &snapshot_count,
                                                               mutation_deadline,
                                                               &status)) {
                    stop_backend = PEAK_DETACH_HOLD_BACKEND_SIGNAL;
                    auto_signal_fallback_pending = TRUE;
                    auto_signal_fallback_status = helper_status;
                    auto_signal_fallback_context = "stop failure";
                    peak_detach_controller_trace_backend_phase("stop-helper-fallback-selected",
                                                               request,
                                                               requested_backend,
                                                               stop_backend,
                                                               PEAK_DETACH_STATUS_SAFE,
                                                               "auto-helper-stop-fallback",
                                                               1);
                } else {
                    if (!helper_closed || remaining_ms <= 0) {
                        status = helper_status;
                    }
                    peak_detach_controller_trace_backend_phase("stop-helper-fallback-failed",
                                                               request,
                                                               requested_backend,
                                                               PEAK_DETACH_HOLD_BACKEND_SIGNAL,
                                                               status,
                                                               "auto-helper-stop-fallback",
                                                               1);
                    peak_detach_controller_clear_stop_window_started();
                    if (creation_gate_active) {
                        peak_detach_controller_end_thread_creation_gate();
                        creation_gate_active = FALSE;
                    }
                    if (status_out != NULL) {
                        *status_out = status;
                    }
                    peak_detach_controller_unlock_mutation_guard();
                    return FALSE;
                }
            } else {
                peak_detach_controller_clear_stop_window_started();
                if (creation_gate_active) {
                    peak_detach_controller_end_thread_creation_gate();
                    creation_gate_active = FALSE;
                }
                if (status_out != NULL) {
                    *status_out = status;
                }
                peak_detach_controller_unlock_mutation_guard();
                return FALSE;
            }
        } else {
            stop_backend = PEAK_DETACH_HOLD_BACKEND_HELPER;
            peak_detach_controller_trace_backend_phase("stop-helper-complete",
                                                       request,
                                                       requested_backend,
                                                       stop_backend,
                                                       PEAK_DETACH_STATUS_SAFE,
                                                       backend_reason,
                                                       1);
        }

        if (stop_backend == PEAK_DETACH_HOLD_BACKEND_NONE) {
            peak_detach_controller_clear_stop_window_started();
            if (creation_gate_active) {
                peak_detach_controller_end_thread_creation_gate();
                creation_gate_active = FALSE;
            }
            if (status_out != NULL) {
                *status_out = status;
            }
            peak_detach_controller_unlock_mutation_guard();
            return FALSE;
        }
    }

    if (auto_signal_fallback_pending &&
        stop_backend == PEAK_DETACH_HOLD_BACKEND_SIGNAL) {
        peak_detach_controller_cache_auto_signal_fallback(
            auto_signal_fallback_status,
            auto_signal_fallback_context);
    }

    peak_detach_controller_trace_backend_phase("validate-snapshots-start",
                                               request,
                                               requested_backend,
                                               stop_backend,
                                               PEAK_DETACH_STATUS_SAFE,
                                               backend_reason,
                                               1);
    if (!peak_detach_controller_validate_stopped_snapshots(snapshots,
                                                          snapshot_count,
                                                          mutation_deadline,
                                                          &status)) {
        PeakDetachStatus resume_status = PEAK_DETACH_STATUS_ERROR;
        peak_detach_controller_trace_backend_phase("validate-snapshots-failed",
                                                   request,
                                                   requested_backend,
                                                   stop_backend,
                                                   status,
                                                   backend_reason,
                                                   1);

        if (!peak_detach_controller_resume_backend(stop_backend,
                                                   peak_detach_controller_deadline_for_timeout(request->timeout_ms),
                                                   &resume_status)) {
            peak_detach_controller_mark_helper_fatal("snapshot validation abort",
                                                     resume_status);
        }
        peak_detach_controller_note_stop_window_finished();
        if (creation_gate_active) {
            peak_detach_controller_end_thread_creation_gate();
            creation_gate_active = FALSE;
        }
        if (status_out != NULL) {
            *status_out = status;
        }
        peak_detach_controller_unlock_mutation_guard();
        return FALSE;
    }
    peak_detach_controller_trace_backend_phase("validate-snapshots-complete",
                                               request,
                                               requested_backend,
                                               stop_backend,
                                               PEAK_DETACH_STATUS_SAFE,
                                               backend_reason,
                                               1);

    peak_detach_controller_trace_backend_phase("classify-start",
                                               request,
                                               requested_backend,
                                               stop_backend,
                                               PEAK_DETACH_STATUS_SAFE,
                                               backend_reason,
                                               1);
    if (!peak_detach_controller_classify_stopped_threads(request,
                                                        &gum_snapshot,
                                                        stop_backend,
                                                        snapshots,
                                                        snapshot_count,
                                                        mutation_deadline,
                                                        &held_mutation,
                                                        &status)) {
        PeakDetachStatus resume_status = PEAK_DETACH_STATUS_ERROR;
        peak_detach_controller_trace_backend_phase("classify-failed",
                                                   request,
                                                   requested_backend,
                                                   stop_backend,
                                                   status,
                                                   backend_reason,
                                                   1);

        if (!peak_detach_controller_resume_backend(stop_backend,
                                                   peak_detach_controller_deadline_for_timeout(request->timeout_ms),
                                                   &resume_status)) {
            peak_detach_controller_mark_helper_fatal("classify abort",
                                                     resume_status);
        }
        peak_detach_controller_note_stop_window_finished();
        if (creation_gate_active) {
            peak_detach_controller_end_thread_creation_gate();
            creation_gate_active = FALSE;
        }

        if (status_out != NULL) {
            *status_out = status;
        }
        peak_detach_controller_unlock_mutation_guard();
        return FALSE;
    }
    peak_detach_controller_trace_backend_phase("classify-complete",
                                               request,
                                               requested_backend,
                                               stop_backend,
                                               PEAK_DETACH_STATUS_SAFE,
                                               backend_reason,
                                               1);

    if (held_mutation.instruction_count > 0) {
        peak_detach_controller_trace_backend_phase("evacuation-start",
                                                   request,
                                                   requested_backend,
                                                   stop_backend,
                                                   PEAK_DETACH_STATUS_SAFE,
                                                   backend_reason,
                                                   held_mutation.instruction_count);
    }
    if (held_mutation.instruction_count > 0 &&
        !peak_detach_controller_evacuate_backend(stop_backend,
                                                 held_mutation.instruction_count,
                                                 held_mutation.instructions,
                                                 mutation_deadline,
                                                 &status)) {
        PeakDetachStatus resume_status = PEAK_DETACH_STATUS_ERROR;

        if (!peak_detach_controller_resume_backend(stop_backend,
                                                   peak_detach_controller_deadline_for_timeout(request->timeout_ms),
                                                   &resume_status)) {
            peak_detach_controller_mark_helper_fatal("evacuate abort",
                                                     resume_status);
        }
        peak_detach_controller_note_stop_window_finished();
        if (creation_gate_active) {
            peak_detach_controller_end_thread_creation_gate();
            creation_gate_active = FALSE;
        }
        held_mutation = (PeakDetachHeldMutation){ 0 };
        if (status_out != NULL) {
            *status_out = status;
        }
        peak_detach_controller_unlock_mutation_guard();
        return FALSE;
    }
    peak_detach_controller_trace_backend_phase("evacuation-complete",
                                               request,
                                               requested_backend,
                                               stop_backend,
                                               status,
                                               backend_reason,
                                               1);
    held_mutation.instruction_count = 0;
    held_mutation.active = TRUE;
    held_mutation.finishing = FALSE;
    held_mutation.backend = stop_backend;
    held_mutation.auto_helper_candidate =
        requested_backend == PEAK_DETACH_REQUESTED_BACKEND_AUTO &&
        stop_backend == PEAK_DETACH_HOLD_BACKEND_HELPER;
    held_mutation.timeout_ms = request->timeout_ms;
    held_mutation.deadline = mutation_deadline;
    peak_signal_policy_push_migration_disabled();

    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    peak_detach_controller_trace_backend_phase("held-mutation-ready",
                                               request,
                                               requested_backend,
                                               stop_backend,
                                               PEAK_DETACH_STATUS_SAFE,
                                               backend_reason,
                                               1);
    peak_detach_controller_unlock_mutation_guard();
    return TRUE;
#endif
}

gboolean
peak_detach_controller_strict_batch_supported(void)
{
    if (peak_detach_controller_mode() == PEAK_SAFE_DETACH_MODE_COMPATIBILITY) {
        return FALSE;
    }

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    return TRUE;
#else
    return FALSE;
#endif
}

void
peak_detach_controller_warmup_backend(void)
{
#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    return;
#else
    if (peak_detach_controller_mode() == PEAK_SAFE_DETACH_MODE_COMPATIBILITY) {
        return;
    }

    PeakDetachRequestedBackend requested_backend =
        peak_detach_controller_requested_backend();
    if (requested_backend == PEAK_DETACH_REQUESTED_BACKEND_AUTO) {
        return;
    }
    if (requested_backend == PEAK_DETACH_REQUESTED_BACKEND_SIGNAL) {
        return;
    }

    peak_detach_controller_init_atfork_once();
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    helper_warmup_active = TRUE;
    if (peak_detach_controller_ensure_helper(&status)) {
        helper_warmup_failed = FALSE;
        helper_warmup_status = PEAK_DETACH_STATUS_SAFE;
    } else {
        helper_warmup_failed = TRUE;
        helper_warmup_status =
            peak_detach_controller_status_allows_auto_signal_fallback(status)
                ? status
                : PEAK_DETACH_STATUS_UNSUPPORTED;
    }
    helper_warmup_active = FALSE;
#endif
}

gboolean
peak_detach_controller_prepare_hook_mutation_batch(
    const PeakDetachRequest* requests,
    size_t request_count,
    PeakDetachBatchResult* results,
    size_t* prepared_count_out,
    PeakDetachStatus* status_out)
{
    peak_detach_controller_clear_failure_detail();
#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    peak_detach_controller_init_atfork_once();
#endif
    if (prepared_count_out != NULL) {
        *prepared_count_out = 0;
    }

    if (requests == NULL || results == NULL || request_count == 0) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    for (size_t i = 0; i < request_count; i++) {
        results[i].prepared = FALSE;
        results[i].uses_physical_patch = FALSE;
        results[i].status = PEAK_DETACH_STATUS_ERROR;
    }

    if (peak_detach_controller_mode() == PEAK_SAFE_DETACH_MODE_COMPATIBILITY) {
        for (size_t i = 0; i < request_count; i++) {
            results[i].status = PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED;
        }
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED;
        }
        return FALSE;
    }

#ifndef PEAK_HAVE_GUM_PEAK_PC_API
    if (!warned_missing_gum_api) {
        warned_missing_gum_api = TRUE;
        g_printerr("[peak] strict safe detach requested, but selected Frida Gum does not expose PEAK PC classification APIs; refusing batched Gum target mutations\n");
    }
    for (size_t i = 0; i < request_count; i++) {
        results[i].status = PEAK_DETACH_STATUS_MISSING_GUM_API;
    }
    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_MISSING_GUM_API;
    }
    return FALSE;
#else
    size_t usable_count = request_count;
    PeakDetachBatchCandidate* candidates;
    PeakDetachHelperThreadSnapshot snapshots[PEAK_DETACH_HELPER_MAX_THREADS];
    PeakDetachHeldMutation aggregate = { 0 };
    PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
    PeakDetachPhysicalPatchRecord* physical_patch_snapshot = NULL;
    uint32_t snapshot_count = 0;
    PeakDetachRequestedBackend requested_backend =
        peak_detach_controller_requested_backend();
    PeakDetachHoldBackend stop_backend = PEAK_DETACH_HOLD_BACKEND_NONE;
    size_t valid_count = 0;
    size_t safe_count = 0;
    gboolean ambiguous_batch = FALSE;
    gboolean creation_gate_active = FALSE;
    unsigned int stop_timeout_ms = 0;
    double mutation_deadline = 0.0;
    gboolean batch_avoids_external_helper = FALSE;
    for (size_t i = 0; i < request_count; i++) {
        batch_avoids_external_helper =
            batch_avoids_external_helper || requests[i].avoid_external_helper;
    }
    gboolean auto_use_signal_backend =
        requested_backend == PEAK_DETACH_REQUESTED_BACKEND_AUTO &&
        (batch_avoids_external_helper ||
         peak_detach_controller_auto_should_use_signal_backend());
    gboolean auto_signal_fallback_pending = FALSE;
    PeakDetachStatus auto_signal_fallback_status = PEAK_DETACH_STATUS_ERROR;
    const char* auto_signal_fallback_context = NULL;
    const char* backend_reason =
        requested_backend == PEAK_DETACH_REQUESTED_BACKEND_SIGNAL
            ? "explicit-signal"
            : requested_backend == PEAK_DETACH_REQUESTED_BACKEND_HELPER
                  ? "explicit-helper"
                  : auto_use_signal_backend
                        ? (batch_avoids_external_helper
                               ? "auto-avoid-external-helper"
                               : "auto-cached-signal-fallback")
                        : "auto-helper-first";

    if (usable_count > PEAK_DETACH_CONTROLLER_MAX_BATCH_REQUESTS) {
        usable_count = PEAK_DETACH_CONTROLLER_MAX_BATCH_REQUESTS;
        for (size_t i = usable_count; i < request_count; i++) {
            results[i].status = PEAK_DETACH_STATUS_ERROR;
        }
    }

    candidates = calloc(usable_count, sizeof(*candidates));
    if (candidates == NULL) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    peak_detach_controller_lock_mutation_guard();

    if (held_mutation.active) {
        status = PEAK_DETACH_STATUS_ERROR;
        goto fail_all_locked;
    }
    peak_detach_controller_clear_stop_window_started();
    last_stop_window_us = 0.0;
    if (helper_state_fatal) {
        status = PEAK_DETACH_STATUS_ERROR;
        goto fail_all_locked;
    }

    for (size_t i = 0; i < usable_count; i++) {
        PeakDetachStatus request_status = PEAK_DETACH_STATUS_ERROR;

        if (!peak_detach_controller_validate_request(&requests[i],
                                                     &request_status)) {
            results[i].status = request_status;
            continue;
        }
        if (requests[i].operation != PEAK_DETACH_OPERATION_DETACH &&
            requests[i].operation != PEAK_DETACH_OPERATION_REATTACH) {
            results[i].status = PEAK_DETACH_STATUS_UNSUPPORTED;
            continue;
        }

        candidates[i].valid = TRUE;
        candidates[i].status = PEAK_DETACH_STATUS_SAFE;
        results[i].status = PEAK_DETACH_STATUS_SAFE;
    }

    for (size_t i = 0; i < usable_count; i++) {
        if (!candidates[i].valid) {
            continue;
        }
        for (size_t j = i + 1; j < usable_count; j++) {
            if (!candidates[j].valid) {
                continue;
            }
            if (requests[i].hook_id == requests[j].hook_id ||
                requests[i].function_address == requests[j].function_address) {
                candidates[i].valid = FALSE;
                candidates[j].valid = FALSE;
                results[i].status = PEAK_DETACH_STATUS_UNSUPPORTED;
                results[j].status = PEAK_DETACH_STATUS_UNSUPPORTED;
            }
        }
    }

    for (size_t i = 0; i < usable_count; i++) {
        if (!candidates[i].valid) {
            continue;
        }
        if (requests[i].operation == PEAK_DETACH_OPERATION_REATTACH) {
            PeakDetachPhysicalPatchRecord* record =
                peak_detach_controller_find_physical_patch_record(
                    requests[i].hook_id,
                    FALSE);
            if (record == NULL || record->patch_size == 0) {
                peak_detach_controller_note_failure_detail(
                    "batch-reattach-patch-record-missing",
                    0,
                    (uintptr_t)requests[i].function_address,
                    record != NULL ? (uintptr_t)record->patch_size : 0);
                candidates[i].valid = FALSE;
                results[i].status = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
                continue;
            }
        }
        if (!peak_detach_controller_capture_gum_snapshot(&requests[i],
                                                         &candidates[i].snapshot,
                                                         &candidates[i].status)) {
            candidates[i].valid = FALSE;
            results[i].status = candidates[i].status;
            continue;
        }
        valid_count++;
    }

    if (valid_count > 1) {
        gboolean duplicate_indices[PEAK_DETACH_CONTROLLER_MAX_BATCH_REQUESTS] = { FALSE };

        for (size_t i = 0; i < usable_count; i++) {
            if (!candidates[i].valid) {
                continue;
            }
            gpointer address_i =
                peak_detach_controller_canonical_function_address(
                    &requests[i],
                    &candidates[i].snapshot);

            for (size_t j = i + 1; j < usable_count; j++) {
                if (!candidates[j].valid) {
                    continue;
                }
                gpointer address_j =
                    peak_detach_controller_canonical_function_address(
                        &requests[j],
                        &candidates[j].snapshot);

                if (requests[i].hook_id == requests[j].hook_id ||
                    address_i == address_j) {
                    peak_detach_controller_note_failure_detail(
                        "batch-canonical-duplicate",
                        0,
                        (uintptr_t)address_i,
                        (uintptr_t)address_j);
                    duplicate_indices[i] = TRUE;
                    duplicate_indices[j] = TRUE;
                }
            }
        }

        for (size_t i = 0; i < usable_count; i++) {
            if (!duplicate_indices[i] || !candidates[i].valid) {
                continue;
            }
            candidates[i].valid = FALSE;
            results[i].status = PEAK_DETACH_STATUS_UNSUPPORTED;
            if (valid_count > 0) {
                valid_count--;
            }
        }
    }

    if (valid_count == 0) {
        status = peak_detach_controller_empty_batch_status(results,
                                                           usable_count);
        goto fail_without_stop_locked;
    }

    gboolean have_timeout = FALSE;
    for (size_t i = 0; i < usable_count; i++) {
        if (!candidates[i].valid) {
            continue;
        }
        unsigned int effective_timeout_ms =
            peak_detach_controller_effective_timeout_ms(requests[i].timeout_ms);
        if (!have_timeout || effective_timeout_ms < stop_timeout_ms) {
            stop_timeout_ms = effective_timeout_ms;
            have_timeout = TRUE;
        }
    }
    mutation_deadline =
        peak_detach_controller_deadline_for_timeout(stop_timeout_ms);
    peak_detach_controller_trace_backend_phase("batch-prepare-start",
                                               NULL,
                                               requested_backend,
                                               stop_backend,
                                               status,
                                               backend_reason,
                                               valid_count);

    if (requested_backend == PEAK_DETACH_REQUESTED_BACKEND_SIGNAL ||
        auto_use_signal_backend) {
        if (!peak_detach_controller_ensure_signal_backend(&status)) {
            peak_detach_controller_trace_backend_phase("batch-backend-failed",
                                                       NULL,
                                                       requested_backend,
                                                       PEAK_DETACH_HOLD_BACKEND_SIGNAL,
                                                       status,
                                                       backend_reason,
                                                       valid_count);
            goto fail_all_locked;
        }
        stop_backend = PEAK_DETACH_HOLD_BACKEND_SIGNAL;
        peak_detach_controller_trace_backend_phase("batch-backend-selected",
                                                   NULL,
                                                   requested_backend,
                                                   stop_backend,
                                                   PEAK_DETACH_STATUS_SAFE,
                                                   backend_reason,
                                                   valid_count);
    } else if (!peak_detach_controller_ensure_helper_until(mutation_deadline,
                                                           &status)) {
        PeakDetachStatus helper_status = status;
        if (requested_backend == PEAK_DETACH_REQUESTED_BACKEND_AUTO &&
            peak_detach_controller_status_allows_auto_signal_fallback(
                helper_status) &&
            peak_detach_controller_helper_is_closed() &&
            peak_detach_controller_ensure_signal_backend(&status)) {
            stop_backend = PEAK_DETACH_HOLD_BACKEND_SIGNAL;
            auto_signal_fallback_pending = TRUE;
            auto_signal_fallback_status = helper_status;
            auto_signal_fallback_context = "batch startup failure";
            peak_detach_controller_trace_backend_phase("batch-backend-selected",
                                                       NULL,
                                                       requested_backend,
                                                       stop_backend,
                                                       PEAK_DETACH_STATUS_SAFE,
                                                       "auto-helper-fallback",
                                                       valid_count);
        } else {
            peak_detach_controller_trace_backend_phase("batch-backend-failed",
                                                       NULL,
                                                       requested_backend,
                                                       PEAK_DETACH_HOLD_BACKEND_HELPER,
                                                       status,
                                                       backend_reason,
                                                       valid_count);
            goto fail_all_locked;
        }
    } else {
        peak_detach_controller_trace_backend_phase("batch-backend-selected",
                                                   NULL,
                                                   requested_backend,
                                                   PEAK_DETACH_HOLD_BACKEND_HELPER,
                                                   PEAK_DETACH_STATUS_SAFE,
                                                   backend_reason,
                                                   valid_count);
    }

    physical_patch_snapshot = malloc(sizeof(physical_patch_records));
    if (physical_patch_snapshot == NULL) {
        status = PEAK_DETACH_STATUS_ERROR;
        goto fail_all_locked;
    }
    memcpy(physical_patch_snapshot,
           physical_patch_records,
           sizeof(physical_patch_records));

    peak_detach_controller_begin_thread_creation_gate();
    creation_gate_active = TRUE;
    peak_detach_controller_test_delay_after_gate_begin();

    peak_detach_controller_trace_backend_phase(
        stop_backend == PEAK_DETACH_HOLD_BACKEND_SIGNAL
            ? "batch-stop-signal-start"
            : "batch-stop-helper-start",
        NULL,
        requested_backend,
        stop_backend,
        PEAK_DETACH_STATUS_SAFE,
        backend_reason,
        valid_count);
    peak_detach_controller_note_stop_window_started();
    if (stop_backend == PEAK_DETACH_HOLD_BACKEND_SIGNAL) {
        if (!peak_detach_controller_signal_stop_threads(snapshots,
                                                        &snapshot_count,
                                                        mutation_deadline,
                                                        &status)) {
            peak_detach_controller_clear_stop_window_started();
            goto fail_all_locked;
        }
    } else if (!peak_detach_controller_stop_threads(snapshots,
                                                    &snapshot_count,
                                                    mutation_deadline,
                                                    &status)) {
        if (requested_backend == PEAK_DETACH_REQUESTED_BACKEND_AUTO &&
            peak_detach_controller_status_allows_auto_signal_fallback(status)) {
            PeakDetachStatus helper_status = status;
            gboolean helper_closed;
            int remaining_ms = 0;
            peak_detach_controller_trace_backend_phase("batch-stop-helper-fallback-start",
                                                       NULL,
                                                       requested_backend,
                                                       PEAK_DETACH_HOLD_BACKEND_HELPER,
                                                       status,
                                                       "auto-helper-stop-fallback",
                                                       valid_count);
            helper_closed = peak_detach_controller_close_helper();
            if (helper_closed) {
                remaining_ms =
                    peak_detach_controller_deadline_remaining_ms(
                        mutation_deadline);
            }
            if (helper_closed &&
                remaining_ms > 0 &&
                peak_detach_controller_ensure_signal_backend(&status) &&
                peak_detach_controller_signal_stop_threads(snapshots,
                                                           &snapshot_count,
                                                           mutation_deadline,
                                                           &status)) {
                stop_backend = PEAK_DETACH_HOLD_BACKEND_SIGNAL;
                auto_signal_fallback_pending = TRUE;
                auto_signal_fallback_status = helper_status;
                auto_signal_fallback_context = "batch stop failure";
                peak_detach_controller_trace_backend_phase("batch-stop-helper-fallback-selected",
                                                           NULL,
                                                           requested_backend,
                                                           stop_backend,
                                                           PEAK_DETACH_STATUS_SAFE,
                                                           "auto-helper-stop-fallback",
                                                           valid_count);
            } else {
                if (!helper_closed || remaining_ms <= 0) {
                    status = helper_status;
                }
                peak_detach_controller_trace_backend_phase("batch-stop-helper-fallback-failed",
                                                           NULL,
                                                           requested_backend,
                                                           PEAK_DETACH_HOLD_BACKEND_SIGNAL,
                                                           status,
                                                           "auto-helper-stop-fallback",
                                                           valid_count);
                peak_detach_controller_clear_stop_window_started();
                goto fail_all_locked;
            }
        } else {
            peak_detach_controller_clear_stop_window_started();
            goto fail_all_locked;
        }
    } else {
        stop_backend = PEAK_DETACH_HOLD_BACKEND_HELPER;
        peak_detach_controller_trace_backend_phase("batch-stop-helper-complete",
                                                   NULL,
                                                   requested_backend,
                                                   stop_backend,
                                                   PEAK_DETACH_STATUS_SAFE,
                                                   backend_reason,
                                                   valid_count);
    }

    if (auto_signal_fallback_pending &&
        stop_backend == PEAK_DETACH_HOLD_BACKEND_SIGNAL) {
        peak_detach_controller_cache_auto_signal_fallback(
            auto_signal_fallback_status,
            auto_signal_fallback_context);
    }

    peak_detach_controller_trace_backend_phase("batch-validate-snapshots-start",
                                               NULL,
                                               requested_backend,
                                               stop_backend,
                                               PEAK_DETACH_STATUS_SAFE,
                                               backend_reason,
                                               valid_count);
    if (!peak_detach_controller_validate_stopped_snapshots(snapshots,
                                                          snapshot_count,
                                                          mutation_deadline,
                                                          &status)) {
        PeakDetachStatus resume_status = PEAK_DETACH_STATUS_ERROR;
        peak_detach_controller_trace_backend_phase("batch-validate-snapshots-failed",
                                                   NULL,
                                                   requested_backend,
                                                   stop_backend,
                                                   status,
                                                   backend_reason,
                                                   valid_count);

        if (!peak_detach_controller_resume_backend(stop_backend,
                                                   peak_detach_controller_deadline_for_timeout(stop_timeout_ms),
                                                   &resume_status)) {
            peak_detach_controller_mark_helper_fatal("batch snapshot validation abort",
                                                     resume_status);
        }
        peak_detach_controller_note_stop_window_finished();
        if (creation_gate_active) {
            peak_detach_controller_end_thread_creation_gate();
            creation_gate_active = FALSE;
        }
        memcpy(physical_patch_records,
               physical_patch_snapshot,
               sizeof(physical_patch_records));
        goto fail_unlocked_after_resume;
    }
    peak_detach_controller_trace_backend_phase("batch-validate-snapshots-complete",
                                               NULL,
                                               requested_backend,
                                               stop_backend,
                                               PEAK_DETACH_STATUS_SAFE,
                                               backend_reason,
                                               valid_count);

    for (size_t i = 0; i < usable_count; i++) {
        PeakDetachStatus candidate_status = PEAK_DETACH_STATUS_ERROR;

        if ((i & 15u) == 0 &&
            peak_detach_controller_deadline_remaining_ms(mutation_deadline) <= 0) {
            status = PEAK_DETACH_STATUS_TIMEOUT;
            ambiguous_batch = TRUE;
            break;
        }
        if (!candidates[i].valid) {
            continue;
        }

        peak_detach_controller_trace_backend_phase("batch-classify-candidate-start",
                                                   &requests[i],
                                                   requested_backend,
                                                   stop_backend,
                                                   PEAK_DETACH_STATUS_SAFE,
                                                   backend_reason,
                                                   1);
        if (!peak_detach_controller_classify_stopped_threads(
                &requests[i],
                &candidates[i].snapshot,
                stop_backend,
                snapshots,
                snapshot_count,
                mutation_deadline,
                &candidates[i].mutation,
                &candidate_status)) {
            candidates[i].status = candidate_status;
            results[i].status = candidate_status;
            peak_detach_controller_trace_backend_phase("batch-classify-candidate-failed",
                                                       &requests[i],
                                                       requested_backend,
                                                       stop_backend,
                                                       candidate_status,
                                                       backend_reason,
                                                       1);
            continue;
        }
        peak_detach_controller_trace_backend_phase("batch-classify-candidate-complete",
                                                   &requests[i],
                                                   requested_backend,
                                                   stop_backend,
                                                   PEAK_DETACH_STATUS_SAFE,
                                                   backend_reason,
                                                   1);

        if (!peak_detach_controller_append_candidate_mutation(
                &aggregate,
                &candidates[i].mutation,
                &candidate_status)) {
            ambiguous_batch = TRUE;
            status = candidate_status;
            break;
        }

        candidates[i].safe = TRUE;
        candidates[i].status = PEAK_DETACH_STATUS_SAFE;
        results[i].status = PEAK_DETACH_STATUS_SAFE;
        safe_count++;
    }
    peak_detach_controller_trace_backend_phase("batch-classify-loop-complete",
                                               NULL,
                                               requested_backend,
                                               stop_backend,
                                               status,
                                               backend_reason,
                                               valid_count);

    if (ambiguous_batch) {
        PeakDetachStatus resume_status = PEAK_DETACH_STATUS_ERROR;

        if (!peak_detach_controller_resume_backend(stop_backend,
                                                   peak_detach_controller_deadline_for_timeout(stop_timeout_ms),
                                                   &resume_status)) {
            peak_detach_controller_mark_helper_fatal("batch classify abort",
                                                     resume_status);
        }
        peak_detach_controller_note_stop_window_finished();
        if (creation_gate_active) {
            peak_detach_controller_end_thread_creation_gate();
            creation_gate_active = FALSE;
        }
        memcpy(physical_patch_records,
               physical_patch_snapshot,
               sizeof(physical_patch_records));
        for (size_t i = 0; i < usable_count; i++) {
            if (candidates[i].valid) {
                results[i].prepared = FALSE;
                results[i].uses_physical_patch = FALSE;
                results[i].status = status;
            }
        }
        goto fail_unlocked_after_resume;
    }

    if (safe_count == 0) {
        PeakDetachStatus resume_status = PEAK_DETACH_STATUS_ERROR;

        if (!peak_detach_controller_resume_backend(stop_backend,
                                                   peak_detach_controller_deadline_for_timeout(stop_timeout_ms),
                                                   &resume_status)) {
            peak_detach_controller_mark_helper_fatal("batch classify retry",
                                                     resume_status);
        }
        peak_detach_controller_note_stop_window_finished();
        if (creation_gate_active) {
            peak_detach_controller_end_thread_creation_gate();
            creation_gate_active = FALSE;
        }
        memcpy(physical_patch_records,
               physical_patch_snapshot,
               sizeof(physical_patch_records));
        status = PEAK_DETACH_STATUS_CLASSIFY_FAILED;
        goto fail_unlocked_after_resume;
    }

    if (aggregate.instruction_count > 0) {
        peak_detach_controller_trace_backend_phase("batch-evacuation-start",
                                                   NULL,
                                                   requested_backend,
                                                   stop_backend,
                                                   PEAK_DETACH_STATUS_SAFE,
                                                   backend_reason,
                                                   aggregate.instruction_count);
    }
    if (aggregate.instruction_count > 0 &&
        !peak_detach_controller_evacuate_backend(stop_backend,
                                                 aggregate.instruction_count,
                                                 aggregate.instructions,
                                                 mutation_deadline,
                                                 &status)) {
        PeakDetachStatus resume_status = PEAK_DETACH_STATUS_ERROR;

        if (!peak_detach_controller_resume_backend(stop_backend,
                                                   peak_detach_controller_deadline_for_timeout(stop_timeout_ms),
                                                   &resume_status)) {
            peak_detach_controller_mark_helper_fatal("batch evacuate abort",
                                                     resume_status);
        }
        peak_detach_controller_note_stop_window_finished();
        if (creation_gate_active) {
            peak_detach_controller_end_thread_creation_gate();
            creation_gate_active = FALSE;
        }
        held_mutation = (PeakDetachHeldMutation){ 0 };
        for (size_t i = 0; i < usable_count; i++) {
            if (candidates[i].safe) {
                results[i].status = status;
            }
        }
        memcpy(physical_patch_records,
               physical_patch_snapshot,
               sizeof(physical_patch_records));
        goto fail_without_resume_locked;
    }
    peak_detach_controller_trace_backend_phase("batch-evacuation-complete",
                                               NULL,
                                               requested_backend,
                                               stop_backend,
                                               status,
                                               backend_reason,
                                               valid_count);

    aggregate.instruction_count = 0;
    aggregate.active = TRUE;
    aggregate.finishing = FALSE;
    aggregate.batch = TRUE;
    aggregate.owner_thread_set = TRUE;
    aggregate.owner_thread = pthread_self();
    aggregate.operation = PEAK_DETACH_OPERATION_DETACH;
    aggregate.hook_id = (size_t)-1;
    aggregate.function_address = NULL;
    aggregate.listener = NULL;
    aggregate.backend = stop_backend;
    aggregate.auto_helper_candidate =
        requested_backend == PEAK_DETACH_REQUESTED_BACKEND_AUTO &&
        stop_backend == PEAK_DETACH_HOLD_BACKEND_HELPER;
    aggregate.timeout_ms = stop_timeout_ms;
    aggregate.deadline = mutation_deadline;
    aggregate.deferred_cleanup_1 = physical_patch_snapshot;
    aggregate.deferred_cleanup_2 = candidates;
    held_mutation = aggregate;
    peak_signal_policy_push_migration_disabled();

    for (size_t i = 0; i < usable_count; i++) {
        if (candidates[i].safe) {
            results[i].prepared = TRUE;
            results[i].uses_physical_patch =
                candidates[i].mutation.uses_physical_patch;
        }
    }

    if (prepared_count_out != NULL) {
        *prepared_count_out = safe_count;
    }
    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    peak_detach_controller_trace_backend_phase("batch-held-mutation-ready",
                                               NULL,
                                               requested_backend,
                                               stop_backend,
                                               PEAK_DETACH_STATUS_SAFE,
                                               backend_reason,
                                               safe_count);
    physical_patch_snapshot = NULL;
    candidates = NULL;
    peak_detach_controller_trace_backend_phase("batch-prepare-before-unlock",
                                               NULL,
                                               requested_backend,
                                               stop_backend,
                                               PEAK_DETACH_STATUS_SAFE,
                                               backend_reason,
                                               safe_count);
    peak_detach_controller_unlock_mutation_guard();
    peak_detach_controller_trace_backend_phase("batch-prepare-returning",
                                               NULL,
                                               requested_backend,
                                               stop_backend,
                                               PEAK_DETACH_STATUS_SAFE,
                                               backend_reason,
                                               safe_count);
    return TRUE;

fail_all_locked:
    for (size_t i = 0; i < usable_count; i++) {
        if (candidates[i].valid || results[i].status == PEAK_DETACH_STATUS_SAFE) {
            results[i].status = status;
        }
    }
fail_without_stop_locked:
    if (creation_gate_active) {
        peak_detach_controller_end_thread_creation_gate();
        creation_gate_active = FALSE;
    }
    if (status_out != NULL) {
        *status_out = status;
    }
    peak_detach_controller_unlock_mutation_guard();
    free(physical_patch_snapshot);
    free(candidates);
    return FALSE;

fail_without_resume_locked:
    if (creation_gate_active) {
        peak_detach_controller_end_thread_creation_gate();
        creation_gate_active = FALSE;
    }
    if (status_out != NULL) {
        *status_out = status;
    }
    peak_detach_controller_unlock_mutation_guard();
    free(physical_patch_snapshot);
    free(candidates);
    return FALSE;

fail_unlocked_after_resume:
    if (creation_gate_active) {
        peak_detach_controller_end_thread_creation_gate();
        creation_gate_active = FALSE;
    }
    if (status_out != NULL) {
        *status_out = status;
    }
    peak_detach_controller_unlock_mutation_guard();
    free(physical_patch_snapshot);
    free(candidates);
    return FALSE;
#endif
}

void
peak_detach_controller_wait_for_mutation_window(void)
{
#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    pid_t tid = (pid_t)syscall(SYS_gettid);
    int published_epoch = 0;
    double wait_started_at = peak_detach_controller_monotonic_second();

    peak_detach_controller_init_strict_gate_wait_timeout();
    for (;;) {
        int gate_epoch = atomic_load_explicit(&strict_mutation_thread_gate,
                                              memory_order_acquire);
        if (gate_epoch == 0) {
            break;
        }
        if (published_epoch != gate_epoch) {
            if (published_epoch != 0) {
                peak_detach_controller_clear_gate_waiter(tid, published_epoch);
            }
            peak_detach_controller_publish_gate_waiter(tid, gate_epoch);
            published_epoch = gate_epoch;
        }
        if (strict_gate_wait_timeout_s > 0.0 &&
            peak_detach_controller_monotonic_second() - wait_started_at >=
                strict_gate_wait_timeout_s) {
            if (!warned_strict_gate_wait_timeout) {
                warned_strict_gate_wait_timeout = TRUE;
                g_printerr("[peak] Strict detach thread-creation gate waited %.3fs; allowing new thread to start to avoid process hang\n",
                           strict_gate_wait_timeout_s);
            }
            break;
        }
        usleep(100);
    }

    if (published_epoch != 0) {
        peak_detach_controller_clear_gate_waiter(tid, published_epoch);
    }
#endif
}

gboolean
peak_detach_controller_threads_are_held(void)
{
    gboolean active = FALSE;

    if (peak_detach_controller_mode() == PEAK_SAFE_DETACH_MODE_COMPATIBILITY) {
        return FALSE;
    }

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    peak_detach_controller_init_atfork_once();
    peak_detach_controller_lock_mutation_guard();
    active = held_mutation.active;
    peak_detach_controller_unlock_mutation_guard();
#endif

    return active;
}

gboolean
peak_detach_controller_current_mutation_uses_physical_patch(void)
{
    gboolean uses_physical_patch = FALSE;

    if (peak_detach_controller_mode() == PEAK_SAFE_DETACH_MODE_COMPATIBILITY) {
        return FALSE;
    }

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    peak_detach_controller_init_atfork_once();
    peak_detach_controller_lock_mutation_guard();
    uses_physical_patch = held_mutation.active &&
                          held_mutation.uses_physical_patch;
    peak_detach_controller_unlock_mutation_guard();
#endif

    return uses_physical_patch;
}

double
peak_detach_controller_last_stop_window_us(void)
{
    double value = 0.0;

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    peak_detach_controller_init_atfork_once();
    peak_detach_controller_lock_mutation_guard();
    value = last_stop_window_us;
    peak_detach_controller_unlock_mutation_guard();
#endif

    return value;
}

gboolean
peak_detach_controller_finish_hook_mutation(const PeakDetachRequest* request,
                                            PeakDetachStatus* status_out)
{
    if (request != NULL &&
        !peak_detach_controller_operation_is_valid(request->operation)) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    if (peak_detach_controller_mode() == PEAK_SAFE_DETACH_MODE_COMPATIBILITY) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED;
        }
        return TRUE;
    }

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    void* deferred_cleanup_1 = NULL;
    void* deferred_cleanup_2 = NULL;
    gboolean mutation_active = FALSE;
    gboolean wait_for_owner = FALSE;
    gboolean request_mismatch = FALSE;
    gboolean owner_mismatch = FALSE;
    gboolean auto_helper_candidate = FALSE;
    PeakDetachHoldBackend backend = PEAK_DETACH_HOLD_BACKEND_NONE;
    unsigned int timeout_ms = 0;

    peak_detach_controller_init_atfork_once();
    peak_detach_controller_lock_mutation_guard();
    mutation_active = held_mutation.active;
    if (mutation_active) {
        backend = held_mutation.backend;
        timeout_ms = held_mutation.timeout_ms;
        auto_helper_candidate = held_mutation.auto_helper_candidate;
        if (!peak_detach_controller_request_matches_held_mutation(
                request,
                &held_mutation)) {
            request_mismatch = TRUE;
        } else if (held_mutation.finishing) {
            wait_for_owner = TRUE;
        } else if (!peak_detach_controller_current_thread_owns_held_mutation(
                       &held_mutation)) {
            owner_mismatch = TRUE;
        } else {
            held_mutation.finishing = TRUE;
        }
    }
    peak_detach_controller_unlock_mutation_guard();

    peak_detach_controller_trace_backend_phase(
        "finish-enter",
        request,
        PEAK_DETACH_REQUESTED_BACKEND_AUTO,
        mutation_active ? backend : PEAK_DETACH_HOLD_BACKEND_NONE,
        PEAK_DETACH_STATUS_SAFE,
        request_mismatch ? "held-request-mismatch" :
            owner_mismatch ? "held-owner-mismatch" :
            wait_for_owner ? "held-finishing" :
            (mutation_active ? "held-active" : "no-held-mutation"),
        mutation_active ? timeout_ms : 0);

    if (request_mismatch || owner_mismatch) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        return FALSE;
    }

    if (wait_for_owner) {
        double wait_deadline =
            peak_detach_controller_deadline_for_timeout(timeout_ms);
        for (;;) {
            gboolean still_active;

            peak_detach_controller_lock_mutation_guard();
            still_active = held_mutation.active;
            peak_detach_controller_unlock_mutation_guard();

            if (!still_active) {
                mutation_active = FALSE;
                backend = PEAK_DETACH_HOLD_BACKEND_NONE;
                timeout_ms = 0;
                break;
            }
            if (peak_detach_controller_monotonic_second() >= wait_deadline) {
                if (status_out != NULL) {
                    *status_out = PEAK_DETACH_STATUS_TIMEOUT;
                }
                return FALSE;
            }
            usleep(100);
        }
    }

    if (mutation_active) {
        PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;

        peak_detach_controller_trace_backend_phase(
            "finish-resume-start",
            request,
            PEAK_DETACH_REQUESTED_BACKEND_AUTO,
            backend,
            PEAK_DETACH_STATUS_SAFE,
            "held-release",
            timeout_ms);
        gboolean released =
            peak_detach_controller_resume_backend(backend,
                                                  peak_detach_controller_deadline_for_timeout(timeout_ms),
                                                  &status);
        peak_detach_controller_trace_backend_phase(
            released ? "finish-resume-complete" : "finish-resume-failed",
            request,
            PEAK_DETACH_REQUESTED_BACKEND_AUTO,
            backend,
            status,
            "held-release",
            timeout_ms);

        if (!released) {
            peak_detach_controller_lock_mutation_guard();
            peak_detach_controller_clear_stop_window_started();
            held_mutation.finishing = FALSE;
            peak_detach_controller_unlock_mutation_guard();
            if (!warned_helper_resume_failed) {
                warned_helper_resume_failed = TRUE;
                g_printerr("[peak] detach helper failed to resume stopped threads after Gum mutation: %s\n",
                           peak_detach_controller_status_string(status));
            }
            peak_detach_controller_mark_helper_fatal("finish", status);
            if (status_out != NULL) {
                *status_out = status;
            }
            return FALSE;
        }

        peak_detach_controller_lock_mutation_guard();
        peak_detach_controller_note_stop_window_finished();
        if (auto_helper_candidate &&
            backend == PEAK_DETACH_HOLD_BACKEND_HELPER) {
            peak_detach_controller_cache_auto_signal_performance_fallback(
                last_stop_window_us,
                held_mutation.batch ? "batch" : "single");
        }
        peak_detach_controller_unlock_mutation_guard();
        peak_detach_controller_end_thread_creation_gate();
        peak_detach_controller_trace_backend_phase(
            "finish-cleanup-lock-start",
            request,
            PEAK_DETACH_REQUESTED_BACKEND_AUTO,
            backend,
            PEAK_DETACH_STATUS_SAFE,
            "held-release",
            timeout_ms);
        peak_detach_controller_lock_mutation_guard();
        peak_detach_controller_trace_backend_phase(
            "finish-cleanup-lock-acquired",
            request,
            PEAK_DETACH_REQUESTED_BACKEND_AUTO,
            backend,
            PEAK_DETACH_STATUS_SAFE,
            "held-release",
            timeout_ms);
        deferred_cleanup_1 = held_mutation.deferred_cleanup_1;
        deferred_cleanup_2 = held_mutation.deferred_cleanup_2;
        held_mutation.active = FALSE;
        held_mutation.batch = FALSE;
        held_mutation.auto_helper_candidate = FALSE;
        held_mutation.owner_thread_set = FALSE;
        held_mutation.uses_physical_patch = FALSE;
        held_mutation.mutates_entry_bytes = FALSE;
        held_mutation.mutates_gum_metadata = FALSE;
        held_mutation.frees_listener_state = FALSE;
        held_mutation.requires_target_entry_idle = FALSE;
        held_mutation.operation = PEAK_DETACH_OPERATION_ATTACH;
        held_mutation.hook_id = 0;
        held_mutation.function_address = NULL;
        held_mutation.listener = NULL;
        held_mutation.backend = PEAK_DETACH_HOLD_BACKEND_NONE;
        held_mutation.instruction_count = 0;
        held_mutation.deferred_cleanup_1 = NULL;
        held_mutation.deferred_cleanup_2 = NULL;
        held_mutation.finishing = FALSE;
        peak_signal_policy_pop_migration_disabled();
        peak_detach_controller_unlock_mutation_guard();
    }

    free(deferred_cleanup_1);
    free(deferred_cleanup_2);
    peak_detach_controller_trace_backend_phase(
        "finish-complete",
        request,
        PEAK_DETACH_REQUESTED_BACKEND_AUTO,
        PEAK_DETACH_HOLD_BACKEND_NONE,
        PEAK_DETACH_STATUS_SAFE,
        "held-release",
        0);
#endif
    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

gboolean
peak_detach_controller_finish_hook_mutation_batch(PeakDetachStatus* status_out)
{
    return peak_detach_controller_finish_hook_mutation(NULL, status_out);
}

gboolean
peak_detach_controller_shutdown_helper(PeakDetachStatus* status_out)
{
    if (peak_detach_controller_mode() == PEAK_SAFE_DETACH_MODE_COMPATIBILITY) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_COMPATIBILITY_ALLOWED;
        }
        return TRUE;
    }

#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    peak_detach_controller_init_atfork_once();
    peak_detach_controller_lock_mutation_guard();

    if (held_mutation.active) {
        if (status_out != NULL) {
            *status_out = PEAK_DETACH_STATUS_ERROR;
        }
        peak_detach_controller_unlock_mutation_guard();
        return FALSE;
    }

    if (helper_fd >= 0) {
        PeakDetachStatus status = PEAK_DETACH_STATUS_ERROR;
        gboolean ok = peak_detach_controller_send_shutdown(TRUE, &status);
        if (!ok) {
            if (!warned_helper_unavailable) {
                warned_helper_unavailable = TRUE;
                g_printerr("[peak] detach helper was unavailable during idle shutdown: %s\n",
                           peak_detach_controller_status_string(status));
            }
            if (status_out != NULL) {
                *status_out = status;
            }
            peak_detach_controller_unlock_mutation_guard();
            return FALSE;
        }
    }

    peak_detach_controller_unlock_mutation_guard();
#endif

    if (status_out != NULL) {
        *status_out = PEAK_DETACH_STATUS_SAFE;
    }
    return TRUE;
}

void
peak_detach_controller_abort_after_failed_finish(const char* context,
                                                 PeakDetachStatus status)
{
#ifdef PEAK_HAVE_GUM_PEAK_PC_API
    peak_detach_controller_mark_helper_fatal(context, status);
#else
    const char message[] =
        "[peak] fatal safe-detach finish failure; terminating to avoid unpublished Gum hook state\n";

    (void)context;
    (void)status;
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(128);
#endif
}

#ifndef PEAK_HAVE_GUM_PEAK_PC_API
void
peak_detach_controller_note_thread_creation_gate_installed(gboolean installed)
{
    (void)installed;
}

#ifdef PEAK_ENABLE_TEST_HOOKS
int
peak_detach_controller_test_thread_creation_gate_epoch(void)
{
    return 0;
}

size_t
peak_detach_controller_test_gate_waiter_count(void)
{
    return 0;
}

int
peak_detach_controller_test_signal_backend_signum(void)
{
    return 0;
}
#endif
#endif
