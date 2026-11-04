# XeSS-SR review: branch `xesstest`

Commits `05c978d` and `085d371` against `main`, reviewed 2026-10-31. The review read the adapter, patch 2142, the
renderer code around it, the release tooling and Intel's SDK 3.0.2 headers and developer guide, then ran the tests
and a black-box probe on an RTX 5070 (driver 617.42). The game was not run, so in-game behaviour here comes from
reading the code. No code was changed by the review itself.

**Status (updated 2026-11-03):** the fixes were made, the camera-pairing bug was corrected, and the v0.1.8-integrated
build was exercised in gameplay. Whole-scene XeSS jitter remains. Severe surface corruption was also observed with
both XeSS and Bilinear but did not recur in later runs; its trigger is unknown. The branch is not ready as an
image-quality fix. See [Fix record](#fix-record), [Follow-up](#follow-up-whole-scene-jitter-further-issues-and-future-work)
and [Post-merge gameplay findings](#post-merge-gameplay-findings). The original findings below are kept as written, so
their line references are to commit `2c4fa7a`.

The v0.1.8 integration renumbered the GPU patches from 2139-2141 to 2147-2149 and the XeSS patches from
2142-2144 to 2150-2152. Historical patch numbers and line references below describe the versions reviewed at the time.

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

Renderer references point into [patch 2142, now 2150](port/patches/2150-xess-super-resolution.patch) where the code is new, and
otherwise name the function in `libs/video/rsx_live_draw.c` of the patched toolkit.

## Must fix before merge

### F1. FXAA runs on the jittered scene (Read)

`KZ3_HOST_AA` unset means host AA mode 1, so `kz3_mlaa_body` skips SPU MLAA and arms FXAA
([kz3_ppu_overrides.cpp:866](port/spu/kz3_ppu_overrides.cpp#L866), active on 1.14 through
[ppu_overrides-114.txt:10](re/data/overrides/ppu_overrides-114.txt#L10)). The patch then calls `ld_host_aa(i)`
unconditionally, straight after the pre-upscale capture
([patch line 572](port/patches/2150-xess-super-resolution.patch)).

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
`g_xess_camera_frame != g_ld_frames` ([patch line 641](port/patches/2150-xess-super-resolution.patch)), which
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
   `g_xess_camera_frame` still `UINT32_MAX` ([patch lines 330-361](port/patches/2150-xess-super-resolution.patch)).
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
happens at the NV0039 scene copy, before FXAA, post and HUD ([patch lines 536-573](port/patches/2150-xess-super-resolution.patch)).
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
| M4 | A 250 ms hitch resets history ([patch line 682](port/patches/2150-xess-super-resolution.patch)) | Game time is frame-locked, so a hitch does not invalidate history. `frame_ms` has no other use |
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

## Fix record

Made 2026-10-31 in the commits after `2c4fa7a`, on `xesstest`, not pushed. The host and GPU tests passed before the
game run. Native gameplay testing later found and fixed a camera-pairing call-site bug, then isolated a remaining
whole-scene jitter to the projection/execute jitter path. The branch is still experimental and is not ready to push as
an image-quality fix.

### What changed

The renderer changes are in a new patch, [2143, now 2151](port/patches/2151-xess-review-fixes.patch), because `AGENTS.md` asks for a
new numbered patch and it can be dropped on its own terms: revert the commit as a whole, because the adapter API changed
with it.

| Finding | Status | What changed |
|---|---|---|
| F1 FXAA | Done | `ld_host_aa` is skipped on frames that were actually jittered. Frames outside XeSS keep FXAA |
| F2 Mip bias | Done | `log2(input / target)` added to units with a mip chain on jittered draws. The jitter decision moved ahead of sampler table construction. `KZ3_XESS_MIP_BIAS` overrides it |
| F3 Camera | Done after game correction | See [Decisions](#decisions). Flip ring, self-calibrating pairing against the renderer frame counter, same-flip and skipped-flip checks, `[xess] camera pairing` log lines, `KZ3_XESS_CAMERA_LAG` |
| F4 Failure reason | Done, except one part | A draw-time init failure reports `kz3_xess_status()` before the camera check, and failures are latched per device and size. Not done: showing the fallback on the Options row |
| F5 Project rules | Done | `ENHANCEMENT:`, `ASSUMPTION:` and `@0x` comments added; `scene_lr` checked for a preceding call; `features.tsv` updated with a new XeSS row and `FEATURES.md` regenerated |
| F6 After post and HUD | Partly, game-tested | The HUD mask is done: compose capture, zero motion and mask 1.0 on HUD pixels, `KZ3_XESS_HUD_MASK`. Disabling the mask did not change the measured whole-scene jitter. XeSS still runs after tone-mapping and post; moving it is the long-term work |
| F7 Jitter in fallback | Done | A jittered frame that falls back holds jitter off, and the following frames go through XeSS unjittered until one succeeds |
| F8 Aspect check | Done | One output pixel of tolerance. 1360x768 and 1920x1200 stay refused |
| F9 VC++ runtime | Done | `requirements.md` and `XESS.md` updated; the load failure names the redistributable when the DLL exists but a dependency does not |
| F10 Re-init | Done | The present path flushes first. A new `kz3_xess_would_recreate` replaces the `g_xess_context_w` test, so the skipped wait after a failed init is gone too |
| F11 Licence bytes | Done | `-text` in `.gitattributes`; the blobs now hash to the pins |
| F12 `fsrtest` | Done for numbering | The `"fsr31"` test string is restored. At the GPU-integration merge, GPU patches kept 2139-2141 and the XeSS pair moved to 2142-2143, so it no longer occupies the FSR patch numbers |
| F13 Coverage | Partly | Camera logic moved to `xess_camera.h` with a host test; GPU tests now read back mask and motion contents; temporal sign test added; handedness logged once. Not done: CI compiling `xess.cpp`, see below |
| M1 `g_aa_tex` | Done by F1 | FXAA and XeSS no longer alternate its size |
| M2, M3 Depth | Done | Depth is copied to an adapter texture at the scene copy, so no descriptor is rewritten per frame and later zeta writes cannot reach XeSS |
| M4 Hitch reset | Done | Removed, with `frame_ms` |
| M5 Jitter phases | Done | The clamp is now 72 |
| M8 Context lifetime | Done | Switching the filter away releases the context at the next present |
| M9 Pins | Done | `stage_xess.py` and `test_xess.ps1` read `assemble_payload.XESS_PINS`. The tables in the docs stay |
| M10 Include path | Done | CMake copies the three boundary headers into a build folder and only that joins the runtime's path. `xess.h` keeps its name |
| M6 Driver notice, M7 Native AA | Not changed | No small fix. M6 needs an in-game notice; M7 needs a decision on whether 720p output should use XeSS at all |
| M11 Cut thresholds, M13 Lighting jitter | Not changed | Both need a game run to judge. M11 was marked for tuning after F3 |
| M12 Mask jitter | Not changed | Low impact, and there is no cheap way to remove jitter from the captures. Now listed in `docs/XESS.md` |

### Decisions

- **F3 goes further than the review's order.** The review said to measure the flip offset in a run first. A fixed offset
  would have been a guess, so the offset is measured live. The first game run exposed that the proposed FIFO-only
  `g_ld_flip_consumed` counter stays zero on KZ3's active presentation path. That froze the requested flip, caused 600
  ring misses and 43 resets in the first 600 dispatches, and fell back to the latest camera almost every frame. Pairing
  now uses `g_ld_frames`, which advances after every presentation. A frame's own camera is published before its draws,
  so the smallest `latest - renderer frame` over a window is the measured offset. After the correction, the first 600
  dispatches had 4 resets, 3 repeats, 2 jumps and 2 misses. `KZ3_XESS_CAMERA_LAG=-1` still restores the old behaviour.
- **The `scene_lr` check accepts `bl` or `bctrl`.** `bctrl` alone might have rejected the correct 1.14 address, and that
  mistake could not be seen without a game run.
- **The biased sampler is applied after the texture loop.** That leaves the patch 2136 memo holding the guest's sampler,
  so it cannot serve a biased one to a HUD draw. Sampler cache exhaustion keeps the guest's sampler.
- **`flake.nix` is unchanged.** Compiling `xess.cpp` in CI needs MinGW in `.#ci`, and there is no Nix here to evaluate that
  change. `tools/smoke/test_xess_compile.sh` is in `host-tests.txt`, runs wherever a Windows-target compiler exists, and
  prints SKIP in CI today. Adding `win.buildPackages.gcc` to the `ci` shell would turn it on; expect a larger CI closure.
- **HUD pixels stay at XeSS's default 0.8 clamp.** Intel says to raise it only after validating the result, which needs
  the game.

One mistake along the way: a shell escaping error put a stray carriage return in `test_xess.ps1`. It was caught from an
oversized diff and amended before anything left this machine, so it is not in the history.

### What was run

On Windows, RTX 5070, driver 617.42, g++ 16 from WinLibs.

| Check | Result |
|---|---|
| All 292 patches from the pinned commit, with autocrlf off | Apply; the renderer is byte-identical to the edited tree |
| `rsx_live_draw.c` on its own, with and without `KZ3_XESS` | Compiles; no XeSS symbols without it; every adapter function it calls is defined |
| `xess.cpp` with `-Wall -Wextra -Werror` | Clean |
| Complete native game target after regenerating the 292-patch toolkit | Builds with bundled Clang 23.1.3; this is the real `main.cpp`, adapter and renderer compile |
| `test_xess_motion`, `test_xess_inputs`, `test_xess_camera`, `test_xess_compile`, `test_display_mode` | Pass |
| Each fix's bug put back in a scratch copy (7 mutations) | All 7 caught by the intended check |
| `test_xess.ps1`, the whole script | Pass: mask reads 204/0/255, motion is zero under the HUD box and non-zero elsewhere, near-16:9 re-init works, missing and wrong DLL fall back |
| `test_xess_temporal` | Pass, same numbers as the review's probe |
| `test_stage_xess`, `test_assemble_payload`, `test_pack_zip`, `test_launch`, `test_licenses_match`, `tools/port_ui_table.py`, `tools/features.py --check` | Pass |
| The CMake `configure_file` loop in a small test project | Works |
| Native gameplay, normal jitter and HUD mask on | XeSS dispatches 1280x720 to 1920x1080 with generated responsive/HUD masks and mip bias -0.586; camera basis +1.000; 600 dispatches / 4 resets after the pairing correction; constant small whole-scene jitter remains |
| Native gameplay, `KZ3_XESS_HUD_MASK=0` | Whole-scene jitter is unchanged. The HUD remains visible, as expected: this switch disables its mask, not HUD drawing |
| Temporary diagnostic build with projection and execute jitter forced to zero | XeSS remains active with depth, motion and the scene responsive mask; the whole-scene jitter disappears |
| GPU integration plus XeSS, all 295 patches from the pinned commit | Apply. GPU patches 2139-2141 precede the regenerated XeSS patches 2142-2143; the XeSS pair applies to that exact base without whitespace relaxation |
| Combined focused host checks | `test_display_mode`, all XeSS host tests, `test_vtx_gpu`, `test_vtx_dev`, `test_wb_gpu`, native gates, XeSS/release payload tests, UI table, and generated features pass |
| Combined hardware checks on the RTX 5070 | The shared D3D12 GPU-offload test and the complete `test_xess.ps1` dispatch, temporal, re-init, readback, and fallback checks pass |

Not run:

- a native game build or gameplay run after the GPU integration merge; the gameplay results above are from the
  pre-merge XeSS toolkit
- the D3D12 debug layer (the optional Windows Graphics Tools feature is not installed here, so the new barriers are
  checked by reading and clean GPU runs only)
- Proton
- Intel and AMD GPUs
- shellcheck
- `tools/run-host-tests.sh` and the Nix shells
- `flake.nix`

### Gameplay result and remaining test plan

The then-complete 292-patch XeSS toolkit was regenerated before the pre-GPU-merge native build. The normal temporal
path reaches gameplay, but it is not visually stable: a small constant whole-scene jitter remains. Turning the HUD
mask off did not change it.
Forcing both the game projection jitter and the jitter supplied to XeSS to zero removed it, while XeSS continued to
dispatch. That diagnostic was temporary and is not a proposed final mode; it isolates the next investigation to the
actual game projection displacement, the value/sign/scale supplied to XeSS, or inconsistent jitter coverage across
scene draws.

After correcting that path, rerun gameplay at 1920x1080 with `KZ3_DISPLAY_FILTER=xess`, then read the log. In order:

1. `[xess] camera basis: cross(right, forward).up = +1.000`. **If this says -1, stop:** the pose axes are not what the
   reprojection assumes and motion will be wrong everywhere.
2. `[xess] frame N: XeSS-SR 2.0.2 ...` should say `responsive=generated, hud=masked, mip bias -0.586`.
3. `[xess] camera pairing: renderer frame ...` lines. While playing, `used` should rise by one a frame, usually one
   behind `latest` if the PPU runs ahead. `misses`, `repeats` and `jumps` should stay near zero. `repeats` rising during
   pause is expected.

Then look at the picture:

| Check | Expect | If not |
|---|---|---|
| Turn the camera with the HUD up | Text stays crisp, with no smear | The compose capture is not catching the HUD |
| Whole picture looks unaccumulated or aliased | Should not happen | Run with `KZ3_XESS_HUD_MASK=0`. If that fixes it, the first display draw is not the whole compose |
| Compare with `KZ3_XESS_MIP_BIAS=0` | Sharper textures, no new shimmer | Try a smaller bias such as `-0.3` |
| Slow steady pan | No ghosting on static geometry | Try `KZ3_XESS_CAMERA_LAG=1`, then `-1` to compare with the old behaviour |
| Camera cuts in cutscenes | No ghost frame after the cut | Tune M11 |
| Pause menu over the scene | No drift or smear on the frozen image | Report the `repeats` count |
| Drag the window size, Alt+Enter | No device removal or crash; a brief stall per size is expected | Keep the log |
| Options: XeSS to Bilinear and back | Clean switch; XeSS comes back | Keep the log |
| Non-default Brightness | Correct brightness | Keep the log |
| 1366x768 window | XeSS engages | Keep the log |
| Rename `libxess.dll` | One log line naming the real cause, picture stays Bilinear, no shimmer | Keep the log |

If Graphics Tools can be installed, one run with the D3D12 debug layer would check the new barriers.

## Follow-up: whole-scene jitter, further issues and future work

Written 2026-10-31 on `xesstest` at `37a1e3d`, after the first game run. This pass read [docs/XESS.md](docs/XESS.md),
this record, [features.tsv](docs/features.tsv) with the generated [FEATURES.md](docs/FEATURES.md),
[ROADMAP.md](docs/ROADMAP.md), the adapter, and the renderer in the local `work/ps3recomp-patched` tree (it holds the
2143 symbols; it was not diffed against the full series). It also read Intel's SDK 3.0.2 guide and headers. **Nothing
was built or run, there was no game run and no GPU test.** The only command run was `python tools/features.py --check`,
which passes. Everything below is therefore **Read** or **Inferred**, and the causes are ranked hypotheses, not
findings. Renderer references name the function, because the patched toolkit is generated and not in git.

### What is known about the jitter

| Observation | Source |
|---|---|
| Normal jitter gives a constant small whole-scene jitter | Game run, pre-GPU-merge build |
| `KZ3_XESS_HUD_MASK=0` does not change it | Game run |
| Forcing the projection jitter and the jitter given to XeSS to zero removes it; XeSS keeps dispatching with depth, motion and the scene mask | Game run, temporary build |
| 600 dispatches, 4 resets, 3 repeats, 2 jumps, 2 misses after the pairing fix | Game run |
| Jitter cycle at 1080p is 18 phases (`ceil(8 x 1.5^2)`), so roughly one cycle a second at 16-22 frames/s | Read ([xess.cpp:1254](port/kz3/xess.cpp#L1254)) |

The cycle length matters: any error that scales with the jitter shows up as a slow, regular wobble, which is easy to
see and matches the report.

### What the zero-jitter diagnostic does not show

The diagnostic changed two inputs at once, and it also changed what XeSS has to do. With no jitter every frame samples
the same points, so there is nothing to accumulate. A history that XeSS rejects or reprojects wrongly looks perfectly
stable in that mode, because the current frame is already the answer. The result therefore cannot tell "the jitter is
applied or reported wrongly" from "XeSS cannot use its history". The same goes for wrong motion vectors, which are also
invisible without jitter.

[ROADMAP.md:36](docs/ROADMAP.md#L36) says "matching the game projection shift to XeSS is the current blocker". That
states a cause the evidence does not isolate. [XESS.md](docs/XESS.md) known issue 1 and future work 1 name the projection
displacement, the execute value and draw coverage, which is closer, but still leave out the history and phase-lag
causes below.

### What reading the code says is right

- **Offset formula.** Intel's guide (SDK 3.0.2, `xess_sr_developer_guide_english.md` lines 273-276) adds `Jx*2/W` and subtracts `Jy*2/H` in the projection. The renderer adds `2*jx/Wv` to `xf[4]` and subtracts
  `2*jy/Hv` from `xf[5]` in `sink_end_impl`. Because `xf[4..5]` is the viewport offset applied after the divide
  (`_p.w * vp_posoffset` in the shared vertex epilogue), the shift is the same in NDC. It is a viewport translate, so it
  also reaches geometry the game transformed on the SPUs, which a projection edit would not.
- **Consistency.** The motion shader removes the same jitter (`ndc - 2*jx/w`), and the execute call and the draw read the
  same per-frame value. A per-frame mismatch between the two is not visible in the code.
- **Nothing unexpected on the fixed inputs:** `JITTERED_MV` is not set, which is right because motion excludes jitter;
  the sequence stays inside [-0.5, 0.5]; there is no jitter scale or velocity scale call.

What the reading cannot see is the part that matters: whether the pixels on screen actually move by the amount reported.

### What the existing measurements cover

[test_xess_temporal.cpp](tools/smoke/test_xess_temporal.cpp) feeds XeSS a synthetic checker with flat depth `0.5` and
motion values it writes itself. It checks XeSS's conventions in isolation. It does not run the renderer's jitter, the
adapter's depth-to-motion shader, the camera hooks or any game image. The review's "jitter sign and scale" and "motion
vectors" rows in *What holds up* are therefore about XeSS's expectations, not about this game. Also, the sign test
negated both axes together, so a single inverted axis is not excluded.

### Candidate causes, most likely first

| # | Hypothesis | Why plausible | What would settle it |
|---|---|---|---|
| C1 | XeSS cannot use its history, so each output is one jittered frame, which moves with the phase | Matches the symptom and hides in the diagnostic. Motion has only ever been checked on synthetic data. Possible reasons: wrong or mis-paired motion; wrong near and far (they come from unrecovered `vf17` offsets, see F5); the hooked pose differing from the pose the game rendered with, for example through sway or recoil applied later; a large responsive mask (any pixel that differs by 0.2 or more between the opaque and final captures is marked) weighting the current frame; depth that no longer matches a colour that has been through DOF and motion blur | Pause test and the motion-scale test below |
| C2 | Parts of the picture are not shifted while XeSS assumes they are | Only draws into `0x01300000` that are depth-tested against the primary depth, plus the primary pass, are jittered. Skipped: depth-test-off draws into the scene (sky, full-screen and additive effects), draws whose viewport is not exactly full (the check compares floats with `==`), any other target (half-resolution particles, AO, bloom) composited later, HUD, grain and vignette. Where those cover much of the frame, XeSS moves them by minus the jitter. Outdoor scenes with a large sky are the likely worst case | Per-class draw counters; two-phase input comparison |
| C3 | The image XeSS receives was made with a different phase than the one it is told | The jitter is the current renderer frame's, but the colour is the final display surface after the SPU post chain and compose. If any of that chain is a frame late, every dispatch is a phase off. The capture pairing is matched by renderer frame number, not by what the compose sampled | A one-phase lag switch on the dispatch value |
| C4 | One axis, or the whole sign, is inverted in game | Y passes two sign flips (`xf[1]` and `xf[5]`) plus the RSX to D3D viewport convention. A wrong sign makes XeSS "correct" by twice the shift, which looks like jitter. Intel's own debug tip is to try `+1` and `-1` jitter scales | Per-axis scale sweep; measured pixel displacement |
| C5 | Mip bias adds shimmer when accumulation fails | -0.586 on every scene texture sharpens and aliases the input. It amplifies C1 to C4 rather than causing them | `KZ3_XESS_MIP_BIAS=0` |
| C6 | The post-merge build behaves differently | The gameplay evidence predates patches 2139-2141. The shared draw tail and the vertex offload were changed after it. All three vertex epilogues read in the toolkit use `vp_posoffset` alike, but the offload path was not read | Rebuild and rerun the HUD, mask and jitter checks |

Unlikely, and why: FXAA (skipped on jittered frames, see F1), the HUD mask (tested), resets (4 in 600), a wrong
jitter scale (it matches Intel's formula), and exposure (not related to position).

### Experiments that would separate them

Experiments 3-5, 7 and 9 now have opt-in controls and tracing described in
[XeSS jitter diagnostics](docs/XESS.md#jitter-diagnostics). Experiments 6, 8, 10 and 11 still need additional code or
capture tooling. The earlier zero-jitter build was a temporary diagnostic. Anything XeSS dumps must stay under `work/`,
as `AGENTS.md` keeps game material out of the repository.

| # | Experiment | Correct implementation gives | Implicates if not |
|---|---|---|---|
| 1 | **Pause test**: pause on a still scene. The camera state repeats, so motion is zero and the scene is static. This is Intel's static-scene tip | A steady, sharpening picture | If it still shakes, motion is cleared and the cause is C2 to C4 (or C1 for a non-motion reason, such as the mask) |
| 2 | `KZ3_XESS_MIP_BIAS=0`, then `KZ3_XESS_CAMERA_LAG` as in the test plan | Calmer texture shimmer | C5 |
| 3 | Projection jitter on, execute jitter zero, then the reverse | Both shake. A wobble that goes with only one of them points at that side | Which side is wrong |
| 4 | **Two-phase jitter**: alternate `(+0.25, 0)` and `(-0.25, 0)` on the draw and the execute call, camera paused | Input frames differ by half an input pixel, output frames agree | Any remaining shake has a two-frame period, so it is easy to see and measure; its size gives the error in input pixels |
| 5 | Execute-only jitter scale `+1/-1` per axis, by scaling the execute offsets | The existing signs give the steadiest result | C4, and which axis |
| 6 | Velocity scale `0` with `xessSetVelocityScale`, then `+1/-1` | Paused scene: no change, because motion is already zero. Moving scene: zero scale is the worst case for ghosting but removes motion errors | C1 motion, if the jitter changes with the scale in a moving scene |
| 7 | Dispatch jitter lagged by one frame, `KZ3_XESS_EXECUTE_JITTER_LAG=1` | Worse | C3 if it improves things |
| 8 | Responsive mask forced to zero, as `KZ3_XESS_HUD_MASK=0` does for the HUD part | Little change | C1 (mask) if it improves things |
| 9 | Coverage counters per frame: draws into the scene target and the deferred set, split into jittered, depth test off, other depth slot, viewport not full, other target | Nearly all draws jittered | C2, and which class |
| 10 | Capture the pre-XeSS colour at two phases with the camera paused and cross-correlate | Whole-image shift equal to the phase difference, same sign on both axes | C2 (where it is not), C4 (sign) |
| 11 | Intel's `xessStartDump` (in `xess_debug.h`) on a bad frame, replayed offline | A reproducible case for the runtime's own inputs | Any of the above |

Measure rather than judge by eye: take the position shift between consecutive output frames in the paused scene by
cross-correlation. The number it gives should be below a tenth of an output pixel when things are right, and it gives a
figure to compare between builds.

### Additional issues found in this pass

| ID | Issue | Note |
|---|---|---|
| N1 | The zero-jitter diagnostic cannot isolate a cause | See above. Treat the current attribution in ROADMAP and XESS.md as a hypothesis |
| N2 | The adapter's motion pass is checked for content and by host-test maths, never through XeSS with game data | The temporal test supplies its own motion and flat depth. Nothing proves that the depth-to-motion shader and the hooked pose agree with what the game rendered |
| N3 | The camera pairing log shows numbers, not correctness | `used` and `latest` say how the ring was read, not whether the pose is the one the frame was drawn with. A slow steady pan with a measured residual would check it |
| N4 | The jitter and coverage gates use float equality on the viewport | `vp.scale == W*0.5f` and the same for the other three terms. A viewport that differs in the last bit still drops that draw from jitter; the opt-in trace now counts such deferred/scene candidates as `partial-viewport` |
| N5 | No one-line summary of why a frame did or did not jitter | Addressed for diagnostics: the opt-in trace reports each sampled frame's jitter state, phase and draw-class counts |
| N6 | The unrecovered `vf17` near and far offsets are the basis of the depth maths | Already marked `ASSUMPTION:`. A wrong far plane misplaces every pixel's depth. Check against a known object distance |
| N7 | Post-merge HEAD was unbuilt and unrun | The original GPU-offload integration was compiled and startup-tested after this review; the later v0.1.8 integration still needs a paused gameplay check |
| N8 | The test plan expects `mip bias -0.585`; the log prints `-0.586` | The fixed-point value is -150/256. Change the plan to `-0.586` |
| N9 | `reset_history` is also set whenever the camera has no previous state | Not a bug, but a long run of such frames would hide in the "resets" count; log them separately from jump resets |
| N10 | FEATURES.md shows "available" beside a visible defect | The table allows only `available` or `planned` for port rows, and the note carries the caveat, so this is accurate. A reader scanning the status column still sees no warning. Consider a first sentence in the note, "Experimental; visible jitter" |
| N11 | The depth snapshot is taken at the scene copy; colour is taken after the post chain | They are different stages of the same frame. Post effects such as DOF and motion blur change the colour but not the depth, so XeSS's depth-based history checks work from a depth that no longer describes the colour |

The documents are otherwise consistent with each other: the patch numbers, switch names, `KZ3_XESS_*` defaults, GPU-merge
caveat and gameplay figures agree between `XESS.md`, the Fix record and `features.tsv`, and `tools/features.py --check`
passes.

### Future work

Ordered by what unblocks what.

1. **Find the cause of the jitter** with experiments 1, 3, 4, 5 and 9 above. They are small, and between them they decide
   among C1 to C4. Do experiment 1 first, since it costs no code.
2. **Keep the added debug switches and one-line frame summary** (N5). They are cheap, and every later image-quality
   question will need them.
3. **Fix what the experiments show.** Probable shapes: extend jitter to the missed draw classes (C2); move or lag the
   jitter used for dispatch to the phase the image really carries (C3); correct an axis (C4); repair motion or pairing
   (C1).
4. **Extend the merged-branch gameplay checks** (N7): the native build and paused scene have now been observed, but
   the full picture table, intermittent surface corruption, other scenes, and broader hardware remain unverified.
5. **Replace camera-only motion** with per-object and skeletal motion (XESS.md future work 2). Until then, moving
   characters ghost whatever else is fixed.
6. **Move XeSS earlier**: capture before tone-mapping and the HUD (F6) and draw the HUD afterwards at output
   resolution. This removes the HUD from the temporal solve and puts XeSS where Intel recommends. It is the largest
   quality step after the jitter, and it needs the compose and post stages understood first.
7. **Recover the real constants:** near and far, the 1.00 `scene_lr`, the surface offsets, and the cut thresholds, then
   remove the matching `ASSUMPTION:` marks (F5, M11).
8. **Exposure and reactivity:** replace the difference mask with something that carries meaning where the renderer
   exposes it, and test the game's exposure instead of the fixed `1.0` (XESS.md future work 4 and 5).
9. **Wider testing:** Intel and AMD GPUs, Proton (shader compiler limits noted for FXAA), the bundled clang build, the
   D3D12 debug layer, and 1366x768 (see the open items under *Not verified*).
10. **Product questions, once the picture is stable:** an in-game quality selector, showing the fallback state on the
    Options row (the F4 item left open), the driver notice (M6), and whether 720p output should use XeSS at all (M7).
11. **Process:** put the CI compile of `xess.cpp` behind the MinGW addition to `.#ci` that the Fix record describes, and
    move a version of the temporal test into the host tests, with a mode that drives the adapter's own motion shader.

### Documentation changes this implies

Completed during the subsequent PR-preparation pass:

| Document | Change |
|---|---|
| [ROADMAP.md:34-37](docs/ROADMAP.md#L34) | Say the cause is open, instead of naming the projection shift as the blocker |
| [XESS.md](docs/XESS.md) Known issues 1 and Future work 1 | Add history use and phase lag to the list of things to measure, and point to this section |
| [features.tsv:54](docs/features.tsv#L54) | Open the note with "Experimental; visible jitter", then run `python3 tools/features.py` and commit both |
| The *Gameplay result and remaining test plan* above | Change `mip bias -0.585` to `-0.586` (N8) |

### PR-preparation build follow-up

The first full native build of the merged GPU/XeSS tip exposed a renderer compile failure. Patch
[2142, now 2150](port/patches/2150-xess-super-resolution.patch) had generalized the gamma input barrier, but its small barrier
hunk matched the structurally identical FXAA block after the merge. FXAA therefore referred to undeclared
`source`/`source_state` variables, while the gamma pass referred to the removed `sf` variable.

[Patch 2144, now 2152](port/patches/2152-xess-fxaa-gamma-barriers.patch) restores the FXAA surface transition and the gamma
function's supplied source transition. `test_xess_compile` now checks the two barriers within their respective
function bodies. The corrected 296-patch toolkit built the complete native game target with bundled Clang 23.1.3.
The root `xess-jitter-run.cmd` then started that exact executable with the normal 1920x1080 Fit XeSS configuration;
the process remained responsive and the startup log selected XeSS. It was stopped before gameplay, so that run
verified the build and launch path only. The later post-merge gameplay evidence follows.

### Post-merge gameplay findings

On 2026-11-03 the v0.1.8-integrated diagnostic build ran natively at 1280x720 input and 1920x1080 output on an
RTX 5070. Optional GPU vertex offload was disabled. The comparison builds used the same game inputs and effective
cache paths; the exact source revision of the older build is not verified.

**Surface corruption.** Severe corruption appeared in initial runs of the new build with XeSS and, in a separately
started run, Bilinear. The older build did not show it with Bilinear in the compared scene. Later new-build runs
were clean with `LD_NV3089_ROWS=0`, `1`, and `2`; switching the last run from Bilinear to XeSS also remained clean.
The host row-copy equivalence test passed. The first clean run suggested a row-copy regression, but the clean
`2` rerun did not confirm it. The corruption is not currently reproducible, and neither a XeSS-only cause nor a
row-copy cause has been established. Do not treat either filter as proven free of it.

**Jitter.** Normal XeSS visibly jittered in the paused scene; Bilinear did not. With horizontal two-phase jitter the
movement looked horizontal and about as noticeable as normal XeSS. Reversing only the horizontal execute offset
looked calmer, and restoring the original sign looked worse again. For this symmetric two-phase pattern, inversion
and a one-frame phase lag are equivalent except at resets or gaps, so that comparison cannot distinguish them.

Three horizontal-only Halton runs then compared the original execute offset, its inverted sign, and a one-frame
execute lag. The observer reported less jitter with inversion and lag than with the original, but was unsure
whether inversion or lag looked better. The trace confirmed that lag was applied on most sampled successful frames.
For each run, 120 desktop samples of the same selected 320x192 window region were analyzed in memory at about
40 samples per second; no frames were saved. Approximate 95th-percentile horizontal shifts between consecutive
samples were:

| Halton diagnostic | Estimated shift |
|---|---:|
| Original execute offset | 0.84 output px |
| Inverted horizontal execute offset | 0.36 output px |
| One-frame execute lag | 0.17 output px |

These are single short samples, not synchronized GPU-frame measurements. Region content, animation or overlays,
desktop sampling cadence, and the displacement estimator can bias them; they do not certify the `<0.1`
output-pixel target or prove that the input color is one frame late. The diagnostics deliberately make the execute
offset differ from the projection while depth and motion remain on their existing paths. Do not enable either
change by default on this evidence. Next compare the pre-XeSS color phase and scene coverage with the projection,
depth, and camera for the same frames, then validate any coherent fix in static and moving scenes.
