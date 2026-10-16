# Signal native-handoff diagnostic

`supported-handler-native-handoff` is an **opt-in failing contract diagnostic**,
not a green CTest and not a signal-backend repair. It extends the existing real
controller/Gum fixture; it does not fabricate or replay the owned packet.

## Build and run locally

Use a patched Gum provider exposing the PEAK PC API (the unmodified upstream
devkit is insufficient). From the repository root, choose a fresh build path:

```bash
cmake -S . -B build-handoff \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DPEAK_ENABLE_MPI=OFF -DPEAK_ENABLE_OTF2=OFF \
  -DPEAK_ENABLE_FORTRAN_TESTS=OFF -DPEAK_BUILD_BENCHMARKS=OFF \
  -DBUILD_CUDA_PROFILE=OFF -DPEAK_ENABLE_JIT_RUNTIME_PRELOAD_TESTS=OFF \
  -DPEAK_FETCH_DEPS=OFF -DPEAK_FRIDA_GUM_PROVIDER=patched-devkit \
  -DPEAK_REQUIRE_GUM_PEAK_API=ON \
  -DPEAK_PATCHED_GUM_ROOT=/absolute/path/to/patched-gum-provider
cmake --build build-handoff --target test_detach_controller -j2
env PEAK_TARGET=strict_helper_target PEAK_SAFE_DETACH_MODE=signal \
  PEAK_DETACH_BACKEND=signal PEAK_DETACH_HELPER=/no/such/peak_detach_helper \
  timeout --signal=TERM --kill-after=2s 30s \
  build-handoff/test/detach_controller/test_detach_controller \
  supported-handler-native-handoff
```

Run in a clean environment without inherited `PEAK_*` overrides. The preserved
local build used
`/home/iwang/Documents/Codex/2026-10-06/task-3/peak-common-capacity-20261008/providers/v5`
as the provider. The diagnostic needs Linux and the patched PC API; unsupported
configurations return 77. An outer runner must retain stdout, stderr, the actual
return code and timeout/signal outcome, and terminate/reap its process group.
A signal death, timeout, abort or ordinary nonzero return is **not a pass**.

## Scenario and current result

1. Target thread T enters an ordinary application handler P. Native application
   of `P.sa_mask={S}` blocks the reserved signal S without a mask API call.
2. The actual controller sends its owned packet O. The fixture records native
   acceptance, the real owned cookie, and T's blocked/pending S. The controller
   times out before any physical mutation.
3. U installs application `SA_SIGINFO` handler H on S while P waits for U to
   release its pipe. The wrapper return and an actual libc disposition query
   are checked; visible flags and every mask member are compared, not padding.
4. A genuine `pthread_sigqueue` notification with a distinct nonowned payload is
   queued to the **same T**. U then releases P. H distinguishes the real owned
   cookie from the genuine application notification and retains its siginfo.

Exact81a (`81a6e2f10cd77e274183e5e5b97dc9e6d706c8ef`) currently fails:

```text
native-handoff-install ... rc=0 ... native_query_rc=0 exact_H=1 P_returned=0 app_queue_rc=0
native-handoff-delivery owned_H=1 app_H=1 other_H=0 ...
FAIL: owned O must never reach native application H: expected 0, got 1
```

The required oracle remains `owned_H==0`, `app_H==1`, correct native H, and
preserved genuine `SI_QUEUE` sender PID/UID/payload. Do not invert that oracle
or classify O as an application notification. A prior full-mask variant blocked
every RT replacement and instead measured wrapped `sigaction` failing with
`EINVAL`; that is a distinct native-API-refusal failure, **not** an observed
O-to-H leak. No independent native-install comparison or transient `SIG_DFL`
sampling is claimed by this diagnostic. Handler payloads are published before
release-count updates; the target is joined before metadata reads, including
the timeout path, so a late H cannot race those reads.

## Physical and lifecycle limits

The negative diagnostic measures actual Gum target-entry bytes and actual
listener callbacks before/after timeout and handoff. `bytes_equal_active=1` and
`callback_delta=1` prove unchanged instrumentation here, not successful physical
detach. The separate `supported-handler-owned-recovery` and
`active-slot-reader-reuse` scenarios include real entry-byte/callback transitions
for physical detach/reattach after their own recovery conditions. They do not
turn this failing handoff into a pass.

The mandatory kernel-selected-old-handler/frame lifecycle remains unproven:
zero userspace readers or closed sender admission cannot exclude a kernel-bound
old frame that has not entered userspace. Full repair also requires ordinary
notification transparency, the O/H circular-wait case, and actual physical
detach/lifetime evidence. No permanent dispatcher, application API refusal,
helper fallback, or early target termination is an accepted substitute.

## Development-branch status

This `fix/signal-native-handoff-20261016` branch descends from exact81a and
preserves its cumulative partial fixes. It is explicitly **nonfinal**. The
reviewed remote PR123 branch `fix/controller-owned-signal-retirement` at
`a88b21d6089593076773eabcd491e79985ca07d2` diverges by the two effect-related
commits `7c6850fe39a54dcf56e17381ac3e878cce05638f` and
`a88b21d6089593076773eabcd491e79985ca07d2`; this descendant must not overwrite
or silently discard those guards. Publishing this separate development branch
does not replace a qualified PR123 native fix, approve a merge, or establish
that the complete handoff contract is repaired.
