/* Exercise the clock with deterministic wall times and no MPI run. */
#define clock_gettime fake_clock_gettime
#define clock_getres fake_clock_getres
#define PMPI_Query_thread fake_pmpi_query_thread
#define PMPI_Comm_size fake_pmpi_comm_size
#define PMPI_Comm_rank fake_pmpi_comm_rank
#include "../../src/mpi_active_clock.c"
#undef PMPI_Comm_rank
#undef PMPI_Comm_size
#undef PMPI_Query_thread
#undef clock_getres
#undef clock_gettime

#include <assert.h>
#include <limits.h>
#include "internal/mpi_active_clock_sampling.h"

static unsigned qualification_failures;

void
peak_log_message(PeakVerbosity verbosity, const char* format, ...)
{
    assert(verbosity == PEAK_VERBOSITY_DEBUG);
    assert(strstr(format, "reason=clock-qualification"));
    ++qualification_failures;
}

enum { TICK_US = 1000, QUALIFICATION_BATCH_SIZE = 128 };
static const double epoch = 1.0;
static int wall_us;
static unsigned coarse_reads;
static unsigned fine_reads;
static unsigned resolution_checks;
static unsigned mpi_thread_checks;
static unsigned slow_coarse_reads;
static bool coarse_available;
static bool fine_failure;
static unsigned delayed_reads;
static uint64_t opened;

/* Stand in for the listener while exercising the real MPI wrappers offline. */
_Atomic uint64_t peak_mpi_epoch_armed;
static unsigned epoch_samples;
static unsigned expected_epoch_kind;

void
peak_general_listener_mpi_epoch_completed(uint64_t nomination, unsigned attempt,
                                          unsigned kind, double now)
{
    assert(nomination == 1 && attempt == ++epoch_samples);
    assert(kind == expected_epoch_kind);
    assert(fabs(now - (epoch + wall_us * 1e-6)) < 1e-9);
}

static int barrier_result = MPI_SUCCESS;

static int
fake_barrier(MPI_Comm comm)
{
    (void)comm;
    return barrier_result;
}

static int
fake_allreduce(const void* send_buffer, void* recv_buffer, int count,
               MPI_Datatype type, MPI_Op op, MPI_Comm comm)
{
    (void)send_buffer;
    (void)recv_buffer;
    (void)count;
    (void)type;
    (void)op;
    (void)comm;
    /* Simulate an MPI implementation or tool that makes a nested call. */
    return PMPI_Barrier(MPI_COMM_WORLD);
}

static void
test_epoch_boundaries(void)
{
    atomic_store(&residence_state, 0);
    atomic_store(&next_mpi_Barrier, fake_barrier);
    atomic_store(&next_pmpi_Barrier, fake_barrier);
    atomic_store(&next_mpi_Allreduce, fake_allreduce);

    assert(MPI_Barrier(MPI_COMM_WORLD) == MPI_SUCCESS && epoch_samples == 0);
    atomic_store(&peak_mpi_epoch_armed, 1);
    assert(MPI_Barrier(MPI_COMM_SELF) == MPI_SUCCESS);
    assert(MPI_Allreduce(NULL, NULL, 0, MPI_INT, MPI_SUM, MPI_COMM_WORLD) == MPI_SUCCESS);
    assert(epoch_samples == 0);

    expected_epoch_kind = PEAK_MPI_EPOCH_ALLREDUCE;
    int send = 1, receive;
    assert(MPI_Allreduce(&send, &receive, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD) == MPI_SUCCESS);
    assert(epoch_samples == 1);
    assert(MPI_Barrier(MPI_COMM_WORLD) == MPI_SUCCESS && epoch_samples == 1);

    expected_epoch_kind = PEAK_MPI_EPOCH_FAILED;
    barrier_result = MPI_ERR_OTHER;
    assert(MPI_Allreduce(&send, &receive, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD) == MPI_ERR_OTHER);
    assert(epoch_samples == 2);
    barrier_result = MPI_SUCCESS;
    expected_epoch_kind = PEAK_MPI_EPOCH_ALLREDUCE;
    for (unsigned i = 0; i < PEAK_MPI_EPOCH_MAX_ATTEMPTS; ++i) {
        wall_us += TICK_US;
        assert(MPI_Allreduce(&send, &receive, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD) == MPI_SUCCESS);
    }
    assert(epoch_samples == PEAK_MPI_EPOCH_MAX_ATTEMPTS);
    atomic_store(&peak_mpi_epoch_armed, 0);
}

int
fake_clock_gettime(clockid_t clock_id, struct timespec* time)
{
    *time = (struct timespec){0};
    if (clock_id == CLOCK_MONOTONIC)
        ++fine_reads;
    if (clock_id == CLOCK_MONOTONIC && fine_failure)
        return -1;
    if (clock_id == CLOCK_MONOTONIC && delayed_reads) {
        --delayed_reads;
        wall_us += 1000;
    }
    int measured_us = wall_us;
    if (clock_id == CLOCK_MONOTONIC_COARSE) {
        if (!coarse_available)
            return -1;
        ++coarse_reads;
        if (slow_coarse_reads) {
            --slow_coarse_reads;
            ++wall_us;
        }
        measured_us -= measured_us % TICK_US;
    } else {
        assert(clock_id == CLOCK_MONOTONIC);
    }
    time->tv_sec = 1 + measured_us / 1000000;
    time->tv_nsec = (measured_us % 1000000) * 1000L;
    return 0;
}

int
fake_clock_getres(clockid_t clock_id, struct timespec* time)
{
    assert(clock_id == CLOCK_MONOTONIC_COARSE);
    ++resolution_checks;
    time->tv_sec = 0;
    time->tv_nsec = TICK_US * 1000L;
    return 0;
}

int
fake_pmpi_query_thread(int* provided)
{
    ++mpi_thread_checks;
    *provided = MPI_THREAD_FUNNELED;
    return MPI_SUCCESS;
}

int
fake_pmpi_comm_size(MPI_Comm comm, int* size)
{
    assert(comm == MPI_COMM_WORLD);
    *size = 2;
    return MPI_SUCCESS;
}

int
fake_pmpi_comm_rank(MPI_Comm comm, int* rank)
{
    assert(comm == MPI_COMM_WORLD);
    *rank = 0;
    return MPI_SUCCESS;
}

static void
boundary_at(int at_us)
{
    assert(at_us >= wall_us);
    wall_us = at_us;
    if (opened) {
        close_interval(opened);
        opened = 0;
    } else {
        opened = open_interval();
        assert(opened != UINT64_MAX);
    }
}

static double
observe_at(int at_us, double previous)
{
    assert(at_us >= wall_us);
    wall_us = at_us;
    double now = epoch + wall_us * 1e-6;
    double blocked = peak_mpi_active_clock_blocked_seconds(epoch, &now);
    assert(blocked >= previous - 1e-9);
    assert(blocked <= now - epoch + 1e-9);
    return blocked;
}

static void
expect_added(double previous, double blocked, int expected_us, int tolerance_us)
{
    assert(fabs((blocked - previous) - expected_us * 1e-6) <=
           tolerance_us * 1e-6 + 1e-9);
}

typedef enum {
    PHASE_INSIDE,
    PHASE_CROSS,
    PHASE_ALTERNATING,
    PHASE_WITH_LONG_WAIT,
    PHASE_ALL_COMPUTE,
    PHASE_ALL_MPI,
} PhasePattern;

enum { PHASE_SLEEP_US = 80 * TICK_US, PHASE_WINDOWS = 128 };

typedef struct {
    PhasePattern pattern;
    int start_us;
    int entered_us;
    int completed_us;
    unsigned calls;
    int true_blocked_us;
} PhaseTimeline;

static void
next_phase_interval(PhaseTimeline* timeline)
{
    unsigned call = timeline->calls++;
    int step = (int)call * TICK_US;
    int offset = 200;
    int duration = 400;
    switch (timeline->pattern) {
    case PHASE_CROSS:
        offset = 800;
        break;
    case PHASE_ALTERNATING:
        offset = (step / PHASE_SLEEP_US) % 2 == 0 ? 800 : 300;
        break;
    case PHASE_WITH_LONG_WAIT:
        step += (int)(call / 64) * 9000;
        if (call % 64 == 63)
            duration += 9000;
        break;
    case PHASE_ALL_COMPUTE:
        timeline->entered_us = INT_MAX;
        timeline->completed_us = INT_MAX;
        return;
    case PHASE_ALL_MPI:
        offset = 0;
        duration = INT_MAX - timeline->start_us;
        break;
    case PHASE_INSIDE:
        break;
    }
    timeline->entered_us = timeline->start_us + step + offset;
    timeline->completed_us = timeline->entered_us + duration;
}

static int
advance_phase_timeline(PhaseTimeline* timeline, int at_us)
{
    while (timeline->entered_us <= at_us) {
        if (!opened)
            boundary_at(timeline->entered_us);
        if (timeline->completed_us > at_us)
            break;
        boundary_at(timeline->completed_us);
        timeline->true_blocked_us += timeline->completed_us - timeline->entered_us;
        next_phase_interval(timeline);
    }
    assert(wall_us <= at_us);
    wall_us = at_us;
    return timeline->true_blocked_us + (opened ? at_us - timeline->entered_us : 0);
}

static void
check_peek(void)
{
    unsigned coarse = coarse_reads;
    unsigned fine = fine_reads;
    assert(peak_mpi_active_clock_peek(epoch) == (opened != 0));
    assert(peak_mpi_active_clock_peek(epoch + 1.0) < 0);
    assert(coarse_reads == coarse && fine_reads == fine);
}

static void
test_phase_pattern(PhasePattern pattern)
{
    wall_us = 0;
    opened = 0;
    atomic_store(&residence_state, 0);
    PhaseTimeline timeline = {.pattern = pattern, .start_us = TICK_US};
    next_phase_interval(&timeline);
    int true_blocked_us = advance_phase_timeline(&timeline, timeline.start_us);
    check_peek();

    PeakMpiClockSampler sampler = {.random = 42};
    double now = epoch + timeline.start_us * 1e-6;
    double raw = peak_mpi_active_clock_blocked_seconds(epoch, &now);
    assert(raw == 0.0);
    PeakMpiClockResidence residence = {.epoch = epoch, .now = now, .match_sleep = true};
    double corrected = 0.0;
    const uint64_t sleep_ns = PHASE_SLEEP_US * UINT64_C(1000);
    for (int window = 0; window < PHASE_WINDOWS; ++window) {
        int start_us = timeline.start_us + window * PHASE_SLEEP_US;
        sampler.start = now;
        sampler.raw_start = raw;
        sampler.probes = peak_mpi_active_clock_probe_count(sleep_ns);
        sampler.open_probes = 0;
        for (unsigned probe = 0; probe < sampler.probes; ++probe) {
            uint64_t offset = peak_mpi_active_clock_probe_offset(
                &sampler, sleep_ns, probe, sampler.probes);
            assert(offset >= sleep_ns * probe / sampler.probes);
            assert(offset < sleep_ns * (probe + 1) / sampler.probes);
            advance_phase_timeline(&timeline, start_us + (int)(offset / 1000));
            int open = peak_mpi_active_clock_peek(epoch);
            assert(open >= 0);
            sampler.open_probes += (unsigned)open;
        }

        int end_us = start_us + PHASE_SLEEP_US;
        true_blocked_us = advance_phase_timeline(&timeline, end_us);
        sampler.expected_end = epoch + end_us * 1e-6;
        sampler.tolerance = 0.0005;
        double cut = sampler.expected_end;
        double cut_raw = peak_mpi_active_clock_blocked_seconds(epoch, &cut);
        assert(cut_raw >= raw);
        double next = peak_mpi_active_clock_accumulate(
            &residence, &sampler, cut, cut_raw);
        assert(next >= corrected && next - corrected <= cut - now + 1e-9);
        corrected = next;
        now = cut;
        raw = cut_raw;
    }

    double elapsed = PHASE_SLEEP_US * PHASE_WINDOWS * 1e-6;
    double truth = true_blocked_us * 1e-6;
    assert(corrected >= 0.0 && corrected <= elapsed + 1e-9);
    assert(sampler.sampled_windows == PHASE_WINDOWS);
    if (pattern == PHASE_ALL_COMPUTE || pattern == PHASE_ALL_MPI) {
        assert(fabs(raw - truth) <= 1e-9);
        assert(fabs(corrected - truth) <= 1e-9);
        assert(sampler.corrected_windows == 0);
        return;
    }
    assert(sampler.corrected_windows != 0);
    assert(fabs(corrected - truth) < 0.05 * elapsed);
    if (pattern == PHASE_CROSS) {
        /* The same 400 us waits charge a full tick when they straddle it. */
        assert(raw - truth > 0.50 * elapsed);
    } else if (pattern == PHASE_INSIDE || pattern == PHASE_WITH_LONG_WAIT) {
        assert(truth - raw > 0.30 * elapsed);
    } else {
        assert(raw - truth > 0.09 * elapsed);
    }
}

static void
test_measurement_during_quiet_to_aliased_phase(void)
{
    const double span = 0x1p-3;
    PeakMpiClockResidence residence = {.epoch = epoch, .now = epoch, .match_sleep = true};
    PeakMpiClockSampler sampler = {.probes = 8};
    for (unsigned cut = 1; cut <= 18; ++cut) {
        sampler.start = epoch + (cut - 1) * span;
        sampler.expected_end = sampler.start + span;
        sampler.open_probes = cut > 16 ? 6 : 0;
        double blocked = peak_mpi_active_clock_accumulate(
            &residence, &sampler, sampler.expected_end, 0.0);
        assert(sampler.measured_sleep_seconds == span);
        assert(sampler.measured_mpi_seconds == (cut > 16 ? span * 0.75 : 0.0));
        if (cut < 18) {
            assert(blocked == 0.0 && sampler.applied_sleep_seconds == 0.0);
        } else {
            assert(blocked == span * 0.75 && sampler.applied_sleep_seconds == span);
        }
    }
}

static void
test_measurement_available_independent_of_correction(void)
{
    const double span = 0x1p-3;
    PeakMpiClockResidence residence = {.epoch = epoch, .now = epoch, .match_sleep = true};
    PeakMpiClockSampler sampler = {
        .start = epoch, .expected_end = epoch + span,
        .probes = 8, .open_probes = 4,
    };
    double corrected = peak_mpi_active_clock_accumulate(
        &residence, &sampler, epoch + span, span / 2);
    /* This valid sleep selected raw but supplied actual observations. */
    assert(corrected == span / 2 && sampler.applied_sleep_seconds == 0.0);
    assert(sampler.measured_sleep_seconds == span);
    assert(sampler.measured_mpi_seconds == span / 2);
    sampler.incomplete = true;
    corrected = peak_mpi_active_clock_accumulate(
        &residence, &sampler, epoch + 2 * span, span);
    assert(corrected == span && sampler.measured_sleep_seconds == 0.0 &&
           sampler.measured_mpi_seconds == 0.0);
    (void)peak_mpi_active_clock_accumulate(&residence, &sampler, epoch + 3 * span, -1.0);
    assert(sampler.measured_sleep_seconds == 0.0 && sampler.measured_mpi_seconds == 0.0);
}

static void
test_mixed_sampled_and_raw_spans(void)
{
    const double gap = 0x1p-12;
    const double tick = 0x1p-10;
    const double span = 0x1p-5;
    PeakMpiClockResidence residence = {
        .epoch = epoch, .now = epoch + span, .match_sleep = true,
    };
    PeakMpiClockSampler window = {
        .start = residence.now + gap, .raw_start = tick,
        .probes = 2, .open_probes = 2, .use_occupancy = true,
    };
    double raw = tick + span;
    window.expected_end = window.start + span;
    double blocked = peak_mpi_active_clock_accumulate(
        &residence, &window, window.expected_end, raw);
    /* Equal sleep estimates still require bounding the raw management gap. */
    assert(blocked == gap + span);
    assert(window.corrected_windows == 1);

    /* Preserve the raw quantum in a heartbeat that made no probes. */
    window.probes = 0;
    raw += tick;
    double previous = blocked;
    blocked = peak_mpi_active_clock_accumulate(
        &residence, &window, residence.now + gap, raw);
    assert(blocked - previous == tick);

    window.start = residence.now + gap;
    window.expected_end = window.start + span;
    window.raw_start = raw + tick;
    window.probes = 2;
    window.open_probes = 1;
    window.use_occupancy = true;
    raw = window.raw_start + span / 4.0;
    previous = blocked;
    blocked = peak_mpi_active_clock_accumulate(
        &residence, &window, window.expected_end, raw);
    assert(blocked - previous == gap + span / 2.0);
    assert(window.corrected_windows == 2);
}

int
main(void)
{
    double unavailable_cut = epoch + 0.001;
    coarse_available = true;
    assert(setenv("PEAK_MPI_ACTIVE_CLOCK", "0", 1) == 0);
    peak_mpi_active_clock_start(epoch);
    assert(peak_mpi_active_clock_blocked_seconds(epoch, &unavailable_cut) ==
           PEAK_MPI_CLOCK_UNAVAILABLE);
    assert(mpi_thread_checks == 0 && resolution_checks == 0 &&
           coarse_reads == 0 && fine_reads == 0 && qualification_failures == 0);

    assert(unsetenv("PEAK_MPI_ACTIVE_CLOCK") == 0);
    coarse_available = false;
    peak_mpi_active_clock_start(epoch);
    assert(peak_mpi_active_clock_blocked_seconds(epoch, &unavailable_cut) ==
           PEAK_MPI_CLOCK_UNAVAILABLE);
    assert(qualification_failures == 1);

    coarse_available = true;
    /* Each slow read advances wall time by 1us. Include the untimed probe. */
    slow_coarse_reads = 1 + 3 * QUALIFICATION_BATCH_SIZE;
    peak_mpi_active_clock_start(epoch);
    assert(slow_coarse_reads == 0 && coarse_reads == 1 + 3 * QUALIFICATION_BATCH_SIZE);
    assert(fine_reads == 6 && qualification_failures == 2);
    assert(atomic_load(&clock_owner) == 0 && writer_state == WRITER_INACTIVE);
    assert(peak_mpi_active_clock_blocked_seconds(epoch, &unavailable_cut) ==
           PEAK_MPI_CLOCK_UNAVAILABLE);
    assert(unavailable_cut == epoch + 0.001);

    unsigned reads_before = coarse_reads;
    unsigned fine_reads_before = fine_reads;
    slow_coarse_reads = 1 + QUALIFICATION_BATCH_SIZE;
    peak_mpi_active_clock_start(epoch);
    assert(slow_coarse_reads == 0);
    assert(coarse_reads - reads_before == 1 + 2 * QUALIFICATION_BATCH_SIZE);
    assert(fine_reads - fine_reads_before == 4 && qualification_failures == 2);
    assert(atomic_load(&clock_owner) == getpid() && writer_state == WRITER_IDLE);
    double blocked = observe_at(1000, 0);
    expect_added(0, blocked, 0, 0);
    assert(fabs(peak_mpi_active_clock_quantum_seconds() - 0.001) < 1e-9);

    /* Sweep the phase of eight tiny calls followed by a 19ms wait. The
     * whole burst completes between cuts, including the last long call. */
    unsigned clock_reads_before = coarse_reads;
    for (int phase = 0; phase < TICK_US; phase += 23) {
        int start = (wall_us / TICK_US + 2) * TICK_US + phase;
        for (int call = 0; call < 8; ++call) {
            boundary_at(start + 70 * call);
            boundary_at(start + 70 * call + 50);
        }
        boundary_at(start + 560);
        boundary_at(start + 560 + 19000);
        double next = observe_at(start + 22000, blocked);
        expect_added(blocked, next, 19000, TICK_US);
        blocked = next;
    }
    assert(coarse_reads > clock_reads_before);

    /* Distribute short calls across coarse clock phases, then stop calling MPI.
     * Completed time must stay fixed when the application resumes computation. */
    int dense_start = (wall_us / TICK_US + 2) * TICK_US;
    for (int call = 0; call < 50; ++call) {
        int entry = dense_start + 2 * TICK_US * call + 20 * call;
        boundary_at(entry);
        boundary_at(entry + 400);
    }
    double dense_blocked = observe_at(dense_start + 101000, blocked);
    expect_added(blocked, dense_blocked, 20000, 0);
    blocked = observe_at(dense_start + 121000, dense_blocked);
    expect_added(dense_blocked, blocked, 0, 0);

    /* Two sparse waits share the window and their 15ms application gap
     * remains active time. Observe the second wait before it completes. */
    int start = (wall_us / TICK_US + 2) * TICK_US;
    boundary_at(start);
    boundary_at(start + 40000);
    boundary_at(start + 55000);
    double crossing = observe_at(start + 60000, blocked);
    expect_added(blocked, crossing, 45000, 0);
    double same_tick = observe_at(start + 60750, crossing);
    expect_added(crossing, same_tick, 0, 0);
    double still_waiting = observe_at(start + 90000, same_tick);
    expect_added(crossing, still_waiting, 30000, 0);
    boundary_at(start + 100000);
    blocked = observe_at(start + 115000, still_waiting);
    expect_added(still_waiting, blocked, 10000, 0);
    expect_added(blocked, observe_at(start + 130000, blocked), 0, 0);

    /* Retry once with a fresh bracket. A second preemption preserves the
     * caller's cut, and a later coherent sample can recover the estimate. */
    delayed_reads = 1;
    double cut = epoch + wall_us * 1e-6;
    double first_cut = cut;
    expect_added(blocked, peak_mpi_active_clock_blocked_seconds(epoch, &cut), 0, 0);
    assert(fabs(cut - first_cut - 0.001) < 1e-9);
    delayed_reads = 2;
    first_cut = cut;
    assert(peak_mpi_active_clock_blocked_seconds(epoch, &cut) == -1.0);
    assert(cut == first_cut);
    expect_added(blocked, observe_at(wall_us, blocked), 0, 0);

    /* A failed fine-clock read is a transient wall cut failure. A failed
     * coarse-clock read means no sampled residence is usable at this cut. */
    cut = epoch + wall_us * 1e-6;
    first_cut = cut;
    fine_failure = true;
    assert(peak_mpi_active_clock_blocked_seconds(epoch, &cut) == -1.0);
    assert(cut == first_cut);
    fine_failure = false;
    coarse_available = false;
    assert(peak_mpi_active_clock_blocked_seconds(epoch, &cut) ==
           PEAK_MPI_CLOCK_UNAVAILABLE);
    assert(cut == first_cut);
    coarse_available = true;
    expect_added(blocked, observe_at(wall_us, blocked), 0, 0);

    /* Both entry and exit errors latch unavailability even if later clock
     * reads succeed. Reset the packed state to exercise each failure alone. */
    coarse_available = false;
    assert(open_interval() == UINT64_MAX);
    coarse_available = true;
    cut = epoch + wall_us * 1e-6;
    assert(peak_mpi_active_clock_blocked_seconds(epoch, &cut) ==
           PEAK_MPI_CLOCK_UNAVAILABLE);

    atomic_store(&residence_state, 0);
    uint64_t interval = open_interval();
    assert(interval != UINT64_MAX);
    coarse_available = false;
    close_interval(interval);
    coarse_available = true;
    assert(open_interval() == UINT64_MAX);
    assert(peak_mpi_active_clock_blocked_seconds(epoch, &cut) ==
           PEAK_MPI_CLOCK_UNAVAILABLE);
    assert(peak_mpi_active_clock_peek(epoch) < 0);

    test_phase_pattern(PHASE_INSIDE);
    test_phase_pattern(PHASE_CROSS);
    test_phase_pattern(PHASE_ALTERNATING);
    test_phase_pattern(PHASE_WITH_LONG_WAIT);
    test_phase_pattern(PHASE_ALL_COMPUTE);
    test_phase_pattern(PHASE_ALL_MPI);
    test_measurement_available_independent_of_correction();
    test_measurement_during_quiet_to_aliased_phase();
    test_mixed_sampled_and_raw_spans();
    test_epoch_boundaries();
}
