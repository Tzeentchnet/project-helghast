# How the port works

Killzone 3 runs on the PS3's Cell processor: one PowerPC core (the PPU) runs the game, six SPU co-processors run
jobs (physics, animation, audio, post-processing), and the RSX GPU draws. The port translates the game's PPU and SPU code to C ahead of time, compiles it into a
Windows x64 program, and links it against a runtime that answers the PS3 system calls and libraries on the PC. No CPU
emulator runs at play time, but the GPU and the SPUs' surroundings are still reproduced by that runtime (next section).

## What kind of port is this?

"Recomp" covers a wide range: from an emulator's work done ahead of time up to a full rewrite. This port is a static
recompilation with a runtime layer, plus native replacements where they matter. By part, today:

| Part | How it runs today |
|---|---|
| PPU game code | recompiled ahead of time to C, compiled for x64 |
| SPU programs and jobs | recompiled ahead of time; SPURS scheduling and DMA are reproduced by the runtime. With the 1.14 update these run as native C by default: the light-probe job, and the upsample and depth-of-field passes of the post-processing jobs (`KZ3_NATIVE_JOB_LIGHTPROBES=0` and `KZ3_NATIVE_JOB_POST=0` run the recompiled code instead) |
| Graphics | **translated at run time**: the game's RSX command stream is replayed on Direct3D 12, shaders translated from RSX microcode (the same kind of work an emulator's GPU layer does) |
| Memory | the PS3's big-endian layout, kept as the recompiled code expects it |
| System libraries (lv2, cell*) | reimplemented on PC APIs (high-level emulation) |
| Audio output, files, saves | native PC APIs |
| Input | native: SDL3 gamepads (XInput without the SDL3 library), Raw Input mouse look |
| Fixes and extras | native code, e.g. FXAA in place of the SPU anti-aliasing job ([features](FEATURES.md)) |

It is not a decompilation: the game's logic is the recompiled original, not rewritten source, and the repository
holds none of it (everything is built on your PC from your own copy). Ahead-of-time recompilation is not faster by
itself (the RPCS3 emulator runs this game faster today); what it gives is a single PC program, hooks for fixes, mods
and PC features, and no emulator to set up. The plan after feature parity and VR is to replace the run-time layers
with native code step by step: a native renderer fed by the engine's own draw lists, native SPU jobs, then rewritten
game code where it pays off ([roadmap](ROADMAP.md)).

## Pipeline

1. **Inputs.** The player's own disc dump, the 1.14 update and the decrypted EBOOT. Game data stays where it is:
   the program reads the game's `data_ps3.psarc` archive at run time. Nothing from the game is in this repository.
2. **Recompiler.** [ps3recomp](https://github.com/sp00nznet/ps3recomp) (MIT), pinned as a submodule. The port's
   fixes to it are numbered patches in `port/patches/`, applied to a fresh copy before every lift and build. Fixes
   that are not specific to Killzone 3 are meant for ps3recomp itself.
3. **PPU lift.** Each of the EBOOT's ~59,000 functions becomes a C function that works on a modelled register file
   and the guest's big-endian memory. The output is cut into 64 source files by address, so a change to one function
   recompiles one file. A few functions are replaced by host code through an override list
   (`port/spu/ppu_overrides.txt`): hand-written context switches, hot paths and the like.
4. **SPU lift.** The 22 SPU programs embedded in the EBOOT and the 75 SPURS job modules (about 54,000 functions)
   are lifted the same way. Each image is registered by a fingerprint of its code, so when the game uploads SPU
   code at run time the runtime finds the matching compiled C and runs it on a host thread.
5. **Runtime (high-level emulation).** ps3recomp's implementations of the lv2 system calls and the libraries the
   game imports (threads and sync, cellSpurs, cellGcm, cellFs, cellSaveData, cellPad, cellAudio and others), plus
   the port's own code in `port/`: the archive file system, the 1.14 update overlay, SPU scheduling, logging and
   crash reports.
6. **Graphics.** The game builds RSX command buffers exactly as on the PS3. The runtime walks them as they are
   written and replays them on Direct3D 12: RSX vertex and fragment programs are translated to HLSL, render
   targets and textures are mirrored on the GPU, and the targets that the SPU post-processing chain reads from
   memory (MLAA, bloom, depth of field) are written back.
7. **Running.** The result is `kz3.exe`, a native Windows program. On Linux it runs under Proton, which is where it
   is mostly tested (it has also run natively on Windows 11 and on a Windows 10 test machine); a native Linux build is planned.

## Experimental GPU offloads

The following environment switches are **off by default** and require a build incorporating these patches; setting
them does not update an installed release. They are experiments, not measured frame-rate improvements:

- `LD_VTX_GPU=1` fetches and decodes eligible RSX vertex attributes in the host GPU's vertex shader. Existing
  vertex-cache hits still use their cached decoded payloads; fills and verification remain on the CPU. The cache
  retains tracked-write and full source-byte validation. `LD_VTX_CACHE=0` disables the cache for a GPU-fetch-only
  comparison, and `LD_VTX_GPU_MB` caps a draw's uploaded source block (default 8 MB). Oversized blocks and unavailable
  GPU-fetch pipelines use CPU fetch. Diagnostics that require the CPU-decoded payload disable GPU fetch;
  `[vtx-gpu]` reports routing, fallbacks and cache activity.
- `LD_WB_GPU_PACK=1` packs eligible batched colour-surface readbacks into guest byte order on the GPU. The CPU still
  waits for the GPU and copies the packed bytes into guest memory; this does not keep the SPU post-processing chain
  on the GPU. Pipeline or allocation failures are logged and use CPU packing. `LD_WRITEBACK_BATCH=0` selects the
  existing per-surface CPU path, and the unsafe no-wait test mode disables GPU packing. The extra GPU buffer and copy
  can outweigh the saved CPU conversion work.

Compare each switch separately against the default using the same build, scene, input and cache state, including
guest-frame pacing and CPU/GPU timings. These switches do not move graphics-oriented SPU jobs to the GPU: they remain
CPU-side, either recompiled or using native replacements (including 1.14 light probes by default). Replacing a job
requires preserving its guest-visible memory and synchronization effects. No in-level performance or compatibility
result is implied by these switches.

The game-free `tools/smoke/test_gpu_offload_d3d12.sh` builds a Windows executable that compares the shared vertex
and packing shaders with CPU results on D3D12. It uses adapter 0 by default; `RSX_TEST_ADAPTER=N` selects another
zero-based adapter, and `RSX_TEST_WARP=1` selects the software device. The existing renderer integration tests can
assert that the requested path actually submitted work: `KZ3_EXPECT_VTX_GPU=1` for `test_vtx_cache.exe` and
`KZ3_EXPECT_WB_GPU=1` for `test_wb_batch.exe` (use `0` to assert that the respective path stayed off).

## Data

The game's 31 GB archive is read in place through a case-insensitive virtual file system (the game's paths are not
consistent in case). The 1.14 update is layered over it the way the PS3 does: whole replacement files from the
update, and binary patches applied to disc entries on first use and cached.

## What is patched

The patches in `port/patches/` are grouped by number: 0xxx general and lifter fixes, 1xxx SPU and SPURS, 2xxx
graphics, 3xxx lifter and system-library fixes, 5xxx data, update and trophy support. Typical examples:

- runtime tables sized for a smaller game (SPU DMA contexts, GPU I/O mappings, thread-local storage blocks);
- library calls with missing or wrong behaviour (task arguments dropped, a ninth stack argument lost, unlocked
  read-modify-writes racing the SPUs);
- MinGW-specific problems (thread-local variables, slow emulated TLS on hot paths);
- renderer features the game uses (per-unit samplers, texture gamma, float and depth formats, conditional rendering
  with occlusion queries, buffer copies and write-backs);
- an opt-in **ENHANCEMENT** path that feeds validated deferred depth, camera-derived motion, and a generated
  responsive-pixel mask into Intel XeSS Super Resolution, with Bilinear fallback. Architecture and limitations:
  [XESS.md](XESS.md).

Each runtime fix comes with a focused host test in `tools/upstream/`, `tools/smoke/` or `tools/spu/` that shows the
bug on the unpatched code and its cure on the patched code.

## Testing

- **Without the game:** CI runs shell lint, checks that every patch applies to the pinned ps3recomp, and runs the
  host tests listed in `tools/host-tests.txt`.
- **With the game:** scripted runs under Proton on a hidden display, with scripted pad input (`PAD_SCRIPT`), frame
  and performance counters in the log, and a screenshot at the end. Every new picture on screen is kept as a dated
  screenshot with the development records.
- **Reference:** RPCS3 shows what the real game does (logs, RSX frame captures). It is read for behaviour only; no
  RPCS3 code is used.

## Conventions in the source

- Original function addresses are kept in comments (`// @0x00123abc`).
- Values not yet recovered from the binary are marked `ASSUMPTION:`.
- Features the PS3 game does not have are marked `ENHANCEMENT:`. Display improvements may be on by default; anything
  that changes gameplay is opt-in.

Status, feature by feature: [FEATURES.md](FEATURES.md). What comes next: [ROADMAP.md](ROADMAP.md).

## In short

- **PPU:** ~59,000 functions lifted to C by ps3recomp's lifter; a few are replaced by host code (fiber scheduler,
  heap, movie skip) through override hooks.
- **SPU:** all 22 embedded SPU programs and 75 SPURS job modules lifted; SPURS runs on host threads.
- **Graphics:** the game's RSX command stream is walked and replayed on Direct3D 12 (shaders translated from RSX
  microcode), including the SPU post-processing chain's read-backs.
- **Data:** the game's archive is read in place through a case-insensitive virtual file system; the 1.14 update is
  layered over the disc data as the PS3 does.

Terms: [GLOSSARY.md](GLOSSARY.md).
General fixes found here are offered to ps3recomp upstream (see [CONTRIBUTING.md](../.github/CONTRIBUTING.md)).
