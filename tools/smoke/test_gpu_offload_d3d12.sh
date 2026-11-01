#!/usr/bin/env bash
# Build the game-free GPU offload test for Windows; run with tools/proton-test.sh under the GPU lock.
set -euo pipefail
S="${PS3RECOMP_SRC:-work/ps3recomp-patched}"
O=build/smoke-gpu-offload
mkdir -p "$O"
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Wno-unused-function \
  -I"$S/include" -I"$S/libs/video" tools/smoke/test_gpu_offload_d3d12.c "$S/libs/video/rsx_vertex_compact.c" \
  -o "$O/test_gpu_offload_d3d12.exe" -ld3d12 -ld3dcompiler -ldxguid -ldxgi -lole32 -lm -static
echo "built $O/test_gpu_offload_d3d12.exe"
