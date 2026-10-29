#!/usr/bin/env bash
# Host test for the shared GPU readback packing logic. No game data or GPU.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_wb_gpu.h" ]]; then
  echo "[test_wb_gpu] FAIL: missing GPU writeback packing ($V; run tools/ps3recomp-patched.sh)" >&2; exit 1
fi
O="$ROOT/build/smoke-host/wb_gpu"; mkdir -p "$O"
CC="${CC:-cc}"
"$CC" -std=c11 -O2 -Wall -Wextra -Werror -I"$V" "$ROOT/tools/smoke/test_wb_gpu.c" -o "$O/test_wb_gpu"
"$O/test_wb_gpu"
if echo "int main(void){return 0;}" | "$CC" -fsanitize=address,undefined -x c - -o "$O/probe" 2>/dev/null && "$O/probe"; then
  "$CC" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$V" "$ROOT/tools/smoke/test_wb_gpu.c" -o "$O/test_asan"
  "$O/test_asan" >/dev/null && echo "[PASS] ASan/UBSan pass" || { echo "[FAIL] ASan/UBSan pass"; exit 1; }
fi
