#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/peak-statfs-regression.XXXXXX")
cleanup() {
  status=$?
  if ((status==0)); then rm -rf "$build"; else printf 'failed regression artifacts retained: %s\n' "$build" >&2; fi
}
trap cleanup EXIT
"${CC:-cc}" -shared -fPIC -std=gnu11 -Wall -Wextra -Werror "$root/test/filesystem_stat_guard/proxy.c" -o "$build/libproxy.so"
"${CC:-cc}" -shared -fPIC -std=gnu11 -Wall -Wextra -Werror -DPEAK_FILESYSTEM_STAT_GUARD_TESTING -I"$root/include" "$root/src/filesystem_stat_guard.c" -ldl -pthread -o "$build/libguard.so"
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -I"$root/include" "$root/test/filesystem_stat_guard/test_guard.c" -L"$build" -lguard -lproxy -pthread -Wl,-rpath,"$build" -o "$build/test"
timeout --kill-after=2s 15s "$build/test"
