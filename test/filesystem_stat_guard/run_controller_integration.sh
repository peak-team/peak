#!/usr/bin/env bash
# Compile actual controller/signal-policy lifecycle code; section GC excludes
# unrelated physical backend paths. No Gum binary or physical stop is executed.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
: "${PEAK_GUARD_GUM_INCLUDE:?set to a validated Gum devkit header directory}"
build=$(mktemp -d "${TMPDIR:-/tmp}/peak-statfs-controller-regression.XXXXXX")
cleanup() {
  status=$?
  if ((status==0)); then rm -rf "$build"; else printf 'failed controller regression artifacts retained: %s\n' "$build" >&2; fi
}
trap cleanup EXIT
cc=${CC:-cc}
processor=${PEAK_GUARD_SYSTEM_PROCESSOR:-$(uname -m)}
sources=("$root/test/filesystem_stat_guard/test_review.c")
case "$processor" in
  aarch64|arm64|ARM64)
    sources+=("$root/cmake/peak-gum/peak_aarch64_raw_syscall.S")
    ;;
esac
"$cc" -c -O1 -ffunction-sections -fdata-sections -DPEAK_ENABLE_TEST_HOOKS=1 -DPEAK_HAVE_GUM_PEAK_PC_API=1 -I"$root/include" -I"$PEAK_GUARD_GUM_INCLUDE" "$root/src/detach_controller.c" -o "$build/controller.o"
"$cc" -c -O1 -ffunction-sections -fdata-sections -I"$root/include" "$root/src/signal_policy.c" -o "$build/signal.o"
"$cc" -shared -fPIC -std=gnu11 -Wall -Wextra -Werror -DPEAK_FILESYSTEM_STAT_GUARD_TESTING -I"$root/include" "$root/src/filesystem_stat_guard.c" -ldl -pthread -o "$build/libguard.so"
"$cc" -shared -fPIC -std=gnu11 -Wall -Wextra -Werror "$root/test/filesystem_stat_guard/proxy.c" -o "$build/libproxy.so"
# Only process-policy decisions are supplied by this standalone harness. The
# mutex, controller scopes, atfork callbacks and signal policy are production.
cat > "$build/process-policy.c" <<'C'
int peak_process_profile_enabled(void) { return 1; }
int peak_process_requests_work(void) { return 1; }
C
"$cc" -std=gnu11 -Wall -Wextra -Werror -DREVIEW_ACTUAL_CONTROLLER -I"$root/include" "$build/controller.o" "$build/signal.o" "$build/process-policy.c" "${sources[@]}" -Wl,--gc-sections -L"$build" -lguard -lproxy -pthread -ldl -Wl,-rpath,"$build" -o "$build/test"
for scenario in cold cold-warm-before cold-warm-after; do
  timeout --kill-after=2s 5s "$build/test" "$scenario"
done
