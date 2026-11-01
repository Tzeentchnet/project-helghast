#!/usr/bin/env bash
# Compiles the XeSS adapter with the strict flags of test_xess.ps1, so a D3D12 or ABI mistake shows without a GPU. Needs a
# Windows-target C++ compiler: the MinGW cross compiler of `nix develop .#win`, or g++ on Windows. SKIP without one.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
O="$ROOT/build/smoke-host/xess-compile"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
R="$PT/libs/video/rsx_live_draw.c"
if [[ -f "$R" ]]; then
  grep -Fq "latest.guest_flip, g_ld_frames" "$R" ||
    { echo "[test_xess_compile] FAIL: camera pairing does not use the renderer frame counter"; exit 1; }
  if grep -Fq "latest.guest_flip, g_ld_flip_consumed" "$R"; then
    echo "[test_xess_compile] FAIL: camera pairing uses the FIFO-only flip counter"; exit 1
  fi
fi
CXX=""
if command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
  CXX=x86_64-w64-mingw32-g++
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]] && command -v g++ >/dev/null 2>&1; then
  CXX=g++
fi
if [[ -z "$CXX" ]]; then
  echo "[test_xess_compile] SKIP: no Windows-target C++ compiler"
  exit 0
fi
mkdir -p "$O"
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -Wno-cast-function-type \
  -I"$ROOT/port/kz3" -c "$ROOT/port/kz3/xess.cpp" -o "$O/xess.o"
echo "[test_xess_compile] PASS"
