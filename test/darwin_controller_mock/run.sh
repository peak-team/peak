#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -D__APPLE__ -D__arm64__ \
    -DPEAK_ATTACH_RUNTIME -Wall -Wextra -Werror \
    -I"$root/test/darwin_controller_mock/include" -I"$root/include" \
    "$root/test/darwin_controller_mock/test_controller.c" -pthread \
    -o "$work/controller"
"$work/controller"
