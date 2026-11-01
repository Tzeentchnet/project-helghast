# XeSS-SR review: branch `xesstest`

Commits `05c978d` and `085d371` against `main`, reviewed 2026-10-31. The review read the adapter, patch 2139, the
renderer code around it, the release tooling and Intel's SDK 3.0.2 headers and developer guide, then ran the tests
and a black-box probe on an RTX 5070 (driver 617.42). The game was not run, so in-game behaviour here comes from
reading the code. No code was changed.

## Verdict

The integration is mechanically sound and safe as an opt-in experiment, but it should not merge as written.

The hardest parts are correct, and that was measured rather than assumed: the ABI, jitter sign and scale, motion
vector sign, resource states, fallback and packaging. The problems are around them. Image quality is held back by
what happens to the image before XeSS sees it, and two guarantees in `docs/XESS.md` are not what the code does.

Five items should be fixed before merge. All are small except F3.

| ID | Must fix before merge | Why |
|---|---|---|
| F1 | FXAA runs on the jittered scene before XeSS | On by default, undocumented, works against the temporal solve |
| F2 | No texture mip bias | Intel says XeSS-SR requires it; undocumented |
| F3 | Camera freshness and frame alignment are not enforced | The docs say stale cameras fall back; they do not |
| F4 | The real failure reason is hidden from the log | A missing DLL is reported as a camera problem |
| F5 | Project rules: no `ASSUMPTION:`/`ENHANCEMENT:` markers, `features.tsv` not updated | Required by `AGENTS.md` |

## Evidence labels

- **Measured**: run during this review; numbers are in [Measurements](#measurements).
- **Read**: established from the code. The path is deterministic, but it was not observed in a game run.
- **Inferred**: a likely consequence that still needs a run to confirm.

Renderer references point into [patch 2139](port/patches/2139-xess-super-resolution.patch) where the code is new, and
otherwise name the function in `libs/video/rsx_live_draw.c` of the patched toolkit.

## Must fix before merge

### F1. FXAA runs on the jittered scene (Read)

`KZ3_HOST_AA` unset means host AA mode 1, so `kz3_mlaa_body` skips SPU MLAA and arms FXAA
([kz3_ppu_overrides.cpp:866](port/spu/kz3_ppu_overrides.cpp#L866), active on 1.14 through
[ppu_overrides-114.txt:10](re/data/overrides/ppu_overrides-114.txt#L10)). The patch then calls `ld_host_aa(i)`
unconditionally, straight after the pre-upscale capture
([patch line 572](port/patches/2139-xess-super-resolution.patch#L572)).

Every XeSS frame therefore gets a spatial edge filter on its jittered samples before the temporal solve. FXAA responds
differently at each jitter phase, so it softens edges and adds frame-to-frame variation that XeSS has to treat as
signal. It also costs GPU time for nothing. A run log will show `[host-aa] SPU MLAA skipped` and `[host-aa] fxaa on
surface` alongside the `[xess]` lines.

**Fix:** skip `ld_host_aa()` when this frame was jittered, for example
`g_xess_camera_frame == g_ld_frames` under `KZ3_XESS`. Keep MLAA skipped so nothing else takes FXAA's place. This also
removes finding M1.

### F2. No texture mip bias (Read)

The guide says "XeSS-SR requires an additional mip bias". It gives the formula `log2(render / output)`, and "Adjust mip
bias" is in its recommended practices. In the renderer, `sd.MipLODBias` comes only from the guest sampler register, and
the patch never touches it. Textures are therefore picked for 720p and upscaled, which limits detail at every quality
level. `docs/XESS.md` does not list this.

**Fix:** add the bias to scene-pass samplers only, meaning draws that are jittered, not HUD or post. That is -0.585 at
1080p, -1 at 1440p and -1.585 at 4K. The sampler table key must include it.

### F3. Camera freshness and frame alignment are not enforced (Read; consequences Inferred)

`kz3_xess_get_camera` returns the last coherent state however old it is
([xess.cpp:1262](port/kz3/xess.cpp#L1262)). The renderer's only check is
`g_xess_camera_frame != g_ld_frames` ([patch line 641](port/patches/2139-xess-super-resolution.patch#L641)), which
proves a snapshot was taken this frame, not that the snapshot is new. `camera.guest_flip` is never compared with
anything. `docs/XESS.md` line 239 lists a stale camera as a fallback condition, but no such check exists.

The docs present "latest published state at the primary draw" as avoiding a fixed offset between the PPU flip counter
and the FIFO. It does avoid that assumption, but it leaves the pose-to-frame pairing to timing:

- **Repeated state:** if a frame reuses a state, the same non-zero delta is applied again. If publication stops while
  the scene keeps rendering, a static image receives camera motion indefinitely.
- **Lead:** if the PPU is a flip ahead of the FIFO when the primary draw is reached, the depth of frame N is
  reprojected with the pose pair of N+1. At constant velocity this barely shows.
- **Misplaced reset:** `KZ3_XESS_CAMERA_RESET` travels with the state, so with that lead it lands on the frame before
  the cut. The cut frame itself then blends old history and ghosts.

Which of these occur in practice is unknown, because no log records the offset.

**Fix:**

1. Log `camera.guest_flip - g_ld_flip_consumed` per frame in one run to measure the offset.
2. Publish into a small ring keyed by guest flip, and have the renderer ask for the flip it is replaying.
3. If a dispatch sees the same flip as the last one, use previous = current. If the flip jumped, reset history.

### F4. The real failure reason is hidden (Read)

`docs/XESS.md` says the renderer "logs the reason". For the main failure cases it logs the wrong one:

1. `ld_xess_draw_jitter` calls `ld_xess_ensure_context` at the first deferred draw. On failure it returns with
   `g_xess_camera_frame` still `UINT32_MAX` ([patch lines 330-361](port/patches/2139-xess-super-resolution.patch#L330)).
2. `ld_xess_present` reaches the camera check first and reports
   `no coherent single-view camera and projection are available`.
3. The `XESS_BYPASS_CONTEXT` branch that would print `kz3_xess_status()` is reachable only if the draw-time init worked
   and the present-time init failed.

So a missing DLL, a wrong DLL, an unsupported GPU, a rejected resolution and a non-16:9 window all log as a camera
problem. The adapter's own status is correct: the smoke test printed `Win32 error 126` and `missing the required
xessGetVersion export`. The renderer just never shows it.

The same path retries every frame, because no failure is remembered. Measured retry costs are 0.008 ms for a missing
file and 6 ms for a wrong DLL (load, version check, unload). When context creation succeeds and a later step fails,
such as a rejected resolution, a retry costs about 32 ms (create and destroy), which is most of a 30 fps frame. How long
context creation takes to fail on an unsupported GPU was not measured.

**Fix:** record a draw-time init failure and report `kz3_xess_status()` before the camera check. Latch failures per
device and output size. Consider showing the fallback state on the Options row, which currently says XeSS-SR while the
picture is Bilinear.

### F5. Project rules from `AGENTS.md`

- **Markers.** `xess.cpp`, `xess.h`, `xess_api.h`, `xess_inputs.h`, `xess_motion.h` and the new `main.cpp` lines hold no
  `ENHANCEMENT:`, no `ASSUMPTION:` and no `@0x` address comments. Only the CMake comment and the patch header say
  `ENHANCEMENT`. Values that need `ASSUMPTION:` are:
  - the surface offsets at [xess_inputs.h:6-7](port/kz3/xess_inputs.h#L6)
  - the near and far offsets in the `vf17` result ([main.cpp:3012](port/kz3/main.cpp#L3012))
  - the cut thresholds ([xess.cpp:932](port/kz3/xess.cpp#L932), [:941](port/kz3/xess.cpp#L941))
  - the mask constants ([xess.cpp:444](port/kz3/xess.cpp#L444))
  - above all the unvalidated 1.00 `scene_lr` ([main.cpp:2955](port/kz3/main.cpp#L2955))
- **`scene_lr` check.** `kz3_hud_proj_init` verifies the `bctrl` before `v->lr` but not before `scene_lr`. A wrong value
  means no planes and permanent fallback, so it fails safe, but the check is a cheap addition.
- **Feature table.** [features.tsv:74](docs/features.tsv#L74) still says temporal upscaling is "Backlog after parity;
  not started", and [line 53](docs/features.tsv#L53) lists only Bilinear and Nearest, while `ROADMAP.md` was updated.
  Update the table with the recorded run as evidence, then regenerate with `python3 tools/features.py`.

## Should fix

### F6. XeSS runs after post-processing and the HUD (Read)

The guide says "We recommend to run XeSS-SR at the beginning of the post-processing chain before tone-mapping", and
calls later placement "experimental, and good quality is not guaranteed". It expects linear input. The presented
surface is display-referred and has been through FXAA, the SPU post chain (including motion blur), the compose and the
HUD. The known-issues list mentions only the HUD.

The HUD is worse off than the docs suggest. Its pixels receive motion reconstructed from the depth behind them, so
static text is reprojected as if it were world-locked. The responsive mask does not help, because its final capture
happens at the NV0039 scene copy, before FXAA, post and HUD ([patch lines 536-573](port/patches/2139-xess-super-resolution.patch#L536)).
The phrase "immediately before temporal presentation" (XESS.md lines 57 and 160) is therefore inaccurate.

**Fix, cheap step first:** patch 2113 already identifies the compose draw (`compose = first && g_h169_depth_seen`).
Capturing the display surface right after it gives a HUD-free image. The difference from the presented surface is then
a HUD mask: set those pixels to motion 0 and full responsiveness, raising `xessSetMaxResponsiveMaskValue` above the 0.8
default if testing supports it. Moving the dispatch earlier in the chain remains the long-term answer.

### F7. Jitter continues while presentation falls back (Read)

The promise that jitter cannot leak into a Bilinear frame (XESS.md line 134) covers only the runtime and context.
`ld_xess_draw_jitter` does not know about failures at present time. A persistent bypass for colour, depth or dispatch
reasons keeps jittering the scene while Bilinear shows it, which reads as a shimmering picture.

**Fix:** after a dispatch or input failure, suppress jitter until a validation pass succeeds.

### F8. The exact-integer aspect check refuses usable outputs (Measured)

[xess.cpp:853](port/kz3/xess.cpp#L853) requires `render_w * output_h == render_h * output_w`, and the runtime does not:
1366x768 and 1778x1000 both passed init and execute with a 1280x720 input. Because `rsx_dm_fit` rounds, Fit produces an
exact 16:9 rect only for some window sizes. A common 1366x768 laptop panel is also refused outright, with the F4
misdiagnosis on top.

Intel's header says the aspect ratios must be the same. Execute also accepted 1920x1200, so this runtime does not
enforce that note, and acceptance says nothing about image quality.

**Fix:** allow a rounding tolerance of 1 output pixel, and check the picture at 1366x768 before relying on it.

### F9. The VC++ runtime dependency is undocumented (Read)

The guide lists Visual C++ Redistributable 14.40.33810 or later (`msvcp140.dll`, `vcruntime140.dll`,
`vcruntime140_1.dll`) beside `libxess.dll`. The port is a MinGW build, so nothing else in it needs these, and neither
the installer, the release tools nor the docs mention them. The guide also sets a higher Windows floor (10.0.19043)
than `requirements.md`, which says Windows 10 64-bit. On a machine without the redistributable the DLL fails to load and
the game falls back cleanly, with the F4 message.

**Fix:** add both to `docs/requirements.md` and name the redistributable in the load-failure status.

### F10. Re-initialisation relies on an unrelated flush (Read)

The guide requires pending XeSS command lists to complete before re-init. `ld_xess_ensure_context` waits on
`g.fence_value`, which covers only submitted lists, and then `kz3_xess_initialize` releases the capture textures
directly. The rest of the renderer uses `retire_texture` for this exact reason ("the open command list may still read
it").

Normal gameplay is safe, because `ld_writeback_batch` flushes right after the captures. A frame with an opaque capture
but no scene write-back can leave a reference in the open list, and with `LD_NV0039_WRITEBACK=0` the write-back never
runs. Separately, `changed && g_xess_context_w` skips the wait after a failed init because the width was zeroed, even
though the adapter's old resources still exist. Every real size change passes through `ld_dm_sync_size`, which covers
that second case in practice.

**Fix:** flush before re-init in the present path, or hand old resources to `retire_texture`. Do not flush from the
draw path: recorded state would be lost, as the comment in `sink_end_impl` warns.

### F11. Intel licence copies are not byte-identical in the repository (Measured)

The stored blobs are LF (4,167 and 35,224 bytes). The SDK files are CRLF (4,194 and 35,788 bytes), and those are what
the pins and the payload use. `test_licenses_match.py` passes here only because this checkout has `core.autocrlf=true`.
With the `autocrlf=false` the workflows and patch script need, and a staged `work/release/xess`, the test fails. CI
skips it only because that folder is absent.

**Fix:** mark both files `-text` in [.gitattributes](.gitattributes#L6) and re-add them with their CRLF bytes.

### F12. Collision with `fsrtest` (Read)

`fsrtest` also adds `port/patches/2139-*` and edits the same renderer lines, including its own filter enum. Whichever
lands second must renumber and resolve those lines. The `"fsr"` to `"fsr31"` change in
[test_display_mode.c:143](tools/smoke/test_display_mode.c#L143) has nothing to do with XeSS and looks like a leftover
from that branch.

### F13. Test coverage (Read)

- CI never compiles `xess.cpp`; only the manual `test_xess.ps1` does.
- The camera bridge (publication, resets, ambiguity) has real tests, but they sit inside the GPU executable. Moved to a
  platform-free header, they could run in `host-tests.txt`.
- The smoke test dispatches one frame with reset set, so nothing temporal is covered. The probe method below would make
  a useful addition to `test_xess.ps1`.
- `pose_valid` checks vector length but not handedness. A one-time log of `dot(cross(right, forward), up)` would catch
  an axis mistake, which none of the tests here can see.

## Minor

| ID | Finding | Note |
|---|---|---|
| M1 | `ld_gamma_present_resource` sizes the shared `g_aa_tex` to the XeSS output while FXAA needs 1280x720 | With non-default Brightness it is recreated twice a frame (Measured: 1.2 ms). Fixing F1 removes it |
| M2 | The depth SRV is rewritten in one heap slot every frame with two frames in flight ([xess.cpp:1452](port/kz3/xess.cpp#L1452)) | Benign while the resource is unchanged, but a D3D12 rule violation. Write it only when the resource changes |
| M3 | Depth is resolved at present, not at the end of the scene | A later write to that zeta would feed XeSS the wrong depth. Whether the game does this needs a run |
| M4 | A 250 ms hitch resets history ([patch line 682](port/patches/2139-xess-super-resolution.patch#L682)) | Game time is frame-locked, so a hitch does not invalidate history. `frame_ms` has no other use |
| M5 | Jitter phases clamp to 64; Intel's rule gives 72 at 3x | Only reached near 4K output |
| M6 | `xessIsOptimalDriver` goes only to stderr | Intel asks for a notice to the user |
| M7 | The Native AA branch is unreachable | The renderer requires output larger than input |
| M8 | The context stays alive after switching filter away | VRAM is held until exit |
| M9 | Pins appear in `stage_xess.py`, `assemble_payload.py`, `test_xess.ps1` and the docs | Import them from one place |
| M10 | All of `port/kz3` joins the runtime include path | `xess.h` also shares a name with Intel's header |
| M11 | Cut detection needs more than 5 units or 86 degrees | Smaller cuts ghost; tune once F3 is done |
| M12 | The responsive mask contains jitter | The guide says it should not. Low impact |
| M13 | Lighting into `0x01E40000` is not jittered | Sub-pixel, probably invisible. Worth a look if lit edges shimmer |

## What holds up

| Area | Result | How |
|---|---|---|
| ABI | Every field offset and size in the version, execute and init structs, plus the enum values spot-checked, matches SDK 3.0.2 | Compile-time asserts against Intel's headers |
| Runtime identity | API 2.0.2, file version 2.0.2.68, Authenticode valid, signer thumbprint equals the pin | Run on the pinned DLL |
| Pins | DLL, licence, notices and SDK zip all match their SHA-256 values | `sha256sum` |
| Jitter | Sign and scale are right; the offset is post-divide (`_p.w * vp_posoffset`) | Read and Measured |
| Motion vectors | NDC y-up, current to previous, matches the pixel-unit control exactly | Measured |
| Motion shader | Depth linearisation, jitter removal and the 44-constant layout are correct | Read, host test |
| Quality policy | All 16:9 sizes tested from 1600x900 to 3840x2160 fall inside the runtime's input range | Measured |
| Resource states | Match the guide, and are restored on every failure path including failed dispatch | Read |
| Heap and state rebinding | Gamma and blit rebind heaps; `LD_SM_INVALIDATE` is called after dispatch | Read |
| `g_dm_pso` indexing | The new filter value cannot index out of range; all `g_dm_filter` uses checked | Read |
| Isolation | The submodule is untouched, code is guarded by `KZ3_XESS`, and all 291 patches apply to the pinned commit | Patch series applied |
| DLL loading | Absolute path beside the exe, `DLL_LOAD_DIR` and `SYSTEM32` only | Read |
| Fallback | Missing DLL, wrong DLL and bad aspect all fail closed | Run on the RTX 5070 |
| Options UI | Dialog mirrors the FPS warning, the bridge accepts `xess`, reset returns to Bilinear | Read, host test |
| Packaging | The real dry run lists the adapter sources and DLL, and the scrub is clean; the 6 missing sources are fonts, SDL3 and a stale local stamp | Run |
| Licence | Intel's terms are met: unmodified, copyright and terms shipped, no endorsement; NOTICE row present | Read |
| Bilinear and Nearest | Unchanged in effect. Two harmless differences: size sync now precedes gamma, and a mismatched `CopyResource` is refused | Read |

## Measurements

All run on the RTX 5070 with the pinned DLL. The probe used only the public API. It was a throwaway and is not in the
repository.

**Sign conventions.** XeSS received a hard-edged rotated checker for 64 frames, with the scene moving 1.25 and 2.75
input pixels per frame and flat depth. The last frame was scored against a 4x4 supersampled ground truth on the central
quarter. Only the relative differences mean anything.

| Variant | PSNR |
|---|---:|
| Static, jitter as the port applies it | 25.94 dB |
| Static, jitter negated | 16.31 dB |
| Moving, NDC motion as the port writes it | 21.08 dB |
| Moving, pixel-unit motion (control) | 21.09 dB |
| Moving, Y flipped | 10.00 dB |
| Moving, X flipped | 10.30 dB |
| Moving, both flipped | 10.71 dB |
| Moving, zero motion | 10.96 dB |

**Input range for a 1280x720 input.**

| Output | Scale | Quality | Runtime optimal | In range | Adapter aspect check |
|---|---:|---|---|---|---|
| 1366x768 | 1.067 | Ultra Quality Plus | 1051x591 | yes | **refused** |
| 1600x900 | 1.250 | Ultra Quality Plus | 1231x693 | yes | passes |
| 1778x1000 | 1.389 | Ultra Quality | 1186x667 | yes | **refused** |
| 1920x1080 | 1.500 | Ultra Quality | 1280x720 | yes | passes |
| 2560x1440 | 2.000 | Balanced | 1280x720 | yes | passes |
| 3840x2160 | 3.000 | Ultra Performance | 1280x720 | yes | passes |

**Costs.**

| Item | Result |
|---|---|
| Context create | 22 ms |
| First init in a process | 378 ms |
| Later init at any size | 15-18 ms |
| Context destroy | about 10 ms |
| Execute, 1080p / 1440p / 4K output | 0.78 / 1.13 / 2.23 ms |

Execute was timed by fence wall time on an idle GPU with blank inputs, so treat those figures as a lower bound. Even so,
at the 16-22 frames/s the README reports, GPU cost is not the constraint. The first-init figure may benefit from driver
caches, so a clean machine could be slower. That init runs on the frame thread, which also ticks the guest's vblanks.

## Tests run

| Test | Result |
|---|---|
| `test_stage_xess`, `test_assemble_payload`, `test_pack_zip`, `test_launch`, `test_licenses_match`, `test_firstrun_check` | pass (Python on Windows; see F11 for the licence test) |
| `tools/features.py --check`, `tools/port_ui_table.py` | pass |
| `test_xess_motion` (9 checks), `test_xess_inputs` (10 checks) | pass |
| `test_display_mode` | pass, without a real `displaysettings.gum` and without ASan |
| `xess.cpp` with `-Wall -Wextra -Werror`, C++20, g++ | clean |
| `test_xess.cpp`, normal and both `--expect-unsupported` runs | pass |

## Not verified

- **Anything in the game.** The in-game results in `docs/XESS.md` were not reproduced. Pose axis direction and the
  flip-to-frame offset (F3) need a run.
- **Other GPUs and Proton.** Only NVIDIA's cross-vendor path was exercised. Proton is the project's main test
  environment, and the FXAA code notes shader restrictions for Wine's compiler that the adapter's shaders have not
  been tried against.
- **The bundled clang.** Release builds use llvm-mingw; only g++ was tried here.
- **The full CI list.** `tools/run-host-tests.sh`, shellcheck and the Nix shells were not run.
- **The runtime's internals.** `docs/XESS.md` says XeSS-SR does not contact the network. The adapter contains no network
  code, but `libxess.dll` itself was not examined, and Intel's licence forbids reverse engineering it.

## Documentation corrections

| `docs/XESS.md` | Problem |
|---|---|
| Lines 57, 160: "immediately before temporal presentation" | The capture is at the scene copy; FXAA, post and HUD follow (F6) |
| Lines 67-70, 231: logs the reason | Wrong reason for init failures (F4) |
| Line 134: cannot leave a jittered frame | True only for runtime and context failures (F7) |
| Line 239: stale camera falls back | Not checked (F3) |
| Line 272 onward, Known issues | Missing FXAA, mip bias, placement after tone-mapping, VC++ runtime and refused near-16:9 sizes |
| `docs/requirements.md` | Missing the VC++ runtime and the Windows build floor (F9) |
| [features.tsv:51](docs/features.tsv#L51) | Says the MLAA skip is "not active yet", but both override tables have it. Predates this branch and bears on F1 |

## Suggested order

1. F4, so every later run reports honestly.
2. F1 and F2, which are small and give the biggest visible gain.
3. The F3 offset log in one game run, then the ring and flip checks.
4. F5, F9, F11 and the documentation corrections.
5. F6's HUD mask, F7 and F8.
6. F10 and F13, then renumber for F12 at merge time.
